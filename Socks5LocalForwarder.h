#ifndef SOCKS5LOCALFORWARDER_H
#define SOCKS5LOCALFORWARDER_H

// Test client: yek connection SOCKS5 mahali (masalan az browser) ke roye yek
// stream-e Yamux be server ferestade mishe.
//
//   browser <--TCP--> Socks5LocalForwarder <--stream--> tunnel <--> server <--TCP--> maghsad
//
// Greeting-e SOCKS5 hamin ja javab dade mishe; darkhast-e CONNECT bedone taghir
// dakhele stream ferestade mishe va javab 10 byte-i server be browser barmigarde.

#include "TestCommon.h"
#include "TunnelClient.h"
#include "clsTCPSocket.h"
#include "clsMultiplexedTunnel.h"

class Socks5LocalForwarder : public TCPSocket {
public:
    Socks5LocalForwarder() = default;

    // --- TCPSocket ---

    void onAccepted() override {
        // FIN-e browser be FIN-e stream tabdil mishe (va bar aks)
        setAllowHalfClose(true);
        m_client = static_cast<TunnelClient*>(getReactor()->userData());
        VLOG("[client] local connection accepted fd=%d", fd());
    }

    void onReceiveData(const uint8_t* data, size_t length) override {
        if (m_state == State::Relay) {
            if (m_streamId != 0)
                m_tunnel->sendToStream(m_streamId, data, length);
            return;
        }

        m_buffer.append(reinterpret_cast<const char*>(data), length);

        if (m_state == State::Greeting && !processGreeting())
            return;

        if (m_state == State::Request)
            processRequest();
    }

    // browser dige chizi nemiferese
    void onReadEOF() override {
        if (m_streamId != 0) {
            if (!m_sentFin) {
                m_sentFin = true;
                m_tunnel->closeStream(m_streamId, false);   // FIN; daryaft edame dare
            }
        } else {
            close();
        }
    }

    // safe ersal be browser por shod: az stream kamtar begir
    void onPause() override {
        if (m_streamId != 0)
            m_tunnel->pauseStream(m_streamId);
    }

    void onResume() override {
        if (m_streamId != 0)
            m_tunnel->resumeStream(m_streamId);
    }

    void onClose() override {
        VLOG("[client] local connection closed (stream %u)", m_streamId);
        if (m_streamId != 0) {
            uint32_t id = m_streamId;
            m_streamId = 0;

            if (m_sentFin && m_remoteFin) {
                // har do jahat tamam shode; stream khodesh baste mishe
                m_tunnel->detachStream(id);
            } else {
                // browser ghat kard: RST (bad az in hich callback-i az stream nemiad)
                m_tunnel->closeStream(id, true);
            }
        }
        // bad az onClose reactor in object ro delete mikone
    }

private:
    enum class State { Greeting, Request, Relay };

    TunnelClient* m_client = nullptr;
    MultiplexedTunnel* m_tunnel = nullptr;
    uint32_t m_streamId = 0;
    State m_state = State::Greeting;
    std::string m_buffer;       // faghat baraye marhale handshake
    bool m_sentFin = false;     // FIN be stream ferestade shode
    bool m_remoteFin = false;   // FIN az stream daryaft shode

    bool processGreeting() {
        size_t consumed = 0;
        bool noAuth = false;
        auto r = socks5::parseGreeting(reinterpret_cast<const uint8_t*>(m_buffer.data()), m_buffer.size(), consumed, noAuth);
        if (r == socks5::Incomplete)
            return false;

        if (r == socks5::Invalid) {
            close(true);
            return false;
        }

        m_buffer.erase(0, consumed);

        uint8_t response[2] = {0x05, static_cast<uint8_t>(noAuth ? 0x00 : 0xFF)};
        send(response, 2);

        if (!noAuth) {
            close();    // javab ferestade mishe va bad baste mishe
            return false;
        }

        m_state = State::Request;
        return true;
    }

    void processRequest() {
        socks5::Request req;
        auto r = socks5::parseRequest(reinterpret_cast<const uint8_t*>(m_buffer.data()), m_buffer.size(), req);
        if (r == socks5::Incomplete)
            return;

        if (r == socks5::Invalid) {
            sendReplyAndClose(req.error);
            return;
        }

        MultiplexedTunnel* tunnel = m_client ? m_client->tunnel() : nullptr;
        if (!tunnel) {
            VLOG("[client] tunnel is not connected; rejecting %s:%u", req.host.c_str(), req.port);
            sendReplyAndClose(socks5::REP_GENERAL_FAILURE);
            return;
        }

        uint32_t id = tunnel->openStream(&Socks5LocalForwarder::streamData, &Socks5LocalForwarder::streamClosed, this);
        if (id == 0) {
            sendReplyAndClose(socks5::REP_GENERAL_FAILURE);
            return;
        }

        if (MultiplexedTunnel::Stream* s = tunnel->getStream(id)) {
            s->onRemoteFin = &Socks5LocalForwarder::streamRemoteFin;
            s->onPause = &Socks5LocalForwarder::streamPause;
            s->onResume = &Socks5LocalForwarder::streamResume;
        }

        m_tunnel = tunnel;
        m_streamId = id;
        m_state = State::Relay;
        VLOG("[client] stream %u -> %s:%u", id, req.host.c_str(), req.port);

        // darkhast (va har data-i ke browser poshte saresh ferestade) dakhele stream
        std::string pending;
        pending.swap(m_buffer);
        tunnel->sendToStream(id, reinterpret_cast<const uint8_t*>(pending.data()), pending.size());
    }

    void sendReplyAndClose(uint8_t rep) {
        uint8_t reply[socks5::REPLY_SIZE];
        socks5::buildReply(rep, reply);
        send(reply, sizeof(reply));
        close();
    }

    // --- callback haye stream ---

    static void streamData(void* p, uint32_t /*id*/, const uint8_t* data, size_t len) {
        // avalin 10 byte javab-e SOCKS5 server hast va baghie data-e maghsad; hame bedone taghir
        static_cast<Socks5LocalForwarder*>(p)->send(data, len);
    }

    static void streamRemoteFin(void* p, uint32_t /*id*/) {
        Socks5LocalForwarder* self = static_cast<Socks5LocalForwarder*>(p);
        self->m_remoteFin = true;
        self->shutdownWrite();  // bad az ersale data haye dar saf, FIN be browser
    }

    static void streamClosed(void* p, uint32_t id) {
        Socks5LocalForwarder* self = static_cast<Socks5LocalForwarder*>(p);
        VLOG("[client] stream %u closed", id);
        self->m_streamId = 0;
        self->m_tunnel = nullptr;

        // payan-e adi (FIN az server): data haye dar saf ferestade mishan.
        // RST ya ghat shodane tunnel: bastan-e fori.
        self->close(!self->m_remoteFin);
    }

    // safe ersal-e stream por shod: az browser kamtar bekhon
    static void streamPause(void* p, uint32_t /*id*/) {
        static_cast<Socks5LocalForwarder*>(p)->pause_reading();
    }

    static void streamResume(void* p, uint32_t /*id*/) {
        static_cast<Socks5LocalForwarder*>(p)->resume_reading();
    }
};

#endif // SOCKS5LOCALFORWARDER_H
