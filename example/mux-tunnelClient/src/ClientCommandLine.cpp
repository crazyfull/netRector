#include "ClientCommandLine.h"

#include "parseUnsigned.h"
#include "splitHostPort.h"

#include <cstdio>
#include <string>

CommandLineOutcome ClientCommandLine::parse(int argc, char** argv, ClientConfig& config)
{
    for (int i = 1; i < argc; i++) {
        const std::string option = argv[i];

        if (option == "-h" || option == "--help") {
            printUsage(argv[0]);
            return CommandLineOutcome::ShowedHelp;
        }

        if (option == "-v" || option == "--verbose") {
            config.verbose = true;
            continue;
        }

        // all remaining options take one value
        if (i + 1 >= argc) {
            std::fprintf(stderr, "missing value for %s\n", option.c_str());
            return CommandLineOutcome::Invalid;
        }
        const char* value = argv[++i];
        uint64_t number = 0;

        if (option == "--listen") {
            if (!splitHostPort(value, config.listenAddress, config.listenPort)) {
                std::fprintf(stderr, "invalid --listen value: %s\n", value);
                return CommandLineOutcome::Invalid;
            }
        } else if (option == "--cdn") {
            if (!splitHostPort(value, config.cdnAddress, config.cdnPort)) {
                std::fprintf(stderr, "invalid --cdn value: %s\n", value);
                return CommandLineOutcome::Invalid;
            }
        } else if (option == "--host") {
            config.handshakeHost = value;
        } else if (option == "--path") {
            config.handshakePath = value;
        } else if (option == "--ws-key") {
            config.webSocketKey = value;
        } else if (option == "--ws-accept") {
            config.expectedWebSocketAccept = value;
        } else if (option == "--header") {
            config.requestHeaders.emplace_back(value);
        } else if (option == "--tunnels" || option == "--keepalive" || option == "--handshake-timeout"
                   || option == "--max-streams" || option == "--threads") {
            if (!parseUnsigned(value, number)) {
                std::fprintf(stderr, "invalid value for %s: %s\n", option.c_str(), value);
                return CommandLineOutcome::Invalid;
            }
            if (option == "--tunnels") {
                if (number < static_cast<uint64_t>(DefaultClientConfig::MinTunnelCount)
                    || number > static_cast<uint64_t>(DefaultClientConfig::MaxTunnelCount)) {
                    std::fprintf(stderr, "--tunnels must be between %d and %d\n",
                                 DefaultClientConfig::MinTunnelCount, DefaultClientConfig::MaxTunnelCount);
                    return CommandLineOutcome::Invalid;
                }
                config.tunnelCount = static_cast<int>(number);
            } else if (option == "--keepalive") {
                config.keepAliveSeconds = static_cast<uint32_t>(number);
            } else if (option == "--handshake-timeout") {
                config.handshakeTimeoutSeconds = static_cast<uint32_t>(number);
            } else if (option == "--max-streams") {
                config.maxStreamsPerTunnel = static_cast<size_t>(number);
            } else {
                config.threads = static_cast<int>(number);
            }
        } else {
            std::fprintf(stderr, "unknown option: %s\n", option.c_str());
            printUsage(argv[0]);
            return CommandLineOutcome::Invalid;
        }
    }

    return CommandLineOutcome::Run;
}

void ClientCommandLine::printUsage(const char* program)
{
    std::fprintf(stderr,
        "usage: %s [options]\n"
        "\n"
        "Accepts SOCKS5 connections and carries them over mux tunnels to mux-tunnelServer.\n"
        "\n"
        "options (defaults come from DefaultClientConfig.h):\n"
        "  --listen IP:PORT          local SOCKS5 listener (default %s:%u)\n"
        "  --cdn IP:PORT             address the tunnels connect to: CDN edge or server (default %s:%u)\n"
        "  --host NAME               Host header of the upgrade request (default %s)\n"
        "  --path PATH               request path (default %s)\n"
        "  --ws-key KEY              Sec-WebSocket-Key (default %s)\n"
        "  --ws-accept VALUE         expected Sec-WebSocket-Accept, empty = do not check (default %s)\n"
        "  --header LINE             extra request header, e.g. \"Connection: Upgrade\" (repeatable)\n"
        "  --tunnels N               number of mux tunnels, %d-%d (default %d)\n"
        "  --keepalive SEC           mux ping interval, 0 = off (default %u)\n"
        "  --handshake-timeout SEC   time allowed for connect + handshake (default %u)\n"
        "  --max-streams N           concurrent streams per tunnel (default %zu)\n"
        "  --threads N               reactor threads, at most --tunnels (default %d)\n"
        "  -v, --verbose             debug logging\n"
        "  -h, --help                this text\n",
        program,
        DefaultClientConfig::ListenAddress, DefaultClientConfig::ListenPort,
        DefaultClientConfig::CdnAddress, DefaultClientConfig::CdnPort,
        DefaultClientConfig::HandshakeHost, DefaultClientConfig::HandshakePath,
        DefaultClientConfig::WebSocketKey, DefaultClientConfig::ExpectedWebSocketAccept,
        DefaultClientConfig::MinTunnelCount, DefaultClientConfig::MaxTunnelCount, DefaultClientConfig::TunnelCount,
        DefaultClientConfig::KeepAliveSeconds, DefaultClientConfig::HandshakeTimeoutSeconds,
        DefaultClientConfig::MaxStreamsPerTunnel, DefaultClientConfig::Threads);
}
