#ifndef TUNNELSTATS_H
#define TUNNELSTATS_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

// Counters of one established tunnel. Written by the tunnel's reactor thread and read by the
// thread that prints the statistics, hence the atomics.
struct TunnelStats {
    uint64_t id = 0;
    std::string peer;                                           // "ip:port" of the client / CDN
    std::chrono::steady_clock::time_point establishedAt;
    std::atomic<uint32_t> activeStreams { 0 };
    std::atomic<uint64_t> totalStreams { 0 };
};

#endif // TUNNELSTATS_H
