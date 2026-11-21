#include "ServerStats.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

std::shared_ptr<TunnelStats> ServerStats::addTunnel(const std::string& peer)
{
    auto tunnel = std::make_shared<TunnelStats>();
    tunnel->peer = peer;
    tunnel->establishedAt = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(m_mutex);
    tunnel->id = m_nextTunnelId++;
    m_tunnels.push_back(tunnel);
    m_totalTunnels++;
    return tunnel;
}

void ServerStats::removeTunnel(const std::shared_ptr<TunnelStats>& tunnel)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_tunnels.erase(std::remove(m_tunnels.begin(), m_tunnels.end(), tunnel), m_tunnels.end());
}

void ServerStats::channelOpened(TunnelStats& tunnel)
{
    tunnel.activeStreams++;
    tunnel.totalStreams++;
    m_activeChannels++;
    m_totalChannels++;
}

void ServerStats::channelClosed(TunnelStats& tunnel)
{
    tunnel.activeStreams--;
    m_activeChannels--;
}

std::string ServerStats::report() const
{
    std::vector<std::shared_ptr<TunnelStats>> tunnels;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        tunnels = m_tunnels;
    }

    char line[256];
    std::string text;

    std::snprintf(line, sizeof(line),
        "tunnels:            %zu active   (%llu accepted, %llu handshakes rejected)\n"
        "channels (streams): %llu active   (%llu total)\n"
        "target connections: %llu active   (%llu total)\n",
        tunnels.size(),
        static_cast<unsigned long long>(m_totalTunnels.load()),
        static_cast<unsigned long long>(m_rejectedHandshakes.load()),
        static_cast<unsigned long long>(m_activeChannels.load()),
        static_cast<unsigned long long>(m_totalChannels.load()),
        static_cast<unsigned long long>(m_activeTargets.load()),
        static_cast<unsigned long long>(m_totalTargets.load()));
    text += line;

    const auto now = std::chrono::steady_clock::now();
    for (const auto& tunnel : tunnels) {
        const long long seconds = std::chrono::duration_cast<std::chrono::seconds>(now - tunnel->establishedAt).count();
        std::snprintf(line, sizeof(line), "  tunnel #%-4llu %-22s up %llds  channels %u active / %llu total\n",
            static_cast<unsigned long long>(tunnel->id), tunnel->peer.c_str(), seconds,
            tunnel->activeStreams.load(), static_cast<unsigned long long>(tunnel->totalStreams.load()));
        text += line;
    }

    return text;
}
