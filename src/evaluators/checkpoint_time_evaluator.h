#ifndef FOREVERTAS_EVALUATORS_CHECKPOINT_TIME_EVALUATOR_H
#define FOREVERTAS_EVALUATORS_CHECKPOINT_TIME_EVALUATOR_H

#include "evaluators/iteration_evaluator.h"
#include "searches/option_configuration.h"

namespace forevertas {
OptionSettings DefaultCheckpointTimeOptionSettings();
std::optional<std::string> ValidateCheckpointTimeOptionSettings(
        const OptionSettings &settings, std::uint32_t tickDurationMs);
std::unique_ptr<IterationEvaluator> CreateCheckpointTimeEvaluator(
        const OptionSettings &settings, std::uint32_t tickDurationMs);
}  // namespace forevertas

#endif
