#include "Socks5GreetingParser.h"

Socks5ParseResult Socks5GreetingParser::parse(const uint8_t* data, size_t length, size_t& consumed, bool& noAuthOffered)
{
    if (length < 2)
        return Socks5ParseResult::Incomplete;

    if (data[0] != 0x05)
        return Socks5ParseResult::Invalid;

    const size_t methodCount = data[1];
    if (length < 2 + methodCount)
        return Socks5ParseResult::Incomplete;

    noAuthOffered = false;
    for (size_t i = 0; i < methodCount; i++) {
        if (data[2 + i] == 0x00) {
            noAuthOffered = true;
            break;
        }
    }

    consumed = 2 + methodCount;
    return Socks5ParseResult::Ok;
}
