#ifndef FOREVERTAS_EVALUATORS_CUSTOM_VOLUME_ENTRY_EVALUATOR_H
#define FOREVERTAS_EVALUATORS_CUSTOM_VOLUME_ENTRY_EVALUATOR_H

#include "evaluators/iteration_evaluator.h"
#include "searches/option_configuration.h"

#include <memory>
#include <optional>
#include <string>

namespace forevertas {

OptionSettings DefaultCustomVolumeEntryOptionSettings();
forevervalidator::experimental::PhysicsSandboxCudaCustomVolumeEntryEvaluator
BuildCudaCustomVolumeEntryEvaluator(const OptionSettings &settings,
                                   std::uint32_t tickDurationMs);
std::optional<std::string> ValidateCustomVolumeEntryOptionSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs);
std::unique_ptr<IterationEvaluator> CreateCustomVolumeEntryEvaluator(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs);

}  // namespace forevertas

#endif
