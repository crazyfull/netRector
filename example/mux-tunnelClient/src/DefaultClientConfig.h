#ifndef DEFAULTCLIENTCONFIG_H
#define DEFAULTCLIENTCONFIG_H

#include <cstddef>
#include <cstdint>

// Static defaults of mux-tunnelClient. Edit them here and rebuild, or override any of them
// at run time with a command line option (see ClientCommandLine).
namespace DefaultClientConfig {

// Local SOCKS5 listener.
constexpr const char* ListenAddress = "0.0.0.0";
constexpr uint16_t ListenPort = 1080;

// Where the tunnels connect to: the CDN edge (or mux-tunnelServer itself when there is no CDN).
constexpr const char* CdnAddress = "irc.x81.ir";
constexpr uint16_t CdnPort = 8080;

// Websocket-style handshake sent as the first bytes of every tunnel connection.
constexpr const char* HandshakeHost = "mojz.ir";    //bot.mojz.ir
constexpr const char* HandshakePath = "/";
constexpr const char* WebSocketKey = "dGhlIHNhbXBsZSBub25jzQ==";
// Value of Sec-WebSocket-Accept the 101 response must contain (empty = do not check).
constexpr const char* ExpectedWebSocketAccept = "dGhlIHNhbXBsZSBub25jzQ=";

// Number of mux tunnels (1 - 16). New SOCKS5 connections are spread over them round-robin.
constexpr int MinTunnelCount = 1;
constexpr int MaxTunnelCount = 16;
constexpr int TunnelCount = 1;

// Mux ping interval in seconds (0 = off). Also keeps idle CDN connections alive.
constexpr uint32_t KeepAliveSeconds = 30;
// Time a tunnel has to complete connect + handshake before it is dropped and retried.
constexpr uint32_t HandshakeTimeoutSeconds = 10;
// Time a local SOCKS5 client has to finish the greeting.
constexpr uint32_t LocalGreetingTimeoutSeconds = 30;
// Maximum number of concurrent streams (SOCKS5 connections) per tunnel. Keep it <= the server's.
constexpr size_t MaxStreamsPerTunnel = 32768;
// Reactor threads. Never more than TunnelCount; the tunnels are divided between the threads.
constexpr int Threads = 1;

// Delay before a lost tunnel is re-established; doubles on every failure up to the maximum.
constexpr int ReconnectInitialDelayMs = 500;
constexpr int ReconnectMaxDelayMs = 10000;

} // namespace DefaultClientConfig

#endif // DEFAULTCLIENTCONFIG_H
