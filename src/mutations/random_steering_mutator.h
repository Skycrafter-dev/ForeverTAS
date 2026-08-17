#ifndef FOREVERTAS_MUTATIONS_RANDOM_STEERING_MUTATOR_H
#define FOREVERTAS_MUTATIONS_RANDOM_STEERING_MUTATOR_H

#include "mutations/input_mutator.h"
#include "searches/option_configuration.h"
#include "searches/option_fields.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace forevertas {

struct RandomSteeringSettings {
    std::int64_t minimumTimeMs = 1000;
    std::int64_t maximumTimeMs = 6000;
    std::uint32_t seed = 0u;
};

RandomSteeringSettings DefaultRandomSteeringSettings();
OptionSettings DefaultRandomSteeringOptionSettings();
OptionFieldList RandomSteeringOptionFields();
std::optional<std::string> ValidateRandomSteeringOptionSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs);
std::unique_ptr<InputMutator> CreateRandomSteeringMutator(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs);

class RandomSteeringMutator final : public InputMutator {
public:
    explicit RandomSteeringMutator(RandomSteeringSettings settings);

    MutationResult Mutate(const MutationRequest &request) const override;
    std::int64_t EarliestMutationTimeMs() const override;
    MutationTimeRange AffectedTimeRange() const override;

private:
    RandomSteeringSettings settings_;
};

}  // namespace forevertas

#endif
