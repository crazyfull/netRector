#ifndef SOCKS5DIRECTPROXY_H
#define SOCKS5DIRECTPROXY_H

// Test: proxy SOCKS5 mostaghim (bedone tunnel) baraye test-e khode TCPSocket:
// accept, connect, DNS, backpressure va half-close.
//
// Har taraf yek object-e mostaghel hast ke reactor bad az onClose delete mikone.
// Har kodam ghabl az az beyn raftan pointer-e tarafe moghabel ro pak mikone.

#include "TestCommon.h"
#include "clsEpollReactor.h"
#include "clsTCPSocket.h"

class Socks5Session;

// ertebat ba maghsad
class Socks5Outbound : public TCPSocket {
public:
    explicit Socks5Outbound(Socks5Session* session) : m_session(session) {}

    void detach() { m_session = nullptr; }

    void onConnected() override;
    void onConnectFailed() override;
    void onReceiveData(const uint8_t* data, size_t len) override;
    void onReadEOF() override;
    void onPause() override;
    void onResume() override;
    void onClose() override;

private:
    Socks5Session* m_session;
};

// ertebat ba client (accept shode)
class Socks5Session : public TCPSocket {
public:
    void onAccepted() override {
        setAllowHalfClose(true);
        VLOG("[socks5] accepted fd=%d", fd());
    }

    void onReceiveData(const uint8_t* data, size_t len) override {
        if (m_state == State::Relay) {
            if (m_peer)
                m_peer->send(data, len);
            return;
        }

        m_buffer.append(reinterpret_cast<const char*>(data), len);

        if (m_state == State::Greeting && !processGreeting())
            return;

        if (m_state == State::Request)
            processRequest();
        // State::Connecting: data dar m_buffer mimone
    }

    void onReadEOF() override {
        m_gotEOF = true;
        if (m_state == State::Relay && m_peer)
            m_peer->shutdownWrite();
        else if (m_state != State::Connecting)
            close();
    }

    // safe ersal be client por shod: az maghsad kamtar bekhon
    void onPause() override { if (m_peer) m_peer->pause_reading(); }
    void onResume() override { if (m_peer) m_peer->resume_reading(); }

    void onClose() override {
        if (m_peer) {
            Socks5Outbound* peer = m_peer;
            m_peer = nullptr;
            peer->detach();
            peer->close();      // data haye dar saf ferestade mishan
        }
    }

    // --- az tarafe Socks5Outbound ---

    void peerConnected() {
        m_state = State::Relay;
        sendReply(socks5::REP_OK);

        if (!m_buffer.empty()) {
            m_peer->send(m_buffer.data(), m_buffer.size());
            std::string().swap(m_buffer);
        }

        if (m_gotEOF)
            m_peer->shutdownWrite();

        resume_reading();
    }

    void peerConnectFailed(int err) {
        m_peer = nullptr;   // maghsad khodesh baste mishe
        sendReply(socks5::replyFromErrno(err));
        close();
    }

    void peerClosed() {
        m_peer = nullptr;
        close();            // data haye dar saf be client ferestade mishan
    }

private:
    enum class State { Greeting, Request, Connecting, Relay };

    Socks5Outbound* m_peer = nullptr;
    State m_state = State::Greeting;
    std::string m_buffer;
    bool m_gotEOF = false;

    void sendReply(uint8_t rep) {
        uint8_t reply[socks5::REPLY_SIZE];
        socks5::buildReply(rep, reply);
        send(reply, sizeof(reply));
    }

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
            close();
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
            sendReply(req.error);
            close();
            return;
        }

        m_buffer.erase(0, req.length);
        m_state = State::Connecting;

        // ta vasl shodan be maghsad az client nemikhonim (data-e zood-hengam mahdod mimone)
        pause_reading();

        VLOG("[socks5] connect to %s:%u", req.host.c_str(), req.port);
        Socks5Outbound* out = new Socks5Outbound(this);
        out->setReactor(getReactor());
        out->setAllowHalfClose(true);
        m_peer = out;

        if (!out->connectTo(req.host.c_str(), req.port)) {
            // false: hich callback-i seda zade nashode va object male ma hast
            m_peer = nullptr;
            delete out;
            sendReply(socks5::REP_GENERAL_FAILURE);
            close();
        }
    }
};

inline void Socks5Outbound::onConnected()
{
    if (m_session)
        m_session->peerConnected();
    else
        close(true);
}

inline void Socks5Outbound::onConnectFailed()
{
    if (m_session) {
        Socks5Session* s = m_session;
        m_session = nullptr;
        s->peerConnectFailed(getErrorCode());
    }
}

inline void Socks5Outbound::onReceiveData(const uint8_t *data, size_t len)
{
    if (m_session)
        m_session->send(data, len);
}

inline void Socks5Outbound::onReadEOF()
{
    if (m_session)
        m_session->shutdownWrite();
}

// safe ersal be maghsad por shod: az client kamtar bekhon
inline void Socks5Outbound::onPause()
{
    if (m_session)
        m_session->pause_reading();
}

inline void Socks5Outbound::onResume()
{
    if (m_session)
        m_session->resume_reading();
}

inline void Socks5Outbound::onClose()
{
    if (m_session) {
        Socks5Session* s = m_session;
        m_session = nullptr;
        s->peerClosed();
    }
}

#endif // SOCKS5DIRECTPROXY_H
