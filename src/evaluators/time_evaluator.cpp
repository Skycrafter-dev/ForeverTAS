#include "evaluators/time_evaluator.h"

#include "evaluators/evaluator_utils.h"
#include "searches/option_settings_utils.h"
#include "time_format.h"

#include <algorithm>
#include <stdexcept>

namespace forevertas {
namespace {

struct TimeSettings {
    std::int64_t minimumTimeMs = 0;
    std::int64_t maximumTimeMs = 0;
    bool latest = false;
};

// The search only observes ticks on which the conditions hold, so the first
// observation is the moment they are first met.
class TimeSession final : public IterationEvaluationSession {
public:
    std::unique_ptr<IterationEvaluationSession> Clone() const override {
        return std::make_unique<TimeSession>(*this);
    }
    bool IsComplete() const override { return reported_; }

    std::optional<EvaluationSample> Observe(
            const std::optional<
                    forevervalidator::experimental::PhysicsSandboxStateView> &,
            const forevervalidator::experimental::PhysicsSandboxStateView
                    &current) override {
        if (reported_) return std::nullopt;
        reported_ = true;
        const auto timeMs = static_cast<double>(current.timeMs);
        return EvaluationSample{
                timeMs, timeMs,
                "Conditions first met at " + FormatRaceTimeMilliseconds(timeMs)};
    }

private:
    bool reported_ = false;
};

class TimeEvaluator final : public IterationEvaluator {
public:
    explicit TimeEvaluator(TimeSettings settings) : settings_(settings) {}

    EvaluationPlan Plan(std::int64_t simulationHorizonMs,
                        std::int64_t earliestMutationTimeMs,
                        std::uint32_t tickDurationMs) const override {
        static_cast<void>(simulationHorizonMs);
        // Conditions met before the first mutation still count: a later
        // first match would not be the truth for that run.
        static_cast<void>(earliestMutationTimeMs);
        return {std::max<std::int64_t>(settings_.minimumTimeMs, tickDurationMs),
                settings_.maximumTimeMs};
    }
    std::unique_ptr<IterationEvaluationSession> CreateSession() const override {
        return std::make_unique<TimeSession>();
    }
    bool IsBetter(const EvaluationSample &iteration,
                  const EvaluationSample &incumbent) const override {
        return settings_.latest ? iteration.score > incumbent.score
                                : iteration.score < incumbent.score;
    }

private:
    TimeSettings settings_;
};

std::optional<TimeSettings> ParseSettings(const OptionSettings &settings) {
    const auto minimum = ReadTimeSetting(settings, "minTimeMs");
    const auto maximum = ReadTimeSetting(settings, "maxTimeMs");
    const std::string &goal = settings.at("goal");
    if (!minimum || !maximum || (goal != "earliest" && goal != "latest"))
        return std::nullopt;
    return TimeSettings{*minimum, *maximum, goal == "latest"};
}

}  // namespace

OptionSettings DefaultTimeOptionSettings() {
    return {{"minTimeMs", "0"}, {"maxTimeMs", "6000"}, {"goal", "earliest"}};
}

std::optional<std::string> ValidateTimeOptionSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) {
    if (const auto keyError =
                ValidateOptionSettingKeys(settings, DefaultTimeOptionSettings())) {
        return keyError;
    }
    const auto parsed = ParseSettings(settings);
    if (!parsed) return "time target settings contain invalid values";
    return ValidateTimeWindow(parsed->minimumTimeMs,
                              parsed->maximumTimeMs,
                              tickDurationMs,
                              "evaluation",
                              true);
}

std::unique_ptr<IterationEvaluator> CreateTimeEvaluator(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) {
    if (const auto error = ValidateTimeOptionSettings(settings, tickDurationMs)) {
        throw std::invalid_argument(*error);
    }
    return std::make_unique<TimeEvaluator>(*ParseSettings(settings));
}

bool TimeEvaluatorMaximizes(const OptionSettings &settings) {
    const auto goal = settings.find("goal");
    return goal != settings.end() && goal->second == "latest";
}

}  // namespace forevertas
