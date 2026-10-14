// Barname test-e ketabkhane (src/). Se halat:
//
//   client : SOCKS5 mahali -> har connection yek stream-e Yamux roye tunnel (TCP ya TLS)
//   server : tunnel server (Yamux) -> har stream be maghsad vasl mishe  [moadel-e yamux_server/server.py]
//   socks5 : proxy SOCKS5 mostaghim, bedone tunnel
//
// mesal:
//   ./netRector server --listen 0.0.0.0:9191 --cert cert.pem --key key.pem
//   ./netRector client --server 1.2.3.4:9191 --listen 127.0.0.1:1080 --tls --insecure
//   curl --socks5-hostname 127.0.0.1:1080 https://example.com/

#include "clsServer.h"
#include "clsTLSContext.h"
#include "TestCommon.h"
#include "TunnelClient.h"
#include "Socks5LocalForwarder.h"
#include "Socks5StreamHandler.h"
#include "Socks5DirectProxy.h"
#include "HttpTest.h"
#include "clsUDPSocket.h"
#include <fcntl.h>
#include <sys/stat.h>

#include <sys/resource.h>
#include <csignal>
#include <cstdlib>
#include <string>
#include <vector>

bool g_verbose = false;
HttpTestConfig g_http;

static void usage(const char* prog)
{
    fprintf(stderr,
        "usage:\n"
        "  %s client --server HOST:PORT [--listen IP:PORT] [--tls] [--insecure] [--ca FILE] [--sni NAME]\n"
        "  %s server [--listen IP:PORT] [--cert FILE --key FILE]\n"
        "  %s socks5 [--listen IP:PORT]\n"
        "  %s http   [--listen IP:PORT] [--cert FILE --key FILE [--tls-listen IP:PORT]] [--file PATH]   (benchmark server)\n"
        "  %s udp    [--listen IP:PORT]                                          (UDP echo, tests UDPSocket)\n"
        "\n"
        "common options:\n"
        "  --threads N      number of reactor threads (default 1)\n"
        "  --keepalive SEC  yamux ping interval, 0 = off (default 30)\n"
        "  --stats SEC      print connection/stream counters every SEC seconds\n"
        "  --tls-threads N  server/http: run TLS private-key operations on N worker threads\n"
        "  --single-accept  one accept per listener event (default: up to 64 per event)\n"
        "  -v               verbose (per connection / per stream logs)\n"
        "\n"
        "defaults: client listens on 127.0.0.1:1080, server on 0.0.0.0:9191, socks5 on 127.0.0.1:1080\n"
        "TLS (client): --tls verifies the server certificate (system CAs or --ca FILE);\n"
        "              --insecure skips verification (self-signed test certificates).\n",
        prog, prog, prog, prog, prog);
}

// "host:port" ya "[v6]:port"
static bool splitHostPort(const std::string& in, std::string& host, uint16_t& port)
{
    size_t colon = in.rfind(':');
    if (colon == std::string::npos || colon + 1 >= in.size())
        return false;

    host = in.substr(0, colon);
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']')
        host = host.substr(1, host.size() - 2);

    char* end = nullptr;
    long p = strtol(in.c_str() + colon + 1, &end, 10);
    if (*end != '\0' || p <= 0 || p > 65535)
        return false;

    port = (uint16_t)p;
    return true;
}

static TCPSocket* acceptLocalSocks(void* /*ctx*/)
{
    return new Socks5LocalForwarder();
}

static TCPSocket* acceptTunnel(void* ctx)
{
    const TestConfig* cfg = static_cast<const TestConfig*>(ctx);
    TunnelServerEndpoint* t = new TunnelServerEndpoint(cfg);
    if (cfg->tlsCtx)
        t->enableTLS(cfg->tlsCtx);  // onAccepted bad az handshake seda zade mishe
    return t;
}

static TCPSocket* acceptDirectSocks(void* /*ctx*/)
{
    return new Socks5Session();
}

// UDP echo: har datagram be ferestande barmigarde (test-e UDPSocket / recvmmsg)
class UdpEcho : public UDPSocket {
public:
    void onDatagram(const uint8_t* data, size_t len, const struct sockaddr* from, socklen_t fromLen) override {
        sendTo(data, len, from, fromLen);
    }
    void onClose() override { LOG("[udp] closed"); }
};

static TCPSocket* acceptHttp(void* ctx)
{
    const TestConfig* cfg = static_cast<const TestConfig*>(ctx);
    HttpConn* c = new HttpConn();
    if (cfg->tlsCtx)
        c->enableTLS(cfg->tlsCtx);
    return c;
}

static TCPSocket* acceptHttpPlain(void* /*ctx*/)
{
    return new HttpConn();
}

static void startStats(Server& srv, const TestConfig& cfg, const char* mode)
{
    if (cfg.statsInterval == 0)
        return;

    for (int i = 0; i < srv.shardCount(); i++) {
        EpollReactor* r = srv.getShard(i);
        std::string m = mode;
        int interval = (int)cfg.statsInterval * 1000;
        r->post([r, m, interval]() {
            r->addTimer(interval, [r, m]() {
                TunnelClient* tc = (m == "client") ? static_cast<TunnelClient*>(r->userData()) : nullptr;
                size_t streams = (tc && tc->tunnel()) ? tc->tunnel()->streamCount() : 0;
                // sockets: hame fd haye sabt shode dar in reactor (ba listener, timer, dns, wakeup)
                if (tc)
                    LOG("[stats:%d] registered fds=%u tunnel=%s streams=%zu", r->id(), r->connectionCount(), tc->tunnel() ? "up" : "down", streams);
                else
                    LOG("[stats:%d] registered fds=%u", r->id(), r->connectionCount());
            });
        });
    }
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    std::string mode = argv[1];
    if (mode != "client" && mode != "server" && mode != "socks5" && mode != "http" && mode != "udp") {
        usage(argv[0]);
        return 2;
    }

    TestConfig cfg;
    std::string listenArg, serverArg, certFile, keyFile, caFile, filePath, tlsListenArg;
    bool useTLS = false;
    bool insecure = false;
    bool multiAccept = true;
    int tlsThreads = 0;

    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](std::string& out) -> bool {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing value for %s\n", a.c_str());
                return false;
            }
            out = argv[++i];
            return true;
        };

        std::string v;
        if (a == "--listen") { if (!next(listenArg)) return 2; }
        else if (a == "--server") { if (!next(serverArg)) return 2; }
        else if (a == "--cert") { if (!next(certFile)) return 2; }
        else if (a == "--key") { if (!next(keyFile)) return 2; }
        else if (a == "--ca") { if (!next(caFile)) return 2; }
        else if (a == "--sni") { if (!next(cfg.sni)) return 2; }
        else if (a == "--threads") { if (!next(v)) return 2; cfg.threads = std::max(1, atoi(v.c_str())); }
        else if (a == "--keepalive") { if (!next(v)) return 2; cfg.keepAlive = (uint32_t)std::max(0, atoi(v.c_str())); }
        else if (a == "--stats") { if (!next(v)) return 2; cfg.statsInterval = (uint32_t)std::max(0, atoi(v.c_str())); }
        else if (a == "--tls") useTLS = true;
        else if (a == "--file") { if (!next(filePath)) return 2; }
        else if (a == "--tls-listen") { if (!next(tlsListenArg)) return 2; }
        else if (a == "--tls-threads") { if (!next(v)) return 2; tlsThreads = std::max(0, atoi(v.c_str())); }
        else if (a == "--single-accept") multiAccept = false;
        else if (a == "--multi-accept") multiAccept = true;
        else if (a == "--insecure") { insecure = true; useTLS = true; }
        else if (a == "-v" || a == "--verbose") g_verbose = true;
        else if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        else {
            fprintf(stderr, "unknown option: %s\n", a.c_str());
            usage(argv[0]);
            return 2;
        }
    }

    // --- tanzimat har halat ---
    if (listenArg.empty())
        listenArg = (mode == "server") ? "0.0.0.0:9191" : (mode == "http") ? "0.0.0.0:8080" : (mode == "udp") ? "127.0.0.1:5353" : "127.0.0.1:1080";

    if (!splitHostPort(listenArg, cfg.listenIP, cfg.listenPort)) {
        fprintf(stderr, "invalid --listen value: %s\n", listenArg.c_str());
        return 2;
    }

    std::string tlsError;
    if (mode == "client") {
        if (serverArg.empty() || !splitHostPort(serverArg, cfg.serverHost, cfg.serverPort)) {
            fprintf(stderr, "client mode needs --server HOST:PORT\n");
            return 2;
        }

        if (useTLS) {
            cfg.tlsCtx = TLSContext::createClient(!insecure, caFile.empty() ? nullptr : caFile.c_str(), &tlsError);
            if (!cfg.tlsCtx) {
                fprintf(stderr, "TLS client context failed: %s\n", tlsError.c_str());
                return 1;
            }
        }
    } else if (mode == "server" || mode == "http") {
        if (certFile.empty() != keyFile.empty()) {
            fprintf(stderr, "--cert and --key must be given together\n");
            return 2;
        }

        if (!certFile.empty()) {
            cfg.tlsCtx = TLSContext::createServer(certFile.c_str(), keyFile.c_str(), &tlsError);
            if (!cfg.tlsCtx) {
                fprintf(stderr, "TLS server context failed: %s\n", tlsError.c_str());
                return 1;
            }
            if (tlsThreads > 0 && !cfg.tlsCtx->enableAsyncPrivateKey(tlsThreads)) {
                fprintf(stderr, "enableAsyncPrivateKey failed\n");
                return 1;
            }
        }
    }

    if (mode == "http") {
        std::string body = "Hello from netRector\n";
        if (!filePath.empty()) {
            g_http.fileFd = ::open(filePath.c_str(), O_RDONLY | O_CLOEXEC);
            struct stat st;
            if (g_http.fileFd == -1 || fstat(g_http.fileFd, &st) != 0) {
                fprintf(stderr, "cannot open %s\n", filePath.c_str());
                return 1;
            }
            g_http.fileSize = (size_t)st.st_size;
            g_http.fileHeader = "HTTP/1.1 200 OK\r\nServer: netRector\r\nContent-Type: application/octet-stream\r\nContent-Length: "
                                + std::to_string(g_http.fileSize) + "\r\n\r\n";
        }
        std::string head = "HTTP/1.1 200 OK\r\nServer: netRector\r\nContent-Type: text/plain\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
        g_http.response = head + "\r\n" + body;
        g_http.responseClose = head + "Connection: close\r\n\r\n" + body;
    }

    // SIGINT/SIGTERM dar thread asli ba sigwait gerefte mishan; thread haye reactor
    // in mask ro be ers mibaran.
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, nullptr);

    //in to libwrench hast max ro az onja begir
    struct rlimit r;
    getrlimit(RLIMIT_NOFILE, &r);
    if (r.rlim_cur < r.rlim_max) {
        r.rlim_cur = r.rlim_max;    // soft limit ta hadde mojaz bala mire
        setrlimit(RLIMIT_NOFILE, &r);
        getrlimit(RLIMIT_NOFILE, &r);
    }
    int maxfd = (r.rlim_cur > 1048576) ? 1048576 : (int)r.rlim_cur;
    LOG("mode=%s threads=%d maxfd=%d", mode.c_str(), cfg.threads, maxfd);

    std::vector<TunnelClient*> tunnelClients;
    int exitCode = 0;
    {
        Server srv(maxfd, cfg.threads);
        srv.setUseGarbageCollector(false);
        for (int i = 0; i < srv.shardCount(); i++)
            srv.getShard(i)->setMultiAccept(multiAccept);

        bool ok = false;
        if (mode == "client") {
            // har shard tunnel-e khodesh ro dare
            for (int i = 0; i < srv.shardCount(); i++) {
                EpollReactor* reactor = srv.getShard(i);
                TunnelClient* tc = new TunnelClient(reactor, &cfg);
                tunnelClients.push_back(tc);
                reactor->setUserData(tc);
                reactor->post([tc]() { tc->connect(); });   // roye thread-e khode reactor
            }
            ok = srv.AddNewListener(cfg.listenPort, cfg.listenIP.c_str(), acceptLocalSocks, nullptr);
        } else if (mode == "server") {
            ok = srv.AddNewListener(cfg.listenPort, cfg.listenIP.c_str(), acceptTunnel, &cfg);
        } else if (mode == "http") {
            if (!tlsListenArg.empty()) {
                // --listen plain va --tls-listen TLS (ham zaman)
                std::string tlsIP; uint16_t tlsPort = 0;
                ok = cfg.tlsCtx && splitHostPort(tlsListenArg, tlsIP, tlsPort)
                     && srv.AddNewListener(cfg.listenPort, cfg.listenIP.c_str(), acceptHttpPlain, nullptr)
                     && srv.AddNewListener(tlsPort, tlsIP.c_str(), acceptHttp, &cfg);
            } else {
                ok = srv.AddNewListener(cfg.listenPort, cfg.listenIP.c_str(), acceptHttp, &cfg);
            }
        } else if (mode == "udp") {
            // har shard yek socket roye haman port (SO_REUSEPORT)
            ok = true;
            for (int i = 0; i < srv.shardCount(); i++) {
                UdpEcho* u = new UdpEcho();
                u->setReactor(srv.getShard(i));
                u->setMaxDatagramSize(65535);
                if (!u->bind(cfg.listenIP.c_str(), cfg.listenPort, true)) {
                    fprintf(stderr, "udp bind failed: %s\n", strerror(u->getErrorCode()));
                    delete u;
                    ok = false;
                    break;
                }
            }
        } else {
            ok = srv.AddNewListener(cfg.listenPort, cfg.listenIP.c_str(), acceptDirectSocks, nullptr);
        }

        if (!ok) {
            fprintf(stderr, "listen on %s failed\n", listenArg.c_str());
            exitCode = 1;
        } else if (!srv.start()) {
            fprintf(stderr, "start failed\n");
            exitCode = 1;
        } else {
            startStats(srv, cfg, mode.c_str());
            LOG("running; press Ctrl+C to stop");

            int sig = 0;
            sigwait(&sigs, &sig);
            LOG("signal %d received, shutting down", sig);

            for (TunnelClient* tc : tunnelClients)
                tc->stop();     // dige reconnect nakon
            srv.stop();
        }
        // ~Server: reactor ha va hame object haye baghimande azad mishan
    }

    for (TunnelClient* tc : tunnelClients)
        delete tc;
    delete cfg.tlsCtx;
    return exitCode;
}
