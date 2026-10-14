#include "clsMultiplexedTunnel.h"
#include <cstdio>
#include <vector>
#include "clsEpollReactor.h"


// --- Public API ---

MultiplexedTunnel::MultiplexedTunnel(bool isClient) :
    m_isClient(isClient),
    m_nextStreamId(isClient ? 1 : 2)    // client: fard, server: zoj
{
}

MultiplexedTunnel::~MultiplexedTunnel() {
    stopKeepAlive();
}

void MultiplexedTunnel::setOnNewStream(OnNewStreamFn fn, void* arg) {
    m_onNewStream = fn;
    m_newStreamArg = arg;
}

MultiplexedTunnel::Stream *MultiplexedTunnel::findStream(uint32_t streamId)
{
    auto it = m_streams.find(streamId);
    if (it == m_streams.end())
        return nullptr;
    return &it->second;
}

MultiplexedTunnel::Stream *MultiplexedTunnel::getStream(uint32_t streamId)
{
    Stream* s = findStream(streamId);
    return (s && !s->dead) ? s : nullptr;
}

size_t MultiplexedTunnel::streamPendingBytes(uint32_t streamId)
{
    Stream* s = getStream(streamId);
    return (s && s->pendingData) ? s->pendingData->size() : 0;
}

size_t MultiplexedTunnel::streamCount() const
{
    return m_liveStreams;
}

bool MultiplexedTunnel::initSendQueue(Stream& session) {
    if (session.pendingData) {
        return true;
    }

    if (!getReactor())
        return false;

    session.pendingData = new SendQueue;
    session.pendingData->init(getReactor()->bufferPool());
    return true;
}

uint32_t MultiplexedTunnel::openStream(OnStreamDataFn onData, OnStreamCloseFn onClose, void* arg) {
    if (getStatus() != Connected || m_localGoAway || m_remoteGoAway)
        return 0;

    uint32_t id = m_nextStreamId;
    if (id + 2 < id) {
        // ID ha tamam shodan; in session dige stream jadid nemigire
        m_remoteGoAway = true;
        return 0;
    }
    m_nextStreamId += 2;

    // SYN rooye yek WindowUpdate ba delta 0 (mesle hashicorp/yamux).
    // data ro mishe bedone entezar baraye ACK ferestad.
    if (!sendFrame(FrameType::WindowUpdate, FrameFlags::SYN, id, 0))
        return 0;

    Stream& s = m_streams[id];
    s.id = id;
    s.onData = onData;
    s.onClose = onClose;
    s.arg = arg;
    m_liveStreams++;

    return id;
}


bool MultiplexedTunnel::sendToStream(uint32_t streamId, const uint8_t* data, size_t len) {
    if (len == 0)
        return true;

    Stream* s = getStream(streamId);
    if (!s || s->localClosed || getStatus() != Connected)
        return false;

    size_t pos = 0;

    // ersal mostaghim faghat vaghti chizi dar safe stream nist (hefze tartib)
    if (!s->pendingData || s->pendingData->empty()) {
        while (pos < len && s->sendWindow > 0 && !m_txBlocked) {
            uint32_t chunk = (uint32_t)std::min<size_t>({len - pos, (size_t)s->sendWindow, (size_t)MAX_FRAME_PAYLOAD});
            if (!sendFrame(FrameType::Data, 0, streamId, chunk, data + pos))
                return false;

            s->sendWindow -= chunk;
            pos += chunk;
        }

        if (pos == len)
            return true;
    }

    // window tarafe moghabel ya safe tunnel por hast: baghimande dar safe stream mimone
    if (!initSendQueue(*s) || !s->pendingData->push(data + pos, len - pos)) {
        fprintf(stderr, "yamux: out of memory, closing tunnel fd=%d\n", fd());
        abortLater();
        return false;
    }

    if (s->sendWindow > 0) {
        scheduleFlush(s);
        if (!m_txBlocked)
            flushStreams();
    }

    // be app etela bede ke dige data nade (backpressure)
    if (!s->dead && !s->sendBlocked && s->pendingData && s->pendingData->size() > STREAM_HIGH_WATERMARK) {
        s->sendBlocked = true;
        if (s->onPause)
            s->onPause(s->arg, streamId);
    }

    return true;
}

void MultiplexedTunnel::closeStream(uint32_t streamId, bool rst) {
    DispatchGuard guard(this);

    Stream* s = getStream(streamId);
    if (!s)
        return;

    if (rst) {
        if (getStatus() == Connected)
            sendFrame(FrameType::WindowUpdate, FrameFlags::RST, streamId, 0);
        finishStream(s, false);
        return;
    }

    if (s->localClosed)
        return;
    s->localClosed = true;

    // FIN bayad bad az data haye dar saf ferestade beshe
    if (!s->pendingData || s->pendingData->empty())
        sendFin(s);
}

void MultiplexedTunnel::sendFin(Stream *s)
{
    if (s->finSent || s->dead)
        return;

    if (getStatus() == Connected)
        sendFrame(FrameType::WindowUpdate, FrameFlags::FIN, s->id, 0);
    s->finSent = true;

    // har do taraf FIN dadan
    if (s->remoteClosed)
        finishStream(s, true);
}

void MultiplexedTunnel::pauseStream(uint32_t streamId)
{
    Stream* s = getStream(streamId);
    if (s)
        s->readPaused = true;
}

void MultiplexedTunnel::resumeStream(uint32_t streamId)
{
    Stream* s = getStream(streamId);
    if (!s || !s->readPaused)
        return;

    s->readPaused = false;
    maybeSendWindowUpdate(s);
}

void MultiplexedTunnel::detachStream(uint32_t streamId)
{
    DispatchGuard guard(this);

    Stream* s = getStream(streamId);
    if (!s)
        return;

    s->onData = nullptr;
    s->onClose = nullptr;
    s->onRemoteFin = nullptr;
    s->onPause = nullptr;
    s->onResume = nullptr;
    s->arg = nullptr;

    // data haye badi dor rikhte mishan ama window pas dade mishe ta tarafe moghabel gir nakone
    s->readPaused = false;
    maybeSendWindowUpdate(s);

    if (!s->localClosed)
        closeStream(streamId, false);
}

void MultiplexedTunnel::maybeSendWindowUpdate(Stream *s)
{
    if (s->dead || s->readPaused || s->remoteClosed)
        return;

    if (s->unackedRecvBytes < WINDOW_UPDATE_THRESHOLD)
        return;

    uint32_t delta = s->unackedRecvBytes;
    s->unackedRecvBytes = 0;
    s->recvWindow += delta;
    sendFrame(FrameType::WindowUpdate, 0, s->id, delta);
}

void MultiplexedTunnel::sendPing(bool isACK, uint32_t value) {
    if (getStatus() != Connected)
        return;

    // darkhast ping ba SYN va javab ba ACK
    sendFrame(FrameType::Ping, isACK ? FrameFlags::ACK : FrameFlags::SYN, 0, value);
}

void MultiplexedTunnel::setKeepAlive(uint32_t intervalSec)
{
    m_keepAliveInterval = intervalSec;
    stopKeepAlive();
    if (getStatus() == Connected)
        startKeepAlive();
}

void MultiplexedTunnel::startKeepAlive()
{
    if (m_keepAliveInterval == 0 || m_keepAliveTimer != -1 || !getReactor())
        return;

    m_pingOutstanding = false;
    m_keepAliveTimer = getReactor()->addTimer((int)m_keepAliveInterval * 1000, [this]() {
        onKeepAliveTick();
    });
}

void MultiplexedTunnel::stopKeepAlive()
{
    if (m_keepAliveTimer != -1 && getReactor()) {
        getReactor()->removeTimer(m_keepAliveTimer);
    }
    m_keepAliveTimer = -1;
}

void MultiplexedTunnel::onKeepAliveTick()
{
    if (getStatus() != Connected)
        return;

    if (m_pingOutstanding) {
        // ping ghabli bi javab monde: ertebat morde
        fprintf(stderr, "yamux: keepalive timeout, closing tunnel fd=%d\n", fd());
        close(true);
        return;
    }

    m_pingOutstanding = true;
    m_pingValue++;
    sendPing(false, m_pingValue);
}

void MultiplexedTunnel::shutdown(GoAwayCode code) {
    DispatchGuard guard(this);

    if (getStatus() != Connected) {
        close(true);
        return;
    }

    if (!m_localGoAway) {
        m_localGoAway = true;
        sendFrame(FrameType::GoAway, 0, 0, static_cast<uint32_t>(code));
    }

    closeAllStreams(true);

    // GoAway ferestade mishe va bad socket baste mishe
    close(false);
}

void MultiplexedTunnel::protocolError(const char *what)
{
    fprintf(stderr, "yamux: protocol error (%s), closing tunnel fd=%d\n", what, fd());
    shutdown(GoAwayCode::ProtocolError);
}

// --- Protected/Private Helpers ---

bool MultiplexedTunnel::sendFrame(FrameType type, uint16_t flags, uint32_t streamId, uint32_t length, const uint8_t* payload) {
    uint8_t header[HEADER_SIZE];

    header[0] = YAMUX_VERSION;
    header[1] = static_cast<uint8_t>(type);

    uint16_t netFlags = htons(flags);
    memcpy(header + 2, &netFlags, 2);

    uint32_t netStreamId = htonl(streamId);
    memcpy(header + 4, &netStreamId, 4);

    uint32_t netLength = htonl(length);
    memcpy(header + 8, &netLength, 4);

    if (type == FrameType::Data && length > 0 && payload) {
        // header va payload ba ham (yek syscall / yek TLS record)
        struct iovec iov[2];
        iov[0].iov_base = header;
        iov[0].iov_len = HEADER_SIZE;
        iov[1].iov_base = const_cast<uint8_t*>(payload);
        iov[1].iov_len = length;
        return sendv(iov, 2);
    }

    return send(header, HEADER_SIZE);
}

void MultiplexedTunnel::scheduleFlush(Stream *s)
{
    if (s->queued || s->dead)
        return;

    s->queued = true;
    m_flushQueue.push_back(s->id);
}

// ersale safe yek stream ta jaiy ke window, tunnel va sahme nobat ejaze mide
void MultiplexedTunnel::flushStream(Stream *s)
{
    if (s->dead || !s->pendingData)
        return;

    SendQueue* q = s->pendingData;
    size_t sent = 0;

    while (!q->empty() && s->sendWindow > 0 && !m_txBlocked && sent < STREAM_FLUSH_QUANTUM) {
        SendQueue::Chunk* buf = q->front();
        size_t avail = buf->avail();
        if (avail == 0) {
            q->pop_front();
            continue;
        }

        uint32_t chunk = (uint32_t)std::min<size_t>({avail, (size_t)s->sendWindow, (size_t)MAX_FRAME_PAYLOAD});
        if (!sendFrame(FrameType::Data, 0, s->id, chunk, reinterpret_cast<const uint8_t*>(buf->unsent())))
            return;

        q->consume(chunk);
        s->sendWindow -= chunk;
        sent += chunk;
    }

    if (!q->empty()) {
        // agar dalile tavaghof window nabode (tunnel por ya sahme nobat tamam) dobare be saf barmigarde
        if (s->sendWindow > 0)
            scheduleFlush(s);
    } else if (s->localClosed && !s->finSent) {
        sendFin(s);     // momkene stream ro tamam kone
    }

    if (s->dead)
        return;

    if (s->sendBlocked && q->size() <= STREAM_LOW_WATERMARK) {
        s->sendBlocked = false;
        if (s->onResume)
            s->onResume(s->arg, s->id);
    }
}

void MultiplexedTunnel::flushStreams()
{
    if (m_flushing)
        return;

    m_flushing = true;
    {
        DispatchGuard guard(this);

        // round-robin: har stream yek sahm, bad nobat badi
        while (!m_flushQueue.empty() && !m_txBlocked && getStatus() == Connected) {
            uint32_t id = m_flushQueue.front();
            m_flushQueue.pop_front();

            Stream* s = findStream(id);
            if (!s)
                continue;

            s->queued = false;
            if (!s->dead)
                flushStream(s);
        }
    }
    m_flushing = false;
}

// stream tamam shod. hazf az map ta payane dispatch feli aghab miofte.
void MultiplexedTunnel::finishStream(Stream *s, bool notify)
{
    if (s->dead)
        return;

    s->dead = true;
    if (m_liveStreams > 0)
        m_liveStreams--;
    m_deadStreams.push_back(s->id);

    if (s->pendingData) {
        delete s->pendingData;
        s->pendingData = nullptr;
    }

    if (notify && s->onClose)
        s->onClose(s->arg, s->id);

    if (m_dispatchDepth == 0)
        purgeDeadStreams();
}

void MultiplexedTunnel::closeAllStreams(bool notify)
{
    DispatchGuard guard(this);

    std::vector<uint32_t> ids;
    ids.reserve(m_streams.size());
    for (const auto& pair : m_streams) {
        if (!pair.second.dead)
            ids.push_back(pair.first);
    }

    for (uint32_t id : ids) {
        Stream* s = findStream(id);
        if (s && !s->dead)
            finishStream(s, notify);
    }

    m_flushQueue.clear();
}

void MultiplexedTunnel::purgeDeadStreams()
{
    if (m_deadStreams.empty())
        return;

    for (uint32_t id : m_deadStreams) {
        auto it = m_streams.find(id);
        if (it != m_streams.end() && it->second.dead)
            m_streams.erase(it);
    }
    m_deadStreams.clear();
}

// --- TCPSocket hooks ---

void MultiplexedTunnel::handleOnConnected()
{
    startKeepAlive();
    TCPSocket::handleOnConnected();
}

void MultiplexedTunnel::handleOnAccepted()
{
    startKeepAlive();
    TCPSocket::handleOnAccepted();
}

void MultiplexedTunnel::handleOnClose()
{
    stopKeepAlive();

    // hame stream ha ghabl az etela be app baste mishan
    closeAllStreams(true);

    TCPSocket::handleOnClose();
}

// safe ersal socket tunnel por shod: Data frame jadid ferestade nemishe va data
// dar safe har stream mimone (va az onja be app backpressure dade mishe).
// khandan az tunnel motevaghef NEMISHE chon window update haye tarafe moghabel lazem hastan.
void MultiplexedTunnel::handleOnPause()
{
    m_txBlocked = true;
}

void MultiplexedTunnel::handleOnResume()
{
    m_txBlocked = false;
    flushStreams();
}

// parser: header 12 byte jam mishe va payload-e Data frame ha bedone copy va
// bedone buffer kardane kole frame mostaghim be stream tahvil dade mishe
// (yek Data frame mitone ta 256KB bashe va dar chand recv berese).
void MultiplexedTunnel::handleOnData(const uint8_t* data, size_t len) {
    DispatchGuard guard(this);

    size_t pos = 0;
    while (pos < len && getStatus() == Connected) {

        // edame payload frame feli
        if (m_rxRemaining > 0) {
            size_t n = std::min<size_t>(len - pos, m_rxRemaining);
            deliverData(data + pos, n);
            pos += n;
            m_rxRemaining -= (uint32_t)n;

            if (m_rxRemaining == 0 && getStatus() == Connected)
                finishDataFrame();
            continue;
        }

        // header
        const uint8_t* header;
        if (m_rxHeaderLen == 0 && len - pos >= HEADER_SIZE) {
            header = data + pos;    // halate mamool: header kamel dar hamin buffer
            pos += HEADER_SIZE;
        } else {
            size_t c = std::min<size_t>(HEADER_SIZE - m_rxHeaderLen, len - pos);
            memcpy(m_rxHeader + m_rxHeaderLen, data + pos, c);
            m_rxHeaderLen += c;
            pos += c;

            if (m_rxHeaderLen < HEADER_SIZE)
                break;  // header naghes; montazere data bishtar

            m_rxHeaderLen = 0;
            header = m_rxHeader;
        }

        if (!processHeader(header))
            break;
    }
}

bool MultiplexedTunnel::processHeader(const uint8_t *header)
{
    if (header[0] != YAMUX_VERSION) {
        protocolError("bad version");
        return false;
    }

    uint8_t type = header[1];

    uint16_t netFlags;
    memcpy(&netFlags, header + 2, 2);
    uint16_t flags = ntohs(netFlags);

    uint32_t netStreamId;
    memcpy(&netStreamId, header + 4, 4);
    uint32_t streamId = ntohl(netStreamId);

    uint32_t netLength;
    memcpy(&netLength, header + 8, 4);
    uint32_t length = ntohl(netLength);

    switch (static_cast<FrameType>(type)) {
    case FrameType::Data: {
        if (streamId == 0) {
            protocolError("data on stream 0");
            return false;
        }

        m_rxStreamId = streamId;
        m_rxFlags = flags;
        m_rxRemaining = length;
        m_rxDiscard = true;

        Stream* s = findStream(streamId);
        if (flags & FrameFlags::SYN) {
            if (s) {
                protocolError("duplicate stream");
                return false;
            }
            s = acceptStream(streamId);
            if (getStatus() != Connected)
                return false;
        }

        // frame baraye stream nashenakhte (masalan bad az RST ma): payload dor rikhte mishe
        if (s && !s->dead) {
            if (flags & FrameFlags::ACK)
                s->established = true;

            if (length > s->recvWindow) {
                protocolError("receive window exceeded");
                return false;
            }

            s->recvWindow -= length;
            m_rxDiscard = s->remoteClosed;  // data bad az FIN nadide gerefte mishe
        }

        if (length == 0)
            finishDataFrame();
        break;
    }

    case FrameType::WindowUpdate:
        handleWindowUpdateFrame(streamId, length, flags); // Length is Delta
        break;

    case FrameType::Ping:
        handlePingFrame(streamId, length, flags); // Length is Value
        break;

    case FrameType::GoAway:
        handleGoAwayFrame(length); // Length is Code
        break;

    default:
        protocolError("unknown frame type");
        return false;
    }

    return getStatus() == Connected;
}

// stream jadid az tarafe moghabel (SYN)
MultiplexedTunnel::Stream *MultiplexedTunnel::acceptStream(uint32_t streamId)
{
    // ID haye tarafe moghabel: agar ma client hastim zoj, vagarna fard
    bool isRemoteIdValid = m_isClient ? (streamId % 2 == 0) : (streamId % 2 == 1);
    if (streamId == 0 || !isRemoteIdValid) {
        protocolError("invalid stream id");
        return nullptr;
    }

    if (m_localGoAway || !m_onNewStream) {
        // stream jadid ghabul nemikonim
        sendFrame(FrameType::WindowUpdate, FrameFlags::RST, streamId, 0);
        return nullptr;
    }

    Stream& s = m_streams[streamId];
    s.id = streamId;
    s.established = true;
    m_liveStreams++;

    // ACK ghabl az callback (app momkene dar callback bela faseleh data befreste)
    sendFrame(FrameType::WindowUpdate, FrameFlags::ACK, streamId, 0);

    m_onNewStream(m_newStreamArg, streamId, &s);

    return s.dead ? nullptr : &s;
}

void MultiplexedTunnel::deliverData(const uint8_t *data, size_t len)
{
    if (m_rxDiscard)
        return;

    Stream* s = findStream(m_rxStreamId);
    if (!s || s->dead)
        return;

    if (s->onData)
        s->onData(s->arg, s->id, data, len);

    // callback momkene stream ro baste bashe (pointer ta payane dispatch motabar mimone)
    if (s->dead)
        return;

    // data be app tahvil shod; window faghat agar stream pause nabashe pas dade mishe
    s->unackedRecvBytes += (uint32_t)len;
    maybeSendWindowUpdate(s);
}

// payload-e Data frame kamel shod: flag haye payani (FIN/RST) emal mishan
void MultiplexedTunnel::finishDataFrame()
{
    uint16_t flags = m_rxFlags;
    m_rxFlags = 0;

    if (!(flags & (FrameFlags::FIN | FrameFlags::RST)))
        return;

    Stream* s = getStream(m_rxStreamId);
    if (!s)
        return;

    if (flags & FrameFlags::RST)
        finishStream(s, true);
    else
        remoteFin(s);
}

void MultiplexedTunnel::remoteFin(Stream *s)
{
    if (s->remoteClosed || s->dead)
        return;

    s->remoteClosed = true;

    if (s->onRemoteFin) {
        // half-close: app tasmim migire key closeStream bezane
        s->onRemoteFin(s->arg, s->id);
        if (s->dead)
            return;
    } else if (!s->localClosed) {
        closeStream(s->id, false);
        if (s->dead)
            return;
    }

    // har do taraf FIN dadan
    if (s->finSent)
        finishStream(s, true);
}

void MultiplexedTunnel::handleWindowUpdateFrame(uint32_t streamId, uint32_t delta, uint16_t flags) {
    if (streamId == 0)
        return; // Must not be for Session

    Stream* s = findStream(streamId);

    if (flags & FrameFlags::SYN) {
        if (s) {
            protocolError("duplicate stream");
            return;
        }
        s = acceptStream(streamId);
    }

    if (!s || s->dead)
        return; // stream nashenakhte: nadide gerefte mishe

    if (flags & FrameFlags::ACK)
        s->established = true;

    if (delta > 0) {
        // Delta is in the length field in Yamux
        uint32_t room = UINT32_MAX - s->sendWindow;
        s->sendWindow += std::min(delta, room);

        if (s->pendingData && !s->pendingData->empty()) {
            scheduleFlush(s);
            flushStreams();
        }

        if (s->dead)
            return;
    }

    if (flags & FrameFlags::RST) {
        finishStream(s, true);
        return;
    }

    if (flags & FrameFlags::FIN)
        remoteFin(s);
}


void MultiplexedTunnel::handlePingFrame(uint32_t /*streamId*/, uint32_t value, uint16_t flags) {
    if (flags & FrameFlags::SYN) {
        // darkhast: haman meghdar ba ACK bargardande mishe
        sendPing(true, value);
        return;
    }

    if ((flags & FrameFlags::ACK) && value == m_pingValue)
        m_pingOutstanding = false;
}

void MultiplexedTunnel::handleGoAwayFrame(uint32_t code) {
    if (code == static_cast<uint32_t>(GoAwayCode::Normal)) {
        // stream haye feli edame midan; faghat stream jadid baz nemishe
        m_remoteGoAway = true;
        onGoAway(code);
        return;
    }

    fprintf(stderr, "yamux: GoAway received with error code %u, closing tunnel fd=%d\n", code, fd());
    close(true);    // handleOnClose hame stream ha ro mibande
}
