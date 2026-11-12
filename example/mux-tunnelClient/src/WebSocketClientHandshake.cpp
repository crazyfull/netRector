#include "WebSocketClientHandshake.h"

#include "findHeaderValue.h"

namespace {

constexpr size_t MaxResponseSize = 8192;
constexpr std::string_view HeaderTerminator = "\r\n\r\n";
constexpr std::string_view SwitchingProtocolsStatus = "HTTP/1.1 101";

} // namespace

WebSocketClientHandshake::WebSocketClientHandshake(const ClientConfig& config) : m_expectedAccept(config.expectedWebSocketAccept)
{
    m_request = "MUX " + config.handshakePath + " HTTP/1.1\r\n"
                "Host: " + config.handshakeHost + "\r\n"
                "upgrade: websocket\r\n"
                "Sec-WebSocket-Key: " + config.webSocketKey + "\r\n"
                "Sec-WebSocket-Version: 13\r\n";
    for (const std::string& header : config.requestHeaders)
        m_request += header + "\r\n";
    m_request += "\r\n";
}

HandshakeResult WebSocketClientHandshake::evaluate(std::string_view received, size_t& consumed) const
{
    const size_t end = received.find(HeaderTerminator);
    if (end == std::string_view::npos)
        return received.size() > MaxResponseSize ? HandshakeResult::Rejected : HandshakeResult::Incomplete;

    consumed = end + HeaderTerminator.size();
    const std::string_view response = received.substr(0, consumed);

    if (response.substr(0, SwitchingProtocolsStatus.size()) != SwitchingProtocolsStatus)
        return HandshakeResult::Rejected;

    if (!m_expectedAccept.empty()) {
        std::string_view value;
        if (!findHeaderValue(response, "Sec-WebSocket-Accept", value) || value != m_expectedAccept)
            return HandshakeResult::Rejected;
    }

    return HandshakeResult::Accepted;
}
