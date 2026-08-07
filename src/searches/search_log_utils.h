#ifndef FOREVERTAS_SEARCHES_SEARCH_LOG_UTILS_H
#define FOREVERTAS_SEARCHES_SEARCH_LOG_UTILS_H

#include <string>
#include <string_view>

namespace forevertas::detail {

// Values written between quotes in the line-oriented search log must not be
// able to terminate the field or inject another record.
inline std::string EscapeStructuredLogValue(std::string_view value) {
    constexpr char hexadecimal[] = "0123456789abcdef";
    std::string escaped;
    escaped.reserve(value.size());
    for (const unsigned char character : value) {
        switch (character) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            if (character < 0x20u || character == 0x7fu) {
                escaped += "\\x";
                escaped.push_back(hexadecimal[character >> 4u]);
                escaped.push_back(hexadecimal[character & 0x0fu]);
            } else {
                escaped.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    return escaped;
}

}  // namespace forevertas::detail

#endif
