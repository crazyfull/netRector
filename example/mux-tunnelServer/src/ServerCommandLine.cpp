#include "ServerCommandLine.h"

#include "parseUnsigned.h"
#include "splitHostPort.h"

#include <cstdio>
#include <cstring>
#include <string>

CommandLineOutcome ServerCommandLine::parse(int argc, char** argv, ServerConfig& config)
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
        } else if (option == "--ws-key") {
            config.webSocketKey = value;
        } else if (option == "--ws-accept") {
            config.webSocketAccept = value;
        } else if (option == "--response-header") {
            config.responseHeaders.emplace_back(value);
        } else if (option == "--keepalive" || option == "--handshake-timeout" || option == "--max-streams" || option == "--threads") {
            if (!parseUnsigned(value, number)) {
                std::fprintf(stderr, "invalid value for %s: %s\n", option.c_str(), value);
                return CommandLineOutcome::Invalid;
            }
            if (option == "--keepalive")
                config.keepAliveSeconds = static_cast<uint32_t>(number);
            else if (option == "--handshake-timeout")
                config.handshakeTimeoutSeconds = static_cast<uint32_t>(number);
            else if (option == "--max-streams")
                config.maxStreamsPerTunnel = static_cast<size_t>(number);
            else
                config.threads = static_cast<int>(number);
        } else {
            std::fprintf(stderr, "unknown option: %s\n", option.c_str());
            printUsage(argv[0]);
            return CommandLineOutcome::Invalid;
        }
    }

    return CommandLineOutcome::Run;
}

void ServerCommandLine::printUsage(const char* program)
{
    std::fprintf(stderr,
        "usage: %s [options]\n"
        "\n"
        "Accepts mux tunnels from mux-tunnelClient and performs the SOCKS5 requests they carry.\n"
        "\n"
        "options (defaults come from DefaultServerConfig.h):\n"
        "  --listen IP:PORT          tunnel listener (default %s:%u)\n"
        "  --ws-key KEY              required Sec-WebSocket-Key, empty = accept any\n"
        "  --ws-accept VALUE         Sec-WebSocket-Accept sent in the 101 response\n"
        "  --response-header LINE    extra header of the 101 response, e.g. \"Connection: Upgrade\" (repeatable)\n"
        "  --keepalive SEC           mux ping interval, 0 = off (default %u)\n"
        "  --handshake-timeout SEC   time allowed for the handshake (default %u)\n"
        "  --max-streams N           concurrent streams per tunnel (default %zu)\n"
        "  --threads N               reactor threads, 0 = one per CPU core (default %d)\n"
        "  -v, --verbose             debug logging\n"
        "  -h, --help                this text\n",
        program,
        DefaultServerConfig::ListenAddress, DefaultServerConfig::ListenPort,
        DefaultServerConfig::KeepAliveSeconds, DefaultServerConfig::HandshakeTimeoutSeconds,
        DefaultServerConfig::MaxStreamsPerTunnel, DefaultServerConfig::Threads);
}
