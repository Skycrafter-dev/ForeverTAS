#ifndef FOREVERTAS_EVALUATORS_SCRIPTED_TARGET_EVALUATOR_H
#define FOREVERTAS_EVALUATORS_SCRIPTED_TARGET_EVALUATOR_H

#include "evaluators/iteration_evaluator.h"
#include "searches/option_configuration.h"

#include <memory>
#include <optional>
#include <string>

#include <forevervalidator/experimental/physics_sandbox.h>

namespace forevertas {

OptionSettings DefaultScriptedTargetOptionSettings();
std::optional<std::string> ValidateScriptedTargetOptionSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs);
std::unique_ptr<IterationEvaluator> CreateScriptedTargetEvaluator(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs);
forevervalidator::experimental::PhysicsSandboxCudaScriptedEvaluator
BuildCudaScriptedTargetEvaluator(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs);

}  // namespace forevertas

#endif
