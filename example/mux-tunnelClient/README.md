# mux-tunnelClient

Entry side of the SOCKS5-over-mux tunnel. It listens for SOCKS5 connections (default
`0.0.0.0:1080`), carries each one over a mux (Yamux) stream to `mux-tunnelServer`, and relays the
answers back. Built on netRector (epoll).

```
application -> SOCKS5 -> mux-tunnelClient ==(mux tunnel, optional CDN)==> mux-tunnelServer -> target
```

* The SOCKS5 greeting is answered locally; the `CONNECT` request and the reply travel through the
  tunnel, so no extra round trip is added.
* 1 - 16 tunnels (`--tunnels`, default 1). New connections are spread over the ready tunnels
  round-robin. A lost tunnel is reconnected automatically (0.5 s, doubling up to 10 s); while no
  tunnel is ready, new SOCKS5 requests are answered with "general failure".
* With `--threads N` the tunnels are divided between N reactor threads (never more threads than
  tunnels); each thread serves the connections it accepts with its own tunnels.

## Handshake

Right after the TCP connection to the CDN (ip:port) is established, and before anything else, the
client sends the websocket-style upgrade request and waits for the 101 response. Only then are
Yamux frames exchanged.

```
GET / HTTP/1.1                                 HTTP/1.1 101 Switching Protocols
Host: sv1.mojz.ir                              upgrade: websocket
Upgrade: websocket                  <--->      Sec-WebSocket-Accept: dGhlIHNhbXBsZSBub25jzQ=
Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jzQ==
Sec-WebSocket-Version: 13
```

The request is built from `--host`, `--path`, `--ws-key` and any number of `--header` lines; the
response must be `101` with the expected `Sec-WebSocket-Accept` (`--ws-accept ""` skips that check).

## Build (static)

```sh
cmake -S example/mux-tunnelClient -B build-client
cmake --build build-client -j
# -> build-client/mux-tunnelClient (fully static executable)
```

* netRector is built from source as a static library and linked in. Its location is the CMake
  variable `NETRECTOR_DIR` (default: the repository root, `../..`), e.g.
  `-DNETRECTOR_DIR=/path/to/netRector`.
* `-DMUXTUNNEL_STATIC=OFF` links dynamically (needed for sanitizer builds).
* TLS is off. Later: `-DNETRECTOR_ENABLE_TLS=ON -DBORINGSSL_DIR=...`, then call
  `enableTLS()` in `TunnelPool::connectSlot()` (marked there); the handshake request is sent after
  the TLS handshake without further changes.

## Configuration

Static defaults live in `src/DefaultClientConfig.h` (edit + rebuild). Every value can be
overridden at run time:

| option | default | meaning |
|---|---|---|
| `--listen IP:PORT` | `0.0.0.0:1080` | local SOCKS5 listener |
| `--cdn IP:PORT` | `127.0.0.1:9191` | where the tunnels connect (CDN edge or the server) |
| `--host NAME` | `sv1.mojz.ir` | `Host` header of the upgrade request |
| `--path PATH` | `/` | request path |
| `--ws-key KEY` | `dGhlIHNhbXBsZSBub25jzQ==` | `Sec-WebSocket-Key` (must match the server) |
| `--ws-accept VALUE` | `dGhlIHNhbXBsZSBub25jzQ=` | expected `Sec-WebSocket-Accept`, empty = no check |
| `--header LINE` | - | extra request header, repeatable |
| `--tunnels N` | 1 | number of mux tunnels, 1-16 |
| `--keepalive SEC` | 30 | mux ping interval, 0 = off |
| `--handshake-timeout SEC` | 10 | time allowed for connect + handshake |
| `--max-streams N` | 32768 | concurrent streams per tunnel (keep <= the server's) |
| `--threads N` | 1 | reactor threads, at most `--tunnels` |
| `-v` | | debug log |

## Source layout

One class or function per file. Classes and enums are `PascalCase`, functions and methods
`camelCase`.

| file | role |
|---|---|
| `main.cpp`, `ClientApplication` | start-up, signals, reactor threads, listener |
| `TunnelPool` (+ `TunnelSlot`) | the tunnels of one thread: reconnect, round-robin stream assignment |
| `ClientTunnel` | one tunnel connection: handshake, then Yamux |
| `WebSocketClientHandshake` | builds the request, validates the 101 response |
| `LocalSocksConnection` | one SOCKS5 client: greeting, then relay through a stream with back-pressure |
| `Socks5GreetingParser`, `Socks5Reply` | SOCKS5 pieces needed on the client |
| `ClientConfig`, `DefaultClientConfig.h`, `ClientCommandLine` | configuration |
