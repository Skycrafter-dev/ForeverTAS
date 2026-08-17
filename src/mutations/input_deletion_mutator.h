#ifndef FOREVERTAS_MUTATIONS_INPUT_DELETION_MUTATOR_H
#define FOREVERTAS_MUTATIONS_INPUT_DELETION_MUTATOR_H

#include "mutations/input_mutator.h"
#include "searches/option_configuration.h"
#include "searches/option_fields.h"

#include <memory>
#include <optional>
#include <string>

namespace forevertas {

OptionSettings DefaultInputDeletionSettings();
OptionFieldList InputDeletionOptionFields();
std::optional<std::string> ValidateInputDeletionSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs);
std::unique_ptr<InputMutator> CreateInputDeletionMutator(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs);

}  // namespace forevertas

#endif
