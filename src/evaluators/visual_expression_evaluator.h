#ifndef FOREVERTAS_EVALUATORS_VISUAL_EXPRESSION_EVALUATOR_H
#define FOREVERTAS_EVALUATORS_VISUAL_EXPRESSION_EVALUATOR_H

#include "evaluators/iteration_evaluator.h"
#include "searches/option_configuration.h"

#include <forevervalidator/experimental/physics_sandbox.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace forevertas {

// Internal runtime target used when a valid v3 expression does not match one
// of the specialized evaluator fast paths. It is intentionally not exposed in
// EvaluationTargetRegistry(): users build it compositionally in Blockly.
inline constexpr char kVisualExpressionEvaluationId[] = "visual-expression";

OptionSettings DefaultVisualExpressionOptionSettings();

std::optional<std::string> ValidateVisualExpressionOptionSettings(
    const OptionSettings &settings, std::uint32_t tickDurationMs);

std::unique_ptr<IterationEvaluator> CreateVisualExpressionEvaluator(
    const OptionSettings &settings, std::uint32_t tickDurationMs);

std::optional<forevervalidator::experimental::
                  PhysicsSandboxCudaExpressionEvaluator>
BuildCudaVisualExpressionEvaluator(const OptionSettings &settings,
                                   std::uint32_t tickDurationMs,
                                   std::string *error = nullptr);

} // namespace forevertas

#endif
