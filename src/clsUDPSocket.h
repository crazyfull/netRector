#ifndef CLSUDPSOCKET_H
#define CLSUDPSOCKET_H
#include "epoll.h"
#include "SocketContext.h"
#include "clsReactorObject.h"
#include <sys/socket.h>
#include <netinet/in.h>

class EpollReactor;

// ============================== UDPSocket ====================================
// Socket UDP gheyre masdood roye reactor. Daryaft ba recvmmsg (chand datagram dar
// yek syscall) va tahvil har datagram ba callback ya tabe virtual.
//
//   UDPSocket* u = new MyUDP;  u->setReactor(reactor);  u->bind("0.0.0.0", 5353);
//   ... onDatagram(data, len, from, fromLen) ...
//   u->sendTo(buf, n, from, fromLen);  u->close();   // bad az onClose reactor delete mikone
//
// Hamoon ghavaed-e TCPSocket: object ba new, faghat az thread reactor, setAutoDelete.
class UDPSocket : public ReactorObject {
public:
    using OnDatagramFn = void(*)(void* arg, const uint8_t* data, size_t len, const struct sockaddr* from, socklen_t fromLen);
    using OnCloseFn = void(*)(void* arg);

    UDPSocket();
    virtual ~UDPSocket();

    void setReactor(EpollReactor* r);
    EpollReactor* getReactor() const { return m_pReactor; }

    // socket sakhte va bind mishe (ip: IPv4 ya IPv6; nullptr/"" = hame IPv4). port 0 = tasadofi.
    // reusePort: chand reactor mitonan haman port ro bind konan (kernel bar ro pakhsh mikone)
    bool bind(const char* ip, uint16_t port, bool reusePort = false);

    // socket bedone bind (client). family: AF_INET / AF_INET6
    bool open(int family = AF_INET);

    // maghsad-e default baraye send() (connect roye UDP faghat filter/adres-e default hast)
    bool connect(const char* ip, uint16_t port);

    // ersal-e fori. false: kernel buffer por bod (datagram dor rikhte shod) ya khata.
    bool sendTo(const void* data, size_t len, const struct sockaddr* to, socklen_t toLen);
    bool send(const void* data, size_t len);   // faghat bad az connect()

    void close();

    // hadeaksar andaze-ye har datagram (default 2048; ta 65535). datagram haye bozorgtar borida mishan.
    void setMaxDatagramSize(size_t bytes);

    void setOnDatagram(OnDatagramFn fn, void* arg) { m_onDatagram = fn; m_callbackArg = arg; }
    void setOnClose(OnCloseFn fn, void* arg) { m_onClose = fn; m_callbackArg = arg; }
    void setAutoDelete(bool enable) { m_autoDelete = enable; }

    virtual void onDatagram(const uint8_t* /*data*/, size_t /*len*/, const struct sockaddr* /*from*/, socklen_t /*fromLen*/) {}
    virtual void onClose() {}

    int fd() const { return m_SocketContext.fd; }
    bool isOpen() const { return m_SocketContext.fd != -1; }
    uint16_t getLocalPort() const;
    int getErrorCode() const { return m_lastError; }
    SocketContext* getSocketContext() { return &m_SocketContext; }

    // hot path: az reactor seda zade mishe
    void handleEvents(uint32_t ev);
    void onDeferred(int kind);

private:
    EpollReactor* m_pReactor { nullptr };
    SocketContext m_SocketContext {};
    OnDatagramFn m_onDatagram { nullptr };
    OnCloseFn m_onClose { nullptr };
    void* m_callbackArg { nullptr };
    size_t m_maxDatagram { 2048 };
    int m_lastError { 0 };
    bool m_registered { false };
    bool m_autoDelete { true };
    bool m_closed { false };
    bool m_deferredRead { false };

    bool create(int family);
    bool registerWithReactor();
    void readAll();
    void finalizeClose();
};

#endif // CLSUDPSOCKET_H
