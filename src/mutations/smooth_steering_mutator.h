#ifndef FOREVERTAS_MUTATIONS_SMOOTH_STEERING_MUTATOR_H
#define FOREVERTAS_MUTATIONS_SMOOTH_STEERING_MUTATOR_H

#include "mutations/input_mutator.h"
#include "searches/option_configuration.h"
#include "searches/option_fields.h"

#include <memory>
#include <optional>
#include <string>

namespace forevertas {

OptionSettings DefaultSmoothSteeringSettings();
OptionFieldList SmoothSteeringOptionFields();
std::optional<std::string> ValidateSmoothSteeringSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs);
std::unique_ptr<InputMutator> CreateSmoothSteeringMutator(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs);

}  // namespace forevertas

#endif
