#include "mutations/smooth_steering_mutator.h"

#include "mutations/input_event_utils.h"
#include "mutations/modifier_utils.h"

#include <cmath>
#include <stdexcept>

namespace forevertas {
namespace {

struct Settings {
    ModifierWindow window;
    std::uint32_t deformationCount = 1u;
    std::int64_t radiusMs = 100;
    AnalogInputState amplitudeMinimum = 0;
    AnalogInputState amplitudeMaximum = 0;
    std::uint32_t tickDurationMs = 10u;
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
        const std::int64_t minimumTick = settings_.window.minimumTimeMs / tick;
        const std::int64_t maximumTick = settings_.window.maximumTimeMs / tick;
        constexpr double pi = 3.14159265358979323846;
        for (std::uint32_t deformation = 0u;
             deformation < settings_.deformationCount;
             ++deformation) {
            const std::int64_t center = RandomInteger<std::int64_t>(
                    random, minimumTick, maximumTick) * tick;
            const AnalogInputState amplitude =
                    RandomInteger<AnalogInputState>(
                            random,
                            settings_.amplitudeMinimum,
                            settings_.amplitudeMaximum);
            const std::int64_t start = std::max(
                    settings_.window.minimumTimeMs,
                    center - settings_.radiusMs);
            const std::int64_t end = std::min(
                    settings_.window.maximumTimeMs,
                    center + settings_.radiusMs);
            bool pushed = false;
            AnalogInputState lastPushed = 0;
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
                // Read the unmutated baseline: accumulating deltas onto
                // the already-deformed stream turned the bump into a
                // ramp that saturated at full steering lock.
                const AnalogInputState value = SaturateAnalogInputState(
                        static_cast<std::int64_t>(
                                SteeringStateAt(request.baselineInputs,
                                                time)) +
                        weightedDelta);
                inputs.push_back(AnalogEvent(time,
                                             SandboxInputAction::Steer,
                                             value));
                pushed = true;
                lastPushed = value;
            }
            // A bump cut off by the window edge must not leak its last
            // value past the deformed span; restore the baseline state
            // on the first tick after it. AlignInputTime floors, so the
            // next tick boundary is computed explicitly.
            if (pushed) {
                const std::int64_t restoreTime =
                        (end / tick) * tick + tick;
                const AnalogInputState baselineAfter =
                        SteeringStateAt(request.baselineInputs,
                                        restoreTime);
                if (baselineAfter != lastPushed) {
                    inputs.push_back(AnalogEvent(restoreTime,
                                                 SandboxInputAction::Steer,
                                                 baselineAfter));
                }
            }
            NormalizeMutableInputEvents(inputs,
                                    request.baselineInputs,
                                    request.tickDurationMs,
                                    request.mutableFromTimeMs);
        }
        return {inputs,
                EffectiveInputChangeCount(request.baselineInputs, inputs)};
    }

    std::int64_t EarliestMutationTimeMs() const override {
        return settings_.window.minimumTimeMs;
    }

    MutationTimeRange AffectedTimeRange() const override {
        // The baseline-restore event after a window-clamped bump lands
        // one tick past the window, so the declared range must include
        // it for window-patch composition to stay equivalent.
        return MutationTimeRange{
                settings_.window.minimumTimeMs,
                settings_.window.maximumTimeMs
                        + static_cast<std::int64_t>(
                                  settings_.tickDurationMs)};
    }

private:
    Settings settings_;
};

}  // namespace

OptionFieldList SmoothSteeringOptionFields() {
    OptionFieldList fields;
    AppendWindowFields(fields, "1000", "5990");
    AppendSeedField(fields);
    fields.push_back(NumberField("deformationCount", "Deformations", "1"));
    fields.push_back(NumberField("radiusMs", "Radius (ms)", "200"));
    fields.push_back(ClampedField(
            "amplitudeMin", "Amplitude min", "-0.2", -1.0, 1.0, 2, 0.01));
    fields.push_back(ClampedField(
            "amplitudeMax", "Amplitude max", "0.2", -1.0, 1.0, 2, 0.01));
    return fields;
}

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
    Settings parsed = *ParseSettings(settings);
    parsed.tickDurationMs = tickDurationMs;
    return std::make_unique<SmoothSteeringMutator>(parsed);
}

}  // namespace forevertas
