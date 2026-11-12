#ifndef SOCKS5REPLYCODE_H
#define SOCKS5REPLYCODE_H

#include <cstdint>

// REP field of a SOCKS5 reply (RFC 1928, section 6).
enum class Socks5ReplyCode : uint8_t {
    Succeeded = 0x00,
    GeneralFailure = 0x01,
    NetworkUnreachable = 0x03,
    HostUnreachable = 0x04,
    ConnectionRefused = 0x05,
    CommandNotSupported = 0x07,
    AddressTypeNotSupported = 0x08
};

#endif // SOCKS5REPLYCODE_H
