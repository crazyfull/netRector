#include "clsUDPSocket.h"
#include "clsEpollReactor.h"
#include "clsSocketList.h"
#include "clsTCPSocket.h"

namespace {
constexpr int MAX_UDP_BATCH = 64;        // hadeaksar datagram dar har recvmmsg
constexpr int MAX_UDP_ROUNDS = 16;       // edalat beyne socket ha
}

UDPSocket::UDPSocket() {}

UDPSocket::~UDPSocket() {
    if (m_SocketContext.fd != -1) {
        if (m_pReactor && m_registered)
            m_pReactor->del_fd(m_SocketContext.fd, true);
        ::close(m_SocketContext.fd);
        m_SocketContext.fd = -1;
    }
}

void UDPSocket::setReactor(EpollReactor* r) {
    m_pReactor = r;
}

void UDPSocket::setMaxDatagramSize(size_t bytes)
{
    if (bytes < 64) bytes = 64;
    if (bytes > 65535) bytes = 65535;
    m_maxDatagram = bytes;
}

bool UDPSocket::create(int family)
{
    if (m_SocketContext.fd != -1 || m_closed)
        return false;

    int fd = ::socket(family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_UDP);
    if (fd < 0) {
        m_lastError = errno;
        return false;
    }

    m_SocketContext.fd = fd;
    return true;
}

bool UDPSocket::registerWithReactor()
{
    if (!m_pReactor || m_registered)
        return m_registered;

    m_SocketContext.ev.events = EPOLLIN | EPOLLET;
    if (!m_pReactor->register_fd(m_SocketContext.fd, &m_SocketContext.ev, IS_UDP_SOCKET, this)) {
        m_lastError = errno;
        return false;
    }
    m_registered = true;
    return true;
}

bool UDPSocket::open(int family)
{
    if (!m_pReactor)
        return false;

    if (!create(family))
        return false;

    if (!registerWithReactor()) {
        ::close(m_SocketContext.fd);
        m_SocketContext.fd = -1;
        return false;
    }
    return true;
}

bool UDPSocket::bind(const char* ip, uint16_t port, bool reusePort)
{
    if (!m_pReactor)
        return false;

    sockaddr_storage addr;
    memset(&addr, 0, sizeof(addr));
    socklen_t len = 0;
    int family;

    sockaddr_in* a4 = reinterpret_cast<sockaddr_in*>(&addr);
    sockaddr_in6* a6 = reinterpret_cast<sockaddr_in6*>(&addr);
    if (!ip || !*ip || strcmp(ip, "0.0.0.0") == 0) {
        family = AF_INET;
        a4->sin_family = AF_INET;
        a4->sin_addr.s_addr = INADDR_ANY;
        a4->sin_port = htons(port);
        len = sizeof(sockaddr_in);
    } else if (inet_pton(AF_INET, ip, &a4->sin_addr) == 1) {
        family = AF_INET;
        a4->sin_family = AF_INET;
        a4->sin_port = htons(port);
        len = sizeof(sockaddr_in);
    } else if (inet_pton(AF_INET6, ip, &a6->sin6_addr) == 1) {
        family = AF_INET6;
        a6->sin6_family = AF_INET6;
        a6->sin6_port = htons(port);
        len = sizeof(sockaddr_in6);
    } else {
        m_lastError = EINVAL;
        return false;
    }

    if (!create(family))
        return false;

    TCPSocket::setSocketResourceAddress(m_SocketContext.fd, true);
    if (reusePort)
        TCPSocket::setSocketShared(m_SocketContext.fd, true);

    int rcvbuf = 1024 * 1024; // 1MB
    setsockopt(m_SocketContext.fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    if (::bind(m_SocketContext.fd, reinterpret_cast<sockaddr*>(&addr), len) < 0 || !registerWithReactor()) {
        m_lastError = errno;
        ::close(m_SocketContext.fd);
        m_SocketContext.fd = -1;
        return false;
    }

    m_SocketContext.port = port;
    return true;
}

bool UDPSocket::connect(const char* ip, uint16_t port)
{
    if (!ip || !*ip)
        return false;

    sockaddr_storage addr;
    memset(&addr, 0, sizeof(addr));
    socklen_t len = 0;
    sockaddr_in* a4 = reinterpret_cast<sockaddr_in*>(&addr);
    sockaddr_in6* a6 = reinterpret_cast<sockaddr_in6*>(&addr);
    int family;
    if (inet_pton(AF_INET, ip, &a4->sin_addr) == 1) {
        family = AF_INET;
        a4->sin_family = AF_INET;
        a4->sin_port = htons(port);
        len = sizeof(sockaddr_in);
    } else if (inet_pton(AF_INET6, ip, &a6->sin6_addr) == 1) {
        family = AF_INET6;
        a6->sin6_family = AF_INET6;
        a6->sin6_port = htons(port);
        len = sizeof(sockaddr_in6);
    } else {
        m_lastError = EINVAL;
        return false;
    }

    if (m_SocketContext.fd == -1 && !open(family))
        return false;

    if (::connect(m_SocketContext.fd, reinterpret_cast<sockaddr*>(&addr), len) < 0) {
        m_lastError = errno;
        return false;
    }
    return true;
}

bool UDPSocket::sendTo(const void* data, size_t len, const struct sockaddr* to, socklen_t toLen)
{
    if (!data || m_SocketContext.fd == -1 || m_closed)
        return false;

    ssize_t n;
    do {
        n = ::sendto(m_SocketContext.fd, data, len, MSG_NOSIGNAL | MSG_DONTWAIT, to, toLen);
    } while (n == -1 && errno == EINTR);

    if (n == -1) {
        // UDP: dar halate por bodane buffer datagram dor rikhte mishe (mesle shabake)
        m_lastError = errno;
        return false;
    }
    return true;
}

bool UDPSocket::send(const void* data, size_t len)
{
    return sendTo(data, len, nullptr, 0);
}

uint16_t UDPSocket::getLocalPort() const
{
    if (m_SocketContext.fd == -1)
        return 0;

    sockaddr_storage local;
    socklen_t len = sizeof(local);
    if (getsockname(m_SocketContext.fd, reinterpret_cast<sockaddr*>(&local), &len) == -1)
        return 0;

    if (local.ss_family == AF_INET6)
        return ntohs(reinterpret_cast<sockaddr_in6*>(&local)->sin6_port);
    return ntohs(reinterpret_cast<sockaddr_in*>(&local)->sin_port);
}

void UDPSocket::handleEvents(uint32_t ev)
{
    if (m_closed || m_SocketContext.fd == -1)
        return;

    if (ev & EPOLLERR) {
        int err = 0;
        socklen_t l = sizeof(err);
        getsockopt(m_SocketContext.fd, SOL_SOCKET, SO_ERROR, &err, &l);
        m_lastError = err;
        // ICMP unreachable roye socket-e connect shode: socket baz mimone, faghat khata sabt mishe
    }

    if (ev & EPOLLIN)
        readAll();
}

// recvmmsg: chand datagram dar yek syscall, hame dar buffer-e moshtarak reactor
void UDPSocket::readAll()
{
    char* buf = m_pReactor->readBuffer();
    size_t cap = m_pReactor->readBufferSize();

    int batch = (int)(cap / m_maxDatagram);
    if (batch < 1) batch = 1;
    if (batch > MAX_UDP_BATCH) batch = MAX_UDP_BATCH;

    struct mmsghdr msgs[MAX_UDP_BATCH];
    struct iovec iovs[MAX_UDP_BATCH];
    sockaddr_storage addrs[MAX_UDP_BATCH];

    for (int round = 0; round < MAX_UDP_ROUNDS; round++) {
        for (int i = 0; i < batch; i++) {
            iovs[i].iov_base = buf + (size_t)i * m_maxDatagram;
            iovs[i].iov_len = m_maxDatagram;
            memset(&msgs[i].msg_hdr, 0, sizeof(msghdr));
            msgs[i].msg_hdr.msg_iov = &iovs[i];
            msgs[i].msg_hdr.msg_iovlen = 1;
            msgs[i].msg_hdr.msg_name = &addrs[i];
            msgs[i].msg_hdr.msg_namelen = sizeof(addrs[i]);
            msgs[i].msg_len = 0;
        }

        int n;
        do {
            n = ::recvmmsg(m_SocketContext.fd, msgs, (unsigned)batch, MSG_DONTWAIT, nullptr);
        } while (n == -1 && errno == EINTR);

        if (n <= 0) {
            if (n == -1 && errno != EAGAIN && errno != EWOULDBLOCK)
                m_lastError = errno;    // masalan ECONNREFUSED roye socket-e connect shode
            return;
        }


        m_SocketContext.lastActive = m_pReactor->getCachedNow();

        for (int i = 0; i < n; i++) {
            const uint8_t* data = reinterpret_cast<const uint8_t*>(iovs[i].iov_base);
            const sockaddr* from = reinterpret_cast<const sockaddr*>(&addrs[i]);
            if (m_onDatagram)
                m_onDatagram(m_callbackArg, data, msgs[i].msg_len, from, msgs[i].msg_hdr.msg_namelen);
            else
                onDatagram(data, msgs[i].msg_len, from, msgs[i].msg_hdr.msg_namelen);

            if (m_closed)
                return;
        }

        // kamtar az batch yani buffer kernel khali shode (EPOLLET: datagram-e badi edge jadid mide)
        if (n < batch)
            return;
    }

    // sahme in nobat tamam shod ama hanooz datagram hast: edame dar entehaye hamin dor
    // (EPOLLET baraye datagram haye ghablan reside edge jadid nemide)
    if (!m_deferredRead) {
        m_deferredRead = true;
        m_pReactor->defer(m_SocketContext.ev.data.u64, 1 /*DEFER_READ*/);
    }
}

void UDPSocket::onDeferred(int /*kind*/)
{
    m_deferredRead = false;
    if (!m_closed && m_SocketContext.fd != -1)
        readAll();
}

void UDPSocket::close()
{
    finalizeClose();
}

void UDPSocket::finalizeClose()
{
    if (m_closed)
        return;
    m_closed = true;

    if (m_SocketContext.fd != -1) {
        if (m_pReactor && m_registered) {
            m_pReactor->unregister_fd(m_SocketContext.fd);
            m_registered = false;
        }
        ::close(m_SocketContext.fd);
        m_SocketContext.fd = -1;
    }

    if (m_onClose)
        m_onClose(m_callbackArg);
    else
        onClose();

    if (m_autoDelete && m_pReactor)
        m_pReactor->deleteLater(this);
}
