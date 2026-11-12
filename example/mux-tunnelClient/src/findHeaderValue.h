#ifndef FINDHEADERVALUE_H
#define FINDHEADERVALUE_H

#include <string_view>

// Looks up an HTTP header in a complete header block (start line + header lines).
// The name is compared case-insensitively; `value` is the trimmed header value.
bool findHeaderValue(std::string_view headerBlock, std::string_view name, std::string_view& value);

#endif // FINDHEADERVALUE_H
