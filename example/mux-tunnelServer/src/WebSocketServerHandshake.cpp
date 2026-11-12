#include "WebSocketServerHandshake.h"

#include "findHeaderValue.h"

#include <strings.h>

namespace {

constexpr size_t MaxRequestSize = 8192;
constexpr std::string_view HeaderTerminator = "\r\n\r\n";

bool equalsIgnoreCase(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && strncasecmp(a.data(), b.data(), a.size()) == 0;
}

} // namespace

WebSocketServerHandshake::WebSocketServerHandshake(const ServerConfig& config)
    : m_expectedKey(config.webSocketKey)
{
    m_acceptResponse = "HTTP/1.1 101 Switching Protocols\r\n"
                       "upgrade: websocket\r\n"
                       "Sec-WebSocket-Accept: " + config.webSocketAccept + "\r\n";
    for (const std::string& header : config.responseHeaders)
        m_acceptResponse += header + "\r\n";
    m_acceptResponse += "\r\n";

    m_rejectResponse = "HTTP/1.1 404 Not Found\r\n"
                       "Content-Length: 0\r\n"
                       "Connection: close\r\n"
                       "\r\n";
}

HandshakeResult WebSocketServerHandshake::evaluate(std::string_view received, size_t& consumed) const
{
    const size_t end = received.find(HeaderTerminator);
    if (end == std::string_view::npos)
        return received.size() > MaxRequestSize ? HandshakeResult::Rejected : HandshakeResult::Incomplete;

    consumed = end + HeaderTerminator.size();
    const std::string_view request = received.substr(0, consumed);

    if (request.substr(0, 4) != "GET ")
        return HandshakeResult::Rejected;

    std::string_view value;
    if (!findHeaderValue(request, "Upgrade", value) || !equalsIgnoreCase(value, "websocket"))
        return HandshakeResult::Rejected;

    if (!m_expectedKey.empty()) {
        if (!findHeaderValue(request, "Sec-WebSocket-Key", value) || value != m_expectedKey)
            return HandshakeResult::Rejected;
    }

    return HandshakeResult::Accepted;
}
