#ifndef SOCKS5REQUEST_H
#define SOCKS5REQUEST_H

#include "Socks5ReplyCode.h"

#include <cstddef>
#include <cstdint>
#include <string>

enum class Socks5Command : uint8_t {
    Connect = 0x01,     // TCP
    ForwardUdp = 0x05   // UDP-in-TCP of hev-socks5-tunnel (datagram frames over the stream)
};

// A parsed SOCKS5 request (CONNECT or FWD UDP).
struct Socks5Request {
    Socks5Command command = Socks5Command::Connect;
    std::string host;                                           // domain name or textual IP address
    uint16_t port = 0;
    size_t length = 0;                                          // bytes consumed from the input
    Socks5ReplyCode error = Socks5ReplyCode::GeneralFailure;    // reply to send when parsing fails
};

#endif // SOCKS5REQUEST_H
