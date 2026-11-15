#ifndef FOREVERTAS_SEARCHES_OPTION_SETTINGS_UTILS_H
#define FOREVERTAS_SEARCHES_OPTION_SETTINGS_UTILS_H

#include "searches/option_configuration.h"

#include <forevervalidator/input_state.h>

#include <cstdint>
#include <cmath>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <string>

namespace forevertas {

// A component's reason when one of its settings could not be read: the
// configuration then finds and names that setting (ExplainSettingsProblem).
inline constexpr char kUnreadableSetting[] = "a setting could not be read";

inline std::optional<std::string> ValidateOptionSettingKeys(
        const OptionSettings &settings,
        const OptionSettings &defaults) {
    for (const auto &[key, value] : defaults) {
        static_cast<void>(value);
        if (settings.find(key) == settings.end()) {
            return "the setting \"" + key + "\" is missing";
        }
    }
    for (const auto &[key, value] : settings) {
        static_cast<void>(value);
        if (defaults.find(key) == defaults.end()) {
            return "the setting \"" + key + "\" is not one this version knows";
        }
    }
    return std::nullopt;
}

inline bool IsDecimal(const std::string &value) {
    if (value.empty()) {
        return false;
    }
    for (const char character : value) {
        if (character < '0' || character > '9') {
            return false;
        }
    }
    return true;
}

inline std::optional<std::int64_t> ParseSignedDecimal(
        const std::string &value) {
    if (!IsDecimal(value)) {
        return std::nullopt;
    }
    try {
        std::size_t consumed = 0u;
        const long long parsed = std::stoll(value, &consumed, 10);
        if (consumed != value.size()) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(parsed);
    } catch (...) {
        return std::nullopt;
    }
}

inline std::optional<std::uint64_t> ParseUnsignedDecimal64(
        const std::string &value) {
    if (!IsDecimal(value)) {
        return std::nullopt;
    }
    try {
        std::size_t consumed = 0u;
        const unsigned long long parsed = std::stoull(value, &consumed, 10);
        if (consumed != value.size()) {
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(parsed);
    } catch (...) {
        return std::nullopt;
    }
}

inline std::optional<std::uint32_t> ParseUnsignedDecimal32(
        const std::string &value) {
    const std::optional<std::uint64_t> parsed =
            ParseUnsignedDecimal64(value);
    if (!parsed || *parsed > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(*parsed);
}

inline std::optional<double> ParseFiniteDouble(const std::string &value) {
    if (value.empty()) {
        return std::nullopt;
    }

    std::istringstream stream(value);
    stream.imbue(std::locale::classic());
    stream >> std::noskipws;

    double parsed = 0.0;
    if (!(stream >> parsed) || !std::isfinite(parsed)) {
        return std::nullopt;
    }

    char trailing = '\0';
    if (stream >> trailing || !stream.eof()) {
        return std::nullopt;
    }
    return parsed;
}

inline std::optional<forevervalidator::AnalogInputState>
ParseNormalizedAnalogInput(const std::string &value) {
    const std::optional<double> parsed = ParseFiniteDouble(value);
    if (!parsed || *parsed < -1.0 || *parsed > 1.0) {
        return std::nullopt;
    }
    const auto scaled = std::llround(
            *parsed * forevervalidator::kAnalogInputScale);
    if (scaled < forevervalidator::kAnalogInputMinimum ||
        scaled > forevervalidator::kAnalogInputMaximum) {
        return std::nullopt;
    }
    return static_cast<forevervalidator::AnalogInputState>(scaled);
}

inline std::optional<bool> ParseBoolean(const std::string &value) {
    if (value == "true" || value == "1") return true;
    if (value == "false" || value == "0") return false;
    return std::nullopt;
}

inline std::optional<std::string> ValidateTimeWindow(
        std::int64_t minimum,
        std::int64_t maximum,
        std::uint32_t tickDurationMs,
        // what the minimum is called ("the minimum time")
        const std::string &minimumName,
        bool allowTimelineOrigin = false) {
    if (tickDurationMs == 0u) return "the tick length is 0";
    const std::int64_t earliest =
            allowTimelineOrigin
            ? 0 : static_cast<std::int64_t>(tickDurationMs);
    if (minimum < earliest) {
        if (allowTimelineOrigin) {
            return minimumName + " can't be below 0";
        }
        return minimumName + " must be " + std::to_string(tickDurationMs) + " ms or later";
    }
    if (maximum < minimum) return "the maximum time is before the minimum time";
    const std::int64_t tick = static_cast<std::int64_t>(tickDurationMs);
    if (minimum % tick != 0 || maximum % tick != 0) {
        return "times must be in steps of " + std::to_string(tickDurationMs) + " ms";
    }
    return std::nullopt;
}

}  // namespace forevertas

#endif
