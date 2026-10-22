// muxstress: test-e sangin-e multiplexing (Yamux) roye shabake.
//
//   server: muxstress server --listen 0.0.0.0:9300 [--cert c --key k] [--tls-threads N]
//   client: muxstress client --server HOST:9300 [--tls] --streams 200 --messages 300 
//                            --max-size 65536 --slow 0.3 --rst 0.1 --duration 120 --threads 2 (yek khat)
//
// Har stream record haye [len u32][seq u32][crc32 u32][payload] miferese; server har record ro
// check (seq, crc) va echo mikone; client echo ha ro ba haman check ha va tartib verify mikone.
// Stream ha ba ehtemal "slow" (pauseStream/resumeStream -> window por mishe), "rst" (RST vasate kar)
// ya adi (FIN bad az payan) hastan. Dar enteha khata ha chap mishan; exit code 1 agar khata bashe.

#include "clsServer.h"
#include "clsTLSContext.h"
#include "clsMultiplexedTunnel.h"
#include "clsLog.h"

#include <atomic>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <sys/resource.h>
#include <unordered_map>
#include <vector>

// ----------------------------------------------------------------------------- stats
struct Stats {
    std::atomic<uint64_t> streamsOpened{0}, streamsDone{0}, recordsSent{0}, recordsOk{0}, bytesSent{0}, bytesRecv{0};
    std::atomic<uint64_t> errors{0}, rstStreams{0}, tunnelsLost{0}, pauses{0}, rejected{0};
    std::mutex mu;
    std::vector<std::string> messages;

    void error(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char buf[512];
        va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
        errors++;
        std::lock_guard<std::mutex> l(mu);
        if (messages.size() < 100) messages.push_back(buf);
        else if (messages.size() == 100) messages.push_back("... (more errors omitted)");
    }
    void print(const char* who) {
        fprintf(stderr, "---- %s: streams opened=%llu done=%llu rst=%llu rejected=%llu | records sent=%llu ok=%llu | bytes sent=%llu recv=%llu | pauses=%llu tunnels-lost=%llu | ERRORS=%llu\n",
                who, (unsigned long long)streamsOpened.load(), (unsigned long long)streamsDone.load(), (unsigned long long)rstStreams.load(), (unsigned long long)rejected.load(),
                (unsigned long long)recordsSent.load(), (unsigned long long)recordsOk.load(), (unsigned long long)bytesSent.load(),
                (unsigned long long)bytesRecv.load(), (unsigned long long)pauses.load(), (unsigned long long)tunnelsLost.load(), (unsigned long long)errors.load());
        std::lock_guard<std::mutex> l(mu);
        for (auto& m : messages) fprintf(stderr, "  ERROR: %s\n", m.c_str());
    }
};
static Stats g_stats;
static bool g_verbose = false;

// ----------------------------------------------------------------------------- crc32
static uint32_t crc32buf(const uint8_t* p, size_t n, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; table[i] = c; }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// ----------------------------------------------------------------------------- record parser
// record: len(4, tool payload) seq(4) crc(4) payload
struct RecordParser {
    std::string buf;        // byte haye naghes
    uint32_t expectSeq = 0;
    uint64_t records = 0;
    uint64_t bytes = 0;

    // true: ok, false: khata (dalil dar err)
    bool strict = true;     // false (server): record-e kharab = client-e bad, faghat RST va shomaresh

    template <typename F>
    bool feed(const uint8_t* data, size_t len, const char* who, uint32_t sid, F onRecord) {
        bytes += len;
        buf.append(reinterpret_cast<const char*>(data), len);
        size_t pos = 0;
        while (buf.size() - pos >= 12) {
            uint32_t plen, seq, crc;
            memcpy(&plen, buf.data() + pos, 4); memcpy(&seq, buf.data() + pos + 4, 4); memcpy(&crc, buf.data() + pos + 8, 4);
            if (plen > 4 * 1024 * 1024) { if (strict) g_stats.error("%s stream %u: bad record length %u (corrupt stream)", who, sid, plen); else g_stats.rejected++; return false; }
            if (buf.size() - pos < 12 + plen) break;
            const uint8_t* payload = reinterpret_cast<const uint8_t*>(buf.data() + pos + 12);
            if (seq != expectSeq) { if (strict) g_stats.error("%s stream %u: seq %u expected %u (reorder/loss)", who, sid, seq, expectSeq); else g_stats.rejected++; return false; }
            if (crc32buf(payload, plen) != crc) { if (strict) g_stats.error("%s stream %u: crc mismatch on seq %u len %u", who, sid, seq, plen); else g_stats.rejected++; return false; }
            expectSeq++;
            records++;
            onRecord(reinterpret_cast<const uint8_t*>(buf.data() + pos), 12 + plen);
            pos += 12 + plen;
        }
        buf.erase(0, pos);
        return true;
    }
};

static void buildRecord(std::string& out, uint32_t seq, size_t plen, std::mt19937& rng) {
    out.resize(12 + plen);
    uint32_t len32 = (uint32_t)plen;
    memcpy(&out[0], &len32, 4); memcpy(&out[4], &seq, 4);
    // payload: pattern-e vabaste be seq ta eshtebah-e tartib ham crc ro kharab kone
    uint32_t x = seq * 2654435761u + 12345;
    for (size_t i = 0; i < plen; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; out[12 + i] = (char)(x + rng() % 3); }
    uint32_t crc = crc32buf(reinterpret_cast<const uint8_t*>(out.data() + 12), plen);
    memcpy(&out[8], &crc, 4);
}

// ----------------------------------------------------------------------------- server
struct ServerConfig { TLSContext* tls = nullptr; uint32_t keepAlive = 10; };

class ServerTunnel : public MultiplexedTunnel {
public:
    explicit ServerTunnel() : MultiplexedTunnel(false) { setOnNewStream(&ServerTunnel::onNew, this); }
    struct Echo { ServerTunnel* t; uint32_t id; RecordParser parser; bool remoteFin = false; };
    std::unordered_map<uint32_t, Echo*> echos;

    void onAccepted() override { fprintf(stderr, "[server] tunnel accepted fd=%d %s\n", fd(), isTLS() ? tlsVersion() : "plain"); }
    void onClose() override { fprintf(stderr, "[server] tunnel closed (reason %d, streams left %zu)\n", (int)closeReason(), echos.size()); for (auto& e : echos) delete e.second; echos.clear(); }

    static void onNew(void* arg, uint32_t id, Stream* s) {
        ServerTunnel* self = static_cast<ServerTunnel*>(arg);
        Echo* e = new Echo{self, id, {}, false};
        e->parser.strict = false;
        self->echos[id] = e;
        g_stats.streamsOpened++;
        s->arg = e; s->onData = &ServerTunnel::sData; s->onClose = &ServerTunnel::sClose; s->onRemoteFin = &ServerTunnel::sFin;
        s->onPause = &ServerTunnel::sPause; s->onResume = &ServerTunnel::sResume;
    }
    static void sData(void* p, uint32_t id, const uint8_t* d, size_t n) {
        Echo* e = static_cast<Echo*>(p);
        g_stats.bytesRecv += n;
        bool ok = e->parser.feed(d, n, "server", id, [e](const uint8_t* rec, size_t len) {
            g_stats.recordsOk++;
            // agar client stream ro RST karde bashe, stream dige nist va echo rad mishe (khata nist)
            if (!e->t->sendToStream(e->id, rec, len)) { if (e->t->getStream(e->id)) g_stats.error("server stream %u: sendToStream failed", e->id); }
            else g_stats.bytesSent += len;
        });
        if (!ok) e->t->closeStream(id, true);
    }
    static void sFin(void* p, uint32_t id) { Echo* e = static_cast<Echo*>(p); e->remoteFin = true; if (!e->parser.buf.empty()) g_stats.rejected++; e->t->closeStream(id, false); }
    static void sClose(void* p, uint32_t id) { Echo* e = static_cast<Echo*>(p); g_stats.streamsDone++; e->t->echos.erase(id); delete e; }
    // safe ersal-e stream por: daryaft ro negah dar (echo flow control)
    static void sPause(void* p, uint32_t id) { Echo* e = static_cast<Echo*>(p); g_stats.pauses++; e->t->pauseStream(id); }
    static void sResume(void* p, uint32_t id) { Echo* e = static_cast<Echo*>(p); e->t->resumeStream(id); }
};

static TCPSocket* acceptTunnel(void* ctx) {
    ServerConfig* cfg = static_cast<ServerConfig*>(ctx);
    ServerTunnel* t = new ServerTunnel();
    t->setKeepAlive(cfg->keepAlive);
    if (cfg->tls) t->enableTLS(cfg->tls);
    return t;
}

// ----------------------------------------------------------------------------- client
struct ClientConfig {
    std::string host; uint16_t port = 9300; TLSContext* tls = nullptr;
    int streams = 100, messages = 200; size_t minSize = 1, maxSize = 65536;
    double slowRatio = 0.3, rstRatio = 0.1; int duration = 120; uint32_t keepAlive = 10;
    size_t inflightLimit = 1024 * 1024;   // hadeaksar byte-e echo nashode dar har stream
    int rounds = 1;                       // chand bar tunnel/stream ha tekrar beshan
};

class ClientTunnel;
struct CStream {
    ClientTunnel* t; uint32_t id; uint32_t sendSeq = 0; int messages; RecordParser parser;
    uint64_t sent = 0, echoed = 0; bool slow, rst; bool sendDone = false, finSent = false, remoteFin = false, closed = false, blocked = false, paused = false;
    uint32_t rstAt;     // record-e ke dar an RST mishe
    std::mt19937 rng;
};

class ClientTunnel : public MultiplexedTunnel {
public:
    ClientTunnel(const ClientConfig* cfg, EpollReactor* r, int seed) : MultiplexedTunnel(true), m_cfg(cfg), m_rng(seed) { setReactor(r); setKeepAlive(cfg->keepAlive); }
    std::unordered_map<uint32_t, CStream*> streams;
    bool finished = false;
    int done = 0, opened = 0;

    void onConnected() override {
        fprintf(stderr, "[client] tunnel up fd=%d %s; opening %d streams\n", fd(), isTLS() ? tlsVersion() : "plain", m_cfg->streams);
        for (int i = 0; i < m_cfg->streams; i++) openOne();
        m_timer = getReactor()->addTimer(20, [this]() { tick(); });
    }
    void onConnectFailed() override { g_stats.error("client: tunnel connect failed: %s", strerror(getErrorCode())); }
    void onClose() override {
        if (m_timer != -1) getReactor()->removeTimer(m_timer);
        if (!finished) { g_stats.tunnelsLost++; g_stats.error("client: tunnel closed before all streams finished (reason %d, %zu streams open)", (int)closeReason(), streams.size()); }
        for (auto& s : streams) delete s.second;
        streams.clear();
        finished = true;
    }

    void openOne() {
        uint32_t id = openStream(&ClientTunnel::cData, &ClientTunnel::cClose, nullptr);
        if (id == 0) { g_stats.error("client: openStream failed"); return; }
        CStream* s = new CStream{this, id, 0, m_cfg->messages, {}, 0, 0, false, false, false, false, false, false, false, false, 0, std::mt19937(m_rng())};
        std::uniform_real_distribution<double> u(0, 1);
        s->slow = u(m_rng) < m_cfg->slowRatio;
        s->rst = u(m_rng) < m_cfg->rstRatio;
        s->rstAt = s->rst ? (uint32_t)(m_rng() % std::max(1, m_cfg->messages)) : UINT32_MAX;
        Stream* st = getStream(id);
        st->arg = s; st->onRemoteFin = &ClientTunnel::cFin; st->onPause = &ClientTunnel::cPause; st->onResume = &ClientTunnel::cResume;
        streams[id] = s;
        opened++;
        g_stats.streamsOpened++;
        pump(s);
    }

    // ersal-e record ha ta jaiy ke stream block nashode va in-flight kam bashe
    void pump(CStream* s) {
        std::string rec;
        while (!s->sendDone && !s->blocked && !s->closed && (s->sent - s->echoed) < m_cfg->inflightLimit) {
            if (s->sendSeq == s->rstAt) {   // RST vasate kar
                closeStream(s->id, true);
                g_stats.rstStreams++;
                finishStream(s);
                return;
            }
            size_t plen = pickSize(s->rng);
            buildRecord(rec, s->sendSeq, plen, s->rng);
            if (!sendToStream(s->id, reinterpret_cast<const uint8_t*>(rec.data()), rec.size())) {
                g_stats.error("client stream %u: sendToStream failed at seq %u", s->id, s->sendSeq);
                return;
            }
            s->sendSeq++; s->sent += rec.size();
            g_stats.recordsSent++; g_stats.bytesSent += rec.size();
            if ((int)s->sendSeq >= s->messages) {
                s->sendDone = true;
                closeStream(s->id, false);   // FIN: echo ha hanooz miresan
                s->finSent = true;
            }
        }
    }

    size_t pickSize(std::mt19937& rng) {
        // tarkib: kheyli kochik, dor-o-bar-e header, tasadofi, bozorg (ta 256KB = yek window)
        int k = rng() % 10;
        if (k == 0) return m_cfg->minSize;
        if (k == 1) return 11 + rng() % 3;
        if (k == 2) return std::min<size_t>(m_cfg->maxSize, 256 * 1024 - 12 + rng() % 3);
        return m_cfg->minSize + rng() % (m_cfg->maxSize - m_cfg->minSize + 1);
    }

    static void cData(void* p, uint32_t id, const uint8_t* d, size_t n) {
        CStream* s = static_cast<CStream*>(p);
        g_stats.bytesRecv += n;
        if (s->rst) { /* bad az RST hich echo-i nabayad biad; ghabl az an mojaz */ }
        bool ok = s->parser.feed(d, n, "client", id, [s](const uint8_t*, size_t len) { s->echoed += len; g_stats.recordsOk++; });
        if (!ok) { s->t->closeStream(id, true); s->t->finishStream(s); return; }
        if (s->echoed > s->sent) g_stats.error("client stream %u: echoed more than sent (%llu > %llu)", id, (unsigned long long)s->echoed, (unsigned long long)s->sent);
        // slow reader: gah gahi daryaft ro negah dar ta window-e tarafe moghabel por beshe
        if (s->slow && !s->paused && (s->rng() % 4 == 0)) {
            s->paused = true; g_stats.pauses++;
            s->t->pauseStream(id);
            // timer faghat id ro negah midare: stream momkene ta an moghe hazf shode bashe
            ClientTunnel* t = s->t;
            t->getReactor()->addTimer(20 + s->rng() % 200, [t, id]() {
                auto it = t->streams.find(id);
                if (it != t->streams.end() && !it->second->closed) { it->second->paused = false; t->resumeStream(id); }
            }, true);
        }
        s->t->pump(s);
    }
    static void cFin(void* p, uint32_t id) {
        CStream* s = static_cast<CStream*>(p);
        s->remoteFin = true;
        if (!s->sendDone) g_stats.error("client stream %u: server FIN before we finished sending (sent %u records)", id, s->sendSeq);
        if (s->echoed != s->sent) g_stats.error("client stream %u: FIN but echoed %llu of %llu bytes", id, (unsigned long long)s->echoed, (unsigned long long)s->sent);
        if (!s->parser.buf.empty()) g_stats.error("client stream %u: FIN with %zu partial bytes", id, s->parser.buf.size());
    }
    static void cClose(void* p, uint32_t id) {
        CStream* s = static_cast<CStream*>(p);
        if (!s->remoteFin && !s->rst) g_stats.error("client stream %u: closed without FIN (reset or tunnel loss)", id);
        if (s->echoed != s->sent && !s->rst) g_stats.error("client stream %u: closed with echoed %llu of %llu", id, (unsigned long long)s->echoed, (unsigned long long)s->sent);
        s->t->finishStream(s);
    }
    static void cPause(void* p, uint32_t) { static_cast<CStream*>(p)->blocked = true; }
    static void cResume(void* p, uint32_t) { CStream* s = static_cast<CStream*>(p); s->blocked = false; s->t->pump(s); }

    void finishStream(CStream* s) {
        if (s->closed) return;
        s->closed = true;
        streams.erase(s->id);
        done++;
        g_stats.streamsDone++;
        // object dar tick() azad mishe (timer-e slow momkene hanooz be an eshare kone)
        m_dead.push_back(s);
        if (m_round < m_cfg->rounds && opened < m_cfg->streams * m_cfg->rounds && getStatus() == Connected) openOne();
    }

    void tick() {
        for (CStream* s : m_dead) delete s;     // timer haye slow (<=220ms) ... (check paused flag) 
        m_dead.clear();
        if (streams.empty() && opened >= m_cfg->streams * m_cfg->rounds && !finished) {
            finished = true;
            fprintf(stderr, "[client] reactor %d: all %d streams finished\n", getReactor()->id(), opened);
            shutdown(GoAwayCode::Normal);
        }
    }

private:
    const ClientConfig* m_cfg;
    std::mt19937 m_rng;
    int m_timer = -1;
    int m_round = 1;
    std::vector<CStream*> m_dead;
};

// ----------------------------------------------------------------------------- main
static bool splitHostPort(const std::string& in, std::string& host, uint16_t& port) {
    size_t c = in.rfind(':'); if (c == std::string::npos) return false;
    host = in.substr(0, c); port = (uint16_t)atoi(in.c_str() + c + 1); return port != 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: muxstress server|client [options]\n"); return 2; }
    std::string mode = argv[1];
    std::string listen = "0.0.0.0:9300", server, cert, key;
    bool tls = false; int threads = 1, tlsThreads = 0;
    ClientConfig cc; ServerConfig sc;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i]; auto v = [&]() { return (i + 1 < argc) ? std::string(argv[++i]) : std::string(); };
        if (a == "--listen") listen = v(); else if (a == "--server") server = v(); else if (a == "--cert") cert = v(); else if (a == "--key") key = v();
        else if (a == "--tls") tls = true; else if (a == "--threads") threads = std::max(1, atoi(v().c_str())); else if (a == "--tls-threads") tlsThreads = atoi(v().c_str());
        else if (a == "--streams") cc.streams = atoi(v().c_str()); else if (a == "--messages") cc.messages = atoi(v().c_str());
        else if (a == "--min-size") cc.minSize = (size_t)atol(v().c_str()); else if (a == "--max-size") cc.maxSize = (size_t)atol(v().c_str());
        else if (a == "--slow") cc.slowRatio = atof(v().c_str()); else if (a == "--rst") cc.rstRatio = atof(v().c_str());
        else if (a == "--duration") cc.duration = atoi(v().c_str()); else if (a == "--rounds") cc.rounds = std::max(1, atoi(v().c_str()));
        else if (a == "--inflight") cc.inflightLimit = (size_t)atol(v().c_str());
        else if (a == "--keepalive") { cc.keepAlive = sc.keepAlive = (uint32_t)atoi(v().c_str()); }
        else if (a == "-v") g_verbose = true;
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (cc.maxSize < cc.minSize) cc.maxSize = cc.minSize;

    sigset_t sigs; sigemptyset(&sigs); sigaddset(&sigs, SIGINT); sigaddset(&sigs, SIGTERM); sigaddset(&sigs, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &sigs, nullptr);
    struct rlimit r; getrlimit(RLIMIT_NOFILE, &r);
    int maxfd = (r.rlim_cur > 1048576) ? 1048576 : (int)r.rlim_cur;

    std::string ip; uint16_t port;
    std::string err;
    int exitCode = 0;
    {
        Server srv(maxfd, threads);
        if (mode == "server") {
            if (!cert.empty()) { sc.tls = TLSContext::createServer(cert.c_str(), key.c_str(), &err); if (!sc.tls) { fprintf(stderr, "TLS: %s\n", err.c_str()); return 1; } if (tlsThreads) sc.tls->enableAsyncPrivateKey(tlsThreads); }
            if (!splitHostPort(listen, ip, port) || !srv.AddNewListener(port, ip.c_str(), acceptTunnel, &sc)) { fprintf(stderr, "listen failed\n"); return 1; }
            srv.start();
            fprintf(stderr, "[server] listening on %s (%s)\n", listen.c_str(), sc.tls ? "TLS" : "plain");
            int sig; sigwait(&sigs, &sig);
            srv.stop();
            g_stats.print("server");
            exitCode = g_stats.errors ? 1 : 0;
        } else if (mode == "client") {
            if (!splitHostPort(server, cc.host, cc.port)) { fprintf(stderr, "--server HOST:PORT needed\n"); return 2; }
            if (tls) cc.tls = TLSContext::createClient(false, nullptr, &err);
            std::vector<ClientTunnel*> tunnels;
            for (int i = 0; i < srv.shardCount(); i++) {
                ClientTunnel* t = new ClientTunnel(&cc, srv.getShard(i), 1000 + i);
                t->setAutoDelete(false);
                if (cc.tls) t->enableTLS(cc.tls);
                tunnels.push_back(t);
                srv.getShard(i)->post([t, &cc]() { if (!t->connectTo(cc.host.c_str(), cc.port)) g_stats.error("client: connectTo failed"); });
            }
            srv.start();
            // montazer: hame tamam shodan ya timeout
            alarm(0);
            timer_t dummy; (void)dummy;
            int waited = 0;
            while (waited < cc.duration) {
                struct timespec ts{1, 0};
                int sig = sigtimedwait(&sigs, nullptr, &ts);
                if (sig == SIGINT || sig == SIGTERM) break;
                waited++;
                bool all = true;
                for (ClientTunnel* t : tunnels) if (!t->finished) all = false;
                if (all) break;
                if (waited % 10 == 0) g_stats.print("client progress");
            }
            bool stalled = false;
            for (ClientTunnel* t : tunnels) if (!t->finished) stalled = true;
            if (stalled) g_stats.error("client: timeout after %d s; streams still open (stall)", waited);
            srv.stop();
            for (ClientTunnel* t : tunnels) delete t;
            g_stats.print("client");
            exitCode = g_stats.errors ? 1 : 0;
            fprintf(stderr, g_stats.errors ? "RESULT: FAILED\n" : "RESULT: OK\n");
        } else { fprintf(stderr, "mode must be server or client\n"); return 2; }
    }
    delete sc.tls;
    delete cc.tls;
    return exitCode;
}
