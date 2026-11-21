#ifndef SOCKS5REPLY_H
#define SOCKS5REPLY_H

#include "Socks5ReplyCode.h"

#include <cstddef>
#include <cstdint>

// Builds the fixed-size SOCKS5 reply: "VER REP RSV ATYP(IPv4) 0.0.0.0:0".
class Socks5Reply {
public:
    static constexpr size_t Size = 10;

    static void build(Socks5ReplyCode code, uint8_t (&reply)[Size]);

    // Maps the errno of a failed connect() to the closest SOCKS5 reply code.
    static Socks5ReplyCode codeFromError(int error);
};

#endif // SOCKS5REPLY_H
