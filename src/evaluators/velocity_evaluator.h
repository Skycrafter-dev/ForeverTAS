#ifndef FOREVERTAS_EVALUATORS_VELOCITY_EVALUATOR_H
#define FOREVERTAS_EVALUATORS_VELOCITY_EVALUATOR_H

#include "evaluators/iteration_evaluator.h"
#include "searches/option_configuration.h"
#include "searches/option_fields.h"

#include <memory>
#include <optional>
#include <string>

namespace forevertas {

OptionSettings DefaultVelocityOptionSettings();
OptionFieldList VelocityOptionFields();
std::optional<std::string> ValidateVelocityOptionSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs);
std::unique_ptr<IterationEvaluator> CreateVelocityEvaluator(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs);

}  // namespace forevertas

#endif
