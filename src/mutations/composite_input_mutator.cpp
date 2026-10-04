#include "mutations/composite_input_mutator.h"

#include "mutations/input_event_utils.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace forevertas {

CompositeInputMutator::CompositeInputMutator(
        std::vector<std::unique_ptr<InputMutator>> modifiers)
    : modifiers_(std::move(modifiers)) {
    for (const auto &modifier : modifiers_) {
        const MutationTimeRange range = modifier->AffectedTimeRange();
        if (!affectedTimeRange_) {
            affectedTimeRange_ = range;
        } else {
            affectedTimeRange_->minimumTimeMs = std::min(
                    affectedTimeRange_->minimumTimeMs,
                    range.minimumTimeMs);
            affectedTimeRange_->maximumTimeMs = std::max(
                    affectedTimeRange_->maximumTimeMs,
                    range.maximumTimeMs);
        }
    }
}

MutationResult CompositeInputMutator::Mutate(
        const MutationRequest &request) const {
    if (!request.preferWindowPatch || !affectedTimeRange_) {
        return MutateFull(request);
    }

    PrepareLocalBaseline(request);
    if (!localBaselineCache_.usable) return MutateFull(request);

    const std::int64_t patchMinimumTimeMs = std::max(
            affectedTimeRange_->minimumTimeMs,
            request.mutableFromTimeMs);
    if (patchMinimumTimeMs > affectedTimeRange_->maximumTimeMs) {
        return {};
    }

    std::vector<SandboxInputEvent> current = localBaselineCache_.events;
    for (std::size_t index = 0u; index < modifiers_.size(); ++index) {
        MutationResult pass = modifiers_[index]->Mutate({
                current,
                request.iterationIndex,
                static_cast<std::uint32_t>(index),
                request.tickDurationMs,
                request.mutableFromTimeMs,
                false,
                request.baselineGeneration,
                request.pruneRedundantAnalogInsertions,
                request.anchors,
                request.segment});
        current = pass.windowPatch
                ? ApplyInputWindowPatch(current, *pass.windowPatch)
                : std::move(pass.inputs);
    }
    NormalizeMutableInputEvents(current,
                                localBaselineCache_.events,
                                request.tickDurationMs,
                                request.mutableFromTimeMs);

    MutationWindowPatch patch;
    patch.minimumTimeMs = patchMinimumTimeMs;
    patch.maximumTimeMs = affectedTimeRange_->maximumTimeMs;
    const auto first = std::lower_bound(
            current.begin(), current.end(), patch.minimumTimeMs,
            [](const SandboxInputEvent &event, std::int64_t timeMs) {
                return event.timeMs < timeMs;
            });
    const auto last = std::upper_bound(
            first, current.end(), patch.maximumTimeMs,
            [](std::int64_t timeMs, const SandboxInputEvent &event) {
                return timeMs < event.timeMs;
            });
    patch.events.assign(first, last);
    const std::size_t mutationCount =
            EffectiveInputChangeCount(request.baselineInputs, patch);
    return {{}, mutationCount, std::move(patch)};
}

MutationResult CompositeInputMutator::MutateFull(
        const MutationRequest &request) const {
    std::vector<SandboxInputEvent> current = request.baselineInputs;
    for (std::size_t index = 0u; index < modifiers_.size(); ++index) {
        MutationResult pass = modifiers_[index]->Mutate({
                current,
                request.iterationIndex,
                static_cast<std::uint32_t>(index),
                request.tickDurationMs,
                request.mutableFromTimeMs,
                false,
                request.baselineGeneration,
                request.pruneRedundantAnalogInsertions,
                request.anchors,
                request.segment});
        current = pass.windowPatch
                ? ApplyInputWindowPatch(current, *pass.windowPatch)
                : std::move(pass.inputs);
    }
    NormalizeMutableInputEvents(current,
                                request.baselineInputs,
                                request.tickDurationMs,
                                request.mutableFromTimeMs);
    return {current,
            EffectiveInputChangeCount(request.baselineInputs, current)};
}

void CompositeInputMutator::PrepareLocalBaseline(
        const MutationRequest &request) const {
    if (localBaselineCache_.source == &request.baselineInputs &&
        localBaselineCache_.generation == request.baselineGeneration &&
        localBaselineCache_.tickDurationMs == request.tickDurationMs) {
        return;
    }

    localBaselineCache_ = {};
    localBaselineCache_.source = &request.baselineInputs;
    localBaselineCache_.generation = request.baselineGeneration;
    localBaselineCache_.tickDurationMs = request.tickDurationMs;
    if (!affectedTimeRange_ ||
        !InputEventsAreCanonical(
                request.baselineInputs, request.tickDurationMs)) {
        return;
    }

    const auto first = std::lower_bound(
            request.baselineInputs.begin(),
            request.baselineInputs.end(),
            affectedTimeRange_->minimumTimeMs,
            [](const SandboxInputEvent &event, std::int64_t timeMs) {
                return event.timeMs < timeMs;
            });
    const auto last = std::upper_bound(
            first,
            request.baselineInputs.end(),
            affectedTimeRange_->maximumTimeMs,
            [](std::int64_t timeMs, const SandboxInputEvent &event) {
                return timeMs < event.timeMs;
            });

    // State-dependent modifiers only need the most recent value of each
    // input channel before the shared mutation window.
    std::vector<std::size_t> contextIndices;
    const std::size_t firstIndex = static_cast<std::size_t>(
            first - request.baselineInputs.begin());
    for (std::size_t index = firstIndex; index != 0u;) {
        --index;
        const SandboxInputEvent &event = request.baselineInputs[index];
        const bool alreadyPresent = std::any_of(
                contextIndices.begin(), contextIndices.end(),
                [&](std::size_t selected) {
                    const SandboxInputEvent &existing =
                            request.baselineInputs[selected];
                    return existing.action == event.action &&
                            existing.value.kind == event.value.kind;
                });
        if (!alreadyPresent) contextIndices.push_back(index);
    }
    std::sort(contextIndices.begin(), contextIndices.end());

    localBaselineCache_.events.reserve(
            contextIndices.size() +
            static_cast<std::size_t>(last - first));
    for (const std::size_t index : contextIndices) {
        localBaselineCache_.events.push_back(request.baselineInputs[index]);
    }
    localBaselineCache_.events.insert(
            localBaselineCache_.events.end(), first, last);
    localBaselineCache_.usable = true;
}

std::int64_t CompositeInputMutator::EarliestMutationTimeMs() const {
    std::int64_t earliest = std::numeric_limits<std::int64_t>::max();
    for (const auto &modifier : modifiers_) {
        earliest = std::min(earliest, modifier->EarliestMutationTimeMs());
    }
    return earliest == std::numeric_limits<std::int64_t>::max()
            ? 0
            : earliest;
}

MutationTimeRange CompositeInputMutator::AffectedTimeRange() const {
    return affectedTimeRange_.value_or(MutationTimeRange{});
}

}  // namespace forevertas
