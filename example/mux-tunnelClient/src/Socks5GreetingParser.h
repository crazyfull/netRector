#ifndef SOCKS5GREETINGPARSER_H
#define SOCKS5GREETINGPARSER_H

#include "Socks5ParseResult.h"

#include <cstddef>
#include <cstdint>

// Parses the SOCKS5 method selection message "VER NMETHODS METHODS...".
class Socks5GreetingParser {
public:
    // `consumed` is set when the message is complete; `noAuthOffered` tells whether the client
    // offered "no authentication required" (method 0x00).
    static Socks5ParseResult parse(const uint8_t* data, size_t length, size_t& consumed, bool& noAuthOffered);
};

#endif // SOCKS5GREETINGPARSER_H
