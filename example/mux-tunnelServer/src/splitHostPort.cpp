#include "splitHostPort.h"

#include "parseUnsigned.h"

bool splitHostPort(const std::string& input, std::string& host, uint16_t& port)
{
    size_t colon = input.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= input.size())
        return false;

    uint64_t parsedPort = 0;
    if (!parseUnsigned(input.c_str() + colon + 1, parsedPort) || parsedPort == 0 || parsedPort > 65535)
        return false;

    std::string parsedHost = input.substr(0, colon);
    if (parsedHost.size() >= 2 && parsedHost.front() == '[' && parsedHost.back() == ']')
        parsedHost = parsedHost.substr(1, parsedHost.size() - 2);

    host = parsedHost;
    port = static_cast<uint16_t>(parsedPort);
    return true;
}
