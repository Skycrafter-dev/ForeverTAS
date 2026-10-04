#include "mutations/existing_event_perturbation_mutator.h"

#include "mutations/input_event_utils.h"
#include "mutations/modifier_utils.h"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace forevertas {
namespace {

struct Settings {
    ModifierWindow window;
    std::uint32_t minimumCount = 1u;
    std::uint32_t maximumCount = 1u;
    std::int64_t maximumTimeShiftMs = 0;
    bool absoluteSteering = false;
    AnalogInputState steeringDeltaMinimum = 0;
    AnalogInputState steeringDeltaMaximum = 0;
    AnalogInputState steeringAbsoluteMinimum = kAnalogInputMinimum;
    AnalogInputState steeringAbsoluteMaximum = kAnalogInputMaximum;
    bool toggleAccelerate = true;
    bool toggleBrake = true;
};

// std::shuffle produces different sequences with MSVC and libstdc++.
void ShuffleIndices(std::vector<std::size_t> &indices,
                    std::mt19937 &random) {
    const std::size_t count = indices.size();
    if (count <= 1u) return;
    const std::uint64_t generatorRange = UINT32_MAX;
    if (generatorRange / count >= count) {
        std::size_t index = 1u;
        if (count % 2u == 0u) {
            std::swap(indices[index], indices[RandomInteger(random, 0u, 1u)]);
            ++index;
        }
        while (index != count) {
            const std::uint64_t swapRange = index + 1u;
            const std::uint64_t position = RandomUnsigned(
                    random, 0u, swapRange * (swapRange + 1u) - 1u);
            const std::size_t first = position / (swapRange + 1u);
            const std::size_t second = position % (swapRange + 1u);
            std::swap(indices[index], indices[first]);
            ++index;
            std::swap(indices[index], indices[second]);
            ++index;
        }
    } else {
        for (std::size_t index = 1u; index < count; ++index) {
            std::swap(indices[index], indices[RandomUnsigned(random, 0u, index)]);
        }
    }
}

std::optional<Settings> ParseSettings(const OptionSettings &settings) {
    const auto window = ParseModifierWindow(settings);
    const auto minimumCount = ParseUnsignedDecimal32(settings.at("minCount"));
    const auto maximumCount = ParseUnsignedDecimal32(settings.at("maxCount"));
    const auto maximumShift = ParseSignedDecimal(settings.at("maxTimeShiftMs"));
    const auto deltaMinimum =
            ParseNormalizedAnalogInput(settings.at("steerDeltaMin"));
    const auto deltaMaximum =
            ParseNormalizedAnalogInput(settings.at("steerDeltaMax"));
    const auto absoluteMinimum =
            ParseNormalizedAnalogInput(settings.at("steerAbsoluteMin"));
    const auto absoluteMaximum =
            ParseNormalizedAnalogInput(settings.at("steerAbsoluteMax"));
    const auto toggleAccelerate = ParseBoolean(settings.at("toggleAccelerate"));
    const auto toggleBrake = ParseBoolean(settings.at("toggleBrake"));
    const std::string &mode = settings.at("steerMode");
    if (!window || !minimumCount || !maximumCount || !maximumShift ||
        !deltaMinimum || !deltaMaximum || !absoluteMinimum ||
        !absoluteMaximum || !toggleAccelerate || !toggleBrake ||
        (mode != "delta" && mode != "absolute")) {
        return std::nullopt;
    }
    return Settings{*window,
                    *minimumCount,
                    *maximumCount,
                    *maximumShift,
                    mode == "absolute",
                    *deltaMinimum,
                    *deltaMaximum,
                    *absoluteMinimum,
                    *absoluteMaximum,
                    *toggleAccelerate,
                    *toggleBrake};
}

class ExistingEventPerturbationMutator final : public InputMutator {
public:
    explicit ExistingEventPerturbationMutator(Settings settings)
        : settings_(settings) {}

    MutationResult Mutate(const MutationRequest &request) const override {
        if (request.segment != nullptr) return MutateSegment(request);
        PrepareWindow(request.baselineInputs);
        std::vector<SandboxInputEvent> inputs = cachedWindowInputs_;
        std::vector<std::size_t> eligibleIndices = cachedEligibleIndices_;
        if (eligibleIndices.empty()) {
            MutationWindowPatch patch{
                    settings_.window.minimumTimeMs,
                    settings_.window.maximumTimeMs,
                    std::move(inputs)};
            if (request.preferWindowPatch) {
                return {{}, 0u, std::move(patch)};
            }
            return {ApplyInputWindowPatch(request.baselineInputs, patch), 0u};
        }

        std::mt19937 random = ModifierRandom(
                settings_.window.seed, request.iterationIndex, request.passIndex);
        ShuffleIndices(eligibleIndices, random);
        const std::uint32_t requested = RandomInteger(
                random, settings_.minimumCount, settings_.maximumCount);
        const std::size_t count = std::min<std::size_t>(
                requested, eligibleIndices.size());
        const std::int64_t tick = request.tickDurationMs;
        const std::int64_t maximumShiftTicks = tick == 0
                ? 0
                : settings_.maximumTimeShiftMs / tick;
        for (std::size_t n = 0u; n < count; ++n) {
            SandboxInputEvent &event = inputs[eligibleIndices[n]];
            const std::int64_t originalTimeMs = event.timeMs;
            const std::int64_t shiftTicks = RandomInteger<std::int64_t>(
                    random, -maximumShiftTicks, maximumShiftTicks);
            event.timeMs = static_cast<std::int32_t>(
                    ShiftedTimeMs(originalTimeMs, shiftTicks * tick));
            PerturbValue(event, random);
            RecordMutationAnchor(
                    request,
                    std::min<std::int64_t>(originalTimeMs, event.timeMs));
        }
        NormalizeInputEvents(inputs, request.tickDurationMs);
        const std::size_t mutationCount =
                EffectiveInputChangeCount(cachedWindowInputs_, inputs);
        MutationWindowPatch patch{
                settings_.window.minimumTimeMs,
                settings_.window.maximumTimeMs,
                std::move(inputs)};
        if (request.preferWindowPatch) {
            return {{}, mutationCount, std::move(patch)};
        }
        return {ApplyInputWindowPatch(request.baselineInputs, patch),
                mutationCount};
    }

    std::int64_t EarliestMutationTimeMs() const override {
        return settings_.window.minimumTimeMs;
    }

    MutationTimeRange AffectedTimeRange() const override {
        return MutationTimeRange{
                settings_.window.minimumTimeMs,
                settings_.window.maximumTimeMs};
    }

private:
    // Draws only the events whose earliest changed tick falls inside the
    // segment. Rejection keeps each (event, shift) pair distributed as an
    // ordinary draw conditioned on that anchor.
    MutationResult MutateSegment(const MutationRequest &request) const {
        if (SegmentPassIsEmpty(request)) return UnchangedSegmentPass(request);
        const MutationSegment &segment = *request.segment;
        std::vector<SandboxInputEvent> inputs = request.baselineInputs;
        const std::int64_t tick = request.tickDurationMs;
        const std::int64_t maximumShiftTicks = tick == 0
                ? 0
                : settings_.maximumTimeShiftMs / tick;
        // An event before the segment, or more than one maximal shift after
        // it, can never anchor inside it.
        const std::int64_t latestCandidateMs =
                segment.anchorRange.maximumTimeMs + maximumShiftTicks * tick;
        std::vector<std::size_t> candidates;
        for (std::size_t index = 0u; index < inputs.size(); ++index) {
            const SandboxInputEvent &event = inputs[index];
            if (event.timeMs >= settings_.window.minimumTimeMs &&
                event.timeMs <= settings_.window.maximumTimeMs &&
                event.timeMs >= segment.anchorRange.minimumTimeMs &&
                event.timeMs <= latestCandidateMs &&
                IsEligible(event)) {
                candidates.push_back(index);
            }
        }

        std::mt19937 random = ModifierRandom(
                settings_.window.seed, request.iterationIndex, request.passIndex);
        std::uint32_t remaining = SegmentSlotCount(request, 0u);
        const std::size_t attemptLimit =
                (static_cast<std::size_t>(remaining) + candidates.size()) *
                kSegmentAttemptsPerItem;
        for (std::size_t attempt = 0u;
             remaining != 0u && !candidates.empty() &&
             attempt < attemptLimit;
             ++attempt) {
            const std::size_t pick = RandomInteger<std::size_t>(
                    random, 0u, candidates.size() - 1u);
            SandboxInputEvent &event = inputs[candidates[pick]];
            const std::int64_t shiftTicks = RandomInteger<std::int64_t>(
                    random, -maximumShiftTicks, maximumShiftTicks);
            const std::int64_t shiftedTimeMs =
                    ShiftedTimeMs(event.timeMs, shiftTicks * tick);
            if (!InAnchorRange(segment,
                               std::min<std::int64_t>(
                                       event.timeMs, shiftedTimeMs))) {
                continue;
            }
            event.timeMs = static_cast<std::int32_t>(shiftedTimeMs);
            PerturbValue(event, random);
            candidates.erase(candidates.begin() +
                             static_cast<std::ptrdiff_t>(pick));
            --remaining;
        }
        NormalizeMutableInputEvents(inputs,
                                    request.baselineInputs,
                                    request.tickDurationMs,
                                    request.mutableFromTimeMs);
        return {inputs,
                EffectiveInputChangeCount(request.baselineInputs, inputs)};
    }

    std::int64_t ShiftedTimeMs(std::int64_t timeMs,
                               std::int64_t shiftMs) const {
        return std::clamp<std::int64_t>(timeMs + shiftMs,
                                        settings_.window.minimumTimeMs,
                                        settings_.window.maximumTimeMs);
    }

    bool IsEligible(const SandboxInputEvent &event) const {
        return (IsSteerAction(event.action) &&
                event.value.kind == forevervalidator::experimental::
                        PhysicsSandboxInputValueKind::Analog) ||
                (settings_.toggleAccelerate &&
                 IsAccelerateAction(event.action)) ||
                (settings_.toggleBrake && IsBrakeAction(event.action));
    }

    void PerturbValue(SandboxInputEvent &event, std::mt19937 &random) const {
        if ((IsSteerAction(event.action) &&
             event.value.kind == forevervalidator::experimental::
                     PhysicsSandboxInputValueKind::Analog)) {
            if (settings_.absoluteSteering) {
                event.value.analog = RandomInteger<AnalogInputState>(
                        random,
                        settings_.steeringAbsoluteMinimum,
                        settings_.steeringAbsoluteMaximum);
            } else {
                const AnalogInputState delta =
                        RandomInteger<AnalogInputState>(
                                random,
                                settings_.steeringDeltaMinimum,
                                settings_.steeringDeltaMaximum);
                event.value.analog = SaturateAnalogInputState(
                        static_cast<std::int64_t>(event.value.analog) +
                        delta);
            }
        } else if (event.value.kind == forevervalidator::experimental::
                           PhysicsSandboxInputValueKind::Switch) {
            event.value.switchState =
                    event.value.switchState !=
                                    forevervalidator::experimental::
                                            PhysicsSandboxSwitchState::
                                                    Released
                    ? forevervalidator::experimental::
                              PhysicsSandboxSwitchState::Released
                    : forevervalidator::experimental::
                              PhysicsSandboxSwitchState::Pressed;
        }
    }

    void PrepareWindow(
            const std::vector<SandboxInputEvent> &baseline) const {
        std::uint64_t fingerprint = baseline.size();
        const auto first = std::lower_bound(
                baseline.begin(), baseline.end(),
                settings_.window.minimumTimeMs,
                [](const SandboxInputEvent &event, std::int64_t timeMs) {
                    return event.timeMs < timeMs;
                });
        const auto last = std::upper_bound(
                first, baseline.end(), settings_.window.maximumTimeMs,
                [](std::int64_t timeMs, const SandboxInputEvent &event) {
                    return timeMs < event.timeMs;
                });
        for (auto it = first; it != last; ++it) {
            fingerprint = fingerprint * 1099511628211ull ^
                    static_cast<std::uint64_t>(it->timeMs);
            fingerprint = fingerprint * 1099511628211ull ^
                    static_cast<std::uint64_t>(it->action);
            fingerprint = fingerprint * 1099511628211ull ^
                    static_cast<std::uint64_t>(it->value.kind);
            std::uint64_t value = 0u;
            if (it->value.kind == forevervalidator::experimental::
                                          PhysicsSandboxInputValueKind::Analog) {
                value = static_cast<std::uint64_t>(
                        static_cast<std::int64_t>(it->value.analog));
            } else if (it->value.kind == forevervalidator::experimental::
                                                 PhysicsSandboxInputValueKind::Switch) {
                value = static_cast<std::uint64_t>(
                        it->value.switchState);
            }
            fingerprint = fingerprint * 1099511628211ull ^ value;
        }
        if (cachedBaseline_ == &baseline &&
            cachedFingerprint_ == fingerprint) {
            return;
        }
        cachedBaseline_ = &baseline;
        cachedFingerprint_ = fingerprint;
        cachedWindowInputs_.assign(first, last);
        cachedEligibleIndices_.clear();
        for (std::size_t index = 0u;
             index < cachedWindowInputs_.size(); ++index) {
            if (IsEligible(cachedWindowInputs_[index])) {
                cachedEligibleIndices_.push_back(index);
            }
        }
    }

    Settings settings_;
    mutable const std::vector<SandboxInputEvent> *cachedBaseline_ = nullptr;
    mutable std::uint64_t cachedFingerprint_ = 0u;
    mutable std::vector<SandboxInputEvent> cachedWindowInputs_;
    mutable std::vector<std::size_t> cachedEligibleIndices_;
};

}  // namespace

OptionSettings DefaultExistingEventPerturbationSettings() {
    return {{"minTimeMs", "1000"},
            {"maxTimeMs", "5990"},
            {"seed", "1179926867"},
            {"minCount", "1"},
            {"maxCount", "3"},
            {"maxTimeShiftMs", "100"},
            {"steerMode", "delta"},
            {"steerDeltaMin", "-0.15"},
            {"steerDeltaMax", "0.15"},
            {"steerAbsoluteMin", "-1"},
            {"steerAbsoluteMax", "1"},
            {"toggleAccelerate", "true"},
            {"toggleBrake", "true"}};
}

std::optional<std::string> ValidateExistingEventPerturbationSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) {
    if (const auto error = ValidateOptionSettingKeys(
                settings, DefaultExistingEventPerturbationSettings())) {
        return error;
    }
    const auto parsed = ParseSettings(settings);
    if (!parsed) return "existing-event perturbation settings are invalid";
    if (const auto error = ValidateModifierWindow(parsed->window,
                                                   tickDurationMs)) {
        return error;
    }
    if (parsed->minimumCount > parsed->maximumCount) {
        return "minimum perturbation count must not exceed maximum";
    }
    if (parsed->maximumTimeShiftMs < 0 ||
        parsed->maximumTimeShiftMs % tickDurationMs != 0) {
        return "maximum timing shift must be a non-negative whole-tick value";
    }
    if (parsed->steeringDeltaMinimum > parsed->steeringDeltaMaximum ||
        parsed->steeringAbsoluteMinimum >
                parsed->steeringAbsoluteMaximum) {
        return "steering ranges are invalid";
    }
    return std::nullopt;
}

std::unique_ptr<InputMutator> CreateExistingEventPerturbationMutator(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) {
    if (const auto error = ValidateExistingEventPerturbationSettings(
                settings, tickDurationMs)) {
        throw std::invalid_argument(*error);
    }
    return std::make_unique<ExistingEventPerturbationMutator>(
            *ParseSettings(settings));
}

}  // namespace forevertas
