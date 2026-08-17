#include "mutations/input_deletion_mutator.h"

#include "mutations/input_event_utils.h"
#include "mutations/modifier_utils.h"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace forevertas {
namespace {

struct ChannelSettings {
    bool enabled = false;
    std::uint32_t maximumCount = 0u;
};

struct Settings {
    ModifierWindow window;
    ChannelSettings steering;
    ChannelSettings accelerate;
    ChannelSettings brake;
};

std::optional<ChannelSettings> ParseChannel(const OptionSettings &settings,
                                            const char *enabledKey,
                                            const char *countKey) {
    const auto enabled = ParseBoolean(settings.at(enabledKey));
    const auto count = ParseUnsignedDecimal32(settings.at(countKey));
    if (!enabled || !count) return std::nullopt;
    return ChannelSettings{*enabled, *count};
}

std::optional<Settings> ParseSettings(const OptionSettings &settings) {
    const auto window = ParseModifierWindow(settings);
    const auto steering = ParseChannel(
            settings, "steerEnabled", "steerMaxCount");
    const auto accelerate = ParseChannel(
            settings, "accelerateEnabled", "accelerateMaxCount");
    const auto brake = ParseChannel(
            settings, "brakeEnabled", "brakeMaxCount");
    if (!window || !steering || !accelerate || !brake) return std::nullopt;
    return Settings{*window, *steering, *accelerate, *brake};
}

class InputDeletionMutator final : public InputMutator {
public:
    explicit InputDeletionMutator(Settings settings) : settings_(settings) {}

    MutationResult Mutate(const MutationRequest &request) const override {
        std::vector<SandboxInputEvent> inputs = request.baselineInputs;
        const std::vector<SandboxInputEvent> original = inputs;
        std::mt19937 random = ModifierRandom(
                settings_.window.seed, request.iterationIndex, request.passIndex);

        const auto deleteChannel = [&](const ChannelSettings &channel,
                                       auto matches) {
            if (!channel.enabled) return;
            const std::uint32_t requested = RandomInteger(
                    random, 0u, channel.maximumCount);
            for (std::uint32_t removal = 0u; removal < requested; ++removal) {
                std::vector<std::size_t> eligibleIndices;
                for (std::size_t index = 0u; index < inputs.size(); ++index) {
                    const SandboxInputEvent &event = inputs[index];
                    if (event.timeMs >= settings_.window.minimumTimeMs &&
                        event.timeMs <= settings_.window.maximumTimeMs &&
                        matches(event.action)) {
                        eligibleIndices.push_back(index);
                    }
                }
                if (eligibleIndices.empty()) break;
                const std::size_t selected = eligibleIndices[RandomInteger<std::size_t>(
                        random, 0u, eligibleIndices.size() - 1u)];
                inputs.erase(inputs.begin() + static_cast<std::ptrdiff_t>(selected));
            }
        };
        deleteChannel(settings_.steering, IsSteerAction);
        deleteChannel(settings_.accelerate, IsAccelerateAction);
        deleteChannel(settings_.brake, IsBrakeAction);
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

}  // namespace

OptionFieldList InputDeletionOptionFields() {
    OptionFieldList fields;
    AppendWindowFields(fields, "1000", "5990");
    AppendSeedField(fields);
    fields.push_back(BooleanField("steerEnabled", "Delete steering", true));
    fields.push_back(NumberField("steerMaxCount", "Max deletions", "2"));
    fields.push_back(
            BooleanField("accelerateEnabled", "Delete accelerate", false));
    fields.push_back(NumberField("accelerateMaxCount", "Max deletions", "1"));
    fields.push_back(BooleanField("brakeEnabled", "Delete brake", false));
    fields.push_back(NumberField("brakeMaxCount", "Max deletions", "1"));
    return fields;
}

OptionSettings DefaultInputDeletionSettings() {
    return {{"minTimeMs", "1000"},
            {"maxTimeMs", "5990"},
            {"seed", "1179926867"},
            {"steerEnabled", "true"},
            {"steerMaxCount", "2"},
            {"accelerateEnabled", "false"},
            {"accelerateMaxCount", "1"},
            {"brakeEnabled", "false"},
            {"brakeMaxCount", "1"}};
}

std::optional<std::string> ValidateInputDeletionSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) {
    if (const auto error = ValidateOptionSettingKeys(
                settings, DefaultInputDeletionSettings())) return error;
    const auto parsed = ParseSettings(settings);
    if (!parsed) return "input deletion settings are invalid";
    if (const auto error = ValidateModifierWindow(parsed->window,
                                                   tickDurationMs)) return error;
    if (!parsed->steering.enabled && !parsed->accelerate.enabled &&
        !parsed->brake.enabled) {
        return "input deletion must enable at least one channel";
    }
    return std::nullopt;
}

std::unique_ptr<InputMutator> CreateInputDeletionMutator(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) {
    if (const auto error = ValidateInputDeletionSettings(
                settings, tickDurationMs)) throw std::invalid_argument(*error);
    return std::make_unique<InputDeletionMutator>(*ParseSettings(settings));
}

}  // namespace forevertas
