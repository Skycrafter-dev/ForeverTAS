#ifndef FOREVERTAS_MUTATIONS_MODIFIER_UTILS_H
#define FOREVERTAS_MUTATIONS_MODIFIER_UTILS_H

#include "mutations/input_event_utils.h"
#include "searches/option_configuration.h"
#include "searches/option_settings_utils.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <random>
#include <string>
#include <type_traits>

namespace forevertas {

struct ModifierWindow {
    std::int64_t minimumTimeMs = 0;
    std::int64_t maximumTimeMs = 0;
    std::uint32_t seed = 0u;
};

inline std::mt19937 ModifierRandom(std::uint32_t seed,
                                  std::uint64_t iterationIndex,
                                  std::uint32_t passIndex) {
    std::seed_seq sequence{
            seed,
            static_cast<std::uint32_t>(iterationIndex),
            static_cast<std::uint32_t>(iterationIndex >> 32u),
            passIndex};
    return std::mt19937(sequence);
}

// Keep CPU mutation draws identical to the CUDA and Vulkan device samplers.
inline std::uint64_t RandomUnsigned(std::mt19937 &random,
                                    std::uint64_t minimum,
                                    std::uint64_t maximum) {
    if (minimum > maximum) std::swap(minimum, maximum);
    const std::uint64_t range = maximum - minimum;
    std::uint64_t result = 0u;
    if (range < UINT32_MAX) {
        const std::uint32_t extendedRange =
                static_cast<std::uint32_t>(range + 1u);
        std::uint64_t product =
                static_cast<std::uint64_t>(random()) * extendedRange;
        std::uint32_t low = static_cast<std::uint32_t>(product);
        if (low < extendedRange) {
            const std::uint32_t threshold =
                    static_cast<std::uint32_t>(-extendedRange) %
                    extendedRange;
            while (low < threshold) {
                product =
                        static_cast<std::uint64_t>(random()) * extendedRange;
                low = static_cast<std::uint32_t>(product);
            }
        }
        result = product >> 32u;
    } else if (range == UINT32_MAX) {
        result = random();
    } else {
        do {
            constexpr std::uint64_t generatorRange = UINT64_C(1) << 32u;
            const std::uint64_t high = RandomUnsigned(
                    random, 0u, range / generatorRange);
            const std::uint64_t temporary = generatorRange * high;
            result = temporary + random();
            if (result <= range && result >= temporary) break;
        } while (true);
    }
    return result + minimum;
}

template<typename Integer>
Integer RandomInteger(std::mt19937 &random, Integer minimum, Integer maximum) {
    static_assert(std::is_integral_v<Integer> && sizeof(Integer) <= 8);
    if (minimum > maximum) std::swap(minimum, maximum);
    using Unsigned = std::make_unsigned_t<Integer>;
    const Unsigned lower = static_cast<Unsigned>(minimum);
    const Unsigned range =
            static_cast<Unsigned>(maximum) - lower;
    const Unsigned result = static_cast<Unsigned>(
            RandomUnsigned(random, 0u, range) + lower);
    if constexpr (std::is_signed_v<Integer>) {
        Integer signedResult;
        std::memcpy(&signedResult, &result, sizeof(result));
        return signedResult;
    } else {
        return result;
    }
}

inline std::optional<ModifierWindow> ParseModifierWindow(
        const OptionSettings &settings) {
    const auto minimum = ParseSignedDecimal(settings.at("minTimeMs"));
    const auto maximum = ParseSignedDecimal(settings.at("maxTimeMs"));
    const auto seed = ParseUnsignedDecimal32(settings.at("seed"));
    if (!minimum || !maximum || !seed) return std::nullopt;
    return ModifierWindow{*minimum, *maximum, *seed};
}

inline std::optional<std::string> ValidateModifierWindow(
        const ModifierWindow &window,
        std::uint32_t tickDurationMs) {
    return ValidateTimeWindow(window.minimumTimeMs,
                              window.maximumTimeMs,
                              tickDurationMs,
                              "mutation");
}

inline bool IsAccelerateAction(SandboxInputAction action) {
    return action == SandboxInputAction::Accelerate ||
           action == SandboxInputAction::Gas;
}

inline bool IsBrakeAction(SandboxInputAction action) {
    return action == SandboxInputAction::Brake;
}

inline bool IsSteerAction(SandboxInputAction action) {
    return action == SandboxInputAction::Steer;
}

inline SandboxInputEvent AnalogEvent(std::int64_t timeMs,
                                     SandboxInputAction action,
                                     AnalogInputState value) {
    SandboxInputEvent event;
    event.timeMs = timeMs;
    event.action = action;
    event.value.kind = forevervalidator::experimental::
            PhysicsSandboxInputValueKind::Analog;
    event.value.analog = value;
    return event;
}

inline SandboxInputEvent SwitchEvent(std::int64_t timeMs,
                                     SandboxInputAction action,
                                     bool active) {
    SandboxInputEvent event;
    event.timeMs = timeMs;
    event.action = action;
    event.value.kind = forevervalidator::experimental::
            PhysicsSandboxInputValueKind::Switch;
    event.value.switchState = active
            ? forevervalidator::experimental::
                      PhysicsSandboxSwitchState::Pressed
            : forevervalidator::experimental::
                      PhysicsSandboxSwitchState::Released;
    return event;
}

}  // namespace forevertas

#endif
