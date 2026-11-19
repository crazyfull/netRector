#ifndef DESTINATIONPOLICY_H
#define DESTINATIONPOLICY_H

#include <cstdint>
#include <sys/socket.h>

// Which addresses the server may open connections / send datagrams to (TCP and UDP alike).
//
// By default loopback, private (RFC 1918), CGNAT, link-local (incl. the cloud metadata address
// 169.254.169.254), unique-local, multicast, broadcast and reserved addresses are refused, so the
// tunnel cannot be used to reach the server itself or its network. IPv4-mapped IPv6 addresses are
// judged by their IPv4 part. One exception: DNS on the server itself (127.0.0.1:53, [::1]:53).
// `allowPrivate` (--allow-private) switches the whole filter off.
namespace DestinationPolicy {

bool isAllowed(const sockaddr* address, bool allowPrivate);

// TCPSocket::ConnectFilterFn adapter; `argument` is a `const bool*` (allowPrivate).
bool connectFilter(void* argument, const sockaddr* address, socklen_t addressLength);

} // namespace DestinationPolicy

#endif // DESTINATIONPOLICY_H
