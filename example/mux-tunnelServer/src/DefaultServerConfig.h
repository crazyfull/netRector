#ifndef DEFAULTSERVERCONFIG_H
#define DEFAULTSERVERCONFIG_H

#include <cstddef>
#include <cstdint>

// Static defaults of mux-tunnelServer. Edit them here and rebuild, or override any of them
// at run time with a command line option (see ServerCommandLine).
namespace DefaultServerConfig {

// Address the tunnel listener binds to (the CDN / client connects here).
constexpr const char* ListenAddress = "0.0.0.0";
constexpr uint16_t ListenPort = 8080;

// Websocket-style handshake. The client must send this Sec-WebSocket-Key (empty = accept any key).
constexpr const char* WebSocketKey = "dGhlIHNhbXBsZSBub25jzQ==";
// Value of the Sec-WebSocket-Accept header returned in the 101 response.
constexpr const char* WebSocketAccept = "dGhlIHNhbXBsZSBub25jzQ=";

// TLS (HTTPS) on the tunnel listener. The handshake request is read inside the TLS session.
constexpr bool UseTls = false;
// Certificate chain and private key (PEM). When both are empty a self-signed certificate is
// generated at start-up (nothing is written to disk).
constexpr const char* TlsCertFile = "";
constexpr const char* TlsKeyFile = "";
// Name of the generated self-signed certificate.
constexpr const char* TlsCommonName = "mojz.ir";

// Mux ping interval in seconds (0 = off). Also keeps idle CDN connections alive.
constexpr uint32_t KeepAliveSeconds = 30;
// Time a new connection has to complete the handshake before it is dropped.
constexpr uint32_t HandshakeTimeoutSeconds = 10;
// Maximum number of concurrent streams (SOCKS5 connections) per tunnel.
constexpr size_t MaxStreamsPerTunnel = 32768;
// UDP-over-TCP (SOCKS5 FWD UDP of hev-socks5-tunnel, used by the apps for games / calls).
// Each UDP stream of a client gets its own UDP socket on this server.
constexpr bool UdpEnabled = true;
// A UDP stream without any datagram in either direction for this long is closed (0 = never).
constexpr uint32_t UdpIdleSeconds = 120;
// Concurrent UDP streams per tunnel.
constexpr size_t UdpMaxStreamsPerTunnel = 256;
// Largest datagram that is relayed (bigger ones are dropped). The frame length is 16 bit.
constexpr size_t UdpMaxDatagram = 8192;
// Allow UDP towards loopback / private / link-local / multicast addresses (off: dropped, which
// keeps the tunnel from being used to reach the server's own network).
constexpr bool UdpAllowPrivate = false;
// Reactor threads. 0 = one per CPU core.
constexpr int Threads = 0;

} // namespace DefaultServerConfig

#endif // DEFAULTSERVERCONFIG_H
