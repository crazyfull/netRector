#include "DestinationPolicy.h"

#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>

namespace {

constexpr uint16_t DnsPort = 53;

bool isPrivateIPv4(uint32_t host)    // host byte order
{
    const uint8_t a = static_cast<uint8_t>(host >> 24);
    const uint8_t b = static_cast<uint8_t>(host >> 16);
    return a == 0 || a == 10 || a == 127
        || (a == 100 && (b & 0xC0) == 64)               // 100.64.0.0/10 carrier-grade NAT
        || (a == 169 && b == 254)                       // link-local, cloud metadata
        || (a == 172 && (b & 0xF0) == 16)
        || (a == 192 && b == 168)
        || (a == 192 && b == 0 && static_cast<uint8_t>(host >> 8) == 0)   // 192.0.0.0/24
        || a >= 224;                                    // multicast, reserved, broadcast
}

} // namespace

bool DestinationPolicy::isAllowed(const sockaddr* address, bool allowPrivate)
{
    if (address->sa_family == AF_INET) {
        const sockaddr_in* in = reinterpret_cast<const sockaddr_in*>(address);
        if (in->sin_port == 0)
            return false;
        if (allowPrivate)
            return true;
        const uint32_t host = ntohl(in->sin_addr.s_addr);
        if (host == INADDR_LOOPBACK && ntohs(in->sin_port) == DnsPort)
            return true;                                // the server's own resolver
        return !isPrivateIPv4(host);
    }

    if (address->sa_family == AF_INET6) {
        const sockaddr_in6* in6 = reinterpret_cast<const sockaddr_in6*>(address);
        if (in6->sin6_port == 0)
            return false;
        if (allowPrivate)
            return true;

        const uint8_t* a = in6->sin6_addr.s6_addr;
        if (IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr)) {
            uint32_t v4;
            std::memcpy(&v4, a + 12, 4);
            const uint32_t host = ntohl(v4);
            if (host == INADDR_LOOPBACK && ntohs(in6->sin6_port) == DnsPort)
                return true;
            return !isPrivateIPv4(host);
        }
        if (IN6_IS_ADDR_LOOPBACK(&in6->sin6_addr) && ntohs(in6->sin6_port) == DnsPort)
            return true;
        return !(IN6_IS_ADDR_UNSPECIFIED(&in6->sin6_addr) || IN6_IS_ADDR_LOOPBACK(&in6->sin6_addr)
                 || (a[0] & 0xFE) == 0xFC                       // fc00::/7 unique local
                 || (a[0] == 0xFE && (a[1] & 0xC0) == 0x80)     // fe80::/10 link-local
                 || a[0] == 0xFF);                              // multicast
    }

    return false;
}

bool DestinationPolicy::connectFilter(void* argument, const sockaddr* address, socklen_t)
{
    return isAllowed(address, *static_cast<const bool*>(argument));
}
