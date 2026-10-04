#ifndef FOREVERTAS_MUTATIONS_INPUT_MUTATOR_H
#define FOREVERTAS_MUTATIONS_INPUT_MUTATOR_H

#include "mutations/input_event_utils.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace forevertas {

struct MutationTimeRange {
    std::int64_t minimumTimeMs = 0;
    std::int64_t maximumTimeMs = 0;
};

// One applied mutation item: the earliest input time it changes, the pass
// that applied it, and the modifier-defined count slot that produced it.
struct MutationAnchor {
    std::int64_t timeMs = 0;
    std::uint32_t passIndex = 0u;
    std::uint32_t slot = 0u;
};

// Restricts a mutation to the items anchored inside one tree-search segment.
// slotCounts[passIndex][slot] is the number of items each pass must draw;
// modifiers that rewrite every eligible event ignore the counts.
struct MutationSegment {
    MutationTimeRange anchorRange;
    const std::vector<std::vector<std::uint32_t>> *slotCounts = nullptr;
};

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
    // When set, ordinary mutations also append the anchor of every applied
    // item without consuming additional random numbers.
    std::vector<MutationAnchor> *anchors = nullptr;
    // When set, draws only the segment's items instead of a whole candidate.
    const MutationSegment *segment = nullptr;
};

inline std::uint32_t SegmentSlotCount(const MutationRequest &request,
                                      std::uint32_t slot) {
    if (request.segment == nullptr ||
        request.segment->slotCounts == nullptr ||
        request.passIndex >= request.segment->slotCounts->size()) {
        return 0u;
    }
    const std::vector<std::uint32_t> &counts =
            (*request.segment->slotCounts)[request.passIndex];
    return slot < counts.size() ? counts[slot] : 0u;
}

// True for a segment draw in which this pass anchors no items. Count-driven
// passes then return their input unchanged and need no random engine.
inline bool SegmentPassIsEmpty(const MutationRequest &request) {
    if (request.segment == nullptr) return false;
    if (request.segment->slotCounts == nullptr ||
        request.passIndex >= request.segment->slotCounts->size()) {
        return true;
    }
    const std::vector<std::uint32_t> &counts =
            (*request.segment->slotCounts)[request.passIndex];
    return std::all_of(counts.begin(), counts.end(),
                       [](std::uint32_t count) { return count == 0u; });
}

inline bool InAnchorRange(const MutationSegment &segment,
                          std::int64_t timeMs) {
    return timeMs >= segment.anchorRange.minimumTimeMs &&
            timeMs <= segment.anchorRange.maximumTimeMs;
}

inline void RecordMutationAnchor(const MutationRequest &request,
                                 std::int64_t timeMs,
                                 std::uint32_t slot = 0u) {
    if (request.anchors != nullptr) {
        request.anchors->push_back({timeMs, request.passIndex, slot});
    }
}

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

// What a count-driven pass returns when SegmentPassIsEmpty: its input
// normalized exactly as a draw without items would leave it.
inline MutationResult UnchangedSegmentPass(const MutationRequest &request) {
    std::vector<SandboxInputEvent> inputs = request.baselineInputs;
    NormalizeMutableInputEvents(inputs,
                                request.baselineInputs,
                                request.tickDurationMs,
                                request.mutableFromTimeMs);
    const std::size_t mutationCount =
            EffectiveInputChangeCount(request.baselineInputs, inputs);
    return {std::move(inputs), mutationCount};
}

class InputMutator {
public:
    virtual ~InputMutator() = default;
    virtual MutationResult Mutate(const MutationRequest &request) const = 0;
    virtual std::int64_t EarliestMutationTimeMs() const = 0;
    virtual MutationTimeRange AffectedTimeRange() const = 0;
};

}  // namespace forevertas

#endif
