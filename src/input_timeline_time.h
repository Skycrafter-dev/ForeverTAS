#ifndef FOREVERTAS_INPUT_TIMELINE_TIME_H
#define FOREVERTAS_INPUT_TIMELINE_TIME_H

#include "searches/option_configuration.h"
#include "searches/option_settings_utils.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace forevertas {

inline constexpr std::uint32_t kInputTimelineTickDurationMs = 10u;

// The simulation records the first actionable input one physics tick after the
// user-visible timeline origin. Absolute input-setting keys ending in "TimeMs"
// are translated here exactly once. Evaluation and search-policy settings use
// simulation time directly and must never pass through this conversion.
inline constexpr std::string_view kInputTimelineTimeSettingSuffix = "TimeMs";

inline bool IsInputTimelineTimeSetting(std::string_view key) {
    return key.size() >= kInputTimelineTimeSettingSuffix.size() &&
            key.compare(key.size() - kInputTimelineTimeSettingSuffix.size(),
                        kInputTimelineTimeSettingSuffix.size(),
                        kInputTimelineTimeSettingSuffix) == 0;
}

inline std::optional<std::int64_t> SimulationTimelineTimeFromUserTime(
        std::int64_t userTimeMs,
        std::uint32_t tickDurationMs,
        std::int64_t simulationOriginMs = 0) {
    if (tickDurationMs == 0u || userTimeMs < 0 || simulationOriginMs < 0) {
        return std::nullopt;
    }
    const std::int64_t offset = static_cast<std::int64_t>(tickDurationMs);
    if (simulationOriginMs >
                std::numeric_limits<std::int64_t>::max() - offset ||
        userTimeMs > std::numeric_limits<std::int64_t>::max() -
                simulationOriginMs - offset) {
        return std::nullopt;
    }
    return simulationOriginMs + offset + userTimeMs;
}

inline std::int64_t UserTimelineTimeFromSimulationTime(
        std::int64_t simulationTimeMs,
        std::uint32_t tickDurationMs,
        std::int64_t simulationOriginMs = 0) {
    if (tickDurationMs == 0u) return 0;
    const std::int64_t firstInputTime =
            SimulationTimelineTimeFromUserTime(
                    0, tickDurationMs, simulationOriginMs)
                    .value_or(std::numeric_limits<std::int64_t>::max());
    return std::max<std::int64_t>(0, simulationTimeMs - firstInputTime);
}

inline std::optional<OptionSettings> SimulationInputSettingsFromUserTimeline(
        const OptionSettings &userSettings,
        std::uint32_t tickDurationMs) {
    if (tickDurationMs == 0u) return std::nullopt;
    OptionSettings simulationSettings = userSettings;
    for (auto &[key, value] : simulationSettings) {
        if (!IsInputTimelineTimeSetting(key)) continue;
        const std::optional<std::int64_t> userTime = ParseSignedDecimal(value);
        if (!userTime) continue;
        const std::optional<std::int64_t> simulationTime =
                SimulationTimelineTimeFromUserTime(*userTime, tickDurationMs);
        if (!simulationTime) return std::nullopt;
        value = std::to_string(*simulationTime);
    }
    return simulationSettings;
}

inline std::optional<OptionSettings> ResolveHorizonWindowSettings(
        const OptionSettings &settings, std::uint32_t tickDurationMs,
        std::int64_t simulationHorizonMs, bool inputTime) {
    OptionSettings resolved = settings;
    const auto mode = resolved.find("maxTimeMode");
    if (mode == resolved.end()) return resolved;
    if (mode->second != "fixed" && mode->second != "horizon") return std::nullopt;
    if (mode->second == "horizon") {
        const auto maximum = resolved.find("maxTimeMs");
        if (maximum == resolved.end() || tickDurationMs == 0u ||
            simulationHorizonMs < tickDurationMs) return std::nullopt;
        maximum->second = std::to_string(inputTime
                ? UserTimelineTimeFromSimulationTime(simulationHorizonMs, tickDurationMs)
                : simulationHorizonMs);
    }
    resolved.erase(mode);
    return resolved;
}

inline OptionSettings ClampInputWindowToSimulationHorizon(
        const OptionSettings &userSettings,
        std::uint32_t tickDurationMs,
        std::int64_t simulationHorizonMs) {
    OptionSettings clamped = userSettings;
    const std::int64_t maximumUserTimeMs =
            UserTimelineTimeFromSimulationTime(
                    simulationHorizonMs, tickDurationMs);
    for (const char *const key : {"minTimeMs", "maxTimeMs"}) {
        const auto found = clamped.find(key);
        if (found == clamped.end()) continue;
        const std::optional<std::int64_t> time =
                ParseSignedDecimal(found->second);
        if (time && *time > maximumUserTimeMs) {
            found->second = std::to_string(maximumUserTimeMs);
        }
    }
    return clamped;
}

}  // namespace forevertas

#endif
