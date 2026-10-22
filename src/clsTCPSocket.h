#ifndef CLSTCPSOCKET_H
#define CLSTCPSOCKET_H
//
#include "epoll.h"
#include <functional>
#include <memory>
#include <sys/epoll.h>
#include <sys/uio.h>
#include <string>
#include <vector>
#include "SocketContext.h"
#include "clsDNSLookup.h"
#include "clsReactorObject.h"

// forward declaration (BoringSSL)
typedef struct ssl_st SSL;

class Server;
class EpollReactor;
class TLSContext;

// ============================== TCPSocket ====================================
// Malekiyat va omr object:
//   - object bayad ba new sakhte beshe. bad az accept ya connectTo() == true malek
//     an reactor hast va bad az baste shodan khodesh delete mishe (deleteLater).
//   - onClose daghighan yek bar va be onvane akharin callback seda zade mishe.
//     bad az an dige be object dast nazanid.
//   - agar socket ozve yek class dige hast (composition) setAutoDelete(false) bezanid;
//     dar in halat malek bayad bad az onClose va na dar haman callback delete kone.
//   - hame method ha faghat az thread haman reactor seda zade beshan
//     (az thread dige: EpollReactor::post).
class TCPSocket : public ReactorObject
{
public:
    TCPSocket();
    enum socketStatus{
        Ready = 1,
        Closed = 2,
        Closing = 3,        // dar hale khali kardane safe ersal ya montazere FIN tarafe moghabel
        Connecting = 4,     // DNS ya TCP connect
        Connected = 5,
        Handshaking = 6     // TLS handshake (ham accept ham connect)
    };

    // dalil-e baste shodan (dar onClose ba closeReason() va getErrorCode() ghabele khandan)
    enum CloseReason {
        NotClosed = 0,
        ClosedByApp = 1,        // close() tavasote app
        ClosedByPeer = 2,       // FIN / RST az tarafe moghabel
        ClosedByError = 3,      // khataye I/O ya TLS (getErrorCode)
        ClosedByTimeout = 4,    // connect/handshake/idle timeout
        ConnectFailed = 5       // connect ya DNS movafagh nabod (onConnectFailed ham seda zade shode)
    };

    void setReactor(EpollReactor* r);

    using OnDataFn = void(*)(void* , const uint8_t* data, size_t len);
    //using OnConnectingFn = void(*)(void* routin, void* p);
    using OnConnectingFn = void(*)(void* p);

    using OnAcceptedFn = void(*)(void*);
    using OnCloseFn = void(*)(void*);
    using OnConnectedFn = void(*)(void*);
    using OnConnectFailedFn = void(*)(void*);

    using OnPauseFn = void(*)(void* p);
    using OnResumeFn = void(*)(void* p);
    using OnEOFFn = void(*)(void* p);


    void setOnData(OnDataFn fn, void *Arg);
    void setOnConnecting(OnConnectingFn fn, void* Arg);
    void setOnConnected(OnConnectedFn fn, void *Arg);

    void setOnAccepted(OnAcceptedFn fn, void* Arg);
    void setOnClose(OnCloseFn fn, void* Arg);
    void setOnConnectFailed(OnConnectFailedFn fn, void* Arg);

    // onPause: safe ersal in socket az BACK_PRESSURE bishtar shod (dige be in socket data nadid)
    // onResume: safe ersal be LOW_WATERMARK resid
    void setOnPause(OnPauseFn fn, void* Arg);
    void setOnResume(OnResumeFn fn, void* Arg);

    // faghat ba setAllowHalfClose(true): tarafe moghabel FIN ferestad
    void setOnEOF(OnEOFFn fn, void* Arg);


    virtual ~TCPSocket();
    virtual void onAccepted() {}                                // (non-hot) ba TLS: bad az handshake
    virtual void onClose() {}                                   // (non-hot) hamishe akharin callback
    virtual void onConnectFailed(){}                            // (non-hot) ghabl az onClose
    virtual void onConnecting(){}
    virtual void onConnected(){}                                // ba TLS: bad az handshake
    virtual void onReceiveData(const uint8_t* Data, size_t len){}
    virtual void onPause();                                     // default: pause_reading()
    virtual void onResume();                                    // default: resume_reading()
    virtual void onReadEOF(){}


    // dalil-e baste shodan va akharin khata (bad az close ham motabar)
    CloseReason closeReason() const { return m_closeReason; }

    // saghf-e sakht-e safe ersal baraye in socket (0 = MAX_SEND_QUEUE). bishtar az an send() rad mishe.
    void setMaxSendQueue(size_t bytes) { m_maxSendQueue = bytes; }

    // app-side send helper (thread-affinity: shard thread)
    // false: socket vasl nist ya khata dade; data ersal nashode.
    // khata bad az in call (na dakhele an) ba onClose etela dade mishe.
    bool send(const void * data, size_t len);
    bool sendv(const struct iovec* iov, int iovcnt);

    // zero-copy: hafeze-ye app mostaghim az saf ersal mishe; release vaghti seda zade mishe
    // ke dige lazem nist (ba close ham). release == nullptr baraye data-ye sabet (static).
    bool sendNoCopy(const void* data, size_t len, SendQueue::ReleaseFn release = nullptr, void* arg = nullptr);

    // sendfile: baze-ye [offset, offset+len) az fileFd (plain TCP: zero-copy az page cache).
    // fileFd bayad ta payan-e ersal baz bemone.
    bool sendFile(int fileFd, off_t offset, size_t len);

    // force=false: aval safe ersal khali mishe, bad FIN, bad close (onClose dar enteha)
    // force=true : hamin alan baste mishe va safe ersal dor rikhte mishe
    void close(bool force = false);

    // half-close: bad az khali shodan safe ersal FIN ferestade mishe, khandan edame dare
    void shutdownWrite();

    // true: natije (movafagh ya na) ba callback ha miad, hata momkene ghabl az bargasht.
    // false: hich callback seda zade nashode va object hanooz male caller hast.
    bool connectTo(const char *host, uint16_t port);

    // ---- TLS (BoringSSL) ----
    // ghabl az connectTo() ya dakhele accept callback seda zade beshe.
    // serverName (ekhtiari, client): baraye SNI va check certificate; default haman host-e connectTo
    bool enableTLS(TLSContext* ctx, const char* serverName = nullptr);
    bool isTLS() const;
    TLSContext* tlsContext() const { return m_tlsCtx; }
    const char* tlsVersion() const;
    std::string tlsALPN() const;


    // accessors
    int fd() const;
    TCPSocket *getPointer();
    uint64_t getLastActiveTime();
    uint16_t getLocalPort();
    bool getPeerAddress(std::string& ip, uint16_t& port) const;
    size_t pendingBytes() const;
    bool isWriteBlocked() const;
    bool isAccepted() const;

    bool adoptFd(int fd);

    static void setSocketOption(int fd, int name, bool isEnable);
    static void setSocketShared(int fd, bool isEnable);
    static int setSocketNonblocking(int fd);
    static void setSocketResourceAddress(int fd, bool isEnable);
    static void setSocketNoDelay(int fd, bool isEnable);
    static void setSocketCork(int fd, bool isEnable);
    static void setSocketKeepAlive(int fd, bool isEnable);
    static void setSocketLowDelay(int fd, bool isEnable);


    SocketContext *getSocketContext() ;

    void pause_reading();
    void resume_reading();
    bool isReadPaused() const;
    int getErrorCode();

    // false (default): ba daryaft FIN socket baste mishe (bad az khali shodan safe ersal)
    // true: onReadEOF seda zade mishe va socket baraye neveshtan baz mimone
    void setAllowHalfClose(bool enable);

    // false: reactor bad az close object ro delete nemikone
    void setAutoDelete(bool enable);

    // 0 = gheyre faal. connection haye Connected bedone faaliyat baste mishan
    void setIdleTimeout(uint32_t seconds);

    socketStatus getStatus() const;
    EpollReactor *getReactor() const;

    // ---- dakheli (faghat reactor va TLS) ----
    friend class EpollReactor;
    friend class TLSKeyWorkers;
    friend struct TLSKeyMethod;

protected:
    OnDataFn m_onData { nullptr };
    OnAcceptedFn m_onAccepted { nullptr };
    OnCloseFn m_onClose { nullptr };
    OnConnectFailedFn m_onConnectFailed { nullptr };
    OnConnectingFn m_onConnecting { nullptr };
    OnConnectedFn m_onConnected { nullptr };

    OnPauseFn m_onPause { nullptr };
    OnResumeFn m_onResume { nullptr };
    OnEOFFn m_onEOF { nullptr };

    // yek argument moshtarak baraye hame-ye callback ha (akharin setOn* barande ast).
    // tarahi-ye amdi: callback ha ye yek socket hamishe be yek object (owner) taalogh daran.
    void* m_callbacksArg { nullptr };

    struct SocketContext m_SocketContext {}; // composition with low-level TCP
    void setStatus(socketStatus newStatus);

    // dispatcher ha: aval callback (agar set shode) vagarna tabe virtual.
    // layer haye protocol (mesle MultiplexedTunnel) in ha ro override mikonan
    // ta mostaghel az callback haye app kar konan.
    virtual void handleOnData(const uint8_t * d, size_t n);
    virtual void handleOnAccepted();
    virtual void handleOnClose();
    virtual void handleOnConnectFailed();
    virtual void handleOnConnecting();
    virtual void handleOnConnected();
    virtual void handleOnPause();
    virtual void handleOnResume();
    virtual void handleOnEOF();

    // bastan ejbari dar entehaye dor feli halghe (bedone ejraye onClose vasate tabe feli)
    void abortLater();

private:
    // hot path entry — called by shard (friend)
    void handleEvents(uint32_t ev);
    void onDeferred(int kind);
    void checkTimeout(uint64_t now);
    void onReadable();
    void onWritable();
    void _accepted(int fd);

    // amaliat-e async private key (TLSContext::enableAsyncPrivateKey)
    bool tlsStartKeyJob(uint16_t sigalg, const uint8_t* in, size_t len);
    struct TLSKeyJob* tlsKeyJob() const { return m_tlsJob.get(); }
    void tlsClearKeyJob() { m_tlsJob.reset(); }
    void tlsKeyJobDone(struct TLSKeyJob* job);

    EpollReactor* m_pReactor = nullptr;
    socketStatus status {Ready};
    CloseReason m_closeReason { NotClosed };
    size_t m_maxSendQueue { 0 };

    bool m_readPaused { false };
    bool m_pendingClose { false };      // close(false): bad az khali shodan saf baste beshe
    bool m_lingering { false };         // FIN ferestade shode, montazere FIN tarafe moghabel
    bool m_readEOF { false };           // FIN daryaft shode
    bool m_shutdownWrPending { false }; // shutdownWrite() seda zade shode
    bool m_writeShutdown { false };     // FIN ferestade shode
    bool m_writeBlocked { false };      // saf az BACK_PRESSURE gozashte (onPause seda zade shode)
    bool m_abortPending { false };      // khata dar send(); close dar entehaye dor anjam mishe
    bool m_deferredRead { false };
    bool m_eofPending { false };        // event-e RDHUP/HUP dide shode; ta EOF bekhon
    bool m_registered { false };        // fd dar epoll sabt shode
    bool m_isAccepted { false };
    bool m_autoDelete { true };
    bool m_allowHalfClose { false };
    bool m_dnsPending { false };
    bool m_dnsTriedV6 { false };
    bool m_connectFailNotified { false };
    bool m_tlsReadWantsWrite { false };
    bool m_tlsWriteWantsRead { false };

    int m_lastError { 0 };
    uint32_t m_idleTimeout { 0 };

    TLSContext* m_tlsCtx { nullptr };
    SSL* m_ssl { nullptr };
    std::shared_ptr<struct TLSKeyJob> m_tlsJob;

    // faghat baraye connection haye khoroji / TLS ba esm (connection haye accept shode in ro nadaran)
    struct ConnectInfo {
        std::string host;
        std::string serverName;
        std::vector<std::string> addrs;     // javab haye DNS (ta 8 ta)
        size_t nextAddr { 0 };
    };
    ConnectInfo* m_connInfo { nullptr };
    ConnectInfo& connInfo();

    void updateLastActive();

    // connect
    static void connect_cb(const char *hostname, char **ips, size_t count, DNSLookup::QUERY_TYPE qtype, void *p);
    void onResolved(char **ips, size_t count, DNSLookup::QUERY_TYPE qtype);
    void startConnect(const struct sockaddr* addr, socklen_t addrLen);
    void onConnectReady();
    void failConnect(int err);
    bool tryNextAddress(int err);

    // close
    void finalizeClose();
    void drainComplete();
    void doShutdownWrite();
    void lingerDiscard();
    void handleReadEOF();

    // io (plain ya TLS)
    ssize_t ioRead(char* buf, size_t cap);
    ssize_t ioWrite(const void* data, size_t len);
    bool flushQueue();
    bool queueHasRoom(size_t len);
    int socketError();

    // TLS
    bool tlsStart(bool isServer);
    void tlsHandshake();
};



#endif // CLSTCPSOCKET_H
