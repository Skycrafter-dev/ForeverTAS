#include "mutations/smooth_steering_mutator.h"

#include "mutations/input_event_utils.h"
#include "mutations/modifier_utils.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

namespace forevertas {
namespace {

struct Settings {
    ModifierWindow window;
    std::uint32_t deformationCount = 1u;
    std::int64_t radiusMs = 100;
    AnalogInputState amplitudeMinimum = 0;
    AnalogInputState amplitudeMaximum = 0;
};

std::optional<Settings> ParseSettings(const OptionSettings &settings) {
    const auto window = ParseModifierWindow(settings);
    const auto count = ParseUnsignedDecimal32(settings.at("deformationCount"));
    const auto radius = ParseSignedDecimal(settings.at("radiusMs"));
    const auto amplitudeMinimum =
            ParseNormalizedAnalogInput(settings.at("amplitudeMin"));
    const auto amplitudeMaximum =
            ParseNormalizedAnalogInput(settings.at("amplitudeMax"));
    if (!window || !count || !radius || !amplitudeMinimum ||
        !amplitudeMaximum) return std::nullopt;
    return Settings{*window,
                    *count,
                    *radius,
                    *amplitudeMinimum,
                    *amplitudeMaximum};
}

class SmoothSteeringMutator final : public InputMutator {
public:
    explicit SmoothSteeringMutator(Settings settings) : settings_(settings) {}

    MutationResult Mutate(const MutationRequest &request) const override {
        std::vector<SandboxInputEvent> inputs = request.baselineInputs;
        std::mt19937 random = ModifierRandom(
                settings_.window.seed, request.iterationIndex, request.passIndex);
        const std::int64_t tick = request.tickDurationMs;
        const bool indexed = settings_.window.minimumTimeMs >= request.mutableFromTimeMs &&
                InputEventsAreCanonical(inputs, request.tickDurationMs);
        struct SteeringValue {
            AnalogInputState value = 0;
            bool changed = false;
        };
        std::map<std::int64_t, SteeringValue> steering;
        if (indexed) {
            for (const SandboxInputEvent &event : inputs) {
                if (event.action == SandboxInputAction::Steer &&
                    event.value.kind == forevervalidator::experimental::
                            PhysicsSandboxInputValueKind::Analog) {
                    steering.emplace_hint(steering.end(), event.timeMs,
                                          SteeringValue{event.value.analog, false});
                }
            }
        }
        constexpr double pi = 3.14159265358979323846;
        const auto applyDeformation = [&](std::int64_t center,
                                          AnalogInputState amplitude) {
            const std::int64_t start = std::max(
                    settings_.window.minimumTimeMs,
                    center - settings_.radiusMs);
            const std::int64_t end = std::min(
                    settings_.window.maximumTimeMs,
                    center + settings_.radiusMs);
            auto next = steering.lower_bound(start);
            AnalogInputState held = next == steering.begin()
                    ? 0 : std::prev(next)->second.value;
            for (std::int64_t time = AlignInputTime(start, tick);
                 time <= end;
                 time += tick) {
                const double distance = std::abs(
                        static_cast<double>(time - center));
                const double weight = settings_.radiusMs == 0
                        ? 1.0
                        : 0.5 * (1.0 + std::cos(
                                  pi * distance /
                                  static_cast<double>(settings_.radiusMs)));
                const std::int64_t weightedDelta = std::llround(
                        static_cast<double>(amplitude) * weight);
                if (indexed) {
                    while (next != steering.end() && next->first <= time) {
                        held = next->second.value;
                        ++next;
                    }
                }
                const AnalogInputState value = SaturateAnalogInputState(
                        static_cast<std::int64_t>(
                                indexed ? held : SteeringStateAt(inputs, time)) +
                        weightedDelta);
                if (indexed) {
                    // Preserve sequential saturation and held-state propagation.
                    steering.insert_or_assign(next, time, SteeringValue{value, true});
                    held = value;
                } else {
                    inputs.push_back(AnalogEvent(time, SandboxInputAction::Steer, value));
                }
            }
            if (!indexed) NormalizeMutableInputEvents(inputs,
                                    request.baselineInputs,
                                    request.tickDurationMs,
                                    request.mutableFromTimeMs);
        };
        if (request.segment != nullptr) {
            // Rejection keeps each center distributed as an ordinary draw
            // conditioned on the deformation's first changed tick.
            std::uint32_t remaining = SegmentSlotCount(request, 0u);
            const std::size_t attemptLimit =
                    static_cast<std::size_t>(remaining) *
                    kSegmentAttemptsPerItem *
                    static_cast<std::size_t>(std::max<std::int64_t>(
                            1, (settings_.window.maximumTimeMs -
                                settings_.window.minimumTimeMs) / tick + 1));
            for (std::size_t attempt = 0u;
                 remaining != 0u && attempt < attemptLimit;
                 ++attempt) {
                const std::int64_t center = RandomCenter(random, tick);
                if (!InAnchorRange(*request.segment,
                                   FirstChangedTimeMs(center, tick))) {
                    continue;
                }
                applyDeformation(center, RandomAmplitude(random));
                --remaining;
            }
        } else {
            for (std::uint32_t deformation = 0u;
                 deformation < settings_.deformationCount;
                 ++deformation) {
                const std::int64_t center = RandomCenter(random, tick);
                applyDeformation(center, RandomAmplitude(random));
                RecordMutationAnchor(request,
                                     FirstChangedTimeMs(center, tick));
            }
        }
        if (indexed) {
            for (const auto &[time, state] : steering) {
                if (state.changed)
                    inputs.push_back(AnalogEvent(time, SandboxInputAction::Steer, state.value));
            }
            NormalizeMutableInputEvents(inputs, request.baselineInputs,
                                        request.tickDurationMs, request.mutableFromTimeMs);
        }
        return {inputs,
                EffectiveInputChangeCount(request.baselineInputs, inputs)};
    }

    std::int64_t EarliestMutationTimeMs() const override {
        return settings_.window.minimumTimeMs;
    }

    MutationTimeRange AffectedTimeRange() const override {
        return MutationTimeRange{
                settings_.window.minimumTimeMs,
                settings_.window.maximumTimeMs};
    }

private:
    std::int64_t RandomCenter(std::mt19937 &random, std::int64_t tick) const {
        return RandomInteger<std::int64_t>(
                       random,
                       settings_.window.minimumTimeMs / tick,
                       settings_.window.maximumTimeMs / tick) * tick;
    }

    AnalogInputState RandomAmplitude(std::mt19937 &random) const {
        return RandomInteger<AnalogInputState>(
                random,
                settings_.amplitudeMinimum,
                settings_.amplitudeMaximum);
    }

    std::int64_t FirstChangedTimeMs(std::int64_t center,
                                    std::int64_t tick) const {
        return AlignInputTime(
                std::max(settings_.window.minimumTimeMs,
                         center - settings_.radiusMs),
                tick);
    }

    Settings settings_;
};

}  // namespace

OptionSettings DefaultSmoothSteeringSettings() {
    return {{"minTimeMs", "1000"},
            {"maxTimeMs", "5990"},
            {"seed", "1179926867"},
            {"deformationCount", "1"},
            {"radiusMs", "200"},
            {"amplitudeMin", "-0.2"},
            {"amplitudeMax", "0.2"}};
}

std::optional<std::string> ValidateSmoothSteeringSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) {
    if (const auto error = ValidateOptionSettingKeys(
                settings, DefaultSmoothSteeringSettings())) return error;
    const auto parsed = ParseSettings(settings);
    if (!parsed) return "smooth steering settings are invalid";
    if (const auto error = ValidateModifierWindow(parsed->window,
                                                   tickDurationMs)) return error;
    if (parsed->deformationCount == 0u) {
        return "smooth steering deformation count must be greater than zero";
    }
    if (parsed->radiusMs < 0 || parsed->radiusMs % tickDurationMs != 0) {
        return "smooth steering radius must be a non-negative whole-tick value";
    }
    if (parsed->amplitudeMinimum > parsed->amplitudeMaximum) {
        return "smooth steering amplitude minimum must not exceed maximum";
    }
    return std::nullopt;
}

std::unique_ptr<InputMutator> CreateSmoothSteeringMutator(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) {
    if (const auto error = ValidateSmoothSteeringSettings(
                settings, tickDurationMs)) throw std::invalid_argument(*error);
    return std::make_unique<SmoothSteeringMutator>(*ParseSettings(settings));
}

}  // namespace forevertas
