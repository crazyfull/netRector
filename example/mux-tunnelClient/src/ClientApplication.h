#ifndef CLIENTAPPLICATION_H
#define CLIENTAPPLICATION_H

#include "ClientConfig.h"
#include "WebSocketClientHandshake.h"

#include "clsTCPSocket.h"

// Owns the netRector server, the local SOCKS5 listener and the tunnel pools of mux-tunnelClient.
class ClientApplication {
public:
    explicit ClientApplication(const ClientConfig& config);

    // Runs until SIGINT / SIGTERM. Returns the process exit code.
    int run();

private:
    // accept factory of the local SOCKS5 listener
    static netrector::TCPSocket* createLocalConnection(void* context);

    ClientConfig m_config;
    WebSocketClientHandshake m_handshake;
};

#endif // CLIENTAPPLICATION_H
