#ifndef SERVERAPPLICATION_H
#define SERVERAPPLICATION_H

#include "ServerConfig.h"
#include "WebSocketServerHandshake.h"

#include "clsTCPSocket.h"

// Owns the netRector server and the tunnel listener of mux-tunnelServer.
class ServerApplication {
public:
    explicit ServerApplication(const ServerConfig& config);

    // Runs until SIGINT / SIGTERM. Returns the process exit code.
    int run();

private:
    // accept factory of the tunnel listener
    static netrector::TCPSocket* createTunnel(void* context);

    ServerConfig m_config;
    WebSocketServerHandshake m_handshake;
};

#endif // SERVERAPPLICATION_H
