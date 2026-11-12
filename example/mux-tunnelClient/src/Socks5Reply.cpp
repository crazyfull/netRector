#include "Socks5Reply.h"

#include <cstring>

void Socks5Reply::build(Socks5ReplyCode code, uint8_t (&reply)[Size])
{
    std::memset(reply, 0, Size);
    reply[0] = 0x05;
    reply[1] = static_cast<uint8_t>(code);
    reply[3] = 0x01;
}
