#include "evaluators/precise_finish_time_evaluator.h"

#include "evaluators/evaluator_utils.h"
#include "searches/option_settings_utils.h"
#include "time_format.h"

#include <stdexcept>

namespace forevertas {
namespace {

class PreciseFinishTimeSession final : public IterationEvaluationSession {
public:
    std::unique_ptr<IterationEvaluationSession> Clone() const override {
        return std::make_unique<PreciseFinishTimeSession>(*this);
    }

    std::optional<EvaluationSample> Observe(
            const std::optional<
                    forevervalidator::experimental::PhysicsSandboxStateView>
                    &previous,
            const forevervalidator::experimental::PhysicsSandboxStateView
                    &current) override {
        static_cast<void>(previous);
        if (reported_ || !current.raceCompleted ||
            !current.finishTime.has_value() ||
            !current.finishTime->IsValid()) {
            return std::nullopt;
        }
        reported_ = true;
        const std::uint64_t upperBoundNs =
                current.finishTime->upperBoundNs;
        const double timeMs =
                static_cast<double>(upperBoundNs) / 1000000.0;
        return EvaluationSample{
                static_cast<double>(upperBoundNs),
                timeMs,
                "Precise finish time: " +
                        FormatHumanDurationNanoseconds(
                                upperBoundNs)};
    }

private:
    bool reported_ = false;
};

}  // namespace

OptionSettings DefaultPreciseFinishTimeOptionSettings() {
    return {};
}

std::optional<std::string> ValidatePreciseFinishTimeOptionSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) {
    static_cast<void>(tickDurationMs);
    return ValidateOptionSettingKeys(
            settings, DefaultPreciseFinishTimeOptionSettings());
}

std::unique_ptr<IterationEvaluator> CreatePreciseFinishTimeEvaluator(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) {
    if (const auto error =
                ValidatePreciseFinishTimeOptionSettings(
                        settings, tickDurationMs)) {
        throw std::invalid_argument(*error);
    }
    return std::make_unique<PreciseFinishTimeEvaluator>();
}

EvaluationPlan PreciseFinishTimeEvaluator::Plan(
        std::int64_t simulationHorizonMs,
        std::int64_t earliestMutationTimeMs,
        std::uint32_t tickDurationMs) const {
    static_cast<void>(tickDurationMs);
    return {earliestMutationTimeMs, simulationHorizonMs};
}

std::unique_ptr<IterationEvaluationSession>
PreciseFinishTimeEvaluator::CreateSession() const {
    return std::make_unique<PreciseFinishTimeSession>();
}

bool PreciseFinishTimeEvaluator::IsBetter(
        const EvaluationSample &iteration,
        const EvaluationSample &incumbent) const {
    return iteration.score < incumbent.score;
}

}  // namespace forevertas
