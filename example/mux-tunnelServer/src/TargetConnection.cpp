#include "TargetConnection.h"

#include "Socks5Reply.h"
#include "Socks5Request.h"
#include "Socks5RequestParser.h"

#include "clsLog.h"

#include <cstring>
#include <utility>

using namespace netrector;

TargetConnection::TargetConnection(MultiplexedTunnel* tunnel, uint32_t streamId, MultiplexedTunnel::Stream* stream,
                                   ServerStats& stats, std::shared_ptr<TunnelStats> tunnelStats)
    : m_tunnel(tunnel)
    , m_streamId(streamId)
    , m_stats(stats)
    , m_tunnelStats(std::move(tunnelStats))
{
    m_stats.channelOpened(*m_tunnelStats);
    setReactor(tunnel->getReactor());
    setAllowHalfClose(true);        // FIN of the target becomes FIN of the stream and vice versa

    stream->arg = this;
    stream->onData = &TargetConnection::onStreamData;
    stream->onClose = &TargetConnection::onStreamClose;
    stream->onRemoteFin = &TargetConnection::onStreamRemoteFin;
    stream->onPause = &TargetConnection::onStreamPause;
    stream->onResume = &TargetConnection::onStreamResume;
}

TargetConnection::~TargetConnection()
{
    m_stats.channelClosed(*m_tunnelStats);
}

// ---- target socket events ----

void TargetConnection::onConnected()
{
    NR_LOG(LOG_DEBUG, "[server] stream %u connected to target fd=%d", m_streamId, fd());
    m_state = State::Relaying;
    m_stats.targetConnected();
    m_targetCounted = true;
    if (m_streamId == 0)
        return;

    sendReply(Socks5ReplyCode::Succeeded);

    // bytes the client sent while we were connecting
    if (!m_buffer.empty()) {
        send(m_buffer.data(), m_buffer.size());
        std::string().swap(m_buffer);
    }

    if (m_streamFinReceived)
        shutdownWrite();

    if (m_streamId != 0 && !isWriteBlocked())
        m_tunnel->resumeStream(m_streamId);
}

void TargetConnection::onConnectFailed()
{
    NR_LOG(LOG_DEBUG, "[server] stream %u connect failed: %s", m_streamId, strerror(getErrorCode()));
    if (m_streamId == 0)
        return;

    m_state = State::Failed;
    sendReply(Socks5Reply::codeFromError(getErrorCode()));
    // onClose() follows immediately and finishes the stream with a FIN
}

void TargetConnection::onReceiveData(const uint8_t* data, size_t length)
{
    if (m_streamId != 0)
        m_tunnel->sendToStream(m_streamId, data, length);
}

void TargetConnection::onReadEOF()
{
    if (m_streamId != 0 && !m_finSentToStream) {
        m_finSentToStream = true;
        m_tunnel->closeStream(m_streamId, false);   // FIN, receiving continues
    }
}

// The send queue towards the target is full: stop granting window to the client.
void TargetConnection::onPause()
{
    if (m_streamId != 0)
        m_tunnel->pauseStream(m_streamId);
}

void TargetConnection::onResume()
{
    if (m_streamId != 0)
        m_tunnel->resumeStream(m_streamId);
}

void TargetConnection::onClose()
{
    if (m_targetCounted) {
        m_targetCounted = false;
        m_stats.targetClosed();
    }

    if (m_streamId == 0)
        return;

    const uint32_t streamId = m_streamId;
    m_streamId = 0;

    if (m_state == State::Failed || (m_finSentToStream && m_streamFinReceived)) {
        // error reply already queued, or both directions finished: let the tunnel end the stream
        m_tunnel->detachStream(streamId);
    } else {
        // the target connection broke: reset the stream
        m_tunnel->closeStream(streamId, true);
    }
}

// ---- request handling ----

void TargetConnection::handleRequest()
{
    Socks5Request request;
    const Socks5ParseResult result = Socks5RequestParser::parse(
        reinterpret_cast<const uint8_t*>(m_buffer.data()), m_buffer.size(), request);

    if (result == Socks5ParseResult::Incomplete)
        return;

    if (result == Socks5ParseResult::Invalid) {
        failWith(request.error);
        return;
    }

    m_buffer.erase(0, request.length);
    m_state = State::Connecting;

    // Until the target is connected no window is returned, so the client can send at most one
    // window (256 KB) of early data.
    m_tunnel->pauseStream(m_streamId);

    NR_LOG(LOG_DEBUG, "[server] stream %u: connecting to %s:%u", m_streamId, request.host.c_str(), request.port);
    if (!connectTo(request.host.c_str(), request.port))
        failWith(Socks5ReplyCode::GeneralFailure);
    // true: the result arrives through onConnected() / onConnectFailed()
}

void TargetConnection::sendReply(Socks5ReplyCode code)
{
    uint8_t reply[Socks5Reply::Size];
    Socks5Reply::build(code, reply);
    m_tunnel->sendToStream(m_streamId, reply, sizeof(reply));
}

void TargetConnection::failWith(Socks5ReplyCode code)
{
    m_state = State::Failed;
    sendReply(code);
    close();    // -> onClose() -> detachStream() ends the stream with a FIN after the reply
}

// ---- stream callbacks (run on the tunnel's reactor thread, argument is the TargetConnection) ----

void TargetConnection::onStreamData(void* argument, uint32_t /*streamId*/, const uint8_t* data, size_t length)
{
    TargetConnection* self = static_cast<TargetConnection*>(argument);

    switch (self->m_state) {
    case State::AwaitingRequest:
        self->m_buffer.append(reinterpret_cast<const char*>(data), length);
        self->handleRequest();
        break;

    case State::Connecting:
        self->m_buffer.append(reinterpret_cast<const char*>(data), length);
        break;

    case State::Relaying:
        self->send(data, length);
        break;

    case State::Failed:
        break;
    }
}

void TargetConnection::onStreamRemoteFin(void* argument, uint32_t /*streamId*/)
{
    TargetConnection* self = static_cast<TargetConnection*>(argument);
    self->m_streamFinReceived = true;

    switch (self->m_state) {
    case State::Relaying:
        self->shutdownWrite();      // FIN to the target once the queue is flushed
        break;

    case State::AwaitingRequest:
        self->close();              // the client gave up before finishing the request
        break;

    case State::Connecting:         // applied in onConnected()
    case State::Failed:
        break;
    }
}

void TargetConnection::onStreamClose(void* argument, uint32_t streamId)
{
    TargetConnection* self = static_cast<TargetConnection*>(argument);
    NR_LOG(LOG_DEBUG, "[server] stream %u closed", streamId);
    self->m_streamId = 0;

    // Normal end (FIN received): flush what is queued for the target. RST or tunnel loss: close now.
    self->close(!self->m_streamFinReceived);
}

// The stream's send queue is full: read less from the target.
void TargetConnection::onStreamPause(void* argument, uint32_t /*streamId*/)
{
    static_cast<TargetConnection*>(argument)->pause_reading();
}

void TargetConnection::onStreamResume(void* argument, uint32_t /*streamId*/)
{
    static_cast<TargetConnection*>(argument)->resume_reading();
}
