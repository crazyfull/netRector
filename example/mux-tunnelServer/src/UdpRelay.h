#ifndef UDPRELAY_H
#define UDPRELAY_H

#include "ServerConfig.h"
#include "ServerStats.h"

#include "clsMultiplexedTunnel.h"
#include "clsReactorObject.h"
#include "clsUDPSocket.h"
#include "clsEpollReactor.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

// The server side of one UDP-over-TCP stream (SOCKS5 command 0x05, "FWD UDP" of hev-socks5-tunnel).
//
// After the request is answered the stream carries datagram frames in both directions:
//
//     DATLEN(2, big endian) | HDRLEN(1) | ATYP(1) | ADDR | PORT(2) | DATA
//
// HDRLEN = 3 + length of (ATYP + ADDR + PORT). Upstream the address is the destination, downstream
// it is the source of the datagram. One UDP socket per address family is opened lazily, so a
// stream is one "UDP session" of the client (same source port towards every destination).
//
// Nothing is buffered: a datagram that cannot be sent right now is dropped, and downstream
// datagrams are dropped while the stream's send queue is full (UDP semantics, bounded memory).
//
// Owned by the reactor of its tunnel (deleteLater); it takes over the stream's callbacks from the
// TargetConnection that parsed the request.
class UdpRelay : public netrector::ReactorObject {
public:
    UdpRelay(netrector::MultiplexedTunnel* tunnel, uint32_t streamId, const ServerConfig& config,
             ServerStats& stats, std::shared_ptr<TunnelStats> tunnelStats);
    ~UdpRelay() override;

    // Takes over the stream, answers the request and handles `pending` (frames that arrived with it).
    void start(const std::string& pending);

private:
    static void onStreamData(void* argument, uint32_t streamId, const uint8_t* data, size_t length);
    static void onStreamRemoteFin(void* argument, uint32_t streamId);
    static void onStreamClose(void* argument, uint32_t streamId);
    static void onStreamPause(void* argument, uint32_t streamId);
    static void onStreamResume(void* argument, uint32_t streamId);
    static void onDatagram(void* argument, const uint8_t* data, size_t length, const sockaddr* from, socklen_t fromLength);
    static void onSocketClose(void* argument);

    void consume(const uint8_t* data, size_t length);
    void handleFrame(const uint8_t* address, size_t addressLength, const uint8_t* payload, size_t payloadLength);
    netrector::UDPSocket* socketFor(int family);
    void checkIdle();
    void finish(bool detachStream);

    netrector::MultiplexedTunnel* m_tunnel;
    uint32_t m_streamId;                                // 0 once the stream is gone
    const ServerConfig& m_config;
    ServerStats& m_stats;
    std::shared_ptr<TunnelStats> m_tunnelStats;

    netrector::UDPSocket* m_socket4 = nullptr;          // owned by the reactor, cleared in onSocketClose()
    netrector::UDPSocket* m_socket6 = nullptr;

    std::string m_rx;                                   // partial frame from the stream
    std::string m_tx;                                   // scratch buffer of one downstream frame
    bool m_paused = false;                              // the stream's send queue is full
    bool m_finished = false;
    netrector::EpollReactor::TimerId m_idleTimer = -1;
    std::chrono::steady_clock::time_point m_lastActivity;
};

#endif // UDPRELAY_H
