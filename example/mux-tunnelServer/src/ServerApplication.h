#ifndef SERVERAPPLICATION_H
#define SERVERAPPLICATION_H

#include "ServerConfig.h"
#include "ServerStats.h"
#include "WebSocketServerHandshake.h"

#include "clsTCPSocket.h"

#include <csignal>
#include <string>

// Owns the netRector server and the tunnel listener of mux-tunnelServer.
class ServerApplication {
public:
    explicit ServerApplication(const ServerConfig& config);

    // Runs until SIGINT / SIGTERM. Returns the process exit code.
    int run();

private:
    // accept factory of the tunnel listener
    static netrector::TCPSocket* createTunnel(void* context);

    void waitForShutdown(const sigset_t& signals);
    void handleCommand(const std::string& command);
    void printStats();

    ServerConfig m_config;
    WebSocketServerHandshake m_handshake;
    ServerStats m_stats;
};

#endif // SERVERAPPLICATION_H
