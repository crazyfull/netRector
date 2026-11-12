#ifndef DEFAULTSERVERCONFIG_H
#define DEFAULTSERVERCONFIG_H

#include <cstddef>
#include <cstdint>

// Static defaults of mux-tunnelServer. Edit them here and rebuild, or override any of them
// at run time with a command line option (see ServerCommandLine).
namespace DefaultServerConfig {

// Address the tunnel listener binds to (the CDN / client connects here).
constexpr const char* ListenAddress = "0.0.0.0";
constexpr uint16_t ListenPort = 9191;

// Websocket-style handshake. The client must send this Sec-WebSocket-Key (empty = accept any key).
constexpr const char* WebSocketKey = "dGhlIHNhbXBsZSBub25jzQ==";
// Value of the Sec-WebSocket-Accept header returned in the 101 response.
constexpr const char* WebSocketAccept = "dGhlIHNhbXBsZSBub25jzQ=";

// Mux ping interval in seconds (0 = off). Also keeps idle CDN connections alive.
constexpr uint32_t KeepAliveSeconds = 30;
// Time a new connection has to complete the handshake before it is dropped.
constexpr uint32_t HandshakeTimeoutSeconds = 10;
// Maximum number of concurrent streams (SOCKS5 connections) per tunnel.
constexpr size_t MaxStreamsPerTunnel = 32768;
// Reactor threads. 0 = one per CPU core.
constexpr int Threads = 0;

} // namespace DefaultServerConfig

#endif // DEFAULTSERVERCONFIG_H
