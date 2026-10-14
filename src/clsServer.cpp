#include "clsServer.h"
#include <future>

Server::Server(int maxConnection, int shards): m_shardCount(shards > 0 ? shards : 1), m_needToStop(false)
{
    // neveshtan roye socket baste shode nabayad process ro bekoshe
    // (BoringSSL ba send() bedone MSG_NOSIGNAL minevise)
    ::signal(SIGPIPE, SIG_IGN);

    for(int i = 0; i < m_shardCount; ++i)
        m_workerList.emplace_back(std::make_unique<EpollReactor> (i, maxConnection));
}

Server::~Server() {
    stop();
}

bool Server::AddNewListener(int Port, const char *bindIP, EpollReactor::acceptCallback cb, void* ctx)
{
    std::string ip = bindIP ? bindIP : "";

    if (!m_running.load()) {
        // reactor ha hanooz ejra nashodan: mostaghim
        for (auto& worker : m_workerList) {
            if(!worker->add_listener(Port, ip.c_str(), cb, ctx)){
                return false;
            }
        }
    } else {
        // reactor ha dar hale ejra hastan: kar bayad roye thread khodeshon anjam beshe
        std::vector<std::future<bool>> results;
        for (auto& worker : m_workerList) {
            auto done = std::make_shared<std::promise<bool>>();
            results.push_back(done->get_future());
            EpollReactor* w = worker.get();
            w->post([w, Port, ip, cb, ctx, done]() {
                done->set_value(w->add_listener(Port, ip.c_str(), cb, ctx));
            });
        }

        bool ok = true;
        for (auto& r : results)
            ok = r.get() && ok;
        if (!ok)
            return false;
    }

    printf("StartListen: (%s:%d)\n", ip.empty() ? "*" : ip.c_str(), Port);
    return true;

}

void Server::setOnAccepted(EpollReactor::acceptCallback f,void* p) {
    for(auto &worker: m_workerList)
        worker->setOnAccepted(f, p);
}

bool Server::start()
{
    if (m_running.exchange(true))
        return false;   // ghablan start shode

    //disable chon to main signal ro control mikonim
    // setup_signals();

    m_needToStop.store(false);
    m_threads.clear();

    for(int i = 0; i < m_shardCount; ++i) {
        m_threads.emplace_back([this, i]() {
            m_workerList[i]->run(m_needToStop);
        });
    }

    return true;
}

void Server::stop() {
    if (!m_running.exchange(false))
        return;

    m_needToStop.store(true);

    // har reactor dar payane run() listener ha va connection haye khodesh ro
    // roye thread khodesh mibande (shutdown_all)
    for (auto& reactor: m_workerList)
        reactor->wake();

    for (auto& t : m_threads) {
        if (t.joinable() && t.get_id() != std::this_thread::get_id()) {
            t.join();
        }
    }
    m_threads.clear();
}

bool Server::isRunning() const
{
    return m_running.load();
}

void Server::setUseGarbageCollector(bool value)
{
    for(auto &worker: m_workerList)
        worker->setUseGarbageCollector(value);
}

EpollReactor* Server::getRoundRobinShard()
{
    if (m_shardCount == 0) {
        throw std::runtime_error("No shards available");
    }
    //round-robin
    auto idx = m_roundRobin.fetch_add(1) % m_shardCount;
    return m_workerList[idx].get();
}

EpollReactor *Server::getShard(int index)
{
    if (index < 0 || index >= m_shardCount)
        return nullptr;

    return m_workerList[index].get();
}

int Server::shardCount() const
{
    return m_shardCount;
}


void Server::setup_signals() {
    sigset_t mask;
    sigemptyset( &mask);
    sigaddset( &mask, SIGINT);
    sigaddset( &mask, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &mask, nullptr);
    int sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);

    if(sfd != -1)
        ::close(sfd);
}
