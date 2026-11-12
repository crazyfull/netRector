#ifndef SOCKS5REQUESTPARSER_H
#define SOCKS5REQUESTPARSER_H

#include "Socks5ParseResult.h"
#include "Socks5Request.h"

#include <cstddef>
#include <cstdint>

// Parses "VER CMD RSV ATYP DST.ADDR DST.PORT". Only the CONNECT command is supported.
class Socks5RequestParser {
public:
    static Socks5ParseResult parse(const uint8_t* data, size_t length, Socks5Request& request);
};

#endif // SOCKS5REQUESTPARSER_H
