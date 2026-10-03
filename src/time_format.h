#ifndef FOREVERTAS_TIME_FORMAT_H
#define FOREVERTAS_TIME_FORMAT_H

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>

namespace forevertas {

inline std::string FormatHumanDurationNanoseconds(
        std::uint64_t nanoseconds) {
    constexpr std::uint64_t NanosecondsPerSecond = 1000000000u;
    constexpr std::uint64_t SecondsPerMinute = 60u;
    constexpr std::uint64_t SecondsPerHour = 3600u;

    const std::uint64_t totalSeconds =
            nanoseconds / NanosecondsPerSecond;
    const std::uint64_t hours = totalSeconds / SecondsPerHour;
    const std::uint64_t minutes =
            (totalSeconds / SecondsPerMinute) % SecondsPerMinute;
    const std::uint64_t seconds = totalSeconds % SecondsPerMinute;
    const std::uint64_t fraction =
            nanoseconds % NanosecondsPerSecond;

    std::ostringstream stream;
    stream << std::setfill('0');
    if (hours != 0u) {
        stream << hours << ':' << std::setw(2) << minutes << ':'
               << std::setw(2) << seconds;
    } else if (minutes != 0u) {
        stream << minutes << ':' << std::setw(2) << seconds;
    } else {
        stream << seconds;
    }
    stream << '.' << std::setw(9) << fraction;
    return stream.str();
}

inline std::string FormatHumanDurationMilliseconds(double milliseconds) {
    if (!std::isfinite(milliseconds) || milliseconds < 0.0) {
        milliseconds = 0.0;
    }
    const auto rounded = static_cast<std::int64_t>(std::llround(milliseconds));
    const std::int64_t totalHours = rounded / 3600000;
    const std::int64_t minutes = (rounded / 60000) % 60;
    const std::int64_t seconds = (rounded / 1000) % 60;
    const std::int64_t millis = rounded % 1000;

    std::ostringstream stream;
    stream << std::setfill('0') << std::setw(2) << totalHours << ':'
           << std::setw(2) << minutes << ':' << std::setw(2) << seconds;
    if (millis != 0) {
        std::ostringstream fraction;
        fraction << std::setfill('0') << std::setw(3) << millis;
        std::string digits = fraction.str();
        while (!digits.empty() && digits.back() == '0') {
            digits.pop_back();
        }
        stream << '.' << digits;
    }
    return stream.str();
}

inline std::string FormatRaceTimeMilliseconds(double milliseconds) {
    std::string result = FormatHumanDurationMilliseconds(milliseconds);
    const auto decimal = result.find('.');
    if (decimal == std::string::npos) {
        result += ".00";
    } else if (result.size() - decimal - 1u < 2u) {
        result += '0';
    }
    return result;
}

inline std::string FormatFixedDurationMilliseconds(
        std::uint64_t milliseconds) {
    const std::uint64_t totalHours = milliseconds / 3600000u;
    const std::uint64_t minutes = (milliseconds / 60000u) % 60u;
    const std::uint64_t seconds = (milliseconds / 1000u) % 60u;
    const std::uint64_t millis = milliseconds % 1000u;

    std::ostringstream stream;
    stream << std::setfill('0') << std::setw(2) << totalHours << ':'
           << std::setw(2) << minutes << ':' << std::setw(2) << seconds
           << '.' << std::setw(3) << millis;
    return stream.str();
}

inline std::string FormatInterpolatedTimeMilliseconds(double milliseconds) {
    if (!std::isfinite(milliseconds) || milliseconds < 0.0 ||
        milliseconds >= static_cast<double>(std::numeric_limits<std::int64_t>::max()) / 1000.0) {
        return "n/a";
    }
    const auto microseconds = static_cast<std::uint64_t>(std::llround(milliseconds * 1000.0));
    std::ostringstream stream;
    stream << FormatFixedDurationMilliseconds(microseconds / 1000u)
           << std::setfill('0') << std::setw(3) << microseconds % 1000u;
    return stream.str();
}

inline std::string FormatFixedSplitMilliseconds(
        std::uint64_t milliseconds) {
    const std::uint64_t totalSeconds = milliseconds / 1000u;
    const std::uint64_t hours = totalSeconds / 3600u;
    const std::uint64_t minutes = (totalSeconds / 60u) % 60u;
    const std::uint64_t seconds = totalSeconds % 60u;

    std::ostringstream stream;
    stream << std::setfill('0');
    if (hours != 0u) {
        stream << hours << ':' << std::setw(2) << minutes << ':'
               << std::setw(2) << seconds;
    } else if (minutes != 0u) {
        stream << minutes << ':' << std::setw(2) << seconds;
    } else {
        stream << seconds;
    }
    stream << '.' << std::setw(3) << (milliseconds % 1000u);
    return stream.str();
}

inline std::string FormatSignificantDurationMilliseconds(
        std::uint64_t milliseconds) {
    constexpr std::uint64_t MillisecondsPerSecond = 1000u;
    constexpr std::uint64_t SecondsPerMinute = 60u;
    constexpr std::uint64_t SecondsPerHour = 3600u;

    const std::uint64_t totalSeconds =
            milliseconds / MillisecondsPerSecond;
    const std::uint64_t hours = totalSeconds / SecondsPerHour;
    const std::uint64_t minutes =
            (totalSeconds / SecondsPerMinute) % SecondsPerMinute;
    const std::uint64_t seconds = totalSeconds % SecondsPerMinute;
    std::uint64_t fraction =
            milliseconds % MillisecondsPerSecond;

    std::ostringstream stream;
    stream << std::setfill('0');
    if (hours != 0u) {
        stream << hours << ':' << std::setw(2) << minutes << ':'
               << std::setw(2) << seconds;
    } else if (minutes != 0u) {
        stream << minutes << ':' << std::setw(2) << seconds;
    } else {
        stream << seconds;
    }
    if (fraction != 0u) {
        std::uint64_t divisor = 100u;
        while (fraction % 10u == 0u) {
            fraction /= 10u;
            divisor /= 10u;
        }
        stream << '.' << std::setw(
                divisor == 100u ? 3 : divisor == 10u ? 2 : 1)
               << fraction;
    }
    return stream.str();
}

template<typename Rep, typename Period>
std::string FormatHumanDuration(
        const std::chrono::duration<Rep, Period> &duration) {
    return FormatHumanDurationMilliseconds(
            std::chrono::duration<double, std::milli>(duration).count());
}

}  // namespace forevertas

#endif
