#ifndef TUNNELCLIENT_H
#define TUNNELCLIENT_H

// Test client: ertebat paye-e tunnel (Yamux roye TCP ya TLS) be server.
// Har reactor (shard) tunnel-e khodesh ro dare chon object haye ketabkhane
// faghat az thread haman reactor ghabele estefade hastan.

#include "TestCommon.h"
#include "clsEpollReactor.h"
#include "clsMultiplexedTunnel.h"
#include "clsTLSContext.h"

class TunnelClient;

class TunnelClientEndpoint : public MultiplexedTunnel {
public:
    explicit TunnelClientEndpoint(TunnelClient* owner) : MultiplexedTunnel(true), m_owner(owner) {}

    void onConnected() override;
    void onConnectFailed() override;
    void onClose() override;

protected:
    void onGoAway(uint32_t code) override;

private:
    TunnelClient* m_owner;
};

// Modiriyat tunnel-e yek reactor: vasl shodan va vasl-e mojadad
class TunnelClient {
public:
    TunnelClient(EpollReactor* reactor, const TestConfig* cfg) : m_reactor(reactor), m_cfg(cfg) {}

    // bayad roye thread reactor seda zade beshe (ya ghabl az Server::start)
    void connect()
    {
        if (m_stopped || m_connecting || m_tunnel)
            return;

        TunnelClientEndpoint* t = new TunnelClientEndpoint(this);
        t->setReactor(m_reactor);
        t->setKeepAlive(m_cfg->keepAlive);
        if (m_cfg->tlsCtx)
            t->enableTLS(m_cfg->tlsCtx, m_cfg->sni.empty() ? nullptr : m_cfg->sni.c_str());

        m_connecting = true;
        LOG("[client:%d] connecting to tunnel server %s:%u%s", m_reactor->id(), m_cfg->serverHost.c_str(), m_cfg->serverPort, m_cfg->tlsCtx ? " (TLS)" : "");

        // natije ba callback ha miad (momkene ghabl az bargasht-e connectTo)
        if (!t->connectTo(m_cfg->serverHost.c_str(), m_cfg->serverPort)) {
            delete t;   // false: object hanooz male ma hast
            m_connecting = false;
            scheduleReconnect();
        }
    }

    // tunnel-e amade ya nullptr
    MultiplexedTunnel* tunnel() const { return m_tunnel; }
    EpollReactor* reactor() const { return m_reactor; }
    void stop() { m_stopped = true; }

    void onTunnelUp(TunnelClientEndpoint* t)
    {
        m_connecting = false;
        m_tunnel = t;
        m_backoffMs = 500;
        LOG("[client:%d] tunnel established fd=%d %s", m_reactor->id(), t->fd(), t->isTLS() ? t->tlsVersion() : "plain");
    }

    void onTunnelDown(TunnelClientEndpoint* t)
    {
        m_connecting = false;
        if (m_tunnel == t) {
            m_tunnel = nullptr;
            LOG("[client:%d] tunnel lost", m_reactor->id());
        }
        scheduleReconnect();
    }

    void onTunnelGoAway(TunnelClientEndpoint* t)
    {
        // server dige stream jadid ghabul nemikone: tunnel jadid
        if (m_tunnel == t) {
            m_tunnel = nullptr;
            LOG("[client:%d] server sent GoAway; opening a new tunnel", m_reactor->id());
            connect();
        }
    }

private:
    EpollReactor* m_reactor;
    const TestConfig* m_cfg;
    MultiplexedTunnel* m_tunnel = nullptr;
    bool m_connecting = false;
    bool m_stopped = false;
    bool m_reconnectScheduled = false;
    int m_backoffMs = 500;

    void scheduleReconnect()
    {
        if (m_stopped || m_reconnectScheduled || m_tunnel || m_connecting)
            return;

        m_reconnectScheduled = true;
        VLOG("[client:%d] reconnect in %d ms", m_reactor->id(), m_backoffMs);
        m_reactor->addTimer(m_backoffMs, [this]() {
            m_reconnectScheduled = false;
            connect();
        }, true);

        m_backoffMs = std::min(m_backoffMs * 2, 10000);
    }
};

inline void TunnelClientEndpoint::onConnected()
{
    m_owner->onTunnelUp(this);
}

inline void TunnelClientEndpoint::onConnectFailed()
{
    LOG("[client:%d] tunnel connect failed: %s", getReactor()->id(), strerror(getErrorCode()));
}

inline void TunnelClientEndpoint::onClose()
{
    // stream ha ghablan tavasote MultiplexedTunnel baste shodan (onClose-e har stream)
    m_owner->onTunnelDown(this);
}

inline void TunnelClientEndpoint::onGoAway(uint32_t /*code*/)
{
    m_owner->onTunnelGoAway(this);
}

#endif // TUNNELCLIENT_H
