---
name: architecture-map
description: Layering and threading model of socketManager — reactor shards, socket layer, Yamux tunnel, SOCKS5 apps
metadata:
  type: project
---

Layers, bottom to top:

1. **Core** — `Server` (`src/clsServer`) creates N `EpollReactor` shards (`src/clsEpollReactor`) and runs each on its own thread. Each shard has its own listener socket (`SO_REUSEPORT`, dual-stack IPv6) and its own epoll fd in edge-triggered mode.
2. **Per-reactor services** (all owned by one shard, none thread-safe):
   - `SocketList` — array indexed by fd, each slot has a `genID`. The epoll key is `(genID << 32) | fd`, so stale events for a closed fd are dropped.
   - `BufferPool` — TLSF pool per shard (default 200 MB, ctor parameter) for send-queue chunks only; reads use one shared 64 KB buffer per reactor. When the pool is exhausted `SendQueue` falls back to malloc (connections are not dropped).
   - `DNSLookup` — async UDP resolver with LRU cache; callbacks may fire synchronously inside `resolve()` for IP literals and cache hits.
   - `TimerManager` — all timers multiplexed on one `timerfd`.
   - `GCList<ReactorObject>` — deferred deletion of `TCPSocket`/`UDPSocket`; flushed at the end of each loop iteration.
   - maintenance (timeouts) scans a slice of the fd table every 250 ms (full pass every 5 s) instead of all sockets at once.
3. **Socket** — `TCPSocket` (`src/clsTCPSocket`), plain or TLS via `enableTLS(TLSContext*)` (`src/clsTLSContext`, BoringSSL, one `SSL_CTX` shared by all shards; `enableAsyncPrivateKey(n)` moves handshake signing to a thread pool via `SSL_PRIVATE_KEY_METHOD`). `SendQueue` is an intrusive chunk chain inline in `SocketContext` (no heap when idle) with Pool/Heap/External (`sendNoCopy`)/File (`sendFile`, sendfile) chunks. `UDPSocket` (recvmmsg batches) and `Listener` are in `src/clsUDPSocket`, `src/clsListener`. Events are delivered either through C function-pointer callbacks (`setOnData` etc., all sharing one `m_callbacksArg`) or, if no callback is set, through virtual methods (`onReceiveData` etc.). Has a `SendQueue` and pause/resume backpressure (`BACK_PRESSURE` 128 KB, `LOW_WATERMARK` 64 KB).
4. **Tunnel** — `MultiplexedTunnel` (`src/clsMultiplexedTunnel`) inherits `TCPSocket` and implements Yamux framing (12-byte header, 256 KB initial window).
5. **Test apps (root)** — `Socks5LocalForwarder` (local SOCKS5 conn -> one stream; client side), `Socks5StreamHandler` (stream -> outbound socket; it *is* the outbound `TCPSocket`, so the reactor owns it), `Socks5DirectProxy.h` (session + outbound pair), `TunnelClient.h` (per-reactor tunnel with reconnect, stored in `EpollReactor::userData`). All app objects are heap `TCPSocket` subclasses deleted by the reactor after `onClose`.

Threading rule: everything belonging to a shard must be touched only from that shard's thread; from outside use `EpollReactor::post()`. Objects are deleted only at the end of a loop iteration (GCList), so pointers stay valid for the rest of any callback chain.

Perf decisions (2026-10-13, measured with bench/bench.sh vs nginx): listener sets NODELAY/keepalive once (accepted sockets inherit them on Linux — verified), close() without epoll_ctl(DEL), short recv == drained (except when the event carried RDHUP), multi-accept (up to 64 per event) is default because it beat one-accept-per-event by ~35% on connection-per-request load. Unused third-party headers: `src/concurrentqueue.h`, `lightweightsemaphore.h`.

Related: [[project-overview]], [[known-issues-review-2026-10]].
