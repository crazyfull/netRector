#include "ClientTunnel.h"

#include "TunnelPool.h"

#include "clsLog.h"

#include <cstring>

using namespace netrector;

ClientTunnel::ClientTunnel(TunnelPool& pool, const ClientConfig& config, const WebSocketClientHandshake& handshake)
    : MultiplexedTunnel(true)
    , m_pool(pool)
    , m_config(config)
    , m_handshake(handshake)
{
    setMaxStreams(config.maxStreamsPerTunnel);
    setKeepAlive(config.keepAliveSeconds);      // the timer starts once the handshake is complete
}

void ClientTunnel::handleOnConnected()
{
    // The handshake request must be the first thing on the wire, so the mux layer (keep-alive
    // pings) is not started here. MultiplexedTunnel::handleOnConnected() is called from
    // completeHandshake().
    const std::string& request = m_handshake.request();
    if (!send(request.data(), request.size())) {
        close(true);
        return;
    }

    m_state = State::AwaitingResponse;
    setIdleTimeout(m_config.handshakeTimeoutSeconds);
}

void ClientTunnel::handleOnData(const uint8_t* data, size_t length)
{
    switch (m_state) {
    case State::Established:
        MultiplexedTunnel::handleOnData(data, length);
        break;

    case State::AwaitingResponse:
        processHandshake(data, length);
        break;

    case State::Connecting:
    case State::Failed:
        break;
    }
}

void ClientTunnel::processHandshake(const uint8_t* data, size_t length)
{
    m_handshakeBuffer.append(reinterpret_cast<const char*>(data), length);

    size_t consumed = 0;
    switch (m_handshake.evaluate(m_handshakeBuffer, consumed)) {
    case HandshakeResult::Incomplete:
        break;

    case HandshakeResult::Accepted:
        completeHandshake(consumed);
        break;

    case HandshakeResult::Rejected:
        NR_LOG(LOG_WARN, "[client] handshake with %s:%u was refused", m_config.cdnAddress.c_str(), m_config.cdnPort);
        m_state = State::Failed;
        std::string().swap(m_handshakeBuffer);
        close(true);
        break;
    }
}

void ClientTunnel::completeHandshake(size_t consumed)
{
    m_state = State::Established;
    setIdleTimeout(0);

    // mux bytes that arrived in the same read as the response
    std::string pending = m_handshakeBuffer.substr(consumed);
    std::string().swap(m_handshakeBuffer);

    MultiplexedTunnel::handleOnConnected();     // starts keep-alive, calls onConnected()

    if (!pending.empty() && getStatus() == Connected)
        MultiplexedTunnel::handleOnData(reinterpret_cast<const uint8_t*>(pending.data()), pending.size());
}

void ClientTunnel::onConnected()
{
    NR_LOG(LOG_INFO, "[client] tunnel established with %s:%u", m_config.cdnAddress.c_str(), m_config.cdnPort);
    m_pool.onTunnelReady(*this);
}

void ClientTunnel::onConnectFailed()
{
    NR_LOG(LOG_WARN, "[client] cannot connect to %s:%u: %s",
           m_config.cdnAddress.c_str(), m_config.cdnPort, std::strerror(getErrorCode()));
}

void ClientTunnel::onClose()
{
    // All streams were already closed by MultiplexedTunnel.
    m_pool.onTunnelClosed(*this);
}

void ClientTunnel::onGoAway(uint32_t /*code*/)
{
    m_pool.onTunnelGoAway(*this);
}
