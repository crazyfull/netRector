#ifndef SOCKS5STREAMHANDLER_H
#define SOCKS5STREAMHANDLER_H

// Test server (C++): har stream-e Yamux yek darkhast SOCKS5 CONNECT miare.
// In class khodesh socket-e ertebat ba maghsad hast (az TCPSocket ers mibare),
// pas omresh ro reactor modiriyat mikone: bad az onClose khodkar delete mishe.
//
//   stream --(05 01 00 ATYP ADDR PORT)--> connect be maghsad --(javab 10 byte)--> relay

#include "TestCommon.h"
#include "clsEpollReactor.h"
#include "clsTCPSocket.h"
#include "clsMultiplexedTunnel.h"

class Socks5StreamHandler : public TCPSocket {
public:
    Socks5StreamHandler(MultiplexedTunnel* tunnel, uint32_t streamId, MultiplexedTunnel::Stream* stream)
        : m_tunnel(tunnel), m_streamId(streamId)
    {
        setReactor(tunnel->getReactor());
        setAllowHalfClose(true);

        stream->arg = this;
        stream->onData = &Socks5StreamHandler::streamData;
        stream->onClose = &Socks5StreamHandler::streamClosed;
        stream->onRemoteFin = &Socks5StreamHandler::streamRemoteFin;
        stream->onPause = &Socks5StreamHandler::streamPause;
        stream->onResume = &Socks5StreamHandler::streamResume;
    }

    // --- TCPSocket (ertebat ba maghsad) ---

    void onConnected() override {
        VLOG("[server] stream %u connected to destination fd=%d", m_streamId, fd());
        m_state = State::Relay;
        if (m_streamId == 0)
            return;

        sendReply(socks5::REP_OK);

        // data-i ke client ghabl az vasl shodan ferestade
        if (!m_buffer.empty()) {
            send(m_buffer.data(), m_buffer.size());
            std::string().swap(m_buffer);
        }

        if (m_streamFin)
            shutdownWrite();

        if (m_streamId != 0 && !isWriteBlocked())
            m_tunnel->resumeStream(m_streamId);
    }

    void onConnectFailed() override {
        VLOG("[server] stream %u connect failed: %s", m_streamId, strerror(getErrorCode()));
        if (m_streamId == 0)
            return;

        m_state = State::Failed;
        sendReply(socks5::replyFromErrno(getErrorCode()));
        // onClose bela faseleh bad az in miad va stream ro joda mikone (FIN)
    }

    void onReceiveData(const uint8_t* data, size_t len) override {
        if (m_streamId != 0)
            m_tunnel->sendToStream(m_streamId, data, len);
    }

    // maghsad dige chizi nemiferese
    void onReadEOF() override {
        if (m_streamId != 0 && !m_sentFin) {
            m_sentFin = true;
            m_tunnel->closeStream(m_streamId, false);
        }
    }

    // safe ersal be maghsad por shod: window-e stream pas dade nashe
    void onPause() override {
        if (m_streamId != 0)
            m_tunnel->pauseStream(m_streamId);
    }

    void onResume() override {
        if (m_streamId != 0)
            m_tunnel->resumeStream(m_streamId);
    }

    void onClose() override {
        if (m_streamId != 0) {
            uint32_t id = m_streamId;
            m_streamId = 0;

            if (m_state == State::Failed || (m_sentFin && m_streamFin)) {
                // javab-e khata ferestade shode ya har do jahat tamam shodan:
                // stream khodesh ba FIN baste mishe
                m_tunnel->detachStream(id);
            } else {
                // maghsad ghat kard: RST
                m_tunnel->closeStream(id, true);
            }
        }
    }

private:
    enum class State { WaitRequest, Connecting, Relay, Failed };

    MultiplexedTunnel* m_tunnel;
    uint32_t m_streamId;
    State m_state = State::WaitRequest;
    std::string m_buffer;       // darkhast-e naghes ya data-e ghabl az vasl shodan
    bool m_streamFin = false;   // FIN az client (stream) daryaft shode
    bool m_sentFin = false;     // FIN be stream ferestade shode

    void sendReply(uint8_t rep) {
        uint8_t reply[socks5::REPLY_SIZE];
        socks5::buildReply(rep, reply);
        m_tunnel->sendToStream(m_streamId, reply, sizeof(reply));
    }

    void handleRequest() {
        socks5::Request req;
        auto r = socks5::parseRequest(reinterpret_cast<const uint8_t*>(m_buffer.data()), m_buffer.size(), req);
        if (r == socks5::Incomplete)
            return;

        if (r == socks5::Invalid) {
            m_state = State::Failed;
            sendReply(req.error);
            close();    // -> onClose -> detachStream (FIN)
            return;
        }

        m_buffer.erase(0, req.length);
        m_state = State::Connecting;

        // ta vasl shodan be maghsad window pas dade nemishe, pas client
        // hadeaksar be andaze yek window (256KB) data-e zood-hengam miferese
        m_tunnel->pauseStream(m_streamId);

        VLOG("[server] stream %u: connecting to %s:%u", m_streamId, req.host.c_str(), req.port);
        if (!connectTo(req.host.c_str(), req.port)) {
            // false: hich callback-i seda zade nashode
            m_state = State::Failed;
            sendReply(socks5::REP_GENERAL_FAILURE);
            close();
        }
        // true: natije ba onConnected / onConnectFailed miad (momkene hamin alan omade bashe)
    }

    // --- callback haye stream ---

    static void streamData(void* p, uint32_t /*id*/, const uint8_t* data, size_t len) {
        Socks5StreamHandler* self = static_cast<Socks5StreamHandler*>(p);
        switch (self->m_state) {
        case State::WaitRequest:
            self->m_buffer.append(reinterpret_cast<const char*>(data), len);
            self->handleRequest();
            break;

        case State::Connecting:
            self->m_buffer.append(reinterpret_cast<const char*>(data), len);
            break;

        case State::Relay:
            self->send(data, len);
            break;

        case State::Failed:
            break;
        }
    }

    static void streamRemoteFin(void* p, uint32_t /*id*/) {
        Socks5StreamHandler* self = static_cast<Socks5StreamHandler*>(p);
        self->m_streamFin = true;

        switch (self->m_state) {
        case State::Relay:
            self->shutdownWrite();  // FIN be maghsad bad az khali shodan saf
            break;

        case State::WaitRequest:
            self->close();          // client ghabl az darkhast-e kamel tamam kard
            break;

        case State::Connecting:     // dar onConnected emal mishe
        case State::Failed:
            break;
        }
    }

    static void streamClosed(void* p, uint32_t id) {
        Socks5StreamHandler* self = static_cast<Socks5StreamHandler*>(p);
        VLOG("[server] stream %u closed", id);
        self->m_streamId = 0;

        // payan-e adi: data haye dar saf be maghsad ferestade mishan. RST / ghat-e tunnel: fori
        self->close(!self->m_streamFin);
    }

    // safe ersal-e stream por shod: az maghsad kamtar bekhon
    static void streamPause(void* p, uint32_t /*id*/) {
        static_cast<Socks5StreamHandler*>(p)->pause_reading();
    }

    static void streamResume(void* p, uint32_t /*id*/) {
        static_cast<Socks5StreamHandler*>(p)->resume_reading();
    }
};

// Ertebat paye-e tunnel dar samte server
class TunnelServerEndpoint : public MultiplexedTunnel {
public:
    explicit TunnelServerEndpoint(const TestConfig* cfg) : MultiplexedTunnel(false) {
        setOnNewStream(&TunnelServerEndpoint::handleNewStream, this);
        setKeepAlive(cfg->keepAlive);
    }

    void onAccepted() override {
        std::string ip;
        uint16_t port = 0;
        getPeerAddress(ip, port);
        LOG("[server] tunnel accepted fd=%d from %s:%u %s", fd(), ip.c_str(), port, isTLS() ? tlsVersion() : "plain");
    }

    void onClose() override {
        LOG("[server] tunnel closed");
    }

private:
    static void handleNewStream(void* arg, uint32_t streamId, MultiplexedTunnel::Stream* newStream) {
        TunnelServerEndpoint* self = static_cast<TunnelServerEndpoint*>(arg);
        VLOG("[server] new stream %u", streamId);
        new Socks5StreamHandler(self, streamId, newStream);  // malek: reactor (bad az onClose)
    }
};

#endif // SOCKS5STREAMHANDLER_H
