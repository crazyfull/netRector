#ifndef WEBSOCKETCLIENTHANDSHAKE_H
#define WEBSOCKETCLIENTHANDSHAKE_H

#include "ClientConfig.h"
#include "HandshakeResult.h"

#include <cstddef>
#include <string>
#include <string_view>

// Client side of the websocket-style handshake that precedes the mux protocol:
//
//   client -> "GET / HTTP/1.1 ... Upgrade: websocket ... Sec-WebSocket-Key: <key> ..."
//   server -> "HTTP/1.1 101 Switching Protocols ... Sec-WebSocket-Accept: <accept> ..."
//
// The request is the first thing sent on a new tunnel connection (after TCP, and after TLS if it
// is used); only after a valid 101 response are mux frames exchanged. Instances are immutable
// and shared by all tunnels (the request is built once from the configuration).
class WebSocketClientHandshake {
public:
    explicit WebSocketClientHandshake(const ClientConfig& config);

    const std::string& request() const { return m_request; }

    // Inspects the bytes received so far. On Accepted/Rejected `consumed` is the size of the
    // HTTP response (including the blank line); bytes after it already belong to the mux stream.
    HandshakeResult evaluate(std::string_view received, size_t& consumed) const;

private:
    std::string m_request;
    std::string m_expectedAccept;
};

#endif // WEBSOCKETCLIENTHANDSHAKE_H
