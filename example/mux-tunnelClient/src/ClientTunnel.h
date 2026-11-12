#ifndef CLIENTTUNNEL_H
#define CLIENTTUNNEL_H

#include "ClientConfig.h"
#include "WebSocketClientHandshake.h"

#include "clsMultiplexedTunnel.h"

#include <string>

class TunnelPool;

// One mux tunnel connection to mux-tunnelServer (possibly through a CDN).
//
// After the TCP connection (and TLS, if enabled later) is up, the websocket-style upgrade request
// is sent as the very first bytes. Mux frames are neither sent nor interpreted until the server
// answered with a valid 101 response; only then the tunnel is reported ready to its TunnelPool.
class ClientTunnel : public netrector::MultiplexedTunnel {
public:
    ClientTunnel(TunnelPool& pool, const ClientConfig& config, const WebSocketClientHandshake& handshake);

    // Called after the handshake succeeded.
    void onConnected() override;
    void onConnectFailed() override;
    void onClose() override;

protected:
    void handleOnConnected() override;
    void handleOnData(const uint8_t* data, size_t length) override;
    void onGoAway(uint32_t code) override;

private:
    enum class State {
        Connecting,         // TCP (and TLS) not established yet
        AwaitingResponse,   // request sent, waiting for the 101 response
        Established,        // handshake done, mux frames are processed
        Failed              // handshake refused, closing
    };

    void processHandshake(const uint8_t* data, size_t length);
    void completeHandshake(size_t consumed);

    TunnelPool& m_pool;
    const ClientConfig& m_config;
    const WebSocketClientHandshake& m_handshake;
    State m_state = State::Connecting;
    std::string m_handshakeBuffer;
};

#endif // CLIENTTUNNEL_H
