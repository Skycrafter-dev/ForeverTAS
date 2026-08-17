#ifndef FOREVERTAS_EVALUATORS_VOLUME_ENTRY_EVALUATOR_H
#define FOREVERTAS_EVALUATORS_VOLUME_ENTRY_EVALUATOR_H

#include "evaluators/iteration_evaluator.h"
#include "searches/option_configuration.h"
#include "searches/option_fields.h"

#include <memory>
#include <optional>
#include <string>

namespace forevertas {

OptionSettings DefaultVolumeEntryOptionSettings();
OptionFieldList VolumeEntryOptionFields();
std::optional<std::string> ValidateVolumeEntryOptionSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs);
std::unique_ptr<IterationEvaluator> CreateVolumeEntryEvaluator(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs);

}  // namespace forevertas

#endif
