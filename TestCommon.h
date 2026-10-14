#ifndef TESTCOMMON_H
#define TESTCOMMON_H

// Abzar haye moshtarak barname haye test (SOCKS5 parser, log, tanzimat).
// In file ha joze ketabkhane nistan; faghat baraye test-e ketabkhane dar sharayete vaghei.

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <arpa/inet.h>

extern bool g_verbose;

#define LOG(fmt, ...)  do { fprintf(stderr, fmt "\n", ##__VA_ARGS__); } while (0)
#define VLOG(fmt, ...) do { if (g_verbose) fprintf(stderr, fmt "\n", ##__VA_ARGS__); } while (0)

class TLSContext;

struct TestConfig {
    // client
    std::string serverHost;
    uint16_t serverPort = 9191;

    // listen (client: SOCKS5 mahali, server: tunnel, socks5: proxy)
    std::string listenIP;
    uint16_t listenPort = 0;

    TLSContext* tlsCtx = nullptr;   // nullptr = bedone TLS
    std::string sni;
    uint32_t keepAlive = 30;        // sanie; 0 = khamosh
    uint32_t statsInterval = 0;     // sanie; 0 = khamosh
    int threads = 1;
};

namespace socks5 {

enum ParseResult { Incomplete, Invalid, Ok };

// REP codes
constexpr uint8_t REP_OK = 0x00;
constexpr uint8_t REP_GENERAL_FAILURE = 0x01;
constexpr uint8_t REP_NETWORK_UNREACHABLE = 0x03;
constexpr uint8_t REP_HOST_UNREACHABLE = 0x04;
constexpr uint8_t REP_CONNECTION_REFUSED = 0x05;
constexpr uint8_t REP_COMMAND_NOT_SUPPORTED = 0x07;
constexpr uint8_t REP_ADDRESS_NOT_SUPPORTED = 0x08;

constexpr size_t REPLY_SIZE = 10;

struct Request {
    std::string host;
    uint16_t port = 0;
    size_t length = 0;      // tedad byte haye masraf shode
    uint8_t error = 0;      // REP monaseb dar halate Invalid
};

// greeting: VER NMETHODS METHODS...
inline ParseResult parseGreeting(const uint8_t* p, size_t n, size_t& consumed, bool& noAuth)
{
    if (n < 2)
        return Incomplete;
    if (p[0] != 0x05)
        return Invalid;

    size_t nmethods = p[1];
    if (n < 2 + nmethods)
        return Incomplete;

    noAuth = false;
    for (size_t i = 0; i < nmethods; i++) {
        if (p[2 + i] == 0x00) {
            noAuth = true;
            break;
        }
    }

    consumed = 2 + nmethods;
    return Ok;
}

// request: VER CMD RSV ATYP ADDR PORT
inline ParseResult parseRequest(const uint8_t* p, size_t n, Request& out)
{
    if (n < 4)
        return Incomplete;

    if (p[0] != 0x05) {
        out.error = REP_GENERAL_FAILURE;
        return Invalid;
    }

    uint8_t atyp = p[3];
    size_t addrLen = 0;
    size_t addrOff = 4;
    if (atyp == 0x01) {
        addrLen = 4;
    } else if (atyp == 0x03) {
        if (n < 5)
            return Incomplete;
        addrLen = p[4];
        addrOff = 5;
        if (addrLen == 0) {
            out.error = REP_GENERAL_FAILURE;
            return Invalid;
        }
    } else if (atyp == 0x04) {
        addrLen = 16;
    } else {
        out.error = REP_ADDRESS_NOT_SUPPORTED;
        return Invalid;
    }

    size_t total = addrOff + addrLen + 2;
    if (n < total)
        return Incomplete;

    // faghat CONNECT
    if (p[1] != 0x01) {
        out.error = REP_COMMAND_NOT_SUPPORTED;
        out.length = total;
        return Invalid;
    }

    char ip[INET6_ADDRSTRLEN];
    if (atyp == 0x01) {
        inet_ntop(AF_INET, p + addrOff, ip, sizeof(ip));
        out.host = ip;
    } else if (atyp == 0x04) {
        inet_ntop(AF_INET6, p + addrOff, ip, sizeof(ip));
        out.host = ip;
    } else {
        out.host.assign(reinterpret_cast<const char*>(p + addrOff), addrLen);
        // esm nabayad byte sefr dashte bashe
        if (out.host.find('\0') != std::string::npos) {
            out.error = REP_GENERAL_FAILURE;
            return Invalid;
        }
    }

    out.port = (uint16_t)((p[addrOff + addrLen] << 8) | p[addrOff + addrLen + 1]);
    out.length = total;
    return Ok;
}

// javab 10 byte-i: VER REP RSV ATYP(IPv4) 0.0.0.0:0
inline void buildReply(uint8_t rep, uint8_t out[REPLY_SIZE])
{
    memset(out, 0, REPLY_SIZE);
    out[0] = 0x05;
    out[1] = rep;
    out[3] = 0x01;
}

inline uint8_t replyFromErrno(int err)
{
    switch (err) {
    case ECONNREFUSED: return REP_CONNECTION_REFUSED;
    case ENETUNREACH:  return REP_NETWORK_UNREACHABLE;
    case EHOSTUNREACH:
    case ETIMEDOUT:    return REP_HOST_UNREACHABLE;
    default:           return REP_GENERAL_FAILURE;
    }
}

} // namespace socks5

#endif // TESTCOMMON_H
