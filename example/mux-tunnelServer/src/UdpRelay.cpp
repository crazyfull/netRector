#include "UdpRelay.h"

#include "Socks5Reply.h"

#include "clsLog.h"

#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <utility>

using namespace netrector;

namespace {

constexpr uint8_t AddressTypeIPv4 = 0x01;
constexpr uint8_t AddressTypeDomain = 0x03;
constexpr uint8_t AddressTypeIPv6 = 0x04;

// Downstream datagrams are dropped when this much is already queued for the client.
constexpr size_t MaxQueuedDownstreamBytes = 256 * 1024;
// Idle check period (seconds); the idle limit is accurate to about this value.
constexpr int IdleCheckMs = 5000;

bool isPrivateIPv4(uint32_t host)    // host byte order
{
    const uint8_t a = static_cast<uint8_t>(host >> 24);
    const uint8_t b = static_cast<uint8_t>(host >> 16);
    return a == 0 || a == 10 || a == 127
        || (a == 100 && (b & 0xC0) == 64)               // 100.64.0.0/10 carrier-grade NAT
        || (a == 169 && b == 254)                       // link-local, cloud metadata
        || (a == 172 && (b & 0xF0) == 16)
        || (a == 192 && b == 168)
        || (a == 192 && b == 0 && static_cast<uint8_t>(host >> 8) == 0)   // 192.0.0.0/24
        || a >= 224;                                    // multicast, reserved, broadcast
}

} // namespace

UdpRelay::UdpRelay(MultiplexedTunnel* tunnel, uint32_t streamId, const ServerConfig& config,
                   ServerStats& stats, std::shared_ptr<TunnelStats> tunnelStats)
    : m_tunnel(tunnel)
    , m_streamId(streamId)
    , m_config(config)
    , m_stats(stats)
    , m_tunnelStats(std::move(tunnelStats))
{
    m_stats.udpStreamOpened(*m_tunnelStats);
    m_lastActivity = std::chrono::steady_clock::now();
}

UdpRelay::~UdpRelay()
{
    m_stats.udpStreamClosed(*m_tunnelStats);
}

bool UdpRelay::isAllowedDestination(const sockaddr* address, bool allowPrivate)
{
    if (address->sa_family == AF_INET) {
        const sockaddr_in* in = reinterpret_cast<const sockaddr_in*>(address);
        if (in->sin_port == 0)
            return false;
        return allowPrivate || !isPrivateIPv4(ntohl(in->sin_addr.s_addr));
    }

    if (address->sa_family == AF_INET6) {
        const sockaddr_in6* in6 = reinterpret_cast<const sockaddr_in6*>(address);
        if (in6->sin6_port == 0)
            return false;
        if (allowPrivate)
            return true;

        const uint8_t* a = in6->sin6_addr.s6_addr;
        if (IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr)) {
            uint32_t v4;
            std::memcpy(&v4, a + 12, 4);
            return !isPrivateIPv4(ntohl(v4));
        }
        return !(IN6_IS_ADDR_UNSPECIFIED(&in6->sin6_addr) || IN6_IS_ADDR_LOOPBACK(&in6->sin6_addr)
                 || (a[0] & 0xFE) == 0xFC            // fc00::/7 unique local
                 || (a[0] == 0xFE && (a[1] & 0xC0) == 0x80)   // fe80::/10 link-local
                 || a[0] == 0xFF);                   // multicast
    }

    return false;
}

void UdpRelay::start(const std::string& pending)
{
    MultiplexedTunnel::Stream* stream = m_tunnel->getStream(m_streamId);
    if (!stream) {
        finish(false);
        return;
    }

    stream->arg = this;
    stream->onData = &UdpRelay::onStreamData;
    stream->onClose = &UdpRelay::onStreamClose;
    stream->onRemoteFin = &UdpRelay::onStreamRemoteFin;
    stream->onPause = &UdpRelay::onStreamPause;
    stream->onResume = &UdpRelay::onStreamResume;
    m_paused = stream->sendBlocked;

    uint8_t reply[Socks5Reply::Size];
    Socks5Reply::build(Socks5ReplyCode::Succeeded, reply);
    m_tunnel->sendToStream(m_streamId, reply, sizeof(reply));

    if (m_config.udpIdleSeconds > 0 && m_tunnel->getReactor()) {
        m_idleTimer = m_tunnel->getReactor()->addTimer(IdleCheckMs, [this]() { checkIdle(); });
    }

    NR_LOG(LOG_DEBUG, "[server] stream %u: UDP relay started", m_streamId);

    if (!pending.empty())
        consume(reinterpret_cast<const uint8_t*>(pending.data()), pending.size());
}

// ---- upstream: frames from the client ----

void UdpRelay::consume(const uint8_t* data, size_t length)
{
    m_rx.append(reinterpret_cast<const char*>(data), length);

    size_t offset = 0;
    while (!m_finished) {
        const size_t available = m_rx.size() - offset;
        if (available < 4)
            break;

        const uint8_t* frame = reinterpret_cast<const uint8_t*>(m_rx.data()) + offset;
        const size_t dataLength = (static_cast<size_t>(frame[0]) << 8) | frame[1];
        const size_t headerLength = frame[2];       // 3 + ATYP + ADDR + PORT
        const uint8_t addressType = frame[3];

        size_t expectedAddress = 0;
        if (addressType == AddressTypeIPv4) {
            expectedAddress = 1 + 4 + 2;
        } else if (addressType == AddressTypeIPv6) {
            expectedAddress = 1 + 16 + 2;
        } else if (addressType == AddressTypeDomain) {
            if (available < 5)
                break;
            expectedAddress = 1 + 1 + frame[4] + 2;
        } else {
            NR_LOG(LOG_DEBUG, "[server] stream %u: UDP frame with bad address type %u", m_streamId, addressType);
            finish(false);
            return;
        }

        if (headerLength != 3 + expectedAddress) {
            NR_LOG(LOG_DEBUG, "[server] stream %u: UDP frame with bad header length", m_streamId);
            finish(false);
            return;
        }

        if (available < headerLength + dataLength)
            break;

        handleFrame(frame + 3, expectedAddress, frame + headerLength, dataLength);
        offset += headerLength + dataLength;
        if (m_finished)
            return;
    }

    if (offset > 0)
        m_rx.erase(0, offset);
}

void UdpRelay::handleFrame(const uint8_t* address, size_t addressLength, const uint8_t* payload, size_t payloadLength)
{
    m_lastActivity = std::chrono::steady_clock::now();

    if (payloadLength == 0 || payloadLength > m_config.udpMaxDatagram) {
        m_stats.udpDropped();
        return;
    }

    sockaddr_storage destination;
    std::memset(&destination, 0, sizeof(destination));
    socklen_t destinationLength = 0;

    if (address[0] == AddressTypeIPv4) {
        sockaddr_in* in = reinterpret_cast<sockaddr_in*>(&destination);
        in->sin_family = AF_INET;
        std::memcpy(&in->sin_addr, address + 1, 4);
        std::memcpy(&in->sin_port, address + 5, 2);
        destinationLength = sizeof(sockaddr_in);
    } else if (address[0] == AddressTypeIPv6) {
        sockaddr_in6* in6 = reinterpret_cast<sockaddr_in6*>(&destination);
        in6->sin6_family = AF_INET6;
        std::memcpy(&in6->sin6_addr, address + 1, 16);
        std::memcpy(&in6->sin6_port, address + 17, 2);
        destinationLength = sizeof(sockaddr_in6);
    } else {
        // domain names are not resolved here: the apps send IP addresses
        m_stats.udpDropped();
        return;
    }
    (void)addressLength;

    if (!isAllowedDestination(reinterpret_cast<sockaddr*>(&destination), m_config.udpAllowPrivate)) {
        m_stats.udpDropped();
        return;
    }

    UDPSocket* socket = socketFor(destination.ss_family);
    if (!socket || !socket->sendTo(payload, payloadLength, reinterpret_cast<sockaddr*>(&destination), destinationLength)) {
        m_stats.udpDropped();
        return;
    }

    m_stats.udpUp(payloadLength);
}

UDPSocket* UdpRelay::socketFor(int family)
{
    UDPSocket*& slot = family == AF_INET6 ? m_socket6 : m_socket4;
    if (slot)
        return slot;

    UDPSocket* socket = new UDPSocket;
    socket->setReactor(m_tunnel->getReactor());
    socket->setMaxDatagramSize(m_config.udpMaxDatagram);
    socket->setOnDatagram(&UdpRelay::onDatagram, this);
    socket->setOnClose(&UdpRelay::onSocketClose, this);
    if (!socket->open(family)) {
        NR_LOG(LOG_DEBUG, "[server] stream %u: cannot open a UDP socket (family %d): %s", m_streamId, family, strerror(socket->getErrorCode()));
        socket->setOnClose(nullptr, nullptr);
        delete socket;
        return nullptr;
    }

    slot = socket;
    return slot;
}

// ---- downstream: datagrams from the Internet ----

void UdpRelay::onDatagram(void* argument, const uint8_t* data, size_t length, const sockaddr* from, socklen_t /*fromLength*/)
{
    UdpRelay* self = static_cast<UdpRelay*>(argument);
    if (self->m_finished || self->m_streamId == 0)
        return;

    if (length == 0 || self->m_paused
        || self->m_tunnel->streamPendingBytes(self->m_streamId) > MaxQueuedDownstreamBytes) {
        self->m_stats.udpDropped();
        return;
    }

    uint8_t address[1 + 16 + 2];
    size_t addressLength = 0;
    if (from->sa_family == AF_INET) {
        const sockaddr_in* in = reinterpret_cast<const sockaddr_in*>(from);
        address[0] = AddressTypeIPv4;
        std::memcpy(address + 1, &in->sin_addr, 4);
        std::memcpy(address + 5, &in->sin_port, 2);
        addressLength = 7;
    } else if (from->sa_family == AF_INET6) {
        const sockaddr_in6* in6 = reinterpret_cast<const sockaddr_in6*>(from);
        if (IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr)) {
            address[0] = AddressTypeIPv4;
            std::memcpy(address + 1, in6->sin6_addr.s6_addr + 12, 4);
            std::memcpy(address + 5, &in6->sin6_port, 2);
            addressLength = 7;
        } else {
            address[0] = AddressTypeIPv6;
            std::memcpy(address + 1, &in6->sin6_addr, 16);
            std::memcpy(address + 17, &in6->sin6_port, 2);
            addressLength = 19;
        }
    } else {
        return;
    }

    self->m_lastActivity = std::chrono::steady_clock::now();

    std::string& frame = self->m_tx;
    frame.resize(3 + addressLength + length);
    uint8_t* out = reinterpret_cast<uint8_t*>(&frame[0]);
    out[0] = static_cast<uint8_t>(length >> 8);
    out[1] = static_cast<uint8_t>(length);
    out[2] = static_cast<uint8_t>(3 + addressLength);
    std::memcpy(out + 3, address, addressLength);
    std::memcpy(out + 3 + addressLength, data, length);

    // one frame in one call: the mux keeps the order, and TCP_NODELAY of the tunnel sends it at once
    if (self->m_tunnel->sendToStream(self->m_streamId, out, frame.size()))
        self->m_stats.udpDown(length);
    else
        self->m_stats.udpDropped();
}

void UdpRelay::onSocketClose(void* argument)
{
    // the reactor deletes the socket after this callback
    UdpRelay* self = static_cast<UdpRelay*>(argument);
    // only reached when the socket closed by itself; finish() clears the callback before closing
    self->m_socket4 = nullptr;
    self->m_socket6 = nullptr;
}

// ---- idle / end of life ----

void UdpRelay::checkIdle()
{
    if (m_finished || m_config.udpIdleSeconds == 0)
        return;

    const auto idle = std::chrono::steady_clock::now() - m_lastActivity;
    if (idle >= std::chrono::seconds(m_config.udpIdleSeconds)) {
        NR_LOG(LOG_DEBUG, "[server] stream %u: UDP stream idle, closing", m_streamId);
        finish(true);
    }
}

void UdpRelay::finish(bool detachStream)
{
    if (m_finished)
        return;
    m_finished = true;

    EpollReactor* reactor = m_tunnel ? m_tunnel->getReactor() : nullptr;

    if (m_idleTimer != -1 && reactor) {
        reactor->removeTimer(m_idleTimer);
        m_idleTimer = -1;
    }

    for (UDPSocket** slot : { &m_socket4, &m_socket6 }) {
        if (*slot) {
            UDPSocket* socket = *slot;
            *slot = nullptr;
            socket->setOnDatagram(nullptr, nullptr);
            socket->setOnClose(nullptr, nullptr);
            socket->close();        // the reactor deletes it
        }
    }

    if (m_streamId != 0) {
        const uint32_t streamId = m_streamId;
        m_streamId = 0;
        if (detachStream)
            m_tunnel->detachStream(streamId);       // FIN, the tunnel finishes the stream
        else
            m_tunnel->closeStream(streamId, true);  // protocol error / tunnel problem: RST
    }

    if (reactor)
        reactor->deleteLater(this);
    else
        delete this;
}

// ---- stream callbacks (run on the tunnel's reactor thread, argument is the UdpRelay) ----

void UdpRelay::onStreamData(void* argument, uint32_t /*streamId*/, const uint8_t* data, size_t length)
{
    static_cast<UdpRelay*>(argument)->consume(data, length);
}

void UdpRelay::onStreamRemoteFin(void* argument, uint32_t /*streamId*/)
{
    // the client closed its UDP session
    static_cast<UdpRelay*>(argument)->finish(true);
}

void UdpRelay::onStreamClose(void* argument, uint32_t /*streamId*/)
{
    UdpRelay* self = static_cast<UdpRelay*>(argument);
    self->m_streamId = 0;       // the stream is gone: nothing to detach
    self->finish(false);
}

void UdpRelay::onStreamPause(void* argument, uint32_t /*streamId*/)
{
    static_cast<UdpRelay*>(argument)->m_paused = true;
}

void UdpRelay::onStreamResume(void* argument, uint32_t /*streamId*/)
{
    static_cast<UdpRelay*>(argument)->m_paused = false;
}
