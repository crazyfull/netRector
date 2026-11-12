#include "ServerTunnel.h"

#include "TargetConnection.h"

#include "clsLog.h"

using namespace netrector;

ServerTunnel::ServerTunnel(const ServerConfig& config, const WebSocketServerHandshake& handshake)
    : MultiplexedTunnel(false)
    , m_config(config)
    , m_handshake(handshake)
{
    setOnNewStream(&ServerTunnel::handleNewStream, this);
    setMaxStreams(config.maxStreamsPerTunnel);
    setKeepAlive(config.keepAliveSeconds);      // the timer starts once the handshake is complete
}

void ServerTunnel::handleOnAccepted()
{
    // Do not start the mux layer (keep-alive pings) before the handshake. MultiplexedTunnel::
    // handleOnAccepted() is called from completeHandshake().
    m_state = State::AwaitingRequest;
    setIdleTimeout(m_config.handshakeTimeoutSeconds);
}

void ServerTunnel::handleOnData(const uint8_t* data, size_t length)
{
    switch (m_state) {
    case State::Established:
        MultiplexedTunnel::handleOnData(data, length);
        break;

    case State::AwaitingRequest:
        processHandshake(data, length);
        break;

    case State::Rejected:
        break;
    }
}

void ServerTunnel::processHandshake(const uint8_t* data, size_t length)
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
        rejectHandshake();
        break;
    }
}

void ServerTunnel::completeHandshake(size_t consumed)
{
    const std::string& response = m_handshake.acceptResponse();
    if (!send(response.data(), response.size())) {
        close(true);
        return;
    }

    m_state = State::Established;
    setIdleTimeout(0);

    // mux bytes that arrived in the same read as the request
    std::string pending = m_handshakeBuffer.substr(consumed);
    std::string().swap(m_handshakeBuffer);

    MultiplexedTunnel::handleOnAccepted();      // starts keep-alive, calls onAccepted()

    if (!pending.empty() && getStatus() == Connected)
        MultiplexedTunnel::handleOnData(reinterpret_cast<const uint8_t*>(pending.data()), pending.size());
}

void ServerTunnel::rejectHandshake()
{
    std::string peerAddress;
    uint16_t peerPort = 0;
    getPeerAddress(peerAddress, peerPort);
    NR_LOG(LOG_INFO, "[server] handshake rejected from %s:%u", peerAddress.c_str(), peerPort);

    m_state = State::Rejected;
    std::string().swap(m_handshakeBuffer);

    const std::string& response = m_handshake.rejectResponse();
    send(response.data(), response.size());
    close();
}

void ServerTunnel::onAccepted()
{
    std::string peerAddress;
    uint16_t peerPort = 0;
    getPeerAddress(peerAddress, peerPort);
    NR_LOG(LOG_INFO, "[server] tunnel established from %s:%u", peerAddress.c_str(), peerPort);
}

void ServerTunnel::onClose()
{
    NR_LOG(LOG_DEBUG, "[server] tunnel closed (fd closed, state=%d)", static_cast<int>(m_state));
}

void ServerTunnel::handleNewStream(void* argument, uint32_t streamId, Stream* stream)
{
    ServerTunnel* tunnel = static_cast<ServerTunnel*>(argument);
    NR_LOG(LOG_DEBUG, "[server] new stream %u", streamId);

    // The object registers itself as the stream's callback target and is owned by the reactor
    // once it is connected or closed.
    new TargetConnection(tunnel, streamId, stream);
}
