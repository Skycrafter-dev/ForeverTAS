#include "app/compact_number_format.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <tuple>
#include <utility>

namespace forevertas::app {
QString FormatExactCount(std::uint64_t value) {
    QString text = QString::number(static_cast<qulonglong>(value));
    for (qsizetype index = text.size() - 3; index > 0; index -= 3) {
        text.insert(index, QLatin1Char(','));
    }
    return text;
}

// Three significant digits with a magnitude suffix and no trailing zeros:
// 999, 1k, 1.23k, 12.3k, 600k, 420M.
QString FormatCompactNumber(double value) {
    if (!std::isfinite(value)) {
        return QString::number(value);
    }

    constexpr std::array<const char *, 6> suffixes{
            "", "k", "M", "B", "T", "Q"};
    const auto roundSignificant = [](double scaled) {
        const double magnitude = std::abs(scaled);
        const int decimals = magnitude == 0.0 ? 0
                : std::clamp(2 - static_cast<int>(std::floor(std::log10(magnitude))), 0, 6);
        const double factor = std::pow(10.0, decimals);
        return std::pair{std::round(scaled * factor) / factor, decimals};
    };

    const double absolute = std::abs(value);
    std::size_t suffixIndex = 0u;
    double scale = 1.0;
    while (suffixIndex + 1u < suffixes.size() &&
           absolute >= scale * 1000.0) {
        ++suffixIndex;
        scale *= 1000.0;
    }

    auto [rounded, decimals] = roundSignificant(value / scale);
    if (suffixIndex + 1u < suffixes.size() &&
        std::abs(rounded) >= 1000.0) {
        ++suffixIndex;
        scale *= 1000.0;
        std::tie(rounded, decimals) = roundSignificant(value / scale);
    }

    QString text = QString::number(rounded, 'f', decimals);
    if (text.contains(QLatin1Char('.'))) {
        while (text.endsWith(QLatin1Char('0'))) {
            text.chop(1);
        }
        if (text.endsWith(QLatin1Char('.'))) {
            text.chop(1);
        }
    }
    if (text == QStringLiteral("-0")) {
        text = QStringLiteral("0");
    }
    return text + QString::fromLatin1(suffixes[suffixIndex]);
}

}  // namespace forevertas::app
