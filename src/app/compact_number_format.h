#ifndef FOREVERTAS_APP_COMPACT_NUMBER_FORMAT_H
#define FOREVERTAS_APP_COMPACT_NUMBER_FORMAT_H

#include <QString>
#include <cstdint>

namespace forevertas::app {

QString FormatCompactNumber(double value);
QString FormatExactCount(std::uint64_t value);

}  // namespace forevertas::app

#endif
