#ifndef TARGETCONNECTION_H
#define TARGETCONNECTION_H

#include "ServerStats.h"
#include "Socks5ReplyCode.h"

#include "clsMultiplexedTunnel.h"
#include "clsTCPSocket.h"

#include <cstdint>
#include <memory>
#include <string>

// The outbound side of one tunnel stream.
//
// Every mux stream carries one SOCKS5 CONNECT request. This object parses it, connects to the
// requested target, answers with the SOCKS5 reply and then relays bytes in both directions.
// It is itself the TCP socket to the target, so the reactor owns it and deletes it after
// onClose(). It never outlives its stream's callbacks: the stream callbacks are cleared (RST /
// detach) before the object goes away.
class TargetConnection : public netrector::TCPSocket {
public:
    TargetConnection(netrector::MultiplexedTunnel* tunnel, uint32_t streamId, netrector::MultiplexedTunnel::Stream* stream,
                     ServerStats& stats, std::shared_ptr<TunnelStats> tunnelStats);
    ~TargetConnection() override;

    void onConnected() override;
    void onConnectFailed() override;
    void onReceiveData(const uint8_t* data, size_t length) override;
    void onReadEOF() override;
    void onPause() override;
    void onResume() override;
    void onClose() override;

private:
    enum class State {
        AwaitingRequest,    // collecting the SOCKS5 request from the stream
        Connecting,         // connecting to the target
        Relaying,           // connected, forwarding data
        Failed              // an error reply was sent, closing
    };

    void handleRequest();
    void sendReply(Socks5ReplyCode code);
    void failWith(Socks5ReplyCode code);

    static void onStreamData(void* argument, uint32_t streamId, const uint8_t* data, size_t length);
    static void onStreamRemoteFin(void* argument, uint32_t streamId);
    static void onStreamClose(void* argument, uint32_t streamId);
    static void onStreamPause(void* argument, uint32_t streamId);
    static void onStreamResume(void* argument, uint32_t streamId);

    netrector::MultiplexedTunnel* m_tunnel;
    uint32_t m_streamId;                // 0 once the stream is gone
    State m_state = State::AwaitingRequest;
    std::string m_buffer;               // partial request, then data received before the target is connected
    bool m_streamFinReceived = false;   // the client finished sending
    bool m_finSentToStream = false;     // the target finished sending
    ServerStats& m_stats;
    std::shared_ptr<TunnelStats> m_tunnelStats;     // kept alive: this object can outlive its tunnel
    bool m_targetCounted = false;       // targetConnected() was reported
};

#endif // TARGETCONNECTION_H
