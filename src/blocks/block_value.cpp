#include "blocks/block_value.h"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace forevertas::blocks {

std::optional<double> ParseNumberValue(const std::string &text) {
    if (text.empty()) return std::nullopt;
    // Reject values strto* would accept but the settings parsers do not.
    std::size_t index = 0;
    if (text[index] == '+' || text[index] == '-') ++index;
    const bool startsLikeNumber =
            (index < text.size() && std::isdigit(
                     static_cast<unsigned char>(text[index]))) ||
            (index + 1 < text.size() && text[index] == '.' &&
             std::isdigit(static_cast<unsigned char>(text[index + 1])));
    if (!startsLikeNumber) return std::nullopt;
    for (const char character : text) {
        if (std::isspace(static_cast<unsigned char>(character))) {
            return std::nullopt;
        }
    }
    errno = 0;
    char *end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size() || errno == ERANGE ||
        !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

std::string FormatNumberValue(double value) {
    if (!std::isfinite(value)) return "0";
    if (std::abs(value) < 9.007199254740992e15 &&
        value == std::floor(value)) {
        const long long integral = static_cast<long long>(value);
        return std::to_string(integral);
    }
    char buffer[32];
    const auto result = std::to_chars(
            buffer, buffer + sizeof(buffer), value);
    if (result.ec != std::errc()) return "0";
    return std::string(buffer, result.ptr);
}

}  // namespace forevertas::blocks
