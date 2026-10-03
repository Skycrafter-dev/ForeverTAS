#ifndef FOREVERTAS_SPEED_FORMAT_H
#define FOREVERTAS_SPEED_FORMAT_H

#include <locale>
#include <sstream>
#include <string>

namespace forevertas {

inline std::string FormatDisplaySpeed(double metersPerSecond) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream.precision(9);
    stream << metersPerSecond * 3.6 << " km/h (" << metersPerSecond << " m/s)";
    return stream.str();
}

}  // namespace forevertas

#endif
