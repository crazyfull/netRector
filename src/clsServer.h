#ifndef SERVER_H
#define SERVER_H
#include "clsEpollReactor.h"

// ============================== Server / Server (10)(17)(18) =========
#include <thread>
#include <vector>

class Server
{
public:
    // maxConnection: andaze-ye avaliye-ye jadval fd (khodesh roshd mikone); saghf-e vaghei ba setMaxConnections.
    // bufferPoolSize: pool-e safe ersal har shard. maxEvents: batch-e epoll_wait.
    explicit Server(int maxConnection, int shards = std::max(1u, std::thread::hardware_concurrency()),
                    size_t bufferPoolSize = BUFFER_POOL_SIZE, int maxEvents = MAX_EVENTS);

    // saghf-e connection haye TCP dar har shard (0 = bedone saghf)
    void setMaxConnectionsPerShard(uint32_t n);
    ~Server();

    void setOnAccepted(EpollReactor::acceptCallback f, void *p);

    // bindIP: nullptr / "*" / "::" = hame interface ha (IPv4 + IPv6), "0.0.0.0" = hame IPv4,
    //         ya yek adres moshakhas (masalan "127.0.0.1").
    // cb/ctx: factory makhsoos in port; agar nullptr bashe az setOnAccepted estefade mishe.
    bool AddNewListener(int Port, const char *bindIP = nullptr, EpollReactor::acceptCallback cb = nullptr, void* ctx = nullptr);
    bool start();
    void stop();
    bool isRunning() const;


    void setUseGarbageCollector(bool value);
    EpollReactor *getRoundRobinShard();
    EpollReactor *getShard(int index);
    int shardCount() const;

private:
    int m_shardCount;

    std::vector <std::unique_ptr <EpollReactor>> m_workerList {};
    std::vector <std::thread> m_threads {};
    std::atomic <bool> m_needToStop {};
    std::atomic <bool> m_running { false };
    std::atomic <uint32_t> m_roundRobin { 0 };
    void setup_signals();

};

#endif // SERVER_H
