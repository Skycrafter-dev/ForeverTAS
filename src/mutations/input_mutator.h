#ifndef FOREVERTAS_MUTATIONS_INPUT_MUTATOR_H
#define FOREVERTAS_MUTATIONS_INPUT_MUTATOR_H

#include "mutations/input_event_utils.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace forevertas {

struct MutationRequest {
    const std::vector<SandboxInputEvent> &baselineInputs;
    std::uint64_t iterationIndex = 0u;
    std::uint32_t passIndex = 0u;
    std::uint32_t tickDurationMs = 10u;
    std::int64_t mutableFromTimeMs =
            std::numeric_limits<std::int64_t>::min();
    bool preferWindowPatch = false;
    std::uint64_t baselineGeneration = 0u;
    // Only the search can establish that analog timestamps are unobservable.
    bool pruneRedundantAnalogInsertions = false;
};

struct MutationTimeRange {
    std::int64_t minimumTimeMs = 0;
    std::int64_t maximumTimeMs = 0;
};

struct MutationWindowPatch {
    std::int64_t minimumTimeMs = 0;
    std::int64_t maximumTimeMs = 0;
    std::vector<SandboxInputEvent> events;
};

struct MutationResult {
    std::vector<SandboxInputEvent> inputs;
    std::size_t mutationCount = 0u;
    std::optional<MutationWindowPatch> windowPatch;

    MutationResult() = default;
    MutationResult(
            std::vector<SandboxInputEvent> configuredInputs,
            std::size_t configuredMutationCount,
            std::optional<MutationWindowPatch> configuredWindowPatch =
                    std::nullopt)
        : inputs(std::move(configuredInputs)),
          mutationCount(configuredMutationCount),
          windowPatch(std::move(configuredWindowPatch)) {}
};

class InputMutator {
public:
    virtual ~InputMutator() = default;
    virtual MutationResult Mutate(const MutationRequest &request) const = 0;
    virtual std::int64_t EarliestMutationTimeMs() const = 0;
    virtual MutationTimeRange AffectedTimeRange() const = 0;
};

}  // namespace forevertas

#endif
