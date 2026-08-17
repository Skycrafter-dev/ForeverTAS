#ifndef FOREVERTAS_EVALUATORS_STUNT_POINTS_EVALUATOR_H
#define FOREVERTAS_EVALUATORS_STUNT_POINTS_EVALUATOR_H

#include "evaluators/iteration_evaluator.h"
#include "searches/option_configuration.h"
#include "searches/option_fields.h"

#include <memory>
#include <optional>
#include <string>

namespace forevertas {

OptionSettings DefaultStuntPointsOptionSettings();
OptionFieldList StuntPointsOptionFields();
std::optional<std::string> ValidateStuntPointsOptionSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs);
std::unique_ptr<IterationEvaluator> CreateStuntPointsEvaluator(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs);

}  // namespace forevertas

#endif
