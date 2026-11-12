#ifndef SOCKS5REQUEST_H
#define SOCKS5REQUEST_H

#include "Socks5ReplyCode.h"

#include <cstddef>
#include <cstdint>
#include <string>

// A parsed SOCKS5 CONNECT request.
struct Socks5Request {
    std::string host;                                           // domain name or textual IP address
    uint16_t port = 0;
    size_t length = 0;                                          // bytes consumed from the input
    Socks5ReplyCode error = Socks5ReplyCode::GeneralFailure;    // reply to send when parsing fails
};

#endif // SOCKS5REQUEST_H
