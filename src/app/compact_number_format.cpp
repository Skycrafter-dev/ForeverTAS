#include "app/compact_number_format.h"

#include <array>
#include <cmath>

namespace forevertas::app {
QString FormatExactCount(std::uint64_t value) {
    QString text = QString::number(static_cast<qulonglong>(value));
    for (qsizetype index = text.size() - 3; index > 0; index -= 3) {
        text.insert(index, QLatin1Char(','));
    }
    return text;
}

QString FormatCompactNumber(double value) {
    if (!std::isfinite(value)) {
        return QString::number(value);
    }

    constexpr std::array<const char *, 6> suffixes{
            "", "k", "M", "B", "T", "Q"};
    const double absolute = std::abs(value);
    std::size_t suffixIndex = 0u;
    double scale = 1.0;
    while (suffixIndex + 1u < suffixes.size() &&
           absolute >= scale * 1000.0) {
        ++suffixIndex;
        scale *= 1000.0;
    }

    double scaled = value / scale;
    double rounded = std::round(scaled * 100.0) / 100.0;
    if (suffixIndex + 1u < suffixes.size() &&
        std::abs(rounded) >= 1000.0) {
        ++suffixIndex;
        scale *= 1000.0;
        scaled = value / scale;
        rounded = std::round(scaled * 100.0) / 100.0;
    }

    if (suffixIndex == 0u && std::trunc(value) == value) {
        return QString::number(value, 'f', 0);
    }

    return QString::number(rounded, 'f', 2) +
            QString::fromLatin1(suffixes[suffixIndex]);
}

}  // namespace forevertas::app
