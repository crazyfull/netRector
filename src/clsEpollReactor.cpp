#include "clsEpollReactor.h"
#include "clsLog.h"

#include "clsSocketList.h"
#include "clsTimer.h"
#include "clsTimerManager.h"
#include "clsUDPSocket.h"
#include <netinet/in.h>

EpollReactor::EpollReactor(int id, int maxConnection, int max_events, size_t bufferPoolSize): m_reactorID(id), m_maxEvent(max_events), m_maxConnection(maxConnection), m_bufferPool(bufferPoolSize)
{
    init();
}

EpollReactor::~EpollReactor() {
    // thread haye dige (TLS key workers) dige nabayad post() konan
    if (m_liveToken) {
        std::lock_guard<std::mutex> lock(m_liveToken->mutex);
        m_liveToken->reactor = nullptr;
    }

    // tartib mohem hast: har chizi ke az chize dige estefade mikone zoodtar az beyn mire

    // 1) hame connection ha va listener ha
    shutdown_all();

    // 2) DNS: darkhast haye montazer ba natije khali javab dade mishan
    if(m_pDNSLookup){
        delete m_pDNSLookup;
        m_pDNSLookup = nullptr;
    }

    // 3) object haye baste shode (ghabl az nabodi buffer pool)
    m_GCList.flush_all();

    if(m_pTimers){
        delete m_pTimers;
        m_pTimers = nullptr;
    }

    if(m_wakeupFd != -1) {
        del_fd(m_wakeupFd, true);
        ::close(m_wakeupFd);
        m_wakeupFd = -1;
    }

    if(m_spareFd != -1) {
        ::close(m_spareFd);
        m_spareFd = -1;
    }

    if(m_pConnectionList){
        delete m_pConnectionList;
        m_pConnectionList = nullptr;
    }

    if(m_epollSocket != -1) {
        ::close(m_epollSocket);
        m_epollSocket = -1;
    }

    delete[] m_readBuffer;
    delete[] m_scratchBuffer;
}

void EpollReactor::init()
{
    updateCashedTime();
    m_liveToken = std::make_shared<LiveToken>();
    m_liveToken->reactor = this;

    m_readBuffer = new char[READ_BUFFER_SIZE];
    m_scratchBuffer = new char[READ_BUFFER_SIZE + 64];

    //
    m_pConnectionList = new SocketList(m_maxConnection);

    //
    m_epollSocket = epoll_create1(EPOLL_CLOEXEC);
    if(m_epollSocket == -1)
        throw std::runtime_error("epoll_create1");

    //
    m_wakeupFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if(m_wakeupFd == -1)
        throw std::runtime_error("eventfd");

    m_wakeupEvent.events = EPOLLIN;
    if(!register_fd(m_wakeupFd, &m_wakeupEvent, IS_WAKEUP_SOCKET, this))
        throw std::runtime_error("register wakeup fd");

    // baraye inke dar halate EMFILE betonim connection haye montazer ro rad konim
    m_spareFd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);

    //
    m_pDNSLookup = new DNSLookup(this);
    m_pDNSLookup->setTimeout(DNS_LOOKUP_TIMEOUT_SEC);
    m_pDNSLookup->setCache_ttl_sec(DNS_CACHE_TTL_SEC);
    m_pDNSLookup->setMaxRetries(DNS_MAX_RETRIES);

    //
    m_pTimers = new TimerManager;
    m_pTimers->setReactor(this);

    //timeout haye connect / closing / idle: har bar yek tike az jadval (bedone stall)
    m_pTimers->addTimer(MAINTENANCE_SLICE_MS, [this] () {
        maintenance();
    });

    //DNS timeout timer
    m_pTimers->addTimer(DNS_TIMEOUT_INTERVAL_MS, [this] {
        this->checkDnsTimeouts();
    });

    //Garbage collector timer
    m_pTimers->addTimer(GARBAGE_COLLECTOR_INTERVAL_MS, [this] {
        this->runGarbageCollector();
    });

}

void EpollReactor::setOnAccepted(acceptCallback fncallback, void* p) {
    m_onAcceptCallback = fncallback;
    m_onAcceptCallbackCtx = p;
}

uint64_t EpollReactor::make_key(int fd, uint32_t genID) {
    return (static_cast<uint64_t>(genID) << 32) | static_cast<uint32_t>(fd);
}

int EpollReactor::extract_fd(uint64_t key) {
    return static_cast<int>(key & 0xFFFFFFFF);
}

uint32_t EpollReactor::extract_gen(uint64_t key) {
    return static_cast<uint32_t>(key >> 32);
}

bool EpollReactor::addFlags(SocketContext *pContext, uint32_t flags)
{
    if (pContext->fd < 0)
        return false;

    if ((pContext->ev.events & flags) != flags){
        pContext->ev.events |= flags;

        if(epoll_ctl(m_epollSocket, EPOLL_CTL_MOD, pContext->fd, &pContext->ev) == -1){
            NR_PERROR("EPOLL_CTL_MOD mod_add");
            return false;
        }
        return true;
    }
    return false;
}

void EpollReactor::removeFlags(SocketContext *pContext, uint32_t flags)
{
    if (pContext->fd < 0)
        return;

    if (pContext->ev.events & flags) {
        pContext->ev.events &= ~flags;

        if(epoll_ctl(m_epollSocket, EPOLL_CTL_MOD, pContext->fd, &pContext->ev) == -1){
            NR_PERROR("EPOLL_CTL_MOD mod_remove");
        }
    }
}

bool EpollReactor::setFlags(SocketContext *pContext, uint32_t events)
{
    if (pContext->fd < 0)
        return false;

    pContext->ev.events = events;
    if(epoll_ctl(m_epollSocket, EPOLL_CTL_MOD, pContext->fd, &pContext->ev) == -1){
        NR_PERROR("EPOLL_CTL_MOD setFlags");
        return false;
    }
    return true;
}

void EpollReactor::del_fd(int fd, bool removeFromList) {
    if (m_epollSocket != -1)
        epoll_ctl(m_epollSocket, EPOLL_CTL_DEL, fd, nullptr);

    if(removeFromList && m_pConnectionList)
        m_pConnectionList->remove(fd);
}

void EpollReactor::unregister_fd(int fd)
{
    if (m_pConnectionList)
        m_pConnectionList->remove(fd);
}

bool EpollReactor::add_fd(int fd, epoll_event *pEvent, uint32_t events)
{
    pEvent->events = events;
    if(epoll_ctl(m_epollSocket, EPOLL_CTL_ADD, fd, pEvent) == -1){
        NR_PERROR("EPOLL_CTL_ADD add_fd");
        return false;
    }

    return true;
}


bool EpollReactor::add_listener(int port, const char* bindIP, acceptCallback cb, void* ctx)
{
    return add_listener_fd(port, bindIP, cb, ctx) != -1;
}

int EpollReactor::add_listener_fd(int port, const char* bindIP, acceptCallback cb, void* ctx)
{
    sockaddr_storage addr;
    memset(&addr, 0, sizeof(addr));
    socklen_t addrLen = 0;
    int family = AF_INET6;
    bool dualStack = false;

    sockaddr_in* a4 = reinterpret_cast<sockaddr_in*>(&addr);
    sockaddr_in6* a6 = reinterpret_cast<sockaddr_in6*>(&addr);

    if (!bindIP || !*bindIP || strcmp(bindIP, "*") == 0 || strcmp(bindIP, "::") == 0) {
        // Dual-Stack ipv4 and ipv6
        family = AF_INET6;
        dualStack = true;
        a6->sin6_family = AF_INET6;
        a6->sin6_addr = in6addr_any;
        a6->sin6_port = htons((uint16_t) port);
        addrLen = sizeof(sockaddr_in6);
    } else if (inet_pton(AF_INET, bindIP, &a4->sin_addr) == 1) {
        family = AF_INET;
        a4->sin_family = AF_INET;
        a4->sin_port = htons((uint16_t) port);
        addrLen = sizeof(sockaddr_in);
    } else if (inet_pton(AF_INET6, bindIP, &a6->sin6_addr) == 1) {
        family = AF_INET6;
        a6->sin6_family = AF_INET6;
        a6->sin6_port = htons((uint16_t) port);
        addrLen = sizeof(sockaddr_in6);
    } else {
        NR_LOG(netrector::LOG_ERROR, "add_listener: invalid bind address '%s'", bindIP);
        return -1;
    }

    int listen_fd = ::socket(family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if(listen_fd < 0) {
        NR_PERROR("socket(listener)");
        return -1;
    }

    // enable multi listiner (har shard listener khodesh ro dare)
    TCPSocket::setSocketShared(listen_fd, true);
    TCPSocket::setSocketResourceAddress(listen_fd, true);

    // option haye TCP az listener be socket haye accept shode ers miresan (Linux),
    // pas be jaye 5 setsockopt dar har accept, yek bar inja set mishan.
    TCPSocket::setSocketNoDelay(listen_fd, true);
    TCPSocket::setSocketKeepAlive(listen_fd, true);
    if (m_deferAccept > 0)
        setsockopt(listen_fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &m_deferAccept, sizeof(m_deferAccept));

    if (family == AF_INET6) {
        // IPV6_V6ONLY ro 0 mikonim ke dual stack beshe
        int optval = dualStack ? 0 : 1;
        if (setsockopt(listen_fd, IPPROTO_IPV6, IPV6_V6ONLY, &optval, sizeof(optval)) < 0) {
            NR_PERROR("setsockopt(IPV6_V6ONLY)");
            ::close(listen_fd);
            return -1;
        }
    }

    if(bind(listen_fd, (sockaddr*) &addr, addrLen) < 0) {
        NR_PERROR("error bind");
        ::close(listen_fd);
        return -1;
    }

    if(::listen(listen_fd, LISTEN_BACKLOG) < 0) {
        NR_PERROR("error listen");
        ::close(listen_fd);
        return -1;
    }

    ListenerInfo* info = new ListenerInfo{listen_fd, port, cb, ctx};

    // add listener fd to epoll
    struct epoll_event listen_ev;
    memset(&listen_ev, 0, sizeof(listen_ev));
    listen_ev.events = EPOLL_LISTINER_EVENTS;
    if (!register_fd(listen_fd, &listen_ev, IS_TCP_LISTENER, info)) {
        ::close(listen_fd);
        delete info;
        return -1;
    }

    // add to listener list (faghat bad az movafaghiyat kamel)
    m_listenerList.push_back(info);
    return listen_fd;
}

bool EpollReactor::remove_listener(int fd)
{
    for (auto it = m_listenerList.begin(); it != m_listenerList.end(); ++it) {
        if ((*it)->fd == fd) {
            ListenerInfo* info = *it;
            m_listenerList.erase(it);
            unregister_fd(fd);
            ::close(fd);
            delete info;
            return true;
        }
    }
    return false;
}

void EpollReactor::stop_listener()
{
    for (ListenerInfo* info : m_listenerList) {
        del_fd(info->fd, true);
        ::close(info->fd);
        delete info;
    }

    m_listenerList.clear();
}


void EpollReactor::adoptAccepted(ListenerInfo* listener) {

    // default: yek accept dar har event (listener level-triggered hast va agar connection
    // dige-i montazer bashe epoll_wait badi bela faseleh barmigarde). in jori accept-e
    // ezafe-i ke faghat EAGAIN bargardone nadarim.
    int limit = m_multiAccept ? MAX_ACCEPT_PER_EVENT : 1;

    for (int i = 0; i < limit; i++) {
        sockaddr_storage cli;
        socklen_t cl = sizeof cli;
        int fd = ::accept4(listener->fd, (sockaddr *)&cli, &cl, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; // all request accepted
            }

            if (errno == EINTR || errno == ECONNABORTED || errno == EPROTO) {
                continue; // try again
            }

            if (errno == EMFILE || errno == ENFILE) {
                // fd tamam shode. ba azad kardane fd zakhire, connection montazer
                // accept va bela faseleh baste mishe ta dar backlog namone
                // (listener level-triggered hast va bedone in kar halghe mishe).
                if (m_lastAcceptErrorLog != getCachedNow()) {
                    m_lastAcceptErrorLog = getCachedNow();
                    NR_LOG(netrector::LOG_ERROR, "accept4: too many open files, rejecting connections (raise ulimit -n)");
                }

                if (m_spareFd == -1)
                    m_spareFd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);

                if (m_spareFd != -1) {
                    ::close(m_spareFd);
                    m_spareFd = -1;
                    int tmp = ::accept4(listener->fd, nullptr, nullptr, SOCK_CLOEXEC);
                    if (tmp != -1)
                        ::close(tmp);
                    m_spareFd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
                    if (tmp != -1)
                        continue;
                }

                // bedone fd-e zakhire listener-e level-triggered halghe-ye masraf CPU misaze:
                // accept movaghatan (200ms) gheyre faal mishe
                pauseAccept();
                break;
            }

            NR_PERROR("accept4");
            break;
        }

        // option ha (NODELAY, KEEPALIVE) az listener ers reside

        if (m_maxConnections != 0 && tcpConnectionCount() >= m_maxConnections) {
            ::close(fd);    // saghf-e connection ha
            continue;
        }

        TCPSocket *pSocketbase = nullptr;
        if (listener->cb) {
            pSocketbase = listener->cb(listener->ctx);
        } else if (m_onAcceptCallback) {
            pSocketbase = m_onAcceptCallback(m_onAcceptCallbackCtx);
        }

        if (!pSocketbase) {
            //onAccepted callback function not found (ya app connection ro rad kard)
            ::close(fd);
            continue;
        }

        //set reactor
        pSocketbase->setReactor(this);
        //adopt in class
        pSocketbase->_accepted(fd);
    }
}

void EpollReactor::pauseAccept()
{
    if (m_acceptPaused)
        return;
    m_acceptPaused = true;

    for (ListenerInfo* info : m_listenerList) {
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = 0;
        SockInfo* slot = m_pConnectionList->slot((size_t)info->fd);
        if (slot) ev.data.u64 = make_key(info->fd, slot->genID);
        epoll_ctl(m_epollSocket, EPOLL_CTL_MOD, info->fd, &ev);
    }

    addTimer(200, [this]() { resumeAccept(); }, true);
}

void EpollReactor::resumeAccept()
{
    if (!m_acceptPaused)
        return;
    m_acceptPaused = false;

    for (ListenerInfo* info : m_listenerList) {
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLL_LISTINER_EVENTS;
        SockInfo* slot = m_pConnectionList->slot((size_t)info->fd);
        if (slot) ev.data.u64 = make_key(info->fd, slot->genID);
        epoll_ctl(m_epollSocket, EPOLL_CTL_MOD, info->fd, &ev);
    }
}

uint32_t EpollReactor::tcpConnectionCount() const
{
    return m_pConnectionList ? m_pConnectionList->countOfType(IS_TCP_SOCKET) : 0;
}

bool EpollReactor::register_fd(int fd, epoll_event *pEvent,SockTypes sockType, void* ptr)
{
    if(!pEvent)
        return false;


    SockInfo* sockinfo = m_pConnectionList->add(fd, sockType, ptr);
    if (!sockinfo) {
        NR_LOG(netrector::LOG_ERROR, "can not add fd %d to ConnectionList", fd);
        return false;
    }

    if(pEvent->events == 0){
        pEvent->events = EPOLL_EVENTS_TCP_NONBLOCKING;
    }

    //make key
    pEvent->data.u64 = make_key(fd, sockinfo->genID);
    if (epoll_ctl(m_epollSocket, EPOLL_CTL_ADD, fd, pEvent) == -1) {
        NR_PERROR("epoll_ctl add");
        m_pConnectionList->remove(fd);
        return false;
    }

    return true;
}

void EpollReactor::setUseGarbageCollector(bool newUseGarbageCollector)
{
    m_useGarbageCollector = newUseGarbageCollector;
}

bool EpollReactor::getIPbyName(const char *hostname, DNSLookup::callback_t callback, void *p, DNSLookup::QUERY_TYPE QuryType)
{
    if (!m_pDNSLookup)
        return false;

    return m_pDNSLookup->resolve(hostname, callback, p, QuryType);
}

void EpollReactor::cancelDNS(void *p)
{
    if (m_pDNSLookup)
        m_pDNSLookup->cancel(p);
}

void EpollReactor::deleteLater(ReactorObject *obj)
{
    if(obj)
        m_GCList.retire(obj);
}

TCPSocket *EpollReactor::lookupSocket(uint64_t key)
{
    SockInfo* info = m_pConnectionList->get(extract_fd(key), extract_gen(key));
    if (!info || info->type != IS_TCP_SOCKET)
        return nullptr;
    return static_cast<TCPSocket*>(info->socketBasePtr);
}

void EpollReactor::updateCashedTime()
{
    clock_gettime(CLOCK_MONOTONIC_COARSE, &m_cached_now);
}

void EpollReactor::defer(uint64_t key, int kind)
{
    m_deferred.push_back({key, kind});
}

EpollReactor::TimerId EpollReactor::addTimer(int interval_ms, std::function<void ()> cb, bool singleShot)
{
    if (!m_pTimers)
        return -1;

    return m_pTimers->addTimer(interval_ms, std::move(cb), singleShot);
}

void EpollReactor::removeTimer(TimerId id)
{
    if (m_pTimers && id >= 0)
        m_pTimers->removeTimer(id);
}


BufferPool *EpollReactor::bufferPool()
{
    return &m_bufferPool;
}

uint64_t EpollReactor::getCachedNow() const
{
    return m_cached_now.tv_sec;
}

uint32_t EpollReactor::connectionCount() const
{
    return m_pConnectionList ? m_pConnectionList->count() : 0;
}



void EpollReactor::onTCPEvent(int fd, uint32_t &ev, void *ptr){
    TCPSocket* pSockBase = static_cast<TCPSocket*>(ptr);
    if(!pSockBase){
        // nabayad pish biad: fd bedone object
        del_fd(fd, true);
        ::close(fd);
        return;
    }

    // tamame manteghe event ha (connect, TLS handshake, half-close, error) dar khode socket
    pSockBase->handleEvents(ev);
}

void EpollReactor::onUDPEvent(int /*fd*/, uint32_t &ev, void *ptr)
{
    UDPSocket* pSock = static_cast<UDPSocket*>(ptr);
    if (pSock)
        pSock->handleEvents(ev);
}

void EpollReactor::onTimerEvent(int fd, uint32_t &ev, void *ptr)
{
    //
    Timer* pTimer = static_cast<Timer*>(ptr);
    if(!pTimer){
        return;
    }

    if (ev & EPOLLIN) {
        pTimer->onTick();
    }
}

void EpollReactor::onTimerManagerEvent(int fd, uint32_t &ev, void *ptr)
{
    TimerManager* pTimerm = static_cast<TimerManager*>(ptr);
    if(!pTimerm){
        return;
    }

    if (ev & EPOLLIN) {
        pTimerm->onTick();
    }
}

void EpollReactor::onDNSEvent(int fd, uint32_t &ev, void *ptr)
{
    DNSLookup *pDNSLookup = static_cast<DNSLookup*>(ptr);
    if(!pDNSLookup){
        return;
    }

    if (ev & EPOLLERR) {
        pDNSLookup->reset_socket();
        return;
    }

    if (ev & EPOLLIN) {
        pDNSLookup->on_dns_read();
    }
}

void EpollReactor::checkDnsTimeouts()
{
    if (m_pDNSLookup)
        m_pDNSLookup->maintenance();
}

void EpollReactor::runGarbageCollector()
{
    // khode hazf dar entehaye dor halghe anjam mishe (collectGarbage)
    m_gcDue = true;
}

// object haye baste shode faghat inja (bad az tamam shodane hame callback haye in dor)
// delete mishan, pas hich callback-i ba pointer az beyn rafte bar nemigarde.
void EpollReactor::collectGarbage()
{
    if (m_GCList.empty())
        return;

    if (m_useGarbageCollector) {
        // halate ba takhir: object ha hadeaghal GC_MAX_AGE_SECS zende mimonan
        if (m_gcDue) {
            m_gcDue = false;
            m_GCList.flush(GC_MAX_AGE_SECS);
        }
    } else {
        m_GCList.flush_all();
    }
}

void EpollReactor::runPostedTasks()
{
    if (!m_hasTasks.exchange(false, std::memory_order_acq_rel))
        return;

    std::vector<std::function<void()>> tasks;
    {
        std::lock_guard<std::mutex> lock(m_taskMutex);
        tasks.swap(m_tasks);
    }

    for (auto& task : tasks) {
        if (task)
            task();
    }
}

void EpollReactor::runDeferred()
{
    if (m_deferred.empty())
        return;

    // kar haiy ke dar hine ejra ezafe mishan be dor badi montaghel mishan
    m_deferredRunning.swap(m_deferred);

    for (const DeferredItem& item : m_deferredRunning) {
        SockInfo* socketInfo = m_pConnectionList->get(extract_fd(item.key), extract_gen(item.key));
        if (!socketInfo || !socketInfo->socketBasePtr)
            continue;   // socket dar in fasele baste shode

        if (socketInfo->type == IS_TCP_SOCKET)
            static_cast<TCPSocket*>(socketInfo->socketBasePtr)->onDeferred(item.kind);
        else if (socketInfo->type == IS_UDP_SOCKET)
            static_cast<UDPSocket*>(socketInfo->socketBasePtr)->onDeferred(item.kind);
    }

    m_deferredRunning.clear();
}

void EpollReactor::run(std::atomic<bool> &stop)
{
    std::vector <epoll_event> evs(m_maxEvent);
    while(!stop.load(std::memory_order_relaxed))
    {
        // agar kare deferred monde bashe nabayad bekhabim
        int timeout = m_deferred.empty() ? 1000 : 0;
        int n = epoll_wait(m_epollSocket, evs.data(), (int)evs.size(), timeout);

        if(n < 0) {
            if(errno == EINTR)
                continue;

            NR_PERROR("epoll_wait");
            break;
        }

        updateCashedTime();

        for(int i = 0; i < n; ++i)
        {
            uint64_t key = evs[i].data.u64;
            uint32_t ev = evs[i].events;
            int fd = extract_fd(key);
            uint32_t socketgenID = extract_gen(key);

            // agar fd dar haman batch baste shode bashe genID match nemishe
            SockInfo* socketInfo = m_pConnectionList->get(fd, socketgenID);
            if (!socketInfo) {
                continue;
            }

            switch (socketInfo->type) {
            case IS_TCP_SOCKET:
                onTCPEvent(fd, ev, socketInfo->socketBasePtr);
                break;

            case IS_TCP_LISTENER:
                adoptAccepted(static_cast<ListenerInfo*>(socketInfo->socketBasePtr));
                break;

            case IS_TIMER_MANAGER_SOCKET:
                onTimerManagerEvent(fd, ev, socketInfo->socketBasePtr);
                break;

            case IS_DNS_LOOKUP_SOCKET:
                onDNSEvent(fd, ev, socketInfo->socketBasePtr);
                break;

            case IS_TIMER_SOCKET:
                onTimerEvent(fd, ev, socketInfo->socketBasePtr);
                break;

            case IS_UDP_SOCKET:
                onUDPEvent(fd, ev, socketInfo->socketBasePtr);
                break;

            case IS_WAKEUP_SOCKET: {
                uint64_t x;
                ssize_t r = read(m_wakeupFd, &x, sizeof x);
                (void)r;
                break;
            }

            default:
                break;
            }
        }

        runPostedTasks();
        runDeferred();
        collectGarbage();
    }

    shutdown_all();
}

void EpollReactor::wake() {
    if (m_wakeupFd == -1)
        return;

    uint64_t one = 1;
    ssize_t r = write(m_wakeupFd, &one, sizeof one);
    (void)r;
}

void EpollReactor::post(std::function<void ()> task)
{
    {
        std::lock_guard<std::mutex> lock(m_taskMutex);
        m_tasks.push_back(std::move(task));
    }
    m_hasTasks.store(true, std::memory_order_release);
    wake();
}


// timeout haye connect/handshake, closing/linger va idle.
// har bar faghat yek tike az jadval fd ha check mishe (kol jadval har MAINTENANCE_FULL_PASS_MS
// yek bar), pas ba sad hezar connection ham stall-e dore-i nadarim. slot haye jadval sabet
// hastan, pas bastan-e socket ha dar hine check moshkeli nadare.
void EpollReactor::maintenance()
{
    uint64_t now = getCachedNow();
    size_t total = m_pConnectionList->maximumSize();
    if (total == 0)
        return;

    size_t slice = (total * MAINTENANCE_SLICE_MS + MAINTENANCE_FULL_PASS_MS - 1) / MAINTENANCE_FULL_PASS_MS;
    if (slice < 64)
        slice = 64;

    for (size_t i = 0; i < slice && total > 0; i++) {
        if (m_maintCursor >= total)
            m_maintCursor = 0;

        SockInfo* info = m_pConnectionList->slot(m_maintCursor++);
        if (!info || info->type != IS_TCP_SOCKET || !info->socketBasePtr)
            continue;

        static_cast<TCPSocket*>(info->socketBasePtr)->checkTimeout(now);
    }
}

void EpollReactor::shutdown_all() {
    if (!m_pConnectionList)
        return;

    stop_listener();

    // بسته‌کردن همه‌ی اتصال‌های TCP و UDP
    std::vector<uint64_t> keys;
    m_pConnectionList->forEachActive([&](SockInfo* pSocketInfo) {
        if ((pSocketInfo->type == IS_TCP_SOCKET || pSocketInfo->type == IS_UDP_SOCKET) && pSocketInfo->socketBasePtr)
            keys.push_back(make_key(pSocketInfo->fd, pSocketInfo->genID));
    });

    for (uint64_t key : keys) {
        SockInfo* pInfo = m_pConnectionList->get(extract_fd(key), extract_gen(key));
        if (!pInfo || !pInfo->socketBasePtr)
            continue;

        if (pInfo->type == IS_TCP_SOCKET)
            static_cast<TCPSocket*>(pInfo->socketBasePtr)->close(true);
        else if (pInfo->type == IS_UDP_SOCKET)
            static_cast<UDPSocket*>(pInfo->socketBasePtr)->close();
    }

    m_deferred.clear();
    m_GCList.flush_all();
}

