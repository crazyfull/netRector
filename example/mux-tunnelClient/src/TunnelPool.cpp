#include "TunnelPool.h"

#include "ClientTunnel.h"

#include "clsLog.h"

#include <algorithm>

using namespace netrector;

TunnelPool::TunnelPool(EpollReactor& reactor, const ClientConfig& config,
                       const WebSocketClientHandshake& handshake, size_t tunnelCount)
    : m_reactor(reactor)
    , m_config(config)
    , m_handshake(handshake)
    , m_slots(tunnelCount)
{
    for (TunnelSlot& slot : m_slots)
        slot.reconnectDelayMs = DefaultClientConfig::ReconnectInitialDelayMs;
}

void TunnelPool::start()
{
    for (size_t index = 0; index < m_slots.size(); index++)
        connectSlot(index);
}

void TunnelPool::stop()
{
    m_stopped = true;
}

uint32_t TunnelPool::openStream(MultiplexedTunnel::OnStreamDataFn dataFunction,
                                MultiplexedTunnel::OnStreamCloseFn closeFunction,
                                void* argument, MultiplexedTunnel*& tunnel)
{
    const size_t count = m_slots.size();

    for (size_t attempt = 0; attempt < count; attempt++) {
        const size_t index = (m_nextSlot + attempt) % count;
        TunnelSlot& slot = m_slots[index];
        if (!slot.ready)
            continue;

        const uint32_t streamId = slot.tunnel->openStream(dataFunction, closeFunction, argument);
        if (streamId != 0) {
            NR_LOG(LOG_DEBUG, "[client] stream %u assigned to tunnel %zu", streamId, index);
            m_nextSlot = (index + 1) % count;
            tunnel = slot.tunnel;
            return streamId;
        }
        // this tunnel is full or going away: try the next one
    }

    return 0;
}

void TunnelPool::onTunnelReady(ClientTunnel& tunnel)
{
    size_t index = 0;
    TunnelSlot* slot = findSlot(tunnel, index);
    if (!slot)
        return;

    slot->ready = true;
    slot->reconnectDelayMs = DefaultClientConfig::ReconnectInitialDelayMs;
}

void TunnelPool::onTunnelClosed(ClientTunnel& tunnel)
{
    size_t index = 0;
    TunnelSlot* slot = findSlot(tunnel, index);
    if (!slot)
        return;     // a tunnel that was already replaced after a GoAway

    NR_LOG(LOG_INFO, "[client] tunnel %zu lost", index);
    slot->tunnel = nullptr;
    slot->ready = false;
    scheduleReconnect(index);
}

void TunnelPool::onTunnelGoAway(ClientTunnel& tunnel)
{
    size_t index = 0;
    TunnelSlot* slot = findSlot(tunnel, index);
    if (!slot)
        return;

    // The server accepts no new streams on this tunnel. It keeps running until its streams end
    // and is deleted by the reactor when it closes; meanwhile a replacement is connected.
    NR_LOG(LOG_INFO, "[client] tunnel %zu received GoAway, replacing it", index);
    slot->tunnel = nullptr;
    slot->ready = false;
    scheduleReconnect(index);
}

void TunnelPool::connectSlot(size_t index)
{
    TunnelSlot& slot = m_slots[index];
    if (m_stopped || slot.tunnel)
        return;

    ClientTunnel* tunnel = new ClientTunnel(*this, m_config, m_handshake);
    tunnel->setReactor(&m_reactor);
    // To use TLS later: tunnel->enableTLS(context, serverName) goes here, before connectTo().

    slot.tunnel = tunnel;
    slot.ready = false;

    NR_LOG(LOG_DEBUG, "[client] connecting tunnel %zu to %s:%u", index, m_config.cdnAddress.c_str(), m_config.cdnPort);
    if (!tunnel->connectTo(m_config.cdnAddress.c_str(), m_config.cdnPort)) {
        // false: no callback was or will be called, the object is still ours
        slot.tunnel = nullptr;
        delete tunnel;
        scheduleReconnect(index);
    }
    // true: the result arrives through the tunnel callbacks (possibly before connectTo returns)
}

void TunnelPool::scheduleReconnect(size_t index)
{
    TunnelSlot& slot = m_slots[index];
    if (m_stopped || slot.reconnectScheduled || slot.tunnel)
        return;

    slot.reconnectScheduled = true;
    const int delay = slot.reconnectDelayMs;
    NR_LOG(LOG_DEBUG, "[client] reconnecting tunnel %zu in %d ms", index, delay);

    m_reactor.addTimer(delay, [this, index]() {
        m_slots[index].reconnectScheduled = false;
        connectSlot(index);
    }, true);

    slot.reconnectDelayMs = std::min(delay * 2, DefaultClientConfig::ReconnectMaxDelayMs);
}

TunnelSlot* TunnelPool::findSlot(ClientTunnel& tunnel, size_t& index)
{
    for (size_t i = 0; i < m_slots.size(); i++) {
        if (m_slots[i].tunnel == &tunnel) {
            index = i;
            return &m_slots[i];
        }
    }
    return nullptr;
}
