# netRector

Linux-only (epoll), C++17 library for many concurrent TCP/UDP connections. TLS via BoringSSL, optional (see rule 18). All library code is in `namespace netrector`. Library code is `src/`; `example/` is a test app (SOCKS5 over Yamux tunnel, HTTP bench server, UDP echo) that doubles as example code.

## Mental model

- `Server` owns N `EpollReactor` shards, one thread each (default: CPU count). Each shard has its own epoll fd (edge-triggered), listener sockets (`SO_REUSEPORT`), fd table, send-buffer pool, DNS resolver and timers. Shards share nothing except `TLSContext`.
- A connection is a `TCPSocket` (or subclass) bound to exactly one shard. All its methods and callbacks run on that shard's thread.
- You subclass `TCPSocket`, override `onXxx` hooks (or register C function-pointer callbacks), and return new instances from an accept factory: `TCPSocket* factory(void* ctx)`.

## Rules (breaking one causes a crash, leak, deadlock or silent data loss)

**Ownership and lifetime**
1. Create sockets with `new`. After accept, or after `connectTo()` returned `true`, the reactor owns the socket: never `delete` it, call `close()`. It is deleted at the end of the current loop iteration, after `onClose`, so `this` stays valid until your callback chain returns. Null out any pointer you keep to it inside `onClose`.
2. `connectTo()` returning `false` means no callback was or will be called and you still own the object: `delete` it.
3. `onClose` is called exactly once and is always the last callback. A failed connect calls `onConnectFailed` and then `onClose`. `closeReason()` says why: `ClosedByApp`, `ClosedByPeer`, `ClosedByError` (see `getErrorCode()`), `ClosedByTimeout`, `ConnectFailed`.
4. `setAutoDelete(false)` is for sockets that are members of another object: the owner deletes only after `onClose` has run, never inside it.

**Threads**
5. Touch a socket, reactor, timer or DNS object only from its shard thread. From any other thread use `reactor->post(std::function<void()>)`. Thread-safe: `post`, `wake`, `Server::AddNewListener/start/stop/isRunning/getShard/getRoundRobinShard/shardCount`.
6. A posted task runs later. The socket may be closed by then, so re-validate state inside the task instead of capturing raw socket pointers whose lifetime you do not control.
7. Never block in a callback (sleep, blocking I/O, heavy CPU). It stalls every connection on that shard. Offload to your own thread and `post` the result back.
8. State shared by all connections of one shard: `reactor->setUserData(p)`. State shared across shards needs your own locking.

**Data**
9. Pointers passed to callbacks are valid only until the callback returns: `onReceiveData` data (one shared 64 KB buffer per reactor), `onDatagram` data, Yamux `onData` data, DNS `ips`. Copy what you keep. TCP is a byte stream: buffer partial messages yourself.
10. `send()`/`sendv()`/`sendNoCopy()`/`sendFile()` return `bool`. Check it. `false` means: not `Connected` (before `onConnected`/`onAccepted`, while closing, after close), per-socket queue cap exceeded (`setMaxSendQueue`, default 64 MB; the connection stays open), write error or out of memory (the socket is then closed). `send()` never calls `onClose` from inside itself; that close happens at the end of the loop iteration.
11. `sendNoCopy(ptr, len, release, arg)`: `ptr` must stay valid until `release(ptr, arg)` is called (also called on close). `release == nullptr` means static data. `sendFile(fd, off, len)`: keep `fd` open until the data is sent; zero-copy on plain TCP, `pread` on TLS.
12. Backpressure: when a socket's send queue exceeds `BACK_PRESSURE` (128 KB) `onPause` fires, at `LOW_WATERMARK` (64 KB) `onResume`. Default `onPause`/`onResume` pause/resume reading from the same socket, which is right for echo. A relay must instead pause reading from the source socket (`setOnPause`/`setOnResume` on the destination, as in the relay example). Ignoring it grows memory up to the 64 MB cap.

**Callbacks**
13. Per event, if a function-pointer callback was registered with `setOnXxx(fn, arg)` it is used and the virtual `onXxx` is not called. All `setOnXxx` calls on one socket share one `arg` (the last call wins), so always pass the same owner pointer.
14. `close()` is graceful by default (flush send queue, send FIN, wait up to 5 s for peer FIN). `close(true)` drops queued data and closes now. Both are safe to call from any callback on the shard thread, including `onReceiveData`.
15. By default a peer FIN closes the socket after the send queue drains. For half-close call `setAllowHalfClose(true)`: you then get `onReadEOF`, can keep sending, and finish with `shutdownWrite()` or `close()`.
16. `pause_reading()`/`resume_reading()` call no callbacks.

**Connect and TLS**
17. Outbound: `setReactor(reactor)` first, then `connectTo(host, port)` on the shard thread. `host` is an IP literal or a name (resolved asynchronously, all returned addresses are tried). Valid once, while status is `Ready`. Result arrives via `onConnected` or `onConnectFailed`, possibly before `connectTo` returns. Connect plus handshake timeout: 15 s. Outbound sockets get `TCP_NODELAY` only; for keepalive call `setSocketKeepAlive(fd(), true)` in `onConnected`.
18. TLS is optional at build time. Without BoringSSL (`NETRECTOR_HAS_TLS` undefined) the library still builds and works for plain TCP/UDP, `TLSContext::createServer/createClient` return `nullptr` (reason in `*error`), `enableTLS()` returns `false`, and `TLSContext::isSupported()` is `false`. Always check the `createXxx` result. `TLSContext` is created once (`createServer(certFile, keyFile, &err)` or `createClient(verifyPeer, caFile, &err)`), shared by all sockets and shards, and must outlive every socket that uses it: delete it only after `Server::stop()` returned. `createClient(true)` verifies against system CAs or `caFile`.
19. `enableTLS(ctx, serverName=nullptr)` must be called while the socket is `Ready`: in the accept factory, or before `connectTo`. It returns `false` otherwise. With TLS, `onAccepted`/`onConnected` fire after the handshake; do not send earlier. `serverName` sets SNI and the name checked in the certificate (default: the host given to `connectTo`).
20. `ctx->enableAsyncPrivateKey(threads)` (server contexts only, call once, before any connection) moves handshake signing to worker threads and restricts ciphers to ECDHE.

**Server lifecycle**
21. Block `SIGINT`/`SIGTERM` in the main thread before constructing `Server` so worker threads inherit the mask, then `sigwait` in main (see snippet). `Server` ignores `SIGPIPE`.
22. `Server::stop()` closes every socket with `close(true)` on its own shard, so your `onClose` callbacks run during `stop()`. Everything those callbacks touch must still be alive. Destroy your objects and the `TLSContext` only after `stop()` returned. `~Server` calls `stop()`.
23. Raise `RLIMIT_NOFILE` before creating `Server` (`example/main.cpp` shows how). `Server(maxConnection, ...)` is only the initial fd-table size, it grows. When fds run out, new connections are rejected and accepting pauses for 200 ms. Hard caps: `Server::setMaxConnectionsPerShard(n)`.
24. Timers: `reactor->addTimer(ms, fn, singleShot=false)` returns an id for `removeTimer(id)`. Call on the shard thread (or inside `post`). If the lambda captures a pointer to an object that can die, call `removeTimer` in that object's `onClose`.

**Names**
25. Everything is in `namespace netrector`: write `netrector::Server` or `using namespace netrector;` (class names below are given unqualified). Method names are inconsistent (`AddNewListener`, `register_fd`, `getReactor`, `pause_reading`). Use them exactly as written; do not "correct" them to one style.

## Quick start: TCP echo server

```cpp
#include "clsServer.h"
#include <csignal>
using namespace netrector;

class Echo : public TCPSocket {
public:
    void onReceiveData(const uint8_t* d, size_t n) override { send(d, n); }
};
static TCPSocket* makeEcho(void*) { return new Echo(); }   // nullptr = reject connection

int main() {
    sigset_t s; sigemptyset(&s); sigaddset(&s, SIGINT); sigaddset(&s, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &s, nullptr);              // before Server (rule 21)
    Server srv(65536);                                    // shards = CPU count
    if (!srv.AddNewListener(8080, "0.0.0.0", makeEcho, nullptr) || !srv.start())
        return 1;                                         // start() does not block
    int sig; sigwait(&s, &sig);
    srv.stop();
}
```

`bindIP`: `nullptr`/`"*"`/`"::"` = all interfaces (IPv4+IPv6), `"0.0.0.0"` = all IPv4, or one address. `AddNewListener` works before and after `start()` from any thread (up to 5 s wait per call). Listeners without their own factory use `srv.setOnAccepted(factory, ctx)`.

## Outbound connection (optionally TLS)

```cpp
class Client : public TCPSocket {
public:
    void onConnected() override { send("hello", 5); }          // after handshake if TLS
    void onReceiveData(const uint8_t* d, size_t n) override { close(); }
    void onConnectFailed() override { /* onClose follows */ }
    void onClose() override { /* last callback */ }
};

EpollReactor* reactor = srv.getRoundRobinShard();
reactor->post([reactor, tlsCtx]() {                            // must run on the shard thread
    Client* c = new Client();
    c->setReactor(reactor);
    if (tlsCtx) c->enableTLS(tlsCtx, "example.com");
    if (!c->connectTo("example.com", 443)) delete c;           // rule 2
});
```

## Relay with backpressure (function-pointer callbacks)

```cpp
struct Relay { TCPSocket* a; TCPSocket* b; };                  // a -> b
static void onDataA(void* p, const uint8_t* d, size_t n) { static_cast<Relay*>(p)->b->send(d, n); }
static void onPauseB(void* p)  { static_cast<Relay*>(p)->a->pause_reading(); }
static void onResumeB(void* p) { static_cast<Relay*>(p)->a->resume_reading(); }
// a->setOnData(onDataA, r);  b->setOnPause(onPauseB, r);  b->setOnResume(onResumeB, r);
// Also handle onClose on both sides: close the other one and clear pointers in Relay.
```

## UDP

```cpp
class UdpEcho : public UDPSocket {
public:
    void onDatagram(const uint8_t* d, size_t n, const sockaddr* from, socklen_t l) override { sendTo(d, n, from, l); }
};
UdpEcho* u = new UdpEcho();
u->setReactor(reactor);                         // required before bind()
if (!u->bind("0.0.0.0", 5353, /*reusePort*/ true)) delete u;   // one socket per shard
```

`UDPSocket`: `open(family)`, `bind`, `connect`+`send`, `sendTo`, `close()`, `setMaxDatagramSize` (default 2048), `setOnDatagram/setOnClose(fn, arg)`, `setAutoDelete`, `getErrorCode`. Reads use `recvmmsg`. There is no UDP send queue.

## Yamux multiplexing: `MultiplexedTunnel` (is a `TCPSocket`)

```cpp
MultiplexedTunnel* t = new MultiplexedTunnel(/*isClient*/ true);   // server side: false, from accept factory
t->setReactor(reactor);  /* enableTLS optional */  t->connectTo(host, port);
// after onConnected (override it in a subclass):
uint32_t id = t->openStream(onData, onClose, arg);    // 0 = tunnel not usable (not connected / GoAway)
t->sendToStream(id, buf, len);
t->closeStream(id);                                   // FIN (half-close); closeStream(id, true) = RST, no callbacks after
// server side: t->setOnNewStream(fn, arg), fn(void* arg, uint32_t id, Stream* s) sets s->onData, s->onClose, s->arg
```

- Stream callbacks: `onData(arg, id, data, len)`, `onClose(arg, id)`, plus optional `onRemoteFin`, `onPause`, `onResume` set through `getStream(id)` or the `Stream*` in `onNewStream`. In `onNewStream` set only callback/arg fields and do not keep the `Stream*`.
- `onClose` is the last callback of a stream, once (not after `closeStream(id, true)`). Stream ids are invalid afterwards. A tunnel `onClose` closes all streams.
- `onPause` on a stream: stop calling `sendToStream` until `onResume`. To throttle incoming data use `pauseStream(id)`/`resumeStream(id)`.
- `sendToStream`, `closeStream`, `openStream` are allowed inside stream callbacks. `detachStream(id)` clears the callbacks and lets the tunnel finish the stream itself; use it when your object dies before the stream.
- Other: `setMaxStreams(n)` (default 4096; extra SYNs get RST), `setKeepAlive(sec)` (0 = off), `shutdown(GoAwayCode)`, `streamCount()`, virtual `onGoAway(code)`, `remoteGoAway()`. Window 256 KB, max frame payload 32 KB.

## API reference (current names)

**Server**: `Server(int maxConnection, int shards=cpus, size_t bufferPoolSize=200MB, int maxEvents=1024)`, `AddNewListener(port, bindIP, factory, ctx)`, `setOnAccepted`, `start`, `stop`, `isRunning`, `getShard(i)` (nullptr if out of range), `getRoundRobinShard`, `shardCount`, `setMaxConnectionsPerShard`, `setUseGarbageCollector(bool)` (default `false`: dead objects deleted at the end of every loop iteration; `true`: kept at least 10 s).

**EpollReactor**: `post`, `wake`, `addTimer`, `removeTimer`, `id`, `userData/setUserData`, `connectionCount`, `tcpConnectionCount`, `setMaxConnections`, `setMultiAccept(bool)` (default true, up to 64 accepts per event), `setDeferAccept(sec)`, `add_listener(port, bindIP, factory, ctx)`, `remove_listener(fd)`, `getIPbyName(host, cb, arg, A|AAAA)` (callback may run before it returns), `cancelDNS(arg)`, `deleteLater`, `bufferPool`, `liveToken`. Normal code does not call `register_fd/add_fd/del_fd`.

**TCPSocket** (status values `Ready, Connecting, Handshaking, Connected, Closing, Closed`):
- Hooks: `onAccepted`, `onConnecting`, `onConnected`, `onConnectFailed`, `onReceiveData`, `onPause`, `onResume`, `onReadEOF`, `onClose`. Matching `setOnXxx(fn, arg)`; function types `OnDataFn = void(*)(void*, const uint8_t*, size_t)`, the others `void(*)(void*)`.
- I/O: `send`, `sendv(iovec*, n)`, `sendNoCopy`, `sendFile`, `close(force=false)`, `shutdownWrite`, `connectTo`, `pause_reading`, `resume_reading`.
- Config: `enableTLS`, `setAllowHalfClose`, `setAutoDelete`, `setIdleTimeout(sec)` (0 = off, default), `setMaxSendQueue(bytes)`, static `setSocketNoDelay/KeepAlive/Cork/LowDelay(fd, bool)`.
- Info: `fd`, `getStatus`, `closeReason`, `getErrorCode`, `getPeerAddress(ip, port)`, `getLocalPort`, `pendingBytes`, `isWriteBlocked`, `isReadPaused`, `isAccepted`, `isTLS`, `tlsVersion`, `tlsALPN`, `tlsContext`, `getReactor`.

**TLSContext**: `isSupported()`, `createServer`, `createClient`, `setALPN({"h2","http/1.1"})`, `enableAsyncPrivateKey(threads)`, `lastErrorString()`.

**Log** (namespace-level functions): `netrector::setLogHandler(void(*)(int level, const char* msg))`, `setLogLevel(int)`; levels `LOG_ERROR, LOG_WARN, LOG_INFO` (default), `LOG_DEBUG`; default sink is stderr.

## Constants (`src/constants.h`, `src/clsMultiplexedTunnel.h`)

Read buffer 64 KB per reactor; send pool 200 MB per shard (falls back to `malloc` when full, connections are not dropped); `BACK_PRESSURE` 128 KB; `LOW_WATERMARK` 64 KB; `MAX_SEND_QUEUE` 64 MB; `MAX_ACCEPT_PER_EVENT` 64; `MAX_READS_PER_EVENT` 16; connect/handshake timeout 15 s; close drain 60 s; linger 5 s; keepalive 240 s idle, 30 s interval, 4 probes; DNS timeout 2 s, 3 retries, cache TTL 5 min, 2000 entries.

## Known gaps

IPv4 nameservers only; no DNS TCP fallback or negative cache; no `AF_UNIX`; cannot bind outbound source IP; no `SO_LINGER`/`TCP_USER_TIMEOUT` setters; no UDP send queue; `Server::stop` is not graceful (`close(true)`); sample SOCKS5 apps have no authentication (default listen `127.0.0.1`); long soak runs untested.

## Build and test

CMake (`CMakeLists.txt`) is the only build system. It builds the static library `build/lib/libnetRector.a` (public headers copied to `build/include`) and the example app `build/netRector`.

BoringSSL is optional: if it is not found CMake prints a warning and builds without TLS (also `-DNETRECTOR_ENABLE_TLS=OFF`; the example binary shrinks from ~29 MB to ~0.3 MB). To get TLS run `scripts/build_boringssl.sh` once (output `libs/boringssl/{include,lib}`; elsewhere: `-DBORINGSSL_DIR=...` or env `BORINGSSL_DIR`). Then `scripts/build.sh`, or by hand `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j`. CMake options: `NETRECTOR_BUILD_EXAMPLE` (default ON when top-level), `NETRECTOR_STATIC` (static example binary). Using the library from another CMake project: `add_subdirectory(netRector)` and link target `netRector` (it brings the include path and `NETRECTOR_HAS_TLS`). Compiling the sources by hand with TLS needs `-DNETRECTOR_HAS_TLS=1` and the BoringSSL include path. ASan/UBSan build: `scripts/build_asan.sh` (`build-asan/netRector`). Static release binaries via Docker: `scripts/build_release.sh`. Dev image for non-Linux hosts: `docker/`.

```sh
python3 tests/run_tests.py [--bin build-asan/netRector]   # integration: direct, tunnel, tls, tls-ca, py, py-tls
tests/muxstress/build.sh && tests/muxstress/run_muxstress.sh asan   # run after changing TCPSocket or MultiplexedTunnel
```

`src/tlsf.c` and `src/tlsf.h` are vendored third-party code: do not edit. Layout: `src/` library, `example/` example app, `tests/` integration/stress tests and the Python Yamux server (`tests/yamux_server/`), `bench/` nginx comparison (`bench/RESULTS.md`), `scripts/` build helpers, `docker/` dev image, `memory/` maintainer notes.
