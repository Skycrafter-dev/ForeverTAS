#ifndef FOREVERTAS_BLOCKS_BLOCK_VALUE_H
#define FOREVERTAS_BLOCKS_BLOCK_VALUE_H

#include <optional>
#include <string>

namespace forevertas::blocks {

// Locale-independent number parsing/formatting for block field values.
// Settings values are dot-decimal strings; integral results stay integral
// so compiled settings remain byte-identical to hand-entered ones.
std::optional<double> ParseNumberValue(const std::string &text);

std::string FormatNumberValue(double value);

}  // namespace forevertas::blocks

#endif
