#include "Socks5Reply.h"

#include <cerrno>
#include <cstring>

void Socks5Reply::build(Socks5ReplyCode code, uint8_t (&reply)[Size])
{
    std::memset(reply, 0, Size);
    reply[0] = 0x05;
    reply[1] = static_cast<uint8_t>(code);
    reply[3] = 0x01;
}

Socks5ReplyCode Socks5Reply::codeFromError(int error)
{
    switch (error) {
    case ECONNREFUSED:
        return Socks5ReplyCode::ConnectionRefused;
    case ENETUNREACH:
        return Socks5ReplyCode::NetworkUnreachable;
    case EHOSTUNREACH:
    case ETIMEDOUT:
        return Socks5ReplyCode::HostUnreachable;
    default:
        return Socks5ReplyCode::GeneralFailure;
    }
}
