#include "Socks5RequestParser.h"

#include <arpa/inet.h>

namespace {

constexpr uint8_t SocksVersion = 0x05;
constexpr uint8_t CommandConnect = 0x01;
constexpr uint8_t AddressTypeIPv4 = 0x01;
constexpr uint8_t AddressTypeDomain = 0x03;
constexpr uint8_t AddressTypeIPv6 = 0x04;

} // namespace

Socks5ParseResult Socks5RequestParser::parse(const uint8_t* data, size_t length, Socks5Request& request)
{
    if (length < 4)
        return Socks5ParseResult::Incomplete;

    if (data[0] != SocksVersion) {
        request.error = Socks5ReplyCode::GeneralFailure;
        return Socks5ParseResult::Invalid;
    }

    const uint8_t addressType = data[3];
    size_t addressOffset = 4;
    size_t addressLength = 0;

    if (addressType == AddressTypeIPv4) {
        addressLength = 4;
    } else if (addressType == AddressTypeIPv6) {
        addressLength = 16;
    } else if (addressType == AddressTypeDomain) {
        if (length < 5)
            return Socks5ParseResult::Incomplete;
        addressOffset = 5;
        addressLength = data[4];
        if (addressLength == 0) {
            request.error = Socks5ReplyCode::GeneralFailure;
            return Socks5ParseResult::Invalid;
        }
    } else {
        request.error = Socks5ReplyCode::AddressTypeNotSupported;
        return Socks5ParseResult::Invalid;
    }

    const size_t total = addressOffset + addressLength + 2;
    if (length < total)
        return Socks5ParseResult::Incomplete;

    request.length = total;

    if (data[1] != CommandConnect) {
        request.error = Socks5ReplyCode::CommandNotSupported;
        return Socks5ParseResult::Invalid;
    }

    char text[INET6_ADDRSTRLEN];
    if (addressType == AddressTypeIPv4) {
        inet_ntop(AF_INET, data + addressOffset, text, sizeof(text));
        request.host = text;
    } else if (addressType == AddressTypeIPv6) {
        inet_ntop(AF_INET6, data + addressOffset, text, sizeof(text));
        request.host = text;
    } else {
        request.host.assign(reinterpret_cast<const char*>(data + addressOffset), addressLength);
        if (request.host.find('\0') != std::string::npos) {
            request.error = Socks5ReplyCode::GeneralFailure;
            return Socks5ParseResult::Invalid;
        }
    }

    request.port = static_cast<uint16_t>((data[addressOffset + addressLength] << 8) | data[addressOffset + addressLength + 1]);
    return Socks5ParseResult::Ok;
}
