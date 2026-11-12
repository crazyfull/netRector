#ifndef CLIENTCONFIG_H
#define CLIENTCONFIG_H

#include "DefaultClientConfig.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Run-time configuration of mux-tunnelClient. Every field starts with its static default.
struct ClientConfig {
    std::string listenAddress = DefaultClientConfig::ListenAddress;
    uint16_t listenPort = DefaultClientConfig::ListenPort;

    std::string cdnAddress = DefaultClientConfig::CdnAddress;
    uint16_t cdnPort = DefaultClientConfig::CdnPort;

    std::string handshakeHost = DefaultClientConfig::HandshakeHost;
    std::string handshakePath = DefaultClientConfig::HandshakePath;
    std::string webSocketKey = DefaultClientConfig::WebSocketKey;
    std::string expectedWebSocketAccept = DefaultClientConfig::ExpectedWebSocketAccept;
    std::vector<std::string> requestHeaders;    // extra "Name: value" lines of the upgrade request

    int tunnelCount = DefaultClientConfig::TunnelCount;
    uint32_t keepAliveSeconds = DefaultClientConfig::KeepAliveSeconds;
    uint32_t handshakeTimeoutSeconds = DefaultClientConfig::HandshakeTimeoutSeconds;
    uint32_t localGreetingTimeoutSeconds = DefaultClientConfig::LocalGreetingTimeoutSeconds;
    size_t maxStreamsPerTunnel = DefaultClientConfig::MaxStreamsPerTunnel;
    int threads = DefaultClientConfig::Threads;
    bool verbose = false;
};

#endif // CLIENTCONFIG_H
