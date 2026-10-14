#ifndef CLSEPOLLREACTOR_H
#define CLSEPOLLREACTOR_H
#include "clsBufferPool.h"
#include "clsReactorObject.h"
#include "clsTCPSocket.h"
#include "clsGCList.h"
#include "clsSocketList.h"
#include "clsDNSLookup.h"
#include "constants.h"

// EpollReactor
// Har reactor roye yek thread ejra mishe. Hame method ha (be joz post/wake)
// faghat az thread haman reactor ghabele estefade hastan.
//class DNSLookup;
class TimerManager;
class UDPSocket;
class SocketList;
class EpollReactor
{
public:
    EpollReactor(int id, int maxConnection, int max_events = MAX_EVENTS, size_t bufferPoolSize = BUFFER_POOL_SIZE);
    ~EpollReactor();

    // factory from app to create high-level handler for accepted fd
    //using acceptCallback = std::function<TCPSocket*()>; // (extensibility)
    using acceptCallback = TCPSocket* (*)(void*);
    using TimerId = int;

    void setOnAccepted(acceptCallback fncallback, void *p);
    bool register_fd(int fd, epoll_event *pEvent, SockTypes sockType, void *ptr);
    bool addFlags(SocketContext *pContext, uint32_t flags);
    void removeFlags(SocketContext *pContext, uint32_t flags);
    bool setFlags(SocketContext *pContext, uint32_t events);    // EPOLL_CTL_MOD bedone shart (re-arm)
    void del_fd(int fd, bool removeFromList = false);
    void unregister_fd(int fd);     // faghat az ConnectionList (vaghti ::close khodesh fd ro az epoll hazf mikone)
    bool add_fd(int fd, epoll_event *pEvent, uint32_t events);

    // bindIP: nullptr / "" / "*" / "::" = hame interface ha (dual-stack),
    //         "0.0.0.0" = hame IPv4, ya yek adres moshakhas IPv4/IPv6.
    // cb/ctx: factory makhsoos in listener (agar nullptr bashe az setOnAccepted estefade mishe)
    bool add_listener(int port, const char* bindIP = nullptr, acceptCallback cb = nullptr, void* ctx = nullptr);
    int add_listener_fd(int port, const char* bindIP, acceptCallback cb, void* ctx);   // -1: khata
    bool remove_listener(int fd);
    void stop_listener();

    // true (default): dar har event ta MAX_ACCEPT_PER_EVENT connection accept mishe (yek accept-e
    // ezafe ke EAGAIN mide dar har batch). dar benchmark ba connection haye kotah ~35% behtar az
    // yek accept dar har event bod (nginx multi_accept off).
    // false: yek accept dar har event; listener level-triggered khodesh dobare etela mide.
    void setMultiAccept(bool enable) {
        m_multiAccept = enable;
    }

    // TCP_DEFER_ACCEPT roye listener haye badi: connection faghat vaghti data dare tahvil dade mishe
    void setDeferAccept(int seconds) {
        m_deferAccept = seconds;
    }

    void run(std::atomic<bool> &stop);

    // thread-safe: bidar kardane halghe / ejraye yek kar roye thread reactor
    void wake();
    void post(std::function<void()> task);

    void setUseGarbageCollector(bool newUseGarbageCollector);
    bool getIPbyName(const char *hostname, DNSLookup::callback_t callback, void *p, DNSLookup::QUERY_TYPE QuryType = DNSLookup::A);
    void cancelDNS(void *p);
    void deleteLater(ReactorObject* obj);
    TCPSocket* lookupSocket(uint64_t key);   // nullptr agar baste shode
    void updateCashedTime();

    // ejraye yek kar roye socket dar entehaye dor feli halghe (bad az hame event ha).
    // key haman epoll key (fd + genID) hast; agar socket ta an moghe baste beshe nadide gerefte mishe.
    void defer(uint64_t key, int kind);

    // timer ha (roye TimerManager dakheli)
    TimerId addTimer(int interval_ms, std::function<void()> cb, bool singleShot = false);
    void removeTimer(TimerId id);

    BufferPool *bufferPool();

    // buffer khandan moshtarak: mohtava faghat ta payane callback motabar hast
    char* readBuffer() { return m_readBuffer; }
    size_t readBufferSize() const { return READ_BUFFER_SIZE; }

    // buffer movaghat baraye sendv
    char* scratchBuffer() { return m_scratchBuffer; }
    size_t scratchBufferSize() const { return READ_BUFFER_SIZE + 64; }

    uint64_t getCachedNow() const;

    int id() const { return m_reactorID; }
    uint32_t connectionCount() const;

    // yek pointer delkhah baraye app (masalan state makhsoos har shard)
    void setUserData(void* p) { m_userData = p; }
    void* userData() const { return m_userData; }

private:
    struct ListenerInfo {
        int fd;
        int port;
        acceptCallback cb;
        void* ctx;
    };

    struct DeferredItem {
        uint64_t key;
        int kind;
    };

    bool m_useGarbageCollector {true};
    bool m_gcDue {false};
    bool m_multiAccept {true};
    int m_deferAccept {0};
    size_t m_maintCursor {0};
    int m_reactorID {0};
    int m_epollSocket {-1};
    int m_wakeupFd {-1};    //baraye exit safe epoll va post()
    int m_spareFd {-1};     //fd zakhire baraye halate EMFILE dar accept
    int m_maxEvent {100};
    int m_maxConnection {100};
    uint64_t m_lastAcceptErrorLog {0};
    timespec m_cached_now {};
    struct epoll_event m_wakeupEvent {};
    TimerManager *m_pTimers {nullptr};
    void* m_userData {nullptr};

    std::vector<ListenerInfo*> m_listenerList;
    GCList<ReactorObject> m_GCList;
    SocketList *m_pConnectionList {nullptr};
    BufferPool m_bufferPool;
    DNSLookup *m_pDNSLookup {nullptr};

    std::vector<DeferredItem> m_deferred;
    std::vector<DeferredItem> m_deferredRunning;

    std::mutex m_taskMutex;
    std::vector<std::function<void()>> m_tasks;
    std::atomic<bool> m_hasTasks {false};

    char* m_readBuffer {nullptr};
    char* m_scratchBuffer {nullptr};

    acceptCallback m_onAcceptCallback {};
    void* m_onAcceptCallbackCtx {nullptr};

    void init();
    void maintenance();
    void shutdown_all();
    void adoptAccepted(ListenerInfo* listener);
    void runPostedTasks();
    void runDeferred();
    void collectGarbage();

    // (16) telemetry placeholder; could be atomic counters per shard
    uint64_t make_key(int fd, uint32_t genID);
    int extract_fd(uint64_t key);
    uint32_t extract_gen(uint64_t key);

    void onTCPEvent(int fd, uint32_t &ev, void *ptr);
    void onUDPEvent(int fd, uint32_t &ev, void *ptr);
    void onTimerEvent(int fd, uint32_t &ev, void *ptr);
    void onTimerManagerEvent(int fd, uint32_t &ev, void *ptr);
    void onDNSEvent(int fd, uint32_t &ev, void *ptr);

    void checkDnsTimeouts();
    void runGarbageCollector();
};


#endif // CLSEPOLLREACTOR_H
