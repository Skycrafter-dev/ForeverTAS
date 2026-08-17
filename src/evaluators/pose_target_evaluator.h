#ifndef FOREVERTAS_EVALUATORS_POSE_TARGET_EVALUATOR_H
#define FOREVERTAS_EVALUATORS_POSE_TARGET_EVALUATOR_H

#include "evaluators/iteration_evaluator.h"
#include "searches/option_configuration.h"
#include "searches/option_fields.h"

#include <memory>
#include <optional>
#include <string>

namespace forevertas {

OptionSettings DefaultPoseTargetOptionSettings();
OptionFieldList PoseTargetOptionFields();
std::optional<std::string> ValidatePoseTargetOptionSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs);
std::unique_ptr<IterationEvaluator> CreatePoseTargetEvaluator(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs);

}  // namespace forevertas

#endif
