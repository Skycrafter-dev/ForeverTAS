#ifndef FOREVERTAS_MUTATIONS_MODIFIER_UTILS_H
#define FOREVERTAS_MUTATIONS_MODIFIER_UTILS_H

#include "mutations/input_event_utils.h"
#include "searches/option_configuration.h"
#include "searches/option_settings_utils.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
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

inline std::uint64_t RandomUnsignedInteger(std::mt19937 &random,
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
                product = static_cast<std::uint64_t>(random()) *
                        extendedRange;
                low = static_cast<std::uint32_t>(product);
            }
        }
        result = product >> 32u;
    } else if (range == UINT32_MAX) {
        result = random();
    } else {
        do {
            constexpr std::uint64_t generatorRange =
                    UINT64_C(1) << 32u;
            const std::uint64_t high = RandomUnsignedInteger(
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
    static_assert(std::is_integral_v<Integer> && !std::is_same_v<Integer, bool>);
    if (minimum > maximum) std::swap(minimum, maximum);
    using Unsigned = std::make_unsigned_t<Integer>;
    const Unsigned unsignedMinimum = static_cast<Unsigned>(minimum);
    const std::uint64_t range = static_cast<std::uint64_t>(
            static_cast<Unsigned>(maximum) - unsignedMinimum);
    return static_cast<Integer>(static_cast<Unsigned>(
            RandomUnsignedInteger(random, 0u, range)) + unsignedMinimum);
}

// std::shuffle is deliberately not used for mutation streams. Its sequence is
// implementation-defined (MSVC and libstdc++ consume mt19937 differently),
// while the CUDA mutator has to reproduce the same seeded candidates on every
// host toolchain. The distribution above and pairing algorithm below mirror
// the CUDA search executor exactly.
template<typename RandomAccessIterator>
void ShuffleModifierValues(RandomAccessIterator first,
                           RandomAccessIterator last,
                           std::mt19937 &random) {
    using Difference = typename std::iterator_traits<
            RandomAccessIterator>::difference_type;
    const Difference difference = last - first;
    if (difference <= 1) return;

    const std::uint64_t count = static_cast<std::uint64_t>(difference);
    constexpr std::uint64_t generatorRange =
            static_cast<std::uint64_t>(std::mt19937::max()) -
            static_cast<std::uint64_t>(std::mt19937::min());
    if (generatorRange / count >= count) {
        std::uint64_t index = 1u;
        if ((count % 2u) == 0u) {
            const std::uint32_t selected =
                    RandomInteger<std::uint32_t>(random, 0u, 1u);
            std::iter_swap(first + static_cast<Difference>(index),
                           first + static_cast<Difference>(selected));
            ++index;
        }
        while (index != count) {
            const std::uint64_t swapRange = index + 1u;
            const std::uint64_t position = RandomInteger<std::uint64_t>(
                    random, 0u,
                    swapRange * (swapRange + 1u) - 1u);
            const std::uint64_t firstSelected =
                    position / (swapRange + 1u);
            const std::uint64_t secondSelected =
                    position % (swapRange + 1u);
            std::iter_swap(first + static_cast<Difference>(index),
                           first + static_cast<Difference>(firstSelected));
            ++index;
            std::iter_swap(first + static_cast<Difference>(index),
                           first + static_cast<Difference>(secondSelected));
            ++index;
        }
        return;
    }

    for (std::uint64_t index = 1u; index < count; ++index) {
        const std::uint64_t selected = RandomInteger<std::uint64_t>(
                random, 0u, index);
        std::iter_swap(first + static_cast<Difference>(index),
                       first + static_cast<Difference>(selected));
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
