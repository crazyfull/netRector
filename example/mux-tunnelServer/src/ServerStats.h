#ifndef SERVERSTATS_H
#define SERVERSTATS_H

#include "TunnelStats.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Runtime statistics of mux-tunnelServer, shared by all reactor threads.
//
//   tunnel   - an established mux tunnel (handshake done)
//   channel  - a mux stream, i.e. one SOCKS5 request of the client
//   target   - the outbound TCP connection of a channel, once it is connected
class ServerStats {
public:
    std::shared_ptr<TunnelStats> addTunnel(const std::string& peer);
    void removeTunnel(const std::shared_ptr<TunnelStats>& tunnel);

    void handshakeRejected() { m_rejectedHandshakes++; }

    void channelOpened(TunnelStats& tunnel);
    void channelClosed(TunnelStats& tunnel);

    void targetConnected() { m_activeTargets++; m_totalTargets++; }
    void targetClosed() { m_activeTargets--; }

    // Multi-line, human readable report.
    std::string report() const;

private:
    mutable std::mutex m_mutex;                     // guards m_tunnels
    std::vector<std::shared_ptr<TunnelStats>> m_tunnels;
    uint64_t m_nextTunnelId = 1;

    std::atomic<uint64_t> m_totalTunnels { 0 };
    std::atomic<uint64_t> m_rejectedHandshakes { 0 };
    std::atomic<uint64_t> m_activeChannels { 0 };
    std::atomic<uint64_t> m_totalChannels { 0 };
    std::atomic<uint64_t> m_activeTargets { 0 };
    std::atomic<uint64_t> m_totalTargets { 0 };
};

#endif // SERVERSTATS_H
