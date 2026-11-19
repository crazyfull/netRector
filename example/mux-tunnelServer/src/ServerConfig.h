#ifndef SERVERCONFIG_H
#define SERVERCONFIG_H

#include "DefaultServerConfig.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Run-time configuration of mux-tunnelServer. Every field starts with its static default.
struct ServerConfig {
    std::string listenAddress = DefaultServerConfig::ListenAddress;
    uint16_t listenPort = DefaultServerConfig::ListenPort;

    std::string webSocketKey = DefaultServerConfig::WebSocketKey;
    std::string webSocketAccept = DefaultServerConfig::WebSocketAccept;
    std::vector<std::string> responseHeaders;   // extra "Name: value" lines of the 101 response

    bool useTls = DefaultServerConfig::UseTls;
    std::string tlsCertFile = DefaultServerConfig::TlsCertFile;
    std::string tlsKeyFile = DefaultServerConfig::TlsKeyFile;
    std::string tlsCommonName = DefaultServerConfig::TlsCommonName;

    uint32_t keepAliveSeconds = DefaultServerConfig::KeepAliveSeconds;
    uint32_t handshakeTimeoutSeconds = DefaultServerConfig::HandshakeTimeoutSeconds;
    size_t maxStreamsPerTunnel = DefaultServerConfig::MaxStreamsPerTunnel;
    bool udpEnabled = DefaultServerConfig::UdpEnabled;
    uint32_t udpIdleSeconds = DefaultServerConfig::UdpIdleSeconds;
    size_t udpMaxStreamsPerTunnel = DefaultServerConfig::UdpMaxStreamsPerTunnel;
    size_t udpMaxDatagram = DefaultServerConfig::UdpMaxDatagram;
    bool allowPrivate = DefaultServerConfig::AllowPrivate;
    int threads = DefaultServerConfig::Threads;
    bool verbose = false;
};

#endif // SERVERCONFIG_H
