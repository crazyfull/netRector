#ifndef PARSEUNSIGNED_H
#define PARSEUNSIGNED_H

#include <cstdint>

// Parses a complete decimal string. Returns false on empty input, trailing garbage or overflow.
bool parseUnsigned(const char* text, uint64_t& value);

#endif // PARSEUNSIGNED_H
