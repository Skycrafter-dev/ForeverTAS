#ifndef FOREVERTAS_MUTATIONS_EXISTING_EVENT_PERTURBATION_MUTATOR_H
#define FOREVERTAS_MUTATIONS_EXISTING_EVENT_PERTURBATION_MUTATOR_H

#include "mutations/input_mutator.h"
#include "searches/option_configuration.h"
#include "searches/option_fields.h"

#include <memory>
#include <optional>
#include <string>

namespace forevertas {

OptionSettings DefaultExistingEventPerturbationSettings();
OptionFieldList ExistingEventPerturbationOptionFields();
std::optional<std::string> ValidateExistingEventPerturbationSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs);
std::unique_ptr<InputMutator> CreateExistingEventPerturbationMutator(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs);

}  // namespace forevertas

#endif
