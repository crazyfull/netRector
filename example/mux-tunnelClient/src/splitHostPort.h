#ifndef SPLITHOSTPORT_H
#define SPLITHOSTPORT_H

#include <cstdint>
#include <string>

// Splits "host:port" or "[ipv6]:port". Returns false when the port is missing or out of range.
bool splitHostPort(const std::string& input, std::string& host, uint16_t& port);

#endif // SPLITHOSTPORT_H
