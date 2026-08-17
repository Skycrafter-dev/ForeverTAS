#include "evaluators/stunt_points_evaluator.h"

#include "evaluators/evaluator_utils.h"
#include "searches/option_settings_utils.h"

#include <algorithm>
#include <stdexcept>

namespace forevertas {
namespace {

class StuntPointsSession final : public IterationEvaluationSession {
public:
    std::optional<EvaluationSample> Observe(
            const std::optional<
                    forevervalidator::experimental::PhysicsSandboxStateView>
                    &previous,
            const forevervalidator::experimental::PhysicsSandboxStateView
                    &current) override {
        static_cast<void>(previous);
        const std::uint32_t points = current.stuntsScore.value_or(0u);
        const double score = static_cast<double>(points);
        return EvaluationSample{
                score,
                static_cast<double>(current.timeMs),
                "Stunt points: " + std::to_string(points) + " at " +
                        FormatHumanDurationMilliseconds(
                                static_cast<double>(current.timeMs))};
    }
};

class StuntPointsEvaluator final : public IterationEvaluator {
public:
    explicit StuntPointsEvaluator(std::int64_t deadlineMs)
        : deadlineMs_(deadlineMs) {}

    EvaluationPlan Plan(std::int64_t simulationHorizonMs,
                        std::int64_t earliestMutationTimeMs,
                        std::uint32_t tickDurationMs) const override {
        static_cast<void>(earliestMutationTimeMs);
        static_cast<void>(tickDurationMs);
        static_cast<void>(simulationHorizonMs);
        return {deadlineMs_, deadlineMs_};
    }

    std::unique_ptr<IterationEvaluationSession> CreateSession()
            const override {
        return std::make_unique<StuntPointsSession>();
    }

    bool IsBetter(const EvaluationSample &iteration,
                  const EvaluationSample &incumbent) const override {
        return iteration.score > incumbent.score;
    }

private:
    std::int64_t deadlineMs_ = 0;
};

std::optional<std::int64_t> ParseDeadline(
        const OptionSettings &settings) {
    if (const auto found = settings.find("targetTimeMs");
        found != settings.end()) {
        return ParseSignedDecimal(found->second);
    }
    return std::nullopt;
}

}  // namespace

OptionFieldList StuntPointsOptionFields() {
    OptionFieldList fields;
    fields.push_back(NumberField("targetTimeMs", "Deadline (ms)", "6000"));
    return fields;
}

OptionSettings DefaultStuntPointsOptionSettings() {
    return {{"targetTimeMs", "6000"}};
}

std::optional<std::string> ValidateStuntPointsOptionSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) {
    if (const auto keyError = ValidateOptionSettingKeys(
                settings, DefaultStuntPointsOptionSettings())) {
        return keyError;
    }
    const auto deadline = ParseDeadline(settings);
    if (!deadline) {
        return "stunt points target time is invalid";
    }
    return ValidateTimeWindow(
            *deadline,
            *deadline,
            tickDurationMs,
            "stunt points target",
            true);
}

std::unique_ptr<IterationEvaluator> CreateStuntPointsEvaluator(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) {
    if (const auto error =
                ValidateStuntPointsOptionSettings(settings, tickDurationMs)) {
        throw std::invalid_argument(*error);
    }
    return std::make_unique<StuntPointsEvaluator>(*ParseDeadline(settings));
}

}  // namespace forevertas
