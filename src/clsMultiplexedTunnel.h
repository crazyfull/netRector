#ifndef CLSMULTIPLEXEDTUNNEL_H
#define CLSMULTIPLEXEDTUNNEL_H

#include "clsTCPSocket.h"
#include <cstring>      // for memcpy
#include <deque>
#include <unordered_map>
#include <algorithm>    // for std::min

// Yamux Constants (https://github.com/hashicorp/yamux/blob/master/spec.md)
constexpr uint8_t YAMUX_VERSION = 0;
constexpr uint32_t HEADER_SIZE = 12;
constexpr uint32_t INITIAL_WINDOW_SIZE = 256 * 1024;
constexpr uint32_t WINDOW_UPDATE_THRESHOLD = INITIAL_WINDOW_SIZE / 2;   // bad az masrafe nesfe window, update ferestade mishe
constexpr uint32_t MAX_FRAME_PAYLOAD = 32 * 1024;           // hadeaksar payload ersali dar har Data frame
constexpr size_t STREAM_HIGH_WATERMARK = 128 * 1024;        // safe ersal har stream: bishtar az in -> onPause
constexpr size_t STREAM_LOW_WATERMARK = 32 * 1024;          // kamtar az in -> onResume
constexpr size_t STREAM_FLUSH_QUANTUM = 64 * 1024;          // sahme har stream dar har nobat (edalat beyne stream ha)

// Frame types
enum class FrameType : uint8_t {
    Data = 0,
    WindowUpdate = 1,
    Ping = 2,
    GoAway = 3
};

// Flags (16 bits)
enum FrameFlags : uint16_t {
    SYN = 0x01,
    ACK = 0x02,
    FIN = 0x04,
    RST = 0x08
};

// GoAway codes
enum class GoAwayCode : uint32_t {
    Normal = 0,
    ProtocolError = 1,
    InternalError = 2
};

// ============================== MultiplexedTunnel (Yamux) ====================
// Chand stream mostaghel roye yek TCPSocket (plain ya TLS).
//
// Ghavaed callback haye stream:
//   onData      : data daryafti. pointer faghat ta payane callback motabar hast.
//   onRemoteFin : tarafe moghabel FIN ferestad (dige data nemiad) ama ma hanooz
//                 mitonim befrestim. agar set nashe, stream khodkar FIN javab mide.
//                 agar ma ghablan FIN dade bashim bela faseleh bad az an onClose miad.
//   onPause     : safe ersal in stream por shode; dige sendToStream nazanid
//                 (masalan pause_reading roye socket manba).
//   onResume    : safe ersal khali shod.
//   onClose     : stream kamelan tamam shod (FIN az har do taraf, RST az tarafe moghabel,
//                 ya ghat shodane tunnel). daghighan yek bar va akharin callback.
//                 estesna: agar khode app closeStream(id, true) bezane hich callback-i
//                 dige seda zade nemishe.
//
// Dakhele callback ha seda zadane sendToStream/closeStream/openStream mojaz hast.
// Hame method ha faghat az thread reactor-e tunnel.
class MultiplexedTunnel : public TCPSocket {
public:
    // Callbacks definitions
    using OnStreamDataFn = void(*)(void*, uint32_t streamId, const uint8_t* data, size_t len);
    using OnStreamCloseFn = void(*)(void*, uint32_t streamId);
    using OnStreamEventFn = void(*)(void*, uint32_t streamId);

    struct Stream {
        uint32_t id = 0;
        uint32_t sendWindow = INITIAL_WINDOW_SIZE;
        uint32_t recvWindow = INITIAL_WINDOW_SIZE;
        uint32_t unackedRecvBytes = 0;  // masraf shode ama window hanooz pas dade nashode
        bool localClosed = false;       // closeStream() seda zade shode
        bool remoteClosed = false;      // FIN daryaft shode
        bool finSent = false;           // FIN vaghean ferestade shode (bad az khali shodan saf)
        bool established = false;       // ACK daryaft/ersal shode
        bool readPaused = false;        // pauseStream(): window update ferestade nemishe
        bool sendBlocked = false;       // onPause seda zade shode
        bool queued = false;            // dar safe flush hast
        bool dead = false;              // tamam shode; montazere hazf az map

        // safe ersal stream (vaghti window tarafe moghabel ya tunnel por hast)
        SendQueue* pendingData = nullptr;

        OnStreamDataFn onData = nullptr;
        OnStreamCloseFn onClose = nullptr;
        OnStreamEventFn onRemoteFin = nullptr;
        OnStreamEventFn onPause = nullptr;
        OnStreamEventFn onResume = nullptr;
        void* arg = nullptr;

        Stream() = default;
        Stream(const Stream&) = delete;
        Stream& operator=(const Stream&) = delete;

        ~Stream() {
            if (pendingData) {
                delete pendingData;
                pendingData = nullptr;
            }
        }
    };

    // dar in callback faghat field haye callback/arg ro set konid; pointer ro negah nadarid
    using OnNewStreamFn = void(*)(void*, uint32_t streamId, Stream* newStream);


    MultiplexedTunnel(bool isClient = true);
    virtual ~MultiplexedTunnel();

    // API for streams
    // 0: tunnel vasl nist ya GoAway daryaft/ersal shode
    uint32_t openStream(OnStreamDataFn onData, OnStreamCloseFn onClose, void* arg);
    bool sendToStream(uint32_t streamId, const uint8_t* data, size_t len);

    // rst=false: FIN (half-close) bad az ersale data haye dar saf; daryaft edame dare
    // rst=true : RST fori; stream hazf mishe va dige callback-i nadare
    void closeStream(uint32_t streamId, bool rst = false);

    // flow control daryaft: ta zamani ke pause hast window pas dade nemishe,
    // pas tarafe moghabel hadeaksar be andaze window (256KB) dige miferese
    void pauseStream(uint32_t streamId);
    void resumeStream(uint32_t streamId);

    // jodakardane app az stream: callback ha pak mishan va tunnel khodesh stream ro
    // ta payan mibare (FIN miferese va montazere FIN tarafe moghabel mimone).
    // vaghti estefade mishe ke object-e app zoodtar az stream az beyn mire.
    void detachStream(uint32_t streamId);

    // baraye set kardane callback haye ezafe (onRemoteFin/onPause/onResume) bad az openStream.
    // nullptr agar stream vojod nadare. pointer ro negah nadarid.
    Stream* getStream(uint32_t streamId);
    size_t streamPendingBytes(uint32_t streamId);
    size_t streamCount() const;

    //Ping/Pong: isACK=false darkhast (SYN), isACK=true javab (ACK)
    void sendPing(bool isACK, uint32_t value);

    // har intervalSec yek ping; agar ta nobat badi javab nayad tunnel baste mishe. 0 = khamosh
    void setKeepAlive(uint32_t intervalSec);

    // API for connection
    void setOnNewStream(OnNewStreamFn fn, void* arg);

    // GoAway miferese, hame stream ha ro mibande (onClose) va tunnel ro graceful mibande
    void shutdown(GoAwayCode code = GoAwayCode::Normal);

    bool isClient() const { return m_isClient; }
    bool remoteGoAway() const { return m_remoteGoAway; }

protected:
    // tarafe moghabel GoAway(Normal) ferestad: stream jadid nemishe baz kard
    virtual void onGoAway(uint32_t /*code*/) {}

    // layer protocol: in dispatcher ha mostaghel az callback haye app kar mikonan
    void handleOnData(const uint8_t* data, size_t len) override;
    void handleOnClose() override;
    void handleOnPause() override;
    void handleOnResume() override;
    void handleOnConnected() override;
    void handleOnAccepted() override;

private:
    bool m_isClient;
    uint32_t m_nextStreamId;
    std::unordered_map<uint32_t, Stream> m_streams;
    size_t m_liveStreams = 0;
    OnNewStreamFn m_onNewStream = nullptr;
    void* m_newStreamArg = nullptr;

    // vaziat parser (frame ha mitonan beyne chand recv taghsim shode bashan)
    uint8_t m_rxHeader[HEADER_SIZE];
    size_t m_rxHeaderLen = 0;
    uint32_t m_rxRemaining = 0;     // baghimande payload-e Data frame feli
    uint32_t m_rxStreamId = 0;
    uint16_t m_rxFlags = 0;
    bool m_rxDiscard = false;

    // ersal
    bool m_txBlocked = false;       // safe ersal socket tunnel por hast
    bool m_flushing = false;
    std::deque<uint32_t> m_flushQueue;

    // hazf ba takhir: stream ha hargez vasate callback az map hazf nemishan
    int m_dispatchDepth = 0;
    std::vector<uint32_t> m_deadStreams;

    bool m_localGoAway = false;
    bool m_remoteGoAway = false;

    // keepalive
    uint32_t m_keepAliveInterval = 0;
    int m_keepAliveTimer = -1;
    bool m_pingOutstanding = false;
    uint32_t m_pingValue = 0;

    struct DispatchGuard {
        MultiplexedTunnel* t;
        explicit DispatchGuard(MultiplexedTunnel* tunnel) : t(tunnel) { t->m_dispatchDepth++; }
        ~DispatchGuard() { if (--t->m_dispatchDepth == 0) t->purgeDeadStreams(); }
    };

    Stream* findStream(uint32_t streamId);
    bool initSendQueue(Stream& session);   //Lazy Initialization

    bool sendFrame(FrameType type, uint16_t flags, uint32_t streamId, uint32_t length, const uint8_t* payload = nullptr);
    void sendFin(Stream* s);
    void maybeSendWindowUpdate(Stream* s);

    void scheduleFlush(Stream* s);
    void flushStream(Stream* s);
    void flushStreams();

    void finishStream(Stream* s, bool notify);
    void closeAllStreams(bool notify);
    void purgeDeadStreams();
    void protocolError(const char* what);

    // Handlers for received frames
    bool processHeader(const uint8_t* header);
    Stream* acceptStream(uint32_t streamId);
    void deliverData(const uint8_t* data, size_t len);
    void finishDataFrame();
    void remoteFin(Stream* s);
    void handleWindowUpdateFrame(uint32_t streamId, uint32_t delta, uint16_t flags);
    void handlePingFrame(uint32_t streamId, uint32_t value, uint16_t flags);
    void handleGoAwayFrame(uint32_t code);

    void startKeepAlive();
    void stopKeepAlive();
    void onKeepAliveTick();
};

#endif // CLSMULTIPLEXEDTUNNEL_H
