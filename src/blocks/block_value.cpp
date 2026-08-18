#include "blocks/block_value.h"

#include <charconv>
#include <cmath>
#include <limits>

namespace forevertas::blocks {

std::optional<double> ParseNumberValue(const std::string &text) {
  if (text.empty())
    return std::nullopt;
  // Reject values strto* would accept but the settings parsers do not.
  std::size_t index = 0;
  if (text[index] == '+' || text[index] == '-')
    ++index;
  const bool startsLikeNumber =
      (index < text.size() &&
       std::isdigit(static_cast<unsigned char>(text[index]))) ||
      (index + 1 < text.size() && text[index] == '.' &&
       std::isdigit(static_cast<unsigned char>(text[index + 1])));
  if (!startsLikeNumber)
    return std::nullopt;
  for (const char character : text) {
    if (std::isspace(static_cast<unsigned char>(character))) {
      return std::nullopt;
    }
  }
  // std::strtod follows LC_NUMERIC, which made a persisted dot-decimal
  // value such as "0.5" fail after Qt initialized a comma-decimal locale.
  // from_chars is deliberately locale-independent and matches the format
  // used by every block/settings serializer in ForeverTAS. It does not
  // accept a leading '+', so consume that one character explicitly.
  const char *begin = text.data();
  const char *const end = begin + text.size();
  if (*begin == '+')
    ++begin;
  double value = 0.0;
  const auto parsed =
      std::from_chars(begin, end, value, std::chars_format::general);
  if (parsed.ec != std::errc() || parsed.ptr != end || !std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

std::string FormatNumberValue(double value) {
  if (!std::isfinite(value))
    return "0";
  if (std::abs(value) < 9.007199254740992e15 && value == std::floor(value)) {
    const long long integral = static_cast<long long>(value);
    return std::to_string(integral);
  }
  char buffer[32];
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (result.ec != std::errc())
    return "0";
  return std::string(buffer, result.ptr);
}

} // namespace forevertas::blocks
