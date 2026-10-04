#ifndef FOREVERTAS_EVALUATORS_TIME_EVALUATOR_H
#define FOREVERTAS_EVALUATORS_TIME_EVALUATOR_H

#include "evaluators/iteration_evaluator.h"
#include "searches/option_configuration.h"

#include <memory>
#include <optional>
#include <string>

namespace forevertas {

// Time target: the score is the first moment in the evaluation window when
// the search conditions hold; the goal makes it as early or as late as
// possible. Without conditions every run would score the window start.
OptionSettings DefaultTimeOptionSettings();
std::optional<std::string> ValidateTimeOptionSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs);
std::unique_ptr<IterationEvaluator> CreateTimeEvaluator(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs);
bool TimeEvaluatorMaximizes(const OptionSettings &settings);

}  // namespace forevertas

#endif
