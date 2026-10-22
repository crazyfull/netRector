#ifndef CONSTANTS_H
#define CONSTANTS_H
#include <cstddef>

// Epoll and Socket Constants
static constexpr int MAX_EVENTS = 1024;             // batch epoll_wait
static constexpr int LISTEN_BACKLOG = 4096;
static constexpr int MAX_ACCEPT_PER_EVENT = 64;     // fairness: max accept dar har event
static constexpr int MAX_READS_PER_EVENT = 16;      // fairness: max recv dar har event baraye yek socket
//static constexpr int IDLE_TIMEOUT_SEC = 30;       // graceful idle GC


// DNS Lookup Configuration Constants
constexpr unsigned int DNS_LOOKUP_TIMEOUT_SEC = 2;   // har talash (ba deghat mili sanie)
constexpr unsigned int DNS_CACHE_TTL_SEC = 5*60;     // 5 minutes
constexpr unsigned int DNS_MAX_RETRIES = 3;


//buffer config
static constexpr size_t BUFFER_POOL_SIZE = 200 * (1024*1024);    //200M baraye safe ersal (har shard)
static constexpr size_t BACK_PRESSURE = 128*1024;                // high watermark safe ersal
static constexpr size_t SLAB_SIZE = 8 * 1024;                    // 8KB
//static constexpr size_t HIGH_WATERMARK = 64 * 1024;
static constexpr size_t LOW_WATERMARK = 64 * 1024;
static constexpr size_t READ_BUFFER_SIZE = 64 * 1024;            // buffer khandan moshtarak har reactor
static constexpr size_t SEND_BLOCK_MIN = 4 * 1024;               // hadeaghal size block dar SendQueue
static constexpr size_t MAX_SEND_QUEUE = 64 * 1024 * 1024;       // saghf-e sakht-e safe ersal har socket (setMaxSendQueue)


// Timer Intervals (in milliseconds)
constexpr int UPDATE_CACHED_NOW      = 1000;
constexpr int DNS_TIMEOUT_INTERVAL_MS      = 200;
constexpr int GARBAGE_COLLECTOR_INTERVAL_MS = 10*1000;  // 10 seconds
constexpr int IDLE_CONNECTION_INTERVAL_MS = 30*1000;    // 30 seconds
constexpr int CLOSE_WAIT_INTERVAL_MS = 5*1000;          // (ghadimi) fasele check timeout ha
constexpr int MAINTENANCE_SLICE_MS = 250;               // har 250ms yek tike az jadval socket ha
constexpr int MAINTENANCE_FULL_PASS_MS = 5*1000;        // kol jadval har 5 sanie yek bar
constexpr int CLOSING_TIMEOUT_SECS = 60;                // max zaman baraye khali shodan safe ersal
constexpr int LINGER_TIMEOUT_SECS = 5;                  // max entezar baraye FIN tarafe moghabel
constexpr int CONNECT_TIMEOUT_SECS = 15;                // connect + TLS handshake
constexpr int GC_MAX_AGE_SECS = 10;                     // omr object dar halate GC ba takhir

// Keep-Alive socket
//
constexpr int KEEPIDLE_SECS = 240;   // 4 min delay baraye shoro keep laive
constexpr int KEEPINTVL_SECS = 30;   // 30 sanie fasle beyne ersale har Probe
constexpr int KEEPCNT_MAX = 4;       // maximum 4 ersal

//KEEPIDLE_SECS + (KEEPCNT_MAX * KEEPINTVL_SECS) = 6 min

#endif // CONSTANTS_H
