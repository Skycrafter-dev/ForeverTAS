#ifndef FOREVERTAS_EVALUATORS_PRECISE_FINISH_TIME_EVALUATOR_H
#define FOREVERTAS_EVALUATORS_PRECISE_FINISH_TIME_EVALUATOR_H

#include "evaluators/iteration_evaluator.h"
#include "searches/option_configuration.h"

#include <memory>
#include <optional>
#include <string>

namespace forevertas {

OptionSettings DefaultPreciseFinishTimeOptionSettings();
std::optional<std::string> ValidatePreciseFinishTimeOptionSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs);
std::unique_ptr<IterationEvaluator> CreatePreciseFinishTimeEvaluator(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs);

class PreciseFinishTimeEvaluator final : public IterationEvaluator {
public:
    EvaluationPlan Plan(std::int64_t simulationHorizonMs,
                        std::int64_t earliestMutationTimeMs,
                        std::uint32_t tickDurationMs) const override;
    std::unique_ptr<IterationEvaluationSession> CreateSession()
            const override;
    bool IsBetter(const EvaluationSample &iteration,
                  const EvaluationSample &incumbent) const override;
    std::optional<double> LatestImprovingTimeMs(
            const EvaluationSample &incumbent) const override;
};

}  // namespace forevertas

#endif
