#ifndef TUNNELPOOL_H
#define TUNNELPOOL_H

#include "ClientConfig.h"
#include "TunnelSlot.h"
#include "WebSocketClientHandshake.h"

#include "clsEpollReactor.h"
#include "clsMultiplexedTunnel.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

class ClientTunnel;

// The mux tunnels that belong to one reactor thread.
//
// netRector objects may only be used from the thread of their reactor, so every thread owns its
// own set of tunnels and serves the SOCKS5 connections accepted on that thread. The pool keeps
// each tunnel connected (reconnect with exponential back-off) and hands out streams round-robin.
class TunnelPool {
public:
    TunnelPool(netrector::EpollReactor& reactor, const ClientConfig& config,
               const WebSocketClientHandshake& handshake, size_t tunnelCount);

    // Reactor thread: starts connecting all tunnels.
    void start();

    // Any thread: no more reconnects (used when shutting down).
    void stop();

    // Reactor thread: opens a stream on the next ready tunnel (round-robin). Returns the stream id
    // and the tunnel it belongs to, or 0 when no tunnel can take a new stream.
    uint32_t openStream(netrector::MultiplexedTunnel::OnStreamDataFn dataFunction,
                        netrector::MultiplexedTunnel::OnStreamCloseFn closeFunction,
                        void* argument, netrector::MultiplexedTunnel*& tunnel);

    // Notifications from ClientTunnel (reactor thread).
    void onTunnelReady(ClientTunnel& tunnel);
    void onTunnelClosed(ClientTunnel& tunnel);
    void onTunnelGoAway(ClientTunnel& tunnel);

private:
    void connectSlot(size_t index);
    void scheduleReconnect(size_t index);
    TunnelSlot* findSlot(ClientTunnel& tunnel, size_t& index);

    netrector::EpollReactor& m_reactor;
    const ClientConfig& m_config;
    const WebSocketClientHandshake& m_handshake;
    std::vector<TunnelSlot> m_slots;
    size_t m_nextSlot = 0;                  // round-robin cursor
    std::atomic<bool> m_stopped { false };
};

#endif // TUNNELPOOL_H
