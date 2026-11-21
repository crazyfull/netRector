#ifndef SERVERTUNNEL_H
#define SERVERTUNNEL_H

#include "ServerConfig.h"
#include "ServerStats.h"
#include "WebSocketServerHandshake.h"

#include "clsMultiplexedTunnel.h"

#include <memory>
#include <string>

// One accepted tunnel connection from mux-tunnelClient (possibly through a CDN).
//
// The connection starts in the websocket-style handshake. Mux frames are not interpreted until
// the handshake succeeded; after that every new stream becomes a TargetConnection.
class ServerTunnel : public netrector::MultiplexedTunnel {
public:
    ServerTunnel(const ServerConfig& config, const WebSocketServerHandshake& handshake, ServerStats& stats);

    // Called after the handshake succeeded (and after TLS, if it is enabled).
    void onAccepted() override;
    void onClose() override;

protected:
    void handleOnAccepted() override;
    void handleOnData(const uint8_t* data, size_t length) override;

private:
    enum class State {
        AwaitingRequest,    // TCP accepted, handshake request not complete yet
        Established,        // 101 sent, mux frames are processed
        Rejected            // handshake refused, waiting for the connection to close
    };

    static void handleNewStream(void* argument, uint32_t streamId, Stream* stream);

    void processHandshake(const uint8_t* data, size_t length);
    void completeHandshake(size_t consumed);
    void rejectHandshake();

    const ServerConfig& m_config;
    const WebSocketServerHandshake& m_handshake;
    ServerStats& m_stats;
    std::shared_ptr<TunnelStats> m_tunnelStats;     // set while the tunnel is established
    State m_state = State::AwaitingRequest;
    std::string m_handshakeBuffer;
};

#endif // SERVERTUNNEL_H
