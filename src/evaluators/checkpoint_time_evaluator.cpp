#include "evaluators/checkpoint_time_evaluator.h"

#include "searches/option_settings_utils.h"
#include "time_format.h"

#include <stdexcept>

namespace forevertas {
namespace {
struct Settings {
    bool finish = false;
    std::uint32_t checkpointIndex = 0;
    std::uint32_t lap = 1;
    std::optional<std::uint32_t> checkpointSlot;
    std::uint64_t eventIndex = 0;
};

Settings ParseSettings(const OptionSettings &settings) {
    return {settings.at("eventType") == "finish",
            *ParseUnsignedDecimal32(settings.at("checkpointIndex")) - 1u,
            *ParseUnsignedDecimal32(settings.at("lap")),
            settings.at("checkpointSlot") == "-1" ? std::nullopt
                    : ParseUnsignedDecimal32(settings.at("checkpointSlot")),
            *ParseUnsignedDecimal64(settings.at("eventIndex"))};
}

class CheckpointTimeSession final : public IterationEvaluationSession {
public:
    explicit CheckpointTimeSession(Settings settings) : settings_(settings) {}
    bool IsComplete() const override { return reported_; }
    std::optional<EvaluationSample> Observe(
            const std::optional<forevervalidator::experimental::PhysicsSandboxStateView> &,
            const forevervalidator::experimental::PhysicsSandboxStateView &current) override {
        if (reported_) return std::nullopt;
        for (const auto &event : current.acceptedCheckpointEvents) {
            if (event.tick != current.tick || event.timeMs != current.timeMs ||
                event.finish != settings_.finish || event.lap != settings_.lap ||
                (!settings_.finish && event.checkpointIndex != settings_.checkpointIndex) ||
                (settings_.checkpointSlot && event.checkpointSlot != *settings_.checkpointSlot) ||
                (settings_.eventIndex != 0 && event.eventIndex != settings_.eventIndex)) continue;
            reported_ = true;
            const auto description = std::string(event.finish ? "Finish" : "Checkpoint") +
                    " event " + std::to_string(event.eventIndex) + ", slot " +
                    std::to_string(event.checkpointSlot) + ", lap " + std::to_string(event.lap) +
                    ": " + FormatRaceTimeMilliseconds(event.timeMs);
            return EvaluationSample{static_cast<double>(event.timeMs),
                                    static_cast<double>(event.timeMs), description};
        }
        return std::nullopt;
    }
private:
    Settings settings_;
    bool reported_ = false;
};

class CheckpointTimeEvaluator final : public IterationEvaluator {
public:
    explicit CheckpointTimeEvaluator(Settings settings) : settings_(settings) {}
    EvaluationPlan Plan(std::int64_t horizonMs, std::int64_t,
                        std::uint32_t tickDurationMs) const override {
        return {tickDurationMs, horizonMs};
    }
    std::unique_ptr<IterationEvaluationSession> CreateSession() const override {
        return std::make_unique<CheckpointTimeSession>(settings_);
    }
    bool IsBetter(const EvaluationSample &candidate, const EvaluationSample &incumbent) const override {
        return candidate.score < incumbent.score;
    }
private:
    Settings settings_;
};
}  // namespace

OptionSettings DefaultCheckpointTimeOptionSettings() {
    return {{"eventType", "checkpoint"}, {"checkpointIndex", "1"}, {"lap", "1"},
            {"checkpointSlot", "-1"}, {"eventIndex", "0"}};
}

std::optional<std::string> ValidateCheckpointTimeOptionSettings(
        const OptionSettings &settings, std::uint32_t tickDurationMs) {
    if (const auto error = ValidateOptionSettingKeys(settings, DefaultCheckpointTimeOptionSettings())) return error;
    if (tickDurationMs == 0) return "checkpoint time requires a positive tick duration";
    if (settings.at("eventType") != "checkpoint" && settings.at("eventType") != "finish")
        return "select a checkpoint or finish event";
    const auto checkpoint = ParseUnsignedDecimal32(settings.at("checkpointIndex"));
    const auto lap = ParseUnsignedDecimal32(settings.at("lap"));
    if (!checkpoint || *checkpoint == 0 || !lap || *lap == 0)
        return "checkpoint number and lap must be positive 32-bit whole numbers";
    if (settings.at("checkpointSlot") != "-1" && !ParseUnsignedDecimal32(settings.at("checkpointSlot")))
        return "checkpoint slot must be -1 (any) or a non-negative 32-bit whole number";
    if (!ParseUnsignedDecimal64(settings.at("eventIndex")))
        return "accepted event index must be a non-negative 64-bit whole number (0 selects any)";
    return std::nullopt;
}

std::unique_ptr<IterationEvaluator> CreateCheckpointTimeEvaluator(
        const OptionSettings &settings, std::uint32_t tickDurationMs) {
    if (const auto error = ValidateCheckpointTimeOptionSettings(settings, tickDurationMs))
        throw std::invalid_argument(*error);
    return std::make_unique<CheckpointTimeEvaluator>(ParseSettings(settings));
}
}  // namespace forevertas
