#include "clsTCPSocket.h"
#include "clsLog.h"
#include "clsEpollReactor.h"
#include "clsSocketList.h"
#include "clsTLSContext.h"
#include "epoll.h"
#include "clsDNSLookup.h"

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <algorithm>
#include <climits>
#include <vector>
#include <sys/sendfile.h>

namespace {
// natije ioRead/ioWrite
constexpr ssize_t IO_AGAIN = -1;
constexpr ssize_t IO_ERROR = -2;

// kar haye deferred (EpollReactor::defer)
constexpr int DEFER_READ = 1;
constexpr int DEFER_CLOSE = 2;

constexpr int MAX_IOV_BATCH = 64;
}

TCPSocket::TCPSocket()
{

}

TCPSocket::~TCPSocket()
{
    // dar halate adi socket ghablan baste shode (fd == -1). in ghesmat baraye
    // object haiy hast ke malek mostaghim delete karde.
    if (m_dnsPending && m_pReactor) {
        m_pReactor->cancelDNS(this);
        m_dnsPending = false;
    }

    if (m_SocketContext.fd != -1) {
        if (m_pReactor && m_registered)
            m_pReactor->del_fd(m_SocketContext.fd, true);
        ::close(m_SocketContext.fd);
        m_SocketContext.fd = -1;
    }

    if (m_ssl) {
        SSL_free(m_ssl);
        m_ssl = nullptr;
    }

    delete m_connInfo;
}

TCPSocket::ConnectInfo& TCPSocket::connInfo()
{
    if (!m_connInfo)
        m_connInfo = new ConnectInfo;
    return *m_connInfo;
}

TCPSocket::socketStatus TCPSocket::getStatus() const
{
    return status;
}

void TCPSocket::setStatus(socketStatus newStatus)
{
    //faghat zamani status mitone avaz beshe ke close nabashe
    if (status != Closed)
        status = newStatus;
}

void TCPSocket::updateLastActive()
{
    if(m_pReactor){
        m_SocketContext.lastActive = m_pReactor->getCachedNow();
    }
}

EpollReactor *TCPSocket::getReactor() const
{
    return m_pReactor;
}

void TCPSocket::setReactor(EpollReactor *r) {
    m_pReactor = r;
    if (m_pReactor)
        m_SocketContext.writeQueue.init(m_pReactor->bufferPool());
}

void TCPSocket::setSocketOption(int fd, int name, bool isEnable)
{
    int ka = isEnable;
    setsockopt(fd, SOL_SOCKET, name, &ka, sizeof(ka));
}

void TCPSocket::setSocketShared(int fd, bool isEnable)
{
    //Allow local port reuse
    setSocketOption(fd, SO_REUSEPORT, isEnable);
}

int TCPSocket::setSocketNonblocking(int fd) {
    int f = fcntl(fd, F_GETFL, 0);
    if(f == -1)
        return -1;

    return fcntl(fd, F_SETFL, f | O_NONBLOCK);
}

void TCPSocket::setSocketResourceAddress(int fd, bool isEnable)
{
    setSocketOption(fd, SO_REUSEADDR, isEnable);
}

void TCPSocket::setSocketNoDelay(int fd, bool isEnable)
{
    // TCP_NODELAY dar sathe IPPROTO_TCP hast (na SOL_SOCKET)
    int v = isEnable ? 1 : 0;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v));
}

void TCPSocket::setSocketCork(int fd, bool isEnable)
{
    //TCP_CORK
    int v = isEnable ? 1 : 0;
    setsockopt(fd, IPPROTO_TCP, TCP_CORK, &v, sizeof(v));
}

void TCPSocket::setSocketKeepAlive(int fd, bool isEnable) {
    if (fd < 0) return;

    int optval = isEnable ? 1 : 0;
    int result;

    // enable/disable Keep-Alive
    result = setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval));
    if (result < 0) {
        NR_PERROR("setsockopt(SO_KEEPALIVE) failed");
        return;
    }

    if (isEnable) {
        result = setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &KEEPIDLE_SECS, sizeof(KEEPIDLE_SECS));
        if (result < 0) {
            NR_PERROR("setsockopt(TCP_KEEPIDLE) failed");
        }

        result = setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &KEEPINTVL_SECS, sizeof(KEEPINTVL_SECS));
        if (result < 0) {
            NR_PERROR("setsockopt(TCP_KEEPINTVL) failed");
        }

        result = setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &KEEPCNT_MAX, sizeof(KEEPCNT_MAX));
        if (result < 0) {
            NR_PERROR("setsockopt(TCP_KEEPCNT) failed");
        }
    }
}

//for Gaming and VOIP
void TCPSocket::setSocketLowDelay(int fd, bool isEnable)
{
    int tos = isEnable ? IPTOS_LOWDELAY : 0;
    setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
}


SocketContext *TCPSocket::getSocketContext()
{
    return &m_SocketContext;
}

// ----------------------------------------------------------------------------
// callback dispatch
// ----------------------------------------------------------------------------
void TCPSocket::handleOnData(const uint8_t *d, size_t n) {
    if(m_onData){
        m_onData(m_callbacksArg, d, n);
    }else{
        onReceiveData(d, n);
    }
}

void TCPSocket::handleOnAccepted()
{
    if(m_onAccepted){
        m_onAccepted(m_callbacksArg);
    }else{
        onAccepted();
    }
}

void TCPSocket::handleOnClose()
{
    if(m_onClose){
        m_onClose(m_callbacksArg);
    }else{
        onClose();
    }
}

void TCPSocket::handleOnConnectFailed()
{
    if(m_onConnectFailed){
        m_onConnectFailed(m_callbacksArg);
    }else{
        onConnectFailed();
    }
}

void TCPSocket::handleOnConnecting()
{
    if(m_onConnecting){

        m_onConnecting(m_callbacksArg);

    }else{
        onConnecting();
    }
}

void TCPSocket::handleOnConnected()
{
    if(m_onConnected){
        m_onConnected(m_callbacksArg);
    }else{
        onConnected();
    }
}

void TCPSocket::handleOnPause() {
    if (m_onPause)
        m_onPause(m_callbacksArg);
    else
        onPause();
}

void TCPSocket::handleOnResume() {
    if (m_onResume)
        m_onResume(m_callbacksArg);
    else
        onResume();
}

void TCPSocket::handleOnEOF()
{
    if (m_onEOF)
        m_onEOF(m_callbacksArg);
    else
        onReadEOF();
}

// raftare default: socketi ke safe ersalesh por shode, khandan az khodesh ro
// motevaghef mikone (monaseb baraye protocol haye request/response).
// proxy ha bayad ba setOnPause/setOnResume khandan az tarafe moghabel ro control konan.
void TCPSocket::onPause()
{
    pause_reading();
}

void TCPSocket::onResume()
{
    resume_reading();
}

// ----------------------------------------------------------------------------
// read control
// ----------------------------------------------------------------------------
void TCPSocket::pause_reading() {
    if (m_readPaused)
        return;

    m_readPaused = true;
    if (m_pReactor && m_registered && status == Connected && !m_readEOF)
        m_pReactor->removeFlags(&m_SocketContext, EPOLLIN);
}

void TCPSocket::resume_reading() {
    if (!m_readPaused)
        return;

    m_readPaused = false;
    if (!m_pReactor || !m_registered || status != Connected || m_readEOF)
        return;

    // MOD dar halate edge-triggered event ro dobare arm mikone; agar data dar
    // buffer kernel monde bashe bela faseleh EPOLLIN miad.
    m_pReactor->setFlags(&m_SocketContext, m_SocketContext.ev.events | EPOLLIN | EPOLLRDHUP);

    // data haye decrypt shode dakhele SSL az dide epoll penhan hastan
    if (m_ssl && !m_deferredRead) {
        m_deferredRead = true;
        m_pReactor->defer(m_SocketContext.ev.data.u64, DEFER_READ);
    }
}

bool TCPSocket::isReadPaused() const
{
    return m_readPaused;
}

void TCPSocket::setAllowHalfClose(bool enable)
{
    m_allowHalfClose = enable;
}

void TCPSocket::setAutoDelete(bool enable)
{
    m_autoDelete = enable;
}

void TCPSocket::setIdleTimeout(uint32_t seconds)
{
    m_idleTimeout = seconds;
}

int TCPSocket::socketError()
{
    int err = 0;
    if(m_SocketContext.fd != -1){
        socklen_t len = sizeof(err);
        if (getsockopt(m_SocketContext.fd, SOL_SOCKET, SO_ERROR, &err, &len) == -1)
            err = errno;
    }
    return err;
}

int TCPSocket::getErrorCode()
{
    if (m_lastError != 0)
        return m_lastError;

    return socketError();
}

// ----------------------------------------------------------------------------
// close
// ----------------------------------------------------------------------------
void TCPSocket::close(bool force) {
    if (status == Closed)
        return;

    if (m_closeReason == NotClosed)
        m_closeReason = ClosedByApp;

    if (!m_pReactor) {
        // hargez be reactor vasl nashode; chizi baraye bastan nist
        status = Closed;
        return;
    }

    if (force || m_abortPending) {
        finalizeClose();
        return;
    }

    if (status == Closing)
        return; // ghablan dar hale baste shodan

    if (status != Connected) {
        // Ready / Connecting / Handshaking: chizi baraye flush nist
        finalizeClose();
        return;
    }

    // graceful: aval safe ersal khali mishe
    m_pendingClose = true;
    setStatus(Closing);
    updateLastActive();

    if (m_SocketContext.writeQueue.empty()) {
        drainComplete();
    } else {
        // dige data be app tahvil nemishe; faghat montazere EPOLLOUT
        m_pReactor->setFlags(&m_SocketContext, (m_SocketContext.ev.events & ~(uint32_t)EPOLLIN) | EPOLLOUT);
    }
}

// safe ersal khali shode: FIN ferestade mishe. agar tarafe moghabel hanooz FIN
// nadade, montazer mimonim (lingering close). bastan fd ba data khande nashode
// bejaye FIN yek RST miferese va data haiy ke hanooz dar buffer kernel hastan az beyn miran.
void TCPSocket::drainComplete()
{
    if (m_ssl && !m_writeShutdown)
        SSL_shutdown(m_ssl);    // close_notify (best effort)

    ::shutdown(m_SocketContext.fd, SHUT_WR);
    m_writeShutdown = true;

    if (m_readEOF) {
        finalizeClose();
        return;
    }

    m_lingering = true;
    updateLastActive();
    m_pReactor->setFlags(&m_SocketContext, EPOLLIN | EPOLLRDHUP | EPOLLET);
    lingerDiscard();
}

void TCPSocket::lingerDiscard()
{
    // buffer-e jodagane: close() momkene az dakhele callback-e yek socket-e dige seda zade beshe
    // ke hamin alan dar hale parse kardan-e buffer-e moshtarak-e reactor hast.
    char buf[4096];
    size_t cap = sizeof(buf);

    for (int i = 0; i < 64; i++) {
        ssize_t n = ::recv(m_SocketContext.fd, buf, cap, 0);
        if (n > 0)
            continue;

        if (n == 0) {
            finalizeClose();
            return;
        }

        if (errno == EINTR)
            continue;

        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return; // montazere FIN (ya LINGER_TIMEOUT_SECS)

        finalizeClose();
        return;
    }

    // tarafe moghabel hamchenan data miferese; bishtar az in montazer nemimonim
    finalizeClose();
}

void TCPSocket::shutdownWrite()
{
    if (status != Connected || m_shutdownWrPending)
        return;

    m_shutdownWrPending = true;
    if (m_SocketContext.writeQueue.empty())
        doShutdownWrite();
    // vagarna bad az khali shodan saf dar onWritable anjam mishe
}

void TCPSocket::doShutdownWrite()
{
    if (m_writeShutdown)
        return;

    if (m_ssl)
        SSL_shutdown(m_ssl);

    ::shutdown(m_SocketContext.fd, SHUT_WR);
    m_writeShutdown = true;

    // har do jahat tamam shode
    if (m_readEOF)
        finalizeClose();
}

// tanha masir baste shodan vaghei. daghighan yek bar ejra mishe.
void TCPSocket::finalizeClose()
{
    if (status == Closed)
        return;

    if (m_closeReason == NotClosed)
        m_closeReason = (m_lastError != 0) ? ClosedByError : ClosedByApp;
    status = Closed;

    if (m_dnsPending) {
        m_pReactor->cancelDNS(this);
        m_dnsPending = false;
    }

    if (m_SocketContext.fd != -1) {
        if (m_registered) {
            // faghat az ConnectionList; ::close khodesh fd ro az epoll hazf mikone (yek syscall kamtar)
            m_pReactor->unregister_fd(m_SocketContext.fd);
            m_registered = false;
        }

        ::close(m_SocketContext.fd);

        // fd bayad batel beshe: kernel hamin shomare ro be connection badi mide
        m_SocketContext.fd = -1;
    }

    if (m_ssl) {
        SSL_free(m_ssl);
        m_ssl = nullptr;
    }

    m_SocketContext.writeQueue.clear();

    if (m_connInfo) {
        delete m_connInfo;
        m_connInfo = nullptr;
    }

    handleOnClose();

    if (m_autoDelete)
        m_pReactor->deleteLater(this);
}

// khata dar masiri ke az dakhele code app seda zade shode (send). baraye inke
// onClose vasate tabe app ejra nashe, close be entehaye dor halghe montaghel mishe.
void TCPSocket::abortLater()
{
    if (m_abortPending || status == Closed)
        return;

    if (m_closeReason == NotClosed)
        m_closeReason = ClosedByError;

    m_abortPending = true;
    if (m_registered)
        m_pReactor->defer(m_SocketContext.ev.data.u64, DEFER_CLOSE);
}

void TCPSocket::onDeferred(int kind)
{
    if (status == Closed)
        return;

    if (kind == DEFER_CLOSE) {
        finalizeClose();
        return;
    }

    if (kind == DEFER_READ) {
        m_deferredRead = false;
        if (m_abortPending) {
            finalizeClose();
            return;
        }
        if (status == Connected)
            onReadable();
    }
}

void TCPSocket::checkTimeout(uint64_t now)
{
    if (status == Closed)
        return;

    uint64_t idle = (now > m_SocketContext.lastActive) ? (now - m_SocketContext.lastActive) : 0;

    switch (status) {
    case Connecting:
    case Handshaking:
        if (idle >= (uint64_t)CONNECT_TIMEOUT_SECS) {
            m_closeReason = ClosedByTimeout;
            m_lastError = ETIMEDOUT;
            if (m_isAccepted)
                finalizeClose();
            else
                failConnect(ETIMEDOUT);
        }
        break;

    case Closing:
        if (idle >= (uint64_t)(m_lingering ? LINGER_TIMEOUT_SECS : CLOSING_TIMEOUT_SECS))
            finalizeClose();    // dalil-e asli (app/peer) negah dashte mishe
        break;

    case Connected:
        if (m_idleTimeout != 0 && idle >= m_idleTimeout) {
            m_closeReason = ClosedByTimeout;
            m_lastError = ETIMEDOUT;
            finalizeClose();
        }
        break;

    default:
        break;
    }
}

// ----------------------------------------------------------------------------
// connect
// ----------------------------------------------------------------------------
void TCPSocket::failConnect(int err)
{
    if (status == Closed)
        return;

    // IP haye dige-ye hamin esm (agar DNS chand javab dade bashe)
    if (tryNextAddress(err))
        return;

    m_lastError = err;
    m_closeReason = (err == ETIMEDOUT) ? ClosedByTimeout : ConnectFailed;
    if (!m_connectFailNotified) {
        m_connectFailNotified = true;
        handleOnConnectFailed();
    }

    finalizeClose();
}

static bool parseLiteralAddress(const char* host, uint16_t port, sockaddr_storage& ss, socklen_t& len)
{
    memset(&ss, 0, sizeof(ss));

    sockaddr_in* a4 = reinterpret_cast<sockaddr_in*>(&ss);
    if (inet_pton(AF_INET, host, &a4->sin_addr) == 1) {
        a4->sin_family = AF_INET;
        a4->sin_port = htons(port);
        len = sizeof(sockaddr_in);
        return true;
    }

    sockaddr_in6* a6 = reinterpret_cast<sockaddr_in6*>(&ss);
    if (inet_pton(AF_INET6, host, &a6->sin6_addr) == 1) {
        a6->sin6_family = AF_INET6;
        a6->sin6_port = htons(port);
        len = sizeof(sockaddr_in6);
        return true;
    }

    return false;
}

bool TCPSocket::connectTo(const char* host, uint16_t port)
{
    if(!m_pReactor){
        NR_LOG(netrector::LOG_INFO, "error: Reactor not found!");
        return false;
    }

    if(!host || !*host || getStatus() != Ready){
        return false;
    }

    m_SocketContext.port = port;
    connInfo().host = host;
    m_isAccepted = false;

    // agar host khodesh IP bod (v4 ya v6) niazi be DNS nist
    sockaddr_storage ss;
    socklen_t len = 0;
    if (parseLiteralAddress(host, port, ss, len)) {
        setStatus(Connecting);
        updateLastActive();
        startConnect(reinterpret_cast<sockaddr*>(&ss), len);
        return true;
    }

    setStatus(Connecting);
    updateLastActive();
    m_dnsPending = true;
    m_dnsTriedV6 = false;
    if (!m_pReactor->getIPbyName(host, connect_cb, this, DNSLookup::A)) {
        m_dnsPending = false;
        status = Ready;
        return false;
    }

    return true;
}

void TCPSocket::connect_cb(const char * /*hostname*/, char **ips, size_t count, DNSLookup::QUERY_TYPE qtype, void *p)
{
    TCPSocket *pSocketBase = static_cast<TCPSocket*>(p);
    if(!pSocketBase){
        return;
    }

    pSocketBase->onResolved(ips, count, qtype);
}

void TCPSocket::onResolved(char **ips, size_t count, DNSLookup::QUERY_TYPE qtype)
{
    m_dnsPending = false;
    if (status != Connecting)
        return;

    if (!ips || count == 0) {
        // record A nadasht: yek bar AAAA emtehan mishe
        if (qtype == DNSLookup::A && !m_dnsTriedV6) {
            m_dnsTriedV6 = true;
            m_dnsPending = true;
            if (m_connInfo && m_pReactor->getIPbyName(m_connInfo->host.c_str(), connect_cb, this, DNSLookup::AAAA))
                return;
            m_dnsPending = false;
        }

        failConnect(EHOSTUNREACH);
        return;
    }

    // hame-ye IP ha negah dashte mishan; agar avali connect nashod badi emtehan mishe
    ConnectInfo& info = connInfo();
    info.addrs.clear();
    for (size_t i = 0; i < count && i < 8; i++)
        info.addrs.push_back(ips[i]);
    info.nextAddr = 0;

    sockaddr_storage ss;
    socklen_t len = 0;
    if (!parseLiteralAddress(info.addrs[0].c_str(), m_SocketContext.port, ss, len)) {
        failConnect(EHOSTUNREACH);
        return;
    }
    info.nextAddr = 1;

    startConnect(reinterpret_cast<sockaddr*>(&ss), len);
}

// bad az shekast-e connect be yek IP: emtehan-e IP badi az haman esm.
// true: talash-e jadid shoro shod (socket hanooz Connecting hast)
bool TCPSocket::tryNextAddress(int err)
{
    if (status != Connecting || !m_connInfo || m_connInfo->nextAddr >= m_connInfo->addrs.size())
        return false;

    // faghat khata haye marbot be host/shabake (na timeout-e koli)
    if (err != ECONNREFUSED && err != EHOSTUNREACH && err != ENETUNREACH && err != ETIMEDOUT && err != ECONNRESET)
        return false;

    sockaddr_storage ss;
    socklen_t len = 0;
    const std::string& ip = m_connInfo->addrs[m_connInfo->nextAddr++];
    if (!parseLiteralAddress(ip.c_str(), m_SocketContext.port, ss, len))
        return false;

    // socket-e ghabli (agar sabt shode) baste mishe
    if (m_SocketContext.fd != -1) {
        if (m_registered) {
            m_pReactor->unregister_fd(m_SocketContext.fd);
            m_registered = false;
        }
        ::close(m_SocketContext.fd);
        m_SocketContext.fd = -1;
    }

    updateLastActive();
    startConnect(reinterpret_cast<sockaddr*>(&ss), len);
    return status == Connecting;
}

void TCPSocket::startConnect(const sockaddr *addr, socklen_t addrLen)
{
    int fd = ::socket(addr->sa_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    if (fd == -1) {
        failConnect(errno);
        return;
    }

    TCPSocket::setSocketNoDelay(fd, true);

    int ret;
    do {
        ret = ::connect(fd, addr, addrLen);
    } while (ret == -1 && errno == EINTR);

    if (ret == -1 && errno != EINPROGRESS) {
        int err = errno;
        ::close(fd);
        failConnect(err);
        return;
    }

    // natije connect (hata agar fori movafagh shode bashe) ba EPOLLOUT miad
    m_SocketContext.fd = fd;
    m_SocketContext.ev.events = EPOLL_EVENTS_TCP_NONBLOCKING | EPOLLOUT;
    if (!m_pReactor->register_fd(fd, &m_SocketContext.ev, IS_TCP_SOCKET, this)) {
        failConnect(ENOMEM);
        return;
    }

    m_registered = true;
    updateLastActive();
    handleOnConnecting();
}

void TCPSocket::onConnectReady()
{
    int err = socketError();
    if (err != 0) {
        failConnect(err);
        return;
    }

    updateLastActive();

    if (m_tlsCtx) {
        if (!tlsStart(false)) {
            failConnect(EPROTO);
            return;
        }
        tlsHandshake();
        return;
    }

    // remove EPOLLOUT
    m_pReactor->removeFlags(&m_SocketContext, EPOLLOUT);

    //connected sucessfully
    setStatus(Connected);
    handleOnConnected();
}

void TCPSocket::_accepted(int fd)
{
    if(getStatus() != Ready || !m_pReactor){
        ::close(fd);
        // object-e factory bedone estefade mande: mesle yek close-e adi azad mishe
        if (m_pReactor && getStatus() == Ready)
            finalizeClose();
        return;
    }

    m_isAccepted = true;
    m_SocketContext.fd = fd;
    updateLastActive();

    m_SocketContext.ev.events = EPOLL_EVENTS_TCP_NONBLOCKING;
    if (!m_pReactor->register_fd(fd, &m_SocketContext.ev, IS_TCP_SOCKET, this)) {
        //be har dalili accept nashod close call beshe ke GC emal beshe
        finalizeClose();
        return;
    }
    m_registered = true;

    if (m_tlsCtx) {
        if (!tlsStart(true)) {
            finalizeClose();
            return;
        }
        tlsHandshake();     // onAccepted bad az handshake seda zade mishe
        return;
    }

    setStatus(TCPSocket::Connected);
    handleOnAccepted();  // callback
}

bool TCPSocket::adoptFd(int fd) {
    m_SocketContext.fd = fd;
    updateLastActive();
    return true;
}

// ----------------------------------------------------------------------------
// TLS
// ----------------------------------------------------------------------------
bool TCPSocket::enableTLS(TLSContext *ctx, const char *serverName)
{
    if (!ctx || status != Ready)
        return false;

    m_tlsCtx = ctx;
    if (serverName && *serverName)
        connInfo().serverName = serverName;
    return true;
}

bool TCPSocket::isTLS() const
{
    return m_tlsCtx != nullptr;
}

const char *TCPSocket::tlsVersion() const
{
    return m_ssl ? SSL_get_version(m_ssl) : "";
}

std::string TCPSocket::tlsALPN() const
{
    if (!m_ssl)
        return std::string();

    const uint8_t* data = nullptr;
    unsigned len = 0;
    SSL_get0_alpn_selected(m_ssl, &data, &len);
    return std::string(reinterpret_cast<const char*>(data), len);
}

bool TCPSocket::tlsStart(bool isServer)
{
    ERR_clear_error();
    m_ssl = SSL_new(m_tlsCtx->native());
    if (!m_ssl)
        return false;

    if (SSL_set_fd(m_ssl, m_SocketContext.fd) != 1) {
        SSL_free(m_ssl);
        m_ssl = nullptr;
        return false;
    }

    if (isServer && m_tlsCtx->asyncPrivateKey())
        SSL_set_ex_data(m_ssl, TLSContext::socketExIndex(), this);

    if (isServer) {
        SSL_set_accept_state(m_ssl);
    } else {
        SSL_set_connect_state(m_ssl);

        std::string name;
        if (m_connInfo)
            name = m_connInfo->serverName.empty() ? m_connInfo->host : m_connInfo->serverName;
        if (!name.empty()) {
            unsigned char tmp[sizeof(in6_addr)];
            bool isIP = inet_pton(AF_INET, name.c_str(), tmp) == 1 || inet_pton(AF_INET6, name.c_str(), tmp) == 1;

            // SNI faghat baraye esm (na IP)
            if (!isIP)
                SSL_set_tlsext_host_name(m_ssl, name.c_str());

            if (m_tlsCtx->verifyPeer()) {
                X509_VERIFY_PARAM* param = SSL_get0_param(m_ssl);
                int ok = isIP ? X509_VERIFY_PARAM_set1_ip_asc(param, name.c_str())
                              : X509_VERIFY_PARAM_set1_host(param, name.c_str(), name.size());
                if (ok != 1) {
                    SSL_free(m_ssl);
                    m_ssl = nullptr;
                    return false;
                }
            }
        }
    }

    setStatus(Handshaking);
    updateLastActive();
    return true;
}

void TCPSocket::tlsHandshake()
{
    ERR_clear_error();
    int ret = SSL_do_handshake(m_ssl);
    if (ret == 1) {
        m_pReactor->removeFlags(&m_SocketContext, EPOLLOUT);
        setStatus(Connected);
        updateLastActive();

        if (m_isAccepted)
            handleOnAccepted();
        else
            handleOnConnected();
        return;
    }

    int err = SSL_get_error(m_ssl, ret);
    if (err == SSL_ERROR_WANT_READ) {
        m_pReactor->removeFlags(&m_SocketContext, EPOLLOUT);
        return;
    }

    if (err == SSL_ERROR_WANT_WRITE) {
        m_pReactor->addFlags(&m_SocketContext, EPOLLOUT);
        return;
    }

    if (err == SSL_ERROR_WANT_PRIVATE_KEY_OPERATION) {
        // emza roye thread-e worker; natije ba tlsKeyJobDone miad
        m_pReactor->removeFlags(&m_SocketContext, EPOLLOUT);
        return;
    }

    // handshake failed (certificate, protocol, ya ghat shodan ertebat)
    m_lastError = EPROTO;
    ERR_clear_error();
    if (m_isAccepted)
        finalizeClose();
    else
        failConnect(EPROTO);
}

bool TCPSocket::tlsStartKeyJob(uint16_t sigalg, const uint8_t *in, size_t len)
{
    if (m_tlsJob || !m_tlsCtx || !m_registered)
        return false;

    auto job = std::make_shared<TLSKeyJob>();
    job->reactor = m_pReactor;
    job->live = m_pReactor->liveToken();
    job->socketKey = m_SocketContext.ev.data.u64;
    job->sigalg = sigalg;
    job->in.assign(in, in + len);
    m_tlsJob = job;
    return m_tlsCtx->submitKeyJob(std::move(job));
}

void TCPSocket::tlsKeyJobDone(TLSKeyJob *job)
{
    if (m_tlsJob.get() != job || status != Handshaking)
        return;

    tlsHandshake();     // SSL_do_handshake -> complete() natije ro migire
    if (status == Connected && !m_readPaused)
        onReadable();   // data-ye app momkene hamrah-e Finished reside bashe
}

// ----------------------------------------------------------------------------
// io
// ----------------------------------------------------------------------------
// >0: tedad byte | 0: EOF | IO_AGAIN | IO_ERROR
ssize_t TCPSocket::ioRead(char *buf, size_t cap)
{
    if (m_ssl) {
        ERR_clear_error();
        int n = SSL_read(m_ssl, buf, (int)std::min<size_t>(cap, INT_MAX));
        if (n > 0)
            return n;

        switch (SSL_get_error(m_ssl, n)) {
        case SSL_ERROR_WANT_READ:
            return IO_AGAIN;

        case SSL_ERROR_WANT_WRITE:
            m_tlsReadWantsWrite = true;
            m_pReactor->addFlags(&m_SocketContext, EPOLLOUT);
            return IO_AGAIN;

        case SSL_ERROR_ZERO_RETURN:
            return 0;   // close_notify

        case SSL_ERROR_SYSCALL:
            // bastan TCP bedone close_notify ham EOF hesab mishe
            if (ERR_peek_error() == 0 && (n == 0 || errno == 0))
                return 0;
            ERR_clear_error();
            return IO_ERROR;

        default:
            ERR_clear_error();
            return IO_ERROR;
        }
    }

    for (;;) {
        ssize_t n = ::recv(m_SocketContext.fd, buf, cap, 0);
        if (n >= 0)
            return n;

        if (errno == EINTR)
            continue;

        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return IO_AGAIN;

        m_lastError = errno;
        return IO_ERROR;
    }
}

// >0: tedad byte | IO_AGAIN | IO_ERROR
ssize_t TCPSocket::ioWrite(const void *data, size_t len)
{
    if (m_ssl) {
        ERR_clear_error();
        int n = SSL_write(m_ssl, data, (int)std::min<size_t>(len, INT_MAX));
        if (n > 0)
            return n;

        switch (SSL_get_error(m_ssl, n)) {
        case SSL_ERROR_WANT_WRITE:
            return IO_AGAIN;

        case SSL_ERROR_WANT_READ:
            m_tlsWriteWantsRead = true;
            return IO_AGAIN;

        default:
            ERR_clear_error();
            return IO_ERROR;
        }
    }

    for (;;) {
        ssize_t n = ::send(m_SocketContext.fd, data, len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n >= 0)
            return n > 0 ? n : IO_AGAIN;

        if (errno == EINTR)
            continue;

        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return IO_AGAIN;

        m_lastError = errno;
        return IO_ERROR;
    }
}

void TCPSocket::handleEvents(uint32_t ev)
{
    if (status == Closed)
        return;

    if (m_abortPending) {
        finalizeClose();
        return;
    }

    // tashkhise inke darim connect mishim ya na #mohem
    if (status == Connecting) {
        if (ev & (EPOLLERR | EPOLLHUP)) {
            int err = socketError();
            failConnect(err != 0 ? err : ECONNREFUSED);
            return;
        }

        if (!(ev & EPOLLOUT))
            return;

        onConnectReady();
        if (status != Connected)
            return;     // Handshaking: montazere event badi

        ev &= ~(uint32_t)EPOLLOUT;
        if (!(ev & (EPOLLIN | EPOLLRDHUP)))
            return;
    }

    if (status == Handshaking) {
        if (ev & EPOLLERR) {
            if (m_isAccepted)
                finalizeClose();
            else
                failConnect(socketError());
            return;
        }

        tlsHandshake();
        if (status != Connected)
            return;

        // momkene data barname hamrah ba akharin payam handshake reside bashe
        ev |= EPOLLIN;
    }

    if (status == Closing) {
        if (m_lingering) {
            if (ev & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR))
                lingerDiscard();
            return;
        }

        // dar hale khali kardane safe ersal
        if (ev & (EPOLLERR | EPOLLHUP)) {
            finalizeClose();
            return;
        }

        if (ev & EPOLLOUT)
            onWritable();
        return;
    }

    if (status != Connected)
        return;

    if (ev & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
        if (ev & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            m_eofPending = true;

        if (m_readPaused || m_readEOF) {
            // khandan motevaghef hast; faghat khataye ghatei residegi mishe.
            // FIN/data bad az resume_reading dobare gozaresh mishe (EPOLL_CTL_MOD).
            if ((ev & EPOLLERR) || ((ev & EPOLLHUP) && m_readEOF)) {
                finalizeClose();
                return;
            }
        } else {
            onReadable();
            if (status == Closed)
                return;
        }
    }

    if ((ev & EPOLLOUT) && (status == Connected || (status == Closing && !m_lingering)))
        onWritable();
}

void TCPSocket::onReadable()
{
    if (status != Connected)
        return;

    // SSL_write ghablan montazere khandan bode
    if (m_tlsWriteWantsRead) {
        m_tlsWriteWantsRead = false;
        if (!flushQueue())
            return;
        if (status != Connected)
            return;
    }

    if (m_readPaused || m_readEOF)
        return;

    // buffer moshtarak reactor: data bayad dar haman callback masraf beshe
    char* buf = m_pReactor->readBuffer();
    size_t cap = m_pReactor->readBufferSize();

    for (int round = 0; ; round++)
    {
        if (round >= MAX_READS_PER_EVENT) {
            // nobat be baghie socket ha; edame dar entehaye hamin dor
            if (!m_deferredRead) {
                m_deferredRead = true;
                m_pReactor->defer(m_SocketContext.ev.data.u64, DEFER_READ);
            }
            break;
        }

        ssize_t bytesRec = ioRead(buf, cap);

        if(bytesRec > 0)  {
            updateLastActive();
            handleOnData(reinterpret_cast<uint8_t*>(buf), (size_t)bytesRec);  // hot-path via fn pointer

            // callback momkene socket ro baste ya pause karde bashe
            if (status != Connected || m_readPaused || m_abortPending)
                break;

            // plain TCP: recv-e kotah yani buffer kernel khali shode; data-ye badi
            // edge jadid misaze (EPOLLET), pas recv-e ezafe ke EAGAIN bede lazem nist.
            // estesna: agar hamin event RDHUP/HUP dashte (FIN reside) bayad ta 0 bekhonim,
            // chon baraye EOF edge-e dige-i nemiad.
            if (!m_ssl && !m_eofPending && (size_t)bytesRec < cap)
                break;

            continue;
        }

        if(bytesRec == IO_AGAIN)
            break;

        if(bytesRec == 0) {
            handleReadEOF();
            break;
        }

        // IO_ERROR
        finalizeClose();
        break;
    }
}

void TCPSocket::handleReadEOF()
{
    m_readEOF = true;
    if (m_closeReason == NotClosed)
        m_closeReason = ClosedByPeer;

    if (!m_allowHalfClose) {
        // safe ersal khali mishe va bad socket baste mishe
        close(false);
        return;
    }

    // half-close: socket baraye neveshtan baz mimone
    handleOnEOF();

    if (status == Connected && m_writeShutdown)
        finalizeClose();    // har do jahat tamam shode
}

bool TCPSocket::send(const void* data, size_t len) {
    if (!data || len == 0)
        return true;

    if (status != Connected || m_abortPending || m_shutdownWrPending || m_SocketContext.fd == -1)
        return false;

    SendQueue* queue = &m_SocketContext.writeQueue;
    if (!queueHasRoom(len))
        return false;
    const char* p = static_cast<const char*>(data);

    // agar saf khalie mostaghim ersal mishe
    if (queue->empty()) {
        while (len > 0) {
            ssize_t n = ioWrite(p, len);
            if (n > 0) {
                p += n;
                len -= (size_t)n;
                continue;
            }

            if (n == IO_AGAIN)
                break;  // kernel buffer por shod

            abortLater();
            return false;
        }

        updateLastActive();
        if (len == 0)
            return true; // hame ersal shod
    }

    if (!queue->push(p, len)) {
        // na pool na heap: hafeze tamam shode
        m_lastError = ENOMEM;
        abortLater();
        return false;
    }

    // harvaght ke data too queue hast, EPOLLOUT ro fa'al mikonim.
    m_pReactor->addFlags(&m_SocketContext, EPOLLOUT);

    if (!m_writeBlocked && queue->size() > BACK_PRESSURE) {
        m_writeBlocked = true;
        handleOnPause();
    }

    return true;
}

bool TCPSocket::sendv(const iovec *iov, int iovcnt)
{
    if (!iov || iovcnt <= 0)
        return true;

    if (iovcnt == 1)
        return send(iov[0].iov_base, iov[0].iov_len);

    size_t total = 0;
    for (int i = 0; i < iovcnt; i++)
        total += iov[i].iov_len;

    if (total == 0)
        return true;

    if (status != Connected || m_abortPending || m_shutdownWrPending || m_SocketContext.fd == -1)
        return false;

    SendQueue* queue = &m_SocketContext.writeQueue;
    if (!queueHasRoom(total))
        return false;

    // saf gheyre khali: tike ha mostaghim be saf (yek copy)
    if (!m_ssl && !queue->empty()) {
        if (!queue->pushv(iov, iovcnt)) {
            abortLater();
            return false;
        }
        if (!m_writeBlocked && queue->size() > BACK_PRESSURE) {
            m_writeBlocked = true;
            handleOnPause();
        }
        return true;
    }

    // TLS: tike ha poshte ham dar yek buffer gharar migiran ta yek record beshan
    if (m_ssl) {
        if (total <= m_pReactor->scratchBufferSize()) {
            char* scratch = m_pReactor->scratchBuffer();
            size_t off = 0;
            for (int i = 0; i < iovcnt; i++) {
                memcpy(scratch + off, iov[i].iov_base, iov[i].iov_len);
                off += iov[i].iov_len;
            }
            return send(scratch, total);
        }

        for (int i = 0; i < iovcnt; i++) {
            if (!send(iov[i].iov_base, iov[i].iov_len))
                return false;
        }
        return true;
    }

    // plain TCP ba safe khali: writev mostaghim (hadeaksar IOV_MAX tike dar yek syscall)
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = const_cast<iovec*>(iov);
    msg.msg_iovlen = (size_t)std::min<int>(iovcnt, IOV_MAX);

    ssize_t n;
    do {
        n = ::sendmsg(m_SocketContext.fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
    } while (n == -1 && errno == EINTR);

    if (n == -1) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            m_lastError = errno;
            abortLater();
            return false;
        }
        n = 0;
    }

    updateLastActive();
    if ((size_t)n == total)
        return true;

    // baghimande be saf ezafe mishe (yek copy, transactional, har tedad iovec)
    struct iovec restStack[64];
    std::vector<struct iovec> restHeap;
    struct iovec* rest = restStack;
    if (iovcnt > 64) {
        restHeap.resize((size_t)iovcnt);
        rest = restHeap.data();
    }
    int restCnt = 0;
    size_t skip = (size_t)n;
    for (int i = 0; i < iovcnt; i++) {
        if (skip >= iov[i].iov_len) {
            skip -= iov[i].iov_len;
            continue;
        }
        rest[restCnt].iov_base = static_cast<char*>(iov[i].iov_base) + skip;
        rest[restCnt].iov_len = iov[i].iov_len - skip;
        restCnt++;
        skip = 0;
    }

    if (!queue->pushv(rest, restCnt)) {
        m_lastError = ENOMEM;
        abortLater();
        return false;
    }

    m_pReactor->addFlags(&m_SocketContext, EPOLLOUT);

    if (!m_writeBlocked && queue->size() > BACK_PRESSURE) {
        m_writeBlocked = true;
        handleOnPause();
    }

    return true;
}

// saghf-e sakht-e safe ersal (setMaxSendQueue). bishtar az an send() ba ENOBUFS rad mishe;
// connection baz mimone (app bayad onPause ro rayat kone).
bool TCPSocket::queueHasRoom(size_t len)
{
    size_t limit = m_maxSendQueue ? m_maxSendQueue : MAX_SEND_QUEUE;
    if (m_SocketContext.writeQueue.size() + len > limit) {
        m_lastError = ENOBUFS;
        return false;
    }
    return true;
}

// ersal safe ta jaiy ke kernel ghabul kone. false: socket baste shod
bool TCPSocket::flushQueue()
{
    SendQueue* queue = &m_SocketContext.writeQueue;

    while (!queue->empty()) {
        ssize_t n;
        SendQueue::Chunk* head = queue->front();

        if (m_ssl) {
            // retry-e SSL_write bayad ba haman data bashe: hamishe az ebtedaye saf
            if (head->kind == SendQueue::File) {
                // file -> scratch -> record (content-e file sabet farz mishe)
                char* scratch = m_pReactor->scratchBuffer();
                size_t want = std::min<size_t>(head->avail(), 16 * 1024);
                ssize_t r = ::pread(head->file.fd, scratch, want, head->file.offset + (off_t)head->off);
                if (r <= 0) {
                    m_lastError = (r == 0) ? EIO : errno;
                    finalizeClose();
                    return false;
                }
                n = ioWrite(scratch, (size_t)r);
            } else {
                n = ioWrite(head->unsent(), head->avail());
            }
        } else if (head->kind == SendQueue::File) {
            off_t off = head->file.offset + (off_t)head->off;
            do {
                n = ::sendfile(m_SocketContext.fd, head->file.fd, &off, head->avail());
            } while (n == -1 && errno == EINTR);

            if (n == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    n = IO_AGAIN;
                } else {
                    m_lastError = errno;
                    n = IO_ERROR;
                }
            } else if (n == 0) {
                // file kotah tar az tool-e elam shode
                m_lastError = EIO;
                n = IO_ERROR;
            }
        } else {
            // chand chunk-e hafeze dar yek sendmsg
            struct iovec iov[MAX_IOV_BATCH];
            int cnt = 0;
            for (SendQueue::Chunk* c = head; c && cnt < MAX_IOV_BATCH && c->kind != SendQueue::File; c = c->next) {
                if (c->avail() == 0)
                    continue;
                iov[cnt].iov_base = const_cast<char*>(c->unsent());
                iov[cnt].iov_len = c->avail();
                cnt++;
            }

            if (cnt == 0) {
                queue->pop_front();     // chunk-e khali
                continue;
            }

            struct msghdr msg;
            memset(&msg, 0, sizeof(msg));
            msg.msg_iov = iov;
            msg.msg_iovlen = (size_t)cnt;

            do {
                n = ::sendmsg(m_SocketContext.fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
            } while (n == -1 && errno == EINTR);

            if (n == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    n = IO_AGAIN;
                } else {
                    m_lastError = errno;
                    n = IO_ERROR;
                }
            } else if (n == 0) {
                n = IO_AGAIN;
            }
        }

        if (n > 0) {
            queue->consume((size_t)n);
            updateLastActive();
            continue;
        }

        if (n == IO_AGAIN)
            break;

        finalizeClose();
        return false;
    }

    return true;
}

// ersal-e hafeze-ye app bedone copy: data bayad ta seda zade shodane release sabet bemone
// (release == nullptr: data-ye sabet, masalan static). bad az close ham release seda zade mishe.
bool TCPSocket::sendNoCopy(const void *data, size_t len, SendQueue::ReleaseFn release, void *arg)
{
    if (!data || len == 0) {
        if (release) release(const_cast<void*>(data), arg);
        return true;
    }

    if (status != Connected || m_abortPending || m_shutdownWrPending || m_SocketContext.fd == -1 || !queueHasRoom(len)) {
        if (release) release(const_cast<void*>(data), arg);
        return false;
    }

    SendQueue* queue = &m_SocketContext.writeQueue;
    size_t sent = 0;

    if (queue->empty()) {
        while (sent < len) {
            ssize_t n = ioWrite(static_cast<const char*>(data) + sent, len - sent);
            if (n > 0) {
                sent += (size_t)n;
                continue;
            }
            if (n == IO_AGAIN)
                break;

            if (release) release(const_cast<void*>(data), arg);
            abortLater();
            return false;
        }

        updateLastActive();
        if (sent == len) {
            if (release) release(const_cast<void*>(data), arg);
            return true;
        }
    }

    if (!queue->pushExternal(data, len, sent, release, arg)) {
        if (release) release(const_cast<void*>(data), arg);
        m_lastError = ENOMEM;
        abortLater();
        return false;
    }

    m_pReactor->addFlags(&m_SocketContext, EPOLLOUT);

    if (!m_writeBlocked && queue->size() > BACK_PRESSURE) {
        m_writeBlocked = true;
        handleOnPause();
    }

    return true;
}

// ersal-e baze-i az yek file ba sendfile (zero-copy az page cache). fd bayad ta payan-e
// ersal baz bemone; malekiyat-e fd ba app hast.
bool TCPSocket::sendFile(int fileFd, off_t offset, size_t len)
{
    if (fileFd < 0 || len == 0)
        return len == 0;

    if (status != Connected || m_abortPending || m_shutdownWrPending || m_SocketContext.fd == -1)
        return false;

    SendQueue* queue = &m_SocketContext.writeQueue;
    if (!queueHasRoom(len))
        return false;
    size_t sent = 0;

    if (queue->empty() && !m_ssl) {
        while (sent < len) {
            off_t off = offset + (off_t)sent;
            ssize_t n;
            do {
                n = ::sendfile(m_SocketContext.fd, fileFd, &off, len - sent);
            } while (n == -1 && errno == EINTR);

            if (n > 0) {
                sent += (size_t)n;
                continue;
            }

            if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;

            m_lastError = (n == 0) ? EIO : errno;
            abortLater();
            return false;
        }

        updateLastActive();
        if (sent == len)
            return true;
    }

    if (!queue->pushFile(fileFd, offset, len, sent)) {
        m_lastError = ENOMEM;
        abortLater();
        return false;
    }

    m_pReactor->addFlags(&m_SocketContext, EPOLLOUT);

    if (!m_writeBlocked && queue->size() > BACK_PRESSURE) {
        m_writeBlocked = true;
        handleOnPause();
    }

    return true;
}

void TCPSocket::onWritable() {

    if (status == Connecting) {
        onConnectReady();
        return;
    }

    if (status == Handshaking) {
        tlsHandshake();
        if (status != Connected)
            return;
    }

    if (status != Connected && !(status == Closing && !m_lingering))
        return;

    // SSL_read ghablan montazere neveshtan bode
    if (m_tlsReadWantsWrite) {
        m_tlsReadWantsWrite = false;
        if (status == Connected) {
            onReadable();
            if (status == Closed)
                return;
        }
    }

    if (!flushQueue())
        return;

    SendQueue* queue = &m_SocketContext.writeQueue;

    if (queue->empty()) {
        if (m_pendingClose) {
            drainComplete();
            return;
        }

        if (m_shutdownWrPending && !m_writeShutdown) {
            doShutdownWrite();
            if (status == Closed)
                return;
        }

        // EPOLLOUT armed mimone: dar halate edge-triggered faghat vaghti buffer-e kernel az
        // por be khali bere event mide, pas hazine-i nadare va do epoll_ctl dar har dor-e
        // por/khali shodan-e saf hazf mishe.
    }

    //resume
    if (m_writeBlocked && queue->size() <= LOW_WATERMARK) {
        m_writeBlocked = false;
        if (status == Connected)
            handleOnResume();
    }
}

// ----------------------------------------------------------------------------
// accessors
// ----------------------------------------------------------------------------
int TCPSocket::fd() const
{
    return m_SocketContext.fd;
}

TCPSocket *TCPSocket::getPointer()
{
    return this;
}

uint64_t TCPSocket::getLastActiveTime()
{
    return m_SocketContext.lastActive;
}

uint16_t TCPSocket::getLocalPort()
{
    if (m_SocketContext.fd == -1)
        return 0;

    struct sockaddr_storage local;
    socklen_t len = sizeof(local);
    if (getsockname(m_SocketContext.fd, (struct sockaddr*)&local, &len) == -1)
        return 0;

    if (local.ss_family == AF_INET6)
        return ntohs(reinterpret_cast<sockaddr_in6*>(&local)->sin6_port);

    return ntohs(reinterpret_cast<sockaddr_in*>(&local)->sin_port);
}

bool TCPSocket::getPeerAddress(std::string &ip, uint16_t &port) const
{
    if (m_SocketContext.fd == -1)
        return false;

    struct sockaddr_storage peer;
    socklen_t len = sizeof(peer);
    if (getpeername(m_SocketContext.fd, (struct sockaddr*)&peer, &len) == -1)
        return false;

    char buf[INET6_ADDRSTRLEN] = {0};
    if (peer.ss_family == AF_INET6) {
        const sockaddr_in6* a6 = reinterpret_cast<const sockaddr_in6*>(&peer);
        if (IN6_IS_ADDR_V4MAPPED(&a6->sin6_addr)) {
            // listener dual-stack: IPv4 be shekle ::ffff:a.b.c.d miad
            inet_ntop(AF_INET, &a6->sin6_addr.s6_addr[12], buf, sizeof(buf));
        } else {
            inet_ntop(AF_INET6, &a6->sin6_addr, buf, sizeof(buf));
        }
        port = ntohs(a6->sin6_port);
    } else {
        const sockaddr_in* a4 = reinterpret_cast<const sockaddr_in*>(&peer);
        inet_ntop(AF_INET, &a4->sin_addr, buf, sizeof(buf));
        port = ntohs(a4->sin_port);
    }

    ip = buf;
    return true;
}

size_t TCPSocket::pendingBytes() const
{
    return m_SocketContext.writeQueue.size();
}

bool TCPSocket::isWriteBlocked() const
{
    return m_writeBlocked;
}

bool TCPSocket::isAccepted() const
{
    return m_isAccepted;
}

void TCPSocket::setOnData(OnDataFn fn, void* Arg) {
    m_onData = fn;
    m_callbacksArg = Arg;
}

void TCPSocket::setOnConnecting(OnConnectingFn fn, void* Arg)
{
    m_onConnecting = fn;
    m_callbacksArg = Arg;
}

void TCPSocket::setOnConnected(OnConnectedFn fn, void* Arg)
{
    m_onConnected= fn;
    m_callbacksArg = Arg;
}

void TCPSocket::setOnAccepted(OnAcceptedFn fn, void* Arg)
{
    m_onAccepted = fn;
    m_callbacksArg = Arg;
}

void TCPSocket::setOnClose(OnCloseFn fn, void *Arg)
{
    m_onClose= fn;
    m_callbacksArg = Arg;
}

void TCPSocket::setOnConnectFailed(OnConnectFailedFn fn, void *Arg)
{
    m_onConnectFailed= fn;
    m_callbacksArg = Arg;
}

void TCPSocket::setOnPause(OnPauseFn fn, void* Arg) {
    m_onPause = fn;
    m_callbacksArg = Arg;
}

void TCPSocket::setOnResume(OnResumeFn fn, void* Arg) {
    m_onResume = fn;
    m_callbacksArg = Arg;
}

void TCPSocket::setOnEOF(OnEOFFn fn, void *Arg)
{
    m_onEOF = fn;
    m_callbacksArg = Arg;
}
