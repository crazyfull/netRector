# mux-tunnelServer

Exit side of the SOCKS5-over-mux tunnel. It accepts tunnel connections from `mux-tunnelClient`
(directly or through a CDN), performs every SOCKS5 `CONNECT` request carried by a mux stream and
relays the data back. Built on netRector (epoll, one reactor thread per CPU core by default).

```
application -> SOCKS5 -> mux-tunnelClient ==(mux tunnel, optional CDN)==> mux-tunnelServer -> target
```

## Handshake

After the TCP connection is accepted, the first bytes must be a websocket-style upgrade request.
Only after the 101 response are normal Yamux frames exchanged.

```
GET / HTTP/1.1                                 HTTP/1.1 101 Switching Protocols
Host: sv1.mojz.ir                              upgrade: websocket
Upgrade: websocket                  <--->      Sec-WebSocket-Accept: dGhlIHNhbXBsZSBub25jzQ=
Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jzQ==
Sec-WebSocket-Version: 13
```

Requests that are not `GET` + `Upgrade: websocket` + the configured key get `404 Not Found`
(the key is the only access control, so change it). A connection that does not finish the
handshake within `--handshake-timeout` seconds is dropped.

## TLS (HTTPS)

`--tls` accepts TLS on the tunnel listener; the websocket-style handshake is read inside the TLS
session.

* `--tls-cert FILE --tls-key FILE`: certificate chain and private key (PEM).
* If **both are omitted** a self-signed ECDSA P-256 certificate (10 years, CN and subjectAltName =
  `--tls-name`, default `mojz.ir`) is generated in memory at every start; nothing is written to
  disk. Clients then must not verify the certificate (the default of mux-tunnelClient).
* Giving only one of the two files is an error; a missing or unreadable file is an error (no silent
  fallback to a generated certificate).
* TLS needs BoringSSL at build time (`-DBORINGSSL_DIR=...`, built by `scripts/build_boringssl.sh`).
  Without it the program still builds, `--tls` is refused at start-up.

## Build (static)

```sh
cmake -S example/mux-tunnelServer -B build-server
cmake --build build-server -j
# -> build-server/mux-tunnelServer (fully static executable)
```

* netRector is built from source as a static library and linked in. Its location is the CMake
  variable `NETRECTOR_DIR` (default: the repository root, `../..`), e.g.
  `-DNETRECTOR_DIR=/path/to/netRector`.
* `-DMUXTUNNEL_STATIC=OFF` links dynamically (needed for sanitizer builds).
* TLS: `-DBORINGSSL_DIR=/path/to/boringssl` (enabled automatically when found).

## Configuration

Static defaults live in `src/DefaultServerConfig.h` (edit + rebuild). Every value can be
overridden at run time:

| option | default | meaning |
|---|---|---|
| `--listen IP:PORT` | `0.0.0.0:9191` | tunnel listener |
| `--ws-key KEY` | `dGhlIHNhbXBsZSBub25jzQ==` | required `Sec-WebSocket-Key` (empty = any) |
| `--ws-accept VALUE` | `dGhlIHNhbXBsZSBub25jzQ=` | `Sec-WebSocket-Accept` of the 101 response |
| `--response-header LINE` | - | extra 101 header, repeatable |
| `--tls` | off | accept TLS on the listener |
| `--tls-cert FILE`, `--tls-key FILE` | - | certificate and key; both omitted = self-signed generated at start-up |
| `--tls-name NAME` | `mojz.ir` | common name of the generated certificate |
| `--keepalive SEC` | 30 | mux ping interval, 0 = off |
| `--handshake-timeout SEC` | 10 | time allowed for the handshake |
| `--max-streams N` | 32768 | concurrent streams per tunnel |
| `--threads N` | 0 | reactor threads, 0 = CPU count |
| `-v` | | debug log |

## Runtime statistics

While the server runs, type `stats` + Enter in its terminal, or send `SIGUSR1` (works for a
daemon too: `kill -USR1 <pid>`). Example:

```
tunnels:            3 active   (3 accepted, 0 handshakes rejected)
channels (streams): 30 active   (30 total)
target connections: 30 active   (30 total)
  tunnel #1    127.0.0.1:46634        up 4s  channels 10 active / 10 total
```

A *channel* is one mux stream (one SOCKS5 request); a *target connection* is its outbound TCP
connection once connected. Implemented in `ServerStats` / `TunnelStats` (atomic counters, no
extra thread: the main thread waits on a signalfd and stdin).

## Source layout

One class or function per file. Classes and enums are `PascalCase`, functions and methods
`camelCase`.

| file | role |
|---|---|
| `main.cpp`, `ServerApplication` | start-up, signals, reactor threads, listener |
| `ServerTunnel` | tunnel connection: handshake, then Yamux; creates a `TargetConnection` per stream |
| `WebSocketServerHandshake` | validates the request, builds the 101 / 404 responses |
| `TargetConnection` | one stream: SOCKS5 request, connect to target, relay with back-pressure |
| `Socks5RequestParser`, `Socks5Reply`, ... | SOCKS5 `CONNECT` (IPv4, IPv6, domain names) |
| `ServerStats`, `TunnelStats` | runtime counters and the `stats` report |
| `ServerConfig`, `DefaultServerConfig.h`, `ServerCommandLine` | configuration |
