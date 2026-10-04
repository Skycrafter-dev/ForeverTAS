#ifndef FOREVERTAS_MUTATIONS_MODIFIER_UTILS_H
#define FOREVERTAS_MUTATIONS_MODIFIER_UTILS_H

#include "mutations/input_event_utils.h"
#include "searches/option_configuration.h"
#include "searches/option_settings_utils.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <type_traits>

namespace forevertas {

// Segment draws reject items anchored outside their segment. The bound only
// stops pathological settings from looping; typical acceptance is far higher.
inline constexpr std::size_t kSegmentAttemptsPerItem = 64u;

struct ModifierWindow {
    std::int64_t minimumTimeMs = 0;
    std::int64_t maximumTimeMs = 0;
    std::uint32_t seed = 0u;
};

// Produces exactly the words std::seed_seq generates for the same four
// inputs; the standard fixes that algorithm. Advancing the wrapped indices
// instead of taking three remainders per step makes seeding several times
// faster, which matters because every draw seeds one engine per pass.
class ModifierSeedSequence {
public:
    using result_type = std::uint_least32_t;

    ModifierSeedSequence(std::uint32_t first,
                         std::uint32_t second,
                         std::uint32_t third,
                         std::uint32_t fourth)
        : values_{first, second, third, fourth} {}

    std::size_t size() const { return kValueCount; }

    template <typename OutputIterator>
    void param(OutputIterator destination) const {
        std::copy(std::begin(values_), std::end(values_), destination);
    }

    template <typename RandomAccessIterator>
    void generate(RandomAccessIterator begin,
                  RandomAccessIterator end) const {
        if (begin == end) return;
        const std::size_t n = static_cast<std::size_t>(end - begin);
        for (std::size_t index = 0u; index < n; ++index) {
            begin[index] = 0x8b8b8b8bu;
        }
        const std::size_t t = n >= 623u ? 11u
                : n >= 68u ? 7u
                : n >= 39u ? 5u
                : n >= 7u ? 3u
                : (n - 1u) / 2u;
        const std::size_t p = (n - t) / 2u;
        const std::size_t q = p + t;
        const std::size_t m = std::max(kValueCount + 1u, n);
        const auto mix = [](std::uint32_t value) {
            return value ^ (value >> 27u);
        };
        // k, k + p, k + q, and k - 1, all modulo n.
        std::size_t current = 0u;
        std::size_t plusP = p % n;
        std::size_t plusQ = q % n;
        std::size_t previous = n - 1u;
        const auto advance = [n](std::size_t &index) {
            if (++index == n) index = 0u;
        };
        const auto step = [&]() {
            previous = current;
            advance(current);
            advance(plusP);
            advance(plusQ);
        };
        for (std::size_t k = 0u; k < m; ++k) {
            const std::uint32_t r1 = 1664525u * mix(static_cast<std::uint32_t>(
                    begin[current] ^ begin[plusP] ^ begin[previous]));
            const std::uint32_t r2 = r1 + static_cast<std::uint32_t>(
                    k == 0u ? kValueCount
                    : k <= kValueCount ? current + values_[k - 1u]
                    : current);
            begin[plusP] = static_cast<std::uint32_t>(begin[plusP] + r1);
            begin[plusQ] = static_cast<std::uint32_t>(begin[plusQ] + r2);
            begin[current] = r2;
            step();
        }
        for (std::size_t k = 0u; k < n; ++k) {
            const std::uint32_t r3 = 1566083941u * mix(static_cast<std::uint32_t>(
                    begin[current] + begin[plusP] + begin[previous]));
            const std::uint32_t r4 =
                    r3 - static_cast<std::uint32_t>(current);
            begin[plusP] = static_cast<std::uint32_t>(begin[plusP] ^ r3);
            begin[plusQ] = static_cast<std::uint32_t>(begin[plusQ] ^ r4);
            begin[current] = r4;
            step();
        }
    }

private:
    static constexpr std::size_t kValueCount = 4u;
    std::uint32_t values_[kValueCount];
};

inline std::mt19937 ModifierRandom(std::uint32_t seed,
                                  std::uint64_t iterationIndex,
                                  std::uint32_t passIndex) {
    ModifierSeedSequence sequence(
            seed,
            static_cast<std::uint32_t>(iterationIndex),
            static_cast<std::uint32_t>(iterationIndex >> 32u),
            passIndex);
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
