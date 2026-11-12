#ifndef WEBSOCKETSERVERHANDSHAKE_H
#define WEBSOCKETSERVERHANDSHAKE_H

#include "HandshakeResult.h"
#include "ServerConfig.h"

#include <cstddef>
#include <string>
#include <string_view>

// Server side of the websocket-style handshake that precedes the mux protocol:
//
//   client -> "GET / HTTP/1.1 ... Upgrade: websocket ... Sec-WebSocket-Key: <key> ..."
//   server -> "HTTP/1.1 101 Switching Protocols ... Sec-WebSocket-Accept: <accept> ..."
//
// After the 101 response both sides speak plain mux frames. Instances are immutable and shared
// by all tunnels (the responses are built once).
class WebSocketServerHandshake {
public:
    explicit WebSocketServerHandshake(const ServerConfig& config);

    // Inspects the bytes received so far. On Accepted/Rejected `consumed` is the size of the
    // HTTP request (including the blank line); bytes after it already belong to the mux stream.
    HandshakeResult evaluate(std::string_view received, size_t& consumed) const;

    const std::string& acceptResponse() const { return m_acceptResponse; }
    const std::string& rejectResponse() const { return m_rejectResponse; }

private:
    std::string m_expectedKey;
    std::string m_acceptResponse;
    std::string m_rejectResponse;
};

#endif // WEBSOCKETSERVERHANDSHAKE_H
