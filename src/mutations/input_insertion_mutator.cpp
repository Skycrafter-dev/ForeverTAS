#include "mutations/input_insertion_mutator.h"

#include "mutations/input_event_utils.h"
#include "mutations/modifier_utils.h"

#include <algorithm>
#include <stdexcept>

namespace forevertas {
namespace {

struct ChannelSettings {
    bool enabled = false;
    std::uint32_t minimumCount = 0u;
    std::uint32_t maximumCount = 0u;
    std::int64_t maximumHoldMs = 0;
};

struct Settings {
    ModifierWindow window;
    ChannelSettings steering;
    ChannelSettings accelerate;
    ChannelSettings brake;
    bool steeringOffset = false;
    AnalogInputState steeringAbsoluteMinimum = kAnalogInputMinimum;
    AnalogInputState steeringAbsoluteMaximum = kAnalogInputMaximum;
    AnalogInputState steeringOffsetMinimum = 0;
    AnalogInputState steeringOffsetMaximum = 0;
};

std::optional<ChannelSettings> ParseChannel(const OptionSettings &settings,
                                            const char *enabledKey,
                                            const char *minimumCountKey,
                                            const char *countKey,
                                            const char *holdKey) {
    const auto enabled = ParseBoolean(settings.at(enabledKey));
    const auto minimumCount =
            ParseUnsignedDecimal32(settings.at(minimumCountKey));
    const auto count = ParseUnsignedDecimal32(settings.at(countKey));
    const auto hold = ParseSignedDecimal(settings.at(holdKey));
    if (!enabled || !minimumCount || !count || !hold) return std::nullopt;
    return ChannelSettings{*enabled, *minimumCount, *count, *hold};
}

std::optional<Settings> ParseSettings(const OptionSettings &settings) {
    const auto window = ParseModifierWindow(settings);
    const auto steering = ParseChannel(settings,
                                       "steerEnabled",
                                       "steerMinCount",
                                       "steerMaxCount",
                                       "steerMaxHoldMs");
    const auto accelerate = ParseChannel(settings,
                                         "accelerateEnabled",
                                         "accelerateMinCount",
                                         "accelerateMaxCount",
                                         "accelerateMaxHoldMs");
    const auto brake = ParseChannel(settings,
                                    "brakeEnabled",
                                    "brakeMinCount",
                                    "brakeMaxCount",
                                    "brakeMaxHoldMs");
    const auto absoluteMinimum =
            ParseNormalizedAnalogInput(settings.at("steerAbsoluteMin"));
    const auto absoluteMaximum =
            ParseNormalizedAnalogInput(settings.at("steerAbsoluteMax"));
    const auto offsetMinimum =
            ParseNormalizedAnalogInput(settings.at("steerOffsetMin"));
    const auto offsetMaximum =
            ParseNormalizedAnalogInput(settings.at("steerOffsetMax"));
    const std::string &mode = settings.at("steerMode");
    if (!window || !steering || !accelerate || !brake || !absoluteMinimum ||
        !absoluteMaximum || !offsetMinimum || !offsetMaximum ||
        (mode != "absolute" && mode != "offset")) {
        return std::nullopt;
    }
    return Settings{*window,
                    *steering,
                    *accelerate,
                    *brake,
                    mode == "offset",
                    *absoluteMinimum,
                    *absoluteMaximum,
                    *offsetMinimum,
                    *offsetMaximum};
}

void RemoveChannelEvents(std::vector<SandboxInputEvent> &inputs,
                         SandboxInputAction action,
                         std::int64_t start,
                         std::int64_t end) {
    inputs.erase(std::remove_if(inputs.begin(), inputs.end(),
                                [=](const SandboxInputEvent &event) {
                                    return event.action == action &&
                                           event.timeMs >= start &&
                                           event.timeMs <= end;
                                }),
                 inputs.end());
}

class InputInsertionMutator final : public InputMutator {
public:
    explicit InputInsertionMutator(Settings settings) : settings_(settings) {}

    MutationResult Mutate(const MutationRequest &request) const override {
        std::vector<SandboxInputEvent> inputs = request.baselineInputs;
        const std::vector<SandboxInputEvent> original = inputs;
        std::mt19937 random = ModifierRandom(
                settings_.window.seed, request.iterationIndex, request.passIndex);
        const std::int64_t tick = request.tickDurationMs;

        const auto randomTime = [&]() {
            return RandomInteger<std::int64_t>(
                           random,
                           settings_.window.minimumTimeMs / tick,
                           settings_.window.maximumTimeMs / tick) * tick;
        };
        const auto randomHold = [&](std::int64_t maximum) {
            if (maximum <= 0) return std::int64_t{0};
            return RandomInteger<std::int64_t>(
                           random, 0, maximum / tick) * tick;
        };

        const std::uint32_t steeringCount = settings_.steering.enabled
                ? RandomInteger(random,
                                settings_.steering.minimumCount,
                                settings_.steering.maximumCount)
                : 0u;
        for (std::uint32_t index = 0u; index < steeringCount; ++index) {
            const std::int64_t start = randomTime();
            const std::int64_t end = std::min(
                    settings_.window.maximumTimeMs,
                    start + randomHold(settings_.steering.maximumHoldMs));
            const AnalogInputState previous = SteeringStateAt(inputs, start);
            const AnalogInputState value = settings_.steeringOffset
                    ? SaturateAnalogInputState(
                              static_cast<std::int64_t>(previous) +
                              RandomInteger<AnalogInputState>(
                                      random,
                                      settings_.steeringOffsetMinimum,
                                      settings_.steeringOffsetMaximum))
                    : RandomInteger<AnalogInputState>(
                              random,
                              settings_.steeringAbsoluteMinimum,
                              settings_.steeringAbsoluteMaximum);
            RemoveChannelEvents(inputs, SandboxInputAction::Steer, start, end);
            inputs.push_back(AnalogEvent(start, SandboxInputAction::Steer,
                                         value));
            if (end > start) {
                inputs.push_back(AnalogEvent(
                        end,
                        SandboxInputAction::Steer,
                        SteeringStateAt(original, end)));
            }
        }

        const auto insertSwitch = [&](const ChannelSettings &channel,
                                      SandboxInputAction action) {
            if (!channel.enabled) return;
            const std::uint32_t count = RandomInteger(
                    random, channel.minimumCount, channel.maximumCount);
            for (std::uint32_t index = 0u; index < count; ++index) {
                const std::int64_t start = randomTime();
                const std::int64_t end = std::min(
                        settings_.window.maximumTimeMs,
                        start + randomHold(channel.maximumHoldMs));
                const bool previous = SwitchStateAt(inputs, action, start);
                RemoveChannelEvents(inputs, action, start, end);
                inputs.push_back(SwitchEvent(start, action, !previous));
                if (end > start) {
                    inputs.push_back(SwitchEvent(
                            end, action, SwitchStateAt(original, action, end)));
                }
            }
        };
        insertSwitch(settings_.accelerate, SandboxInputAction::Accelerate);
        insertSwitch(settings_.brake, SandboxInputAction::Brake);
        NormalizeMutableInputEvents(inputs,
                                    request.baselineInputs,
                                    request.tickDurationMs,
                                    request.mutableFromTimeMs);
        return {inputs, EffectiveInputChangeCount(original, inputs)};
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
    Settings settings_;
};

std::optional<std::string> ValidateChannel(const ChannelSettings &channel,
                                           std::uint32_t tickDurationMs,
                                           const char *name) {
    if (channel.maximumHoldMs < 0 ||
        channel.maximumHoldMs % tickDurationMs != 0) {
        return std::string(name) +
                " maximum hold time must be a non-negative whole-tick value";
    }
    if (channel.minimumCount > channel.maximumCount) {
        return std::string(name) +
                " minimum count must not exceed maximum count";
    }
    return std::nullopt;
}

}  // namespace

OptionFieldList InputInsertionOptionFields() {
    OptionFieldList fields;
    AppendWindowFields(fields, "1000", "5990");
    AppendSeedField(fields);
    fields.push_back(BooleanField("steerEnabled", "Insert steering", true));
    fields.push_back(EnumField("steerMode", "Steering values",
            {{"offset", "Offset from current"},
             {"absolute", "Absolute"}}));
    fields.push_back(ClampedField(
            "steerAbsoluteMin", "Absolute min", "-1", -1.0, 1.0, 3, 0.01));
    fields.push_back(ClampedField(
            "steerAbsoluteMax", "Absolute max", "1", -1.0, 1.0, 3, 0.01));
    fields.push_back(ClampedField(
            "steerOffsetMin", "Offset min", "-0.2", -1.0, 1.0, 3, 0.01));
    fields.push_back(ClampedField(
            "steerOffsetMax", "Offset max", "0.2", -1.0, 1.0, 3, 0.01));
    fields.push_back(NumberField("steerMinCount", "Min inserts", "0"));
    fields.push_back(NumberField("steerMaxCount", "Max inserts", "2"));
    fields.push_back(NumberField("steerMaxHoldMs", "Max hold (ms)", "200"));
    fields.push_back(
            BooleanField("accelerateEnabled", "Insert accelerate", false));
    fields.push_back(NumberField("accelerateMinCount", "Min inserts", "0"));
    fields.push_back(NumberField("accelerateMaxCount", "Max inserts", "1"));
    fields.push_back(
            NumberField("accelerateMaxHoldMs", "Max hold (ms)", "200"));
    fields.push_back(BooleanField("brakeEnabled", "Insert brake", false));
    fields.push_back(NumberField("brakeMinCount", "Min inserts", "0"));
    fields.push_back(NumberField("brakeMaxCount", "Max inserts", "1"));
    fields.push_back(NumberField("brakeMaxHoldMs", "Max hold (ms)", "200"));
    return fields;
}

OptionSettings DefaultInputInsertionSettings() {
    return {{"minTimeMs", "1000"},
            {"maxTimeMs", "5990"},
            {"seed", "1179926867"},
            {"steerEnabled", "true"},
            {"steerMode", "offset"},
            {"steerAbsoluteMin", "-1"},
            {"steerAbsoluteMax", "1"},
            {"steerOffsetMin", "-0.2"},
            {"steerOffsetMax", "0.2"},
            {"steerMinCount", "0"},
            {"steerMaxCount", "2"},
            {"steerMaxHoldMs", "200"},
            {"accelerateEnabled", "false"},
            {"accelerateMinCount", "0"},
            {"accelerateMaxCount", "1"},
            {"accelerateMaxHoldMs", "200"},
            {"brakeEnabled", "false"},
            {"brakeMinCount", "0"},
            {"brakeMaxCount", "1"},
            {"brakeMaxHoldMs", "200"}};
}

std::optional<std::string> ValidateInputInsertionSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) {
    if (const auto error = ValidateOptionSettingKeys(
                settings, DefaultInputInsertionSettings())) return error;
    const auto parsed = ParseSettings(settings);
    if (!parsed) return "input insertion settings are invalid";
    if (const auto error = ValidateModifierWindow(parsed->window,
                                                   tickDurationMs)) return error;
    if (const auto error = ValidateChannel(
                parsed->steering, tickDurationMs, "steering")) return error;
    if (const auto error = ValidateChannel(
                parsed->accelerate, tickDurationMs, "accelerate")) return error;
    if (const auto error = ValidateChannel(
                parsed->brake, tickDurationMs, "brake")) return error;
    if (parsed->steeringAbsoluteMinimum >
                parsed->steeringAbsoluteMaximum ||
        parsed->steeringOffsetMinimum > parsed->steeringOffsetMaximum) {
        return "input insertion steering ranges are invalid";
    }
    return std::nullopt;
}

std::unique_ptr<InputMutator> CreateInputInsertionMutator(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) {
    if (const auto error = ValidateInputInsertionSettings(
                settings, tickDurationMs)) throw std::invalid_argument(*error);
    return std::make_unique<InputInsertionMutator>(*ParseSettings(settings));
}

}  // namespace forevertas
