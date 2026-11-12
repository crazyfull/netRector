#include "findHeaderValue.h"

#include <cctype>

namespace {

bool equalsIgnoreCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

std::string_view trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

} // namespace

bool findHeaderValue(std::string_view headerBlock, std::string_view name, std::string_view& value)
{
    // the first line is the request/status line, not a header
    size_t position = headerBlock.find("\r\n");
    if (position == std::string_view::npos)
        return false;
    position += 2;

    while (position < headerBlock.size()) {
        size_t lineEnd = headerBlock.find("\r\n", position);
        if (lineEnd == std::string_view::npos)
            lineEnd = headerBlock.size();

        std::string_view line = headerBlock.substr(position, lineEnd - position);
        size_t colon = line.find(':');
        if (colon != std::string_view::npos && equalsIgnoreCase(trim(line.substr(0, colon)), name)) {
            value = trim(line.substr(colon + 1));
            return true;
        }

        position = lineEnd + 2;
    }
    return false;
}
