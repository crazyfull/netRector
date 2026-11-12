#include "parseUnsigned.h"

#include <cerrno>
#include <cstdlib>

bool parseUnsigned(const char* text, uint64_t& value)
{
    if (!text || *text == '\0' || *text == '-')
        return false;

    char* end = nullptr;
    errno = 0;
    unsigned long long parsed = std::strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0')
        return false;

    value = parsed;
    return true;
}
