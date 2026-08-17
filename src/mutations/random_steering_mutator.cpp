#include "mutations/random_steering_mutator.h"

#include "mutations/input_event_utils.h"
#include "mutations/modifier_utils.h"

#include <random>
#include <stdexcept>

namespace forevertas {
namespace {

AnalogInputState RandomSteering(std::mt19937 &random) {
    return RandomInteger<AnalogInputState>(
            random, kAnalogInputMinimum, kAnalogInputMaximum);
}

std::optional<RandomSteeringSettings> ParseRandomSteeringSettings(
        const OptionSettings &settings,
        std::string *error) {
    const OptionSettings defaults = DefaultRandomSteeringOptionSettings();
    if (const auto keyError = ValidateOptionSettingKeys(settings, defaults)) {
        *error = *keyError;
        return std::nullopt;
    }
    const auto window = ParseModifierWindow(settings);
    if (!window) {
        *error = "random steering window or seed is invalid";
        return std::nullopt;
    }
    return RandomSteeringSettings{
            window->minimumTimeMs, window->maximumTimeMs, window->seed};
}

}  // namespace

RandomSteeringSettings DefaultRandomSteeringSettings() {
    return {1000, 5990, 1179926867u};
}

OptionFieldList RandomSteeringOptionFields() {
    OptionFieldList fields;
    AppendWindowFields(fields, "1000", "5990");
    AppendSeedField(fields);
    return fields;
}

OptionSettings DefaultRandomSteeringOptionSettings() {
    const RandomSteeringSettings defaults = DefaultRandomSteeringSettings();
    return {{"minTimeMs", std::to_string(defaults.minimumTimeMs)},
            {"maxTimeMs", std::to_string(defaults.maximumTimeMs)},
            {"seed", std::to_string(defaults.seed)}};
}

std::optional<std::string> ValidateRandomSteeringOptionSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) {
    std::string error;
    const auto parsed = ParseRandomSteeringSettings(settings, &error);
    if (parsed) {
        if (const auto windowError = ValidateTimeWindow(
                    parsed->minimumTimeMs,
                    parsed->maximumTimeMs,
                    tickDurationMs,
                    "mutation")) {
            error = *windowError;
        }
    }
    return error.empty() ? std::nullopt
                         : std::optional<std::string>(std::move(error));
}

std::unique_ptr<InputMutator> CreateRandomSteeringMutator(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) {
    if (const auto error = ValidateRandomSteeringOptionSettings(
                settings, tickDurationMs)) {
        throw std::invalid_argument(*error);
    }
    std::string error;
    const auto parsed = ParseRandomSteeringSettings(settings, &error);
    if (!parsed) {
        throw std::invalid_argument(error);
    }
    return std::make_unique<RandomSteeringMutator>(*parsed);
}

RandomSteeringMutator::RandomSteeringMutator(
        RandomSteeringSettings settings)
    : settings_(settings) {}

MutationResult RandomSteeringMutator::Mutate(
        const MutationRequest &request) const {
    using forevervalidator::experimental::PhysicsSandboxInputValueKind;

    std::mt19937 random = ModifierRandom(
            settings_.seed, request.iterationIndex, request.passIndex);

    MutationResult result;
    result.inputs = request.baselineInputs;
    for (SandboxInputEvent &event : result.inputs) {
        if (event.timeMs < settings_.minimumTimeMs ||
            event.timeMs > settings_.maximumTimeMs ||
            event.action != SandboxInputAction::Steer ||
            event.value.kind != PhysicsSandboxInputValueKind::Analog) {
            continue;
        }

        AnalogInputState value = RandomSteering(random);
        if (value == event.value.analog) {
            value = value == kAnalogInputMaximum
                    ? kAnalogInputMinimum
                    : kAnalogInputMaximum;
        }
        event.value.analog = value;
        ++result.mutationCount;
    }
    NormalizeMutableInputEvents(result.inputs,
                                request.baselineInputs,
                                request.tickDurationMs,
                                request.mutableFromTimeMs);
    result.mutationCount = EffectiveInputChangeCount(
            request.baselineInputs, result.inputs);
    return result;
}

std::int64_t RandomSteeringMutator::EarliestMutationTimeMs() const {
    return settings_.minimumTimeMs;
}

MutationTimeRange RandomSteeringMutator::AffectedTimeRange() const {
    return MutationTimeRange{
            settings_.minimumTimeMs, settings_.maximumTimeMs};
}

}  // namespace forevertas
