#ifndef HTTPTEST_H
#define HTTPTEST_H

// Test/benchmark: yek HTTP/1.1 server-e minimal roye TCPSocket (plain ya TLS) baraye
// moghayese ba nginx. Faghat GET; keep-alive va pipelining; javab-e sabet (sendNoCopy)
// ya yek file (sendFile / zero-copy).

#include "TestCommon.h"
#include "clsTCPSocket.h"
#include <string>
#include <strings.h>

struct HttpTestConfig {
    std::string response;       // header + body-e amade (keep-alive)
    std::string responseClose;  // haman ba "Connection: close"
    std::string fileHeader;     // header-e file (keep-alive); body ba sendFile
    int fileFd = -1;
    size_t fileSize = 0;
};

extern HttpTestConfig g_http;

class HttpConn : public TCPSocket {
public:
    void onReceiveData(const uint8_t* data, size_t len) override {
        const char* p = reinterpret_cast<const char*>(data);
        size_t n = len;

        // halate adi: darkhast kamel dar hamin buffer, bedone copy
        if (m_partial.empty()) {
            if (!consume(p, n))
                return;
            if (n > 0)
                m_partial.assign(p, n);     // tike-ye naghes
            return;
        }

        m_partial.append(p, n);
        if (m_partial.size() > 16 * 1024) {
            close(true);    // header bish az had bozorg
            return;
        }
        const char* q = m_partial.data();
        size_t m = m_partial.size();
        if (!consume(q, m))
            return;
        m_partial.erase(0, m_partial.size() - m);
    }

private:
    std::string m_partial;

    // darkhast haye kamel ro pardazesh mikone; p/n be baghimande eshare mikonan.
    // false: socket baste shod
    bool consume(const char*& p, size_t& n) {
        while (n > 0) {
            const char* end = static_cast<const char*>(memmem(p, n, "\r\n\r\n", 4));
            if (!end)
                return true;

            size_t reqLen = (size_t)(end - p) + 4;
            bool closeAfter = hasConnectionClose(p, reqLen);

            if (g_http.fileFd != -1) {
                send(g_http.fileHeader.data(), g_http.fileHeader.size());
                sendFile(g_http.fileFd, 0, g_http.fileSize);
            } else if (closeAfter) {
                sendNoCopy(g_http.responseClose.data(), g_http.responseClose.size());
            } else {
                sendNoCopy(g_http.response.data(), g_http.response.size());
            }

            p += reqLen;
            n -= reqLen;

            if (closeAfter) {
                close();
                return false;
            }
        }
        return true;
    }

    static bool hasConnectionClose(const char* p, size_t n) {
        // jostojoo-ye "connection:" (case-insensitive) va meghdar "close"
        for (size_t i = 0; i + 11 <= n; i++) {
            if ((p[i] == 'C' || p[i] == 'c') && strncasecmp(p + i, "connection:", 11) == 0) {
                size_t j = i + 11;
                while (j < n && (p[j] == ' ' || p[j] == '\t')) j++;
                return j + 5 <= n && strncasecmp(p + j, "close", 5) == 0;
            }
        }
        return false;
    }
};

#endif // HTTPTEST_H
