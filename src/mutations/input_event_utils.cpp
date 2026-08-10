#include "mutations/input_event_utils.h"

#include "mutations/input_mutator.h"

#include <algorithm>
#include <limits>

namespace forevertas {
namespace {

using forevervalidator::experimental::PhysicsSandboxInputValueKind;
using forevervalidator::experimental::PhysicsSandboxSwitchState;

constexpr AnalogInputState kEngineAnalogSteeringDeadZone = 655;

bool SameValue(const SandboxInputEvent &left,
               const SandboxInputEvent &right) {
    if (left.value.kind != right.value.kind) return false;
    switch (left.value.kind) {
    case PhysicsSandboxInputValueKind::None:
        return true;
    case PhysicsSandboxInputValueKind::Switch:
        return left.value.switchState == right.value.switchState;
    case PhysicsSandboxInputValueKind::Analog:
        return left.value.analog == right.value.analog;
    }
    return false;
}

}  // namespace

std::int64_t AlignInputTime(std::int64_t timeMs,
                            std::uint32_t tickDurationMs) {
    if (tickDurationMs == 0u) return timeMs;
    const std::int64_t tick = static_cast<std::int64_t>(tickDurationMs);
    if (timeMs <= 0) return 0;
    return (timeMs / tick) * tick;
}

AnalogInputState SaturateAnalogInputState(std::int64_t value) {
    return static_cast<AnalogInputState>(std::clamp<std::int64_t>(
            value, kAnalogInputMinimum, kAnalogInputMaximum));
}

bool SameInputEvent(const SandboxInputEvent &left,
                    const SandboxInputEvent &right) {
    return left.timeMs == right.timeMs && left.action == right.action &&
           SameValue(left, right);
}

void NormalizeInputEvents(std::vector<SandboxInputEvent> &events,
                          std::uint32_t tickDurationMs) {
    for (SandboxInputEvent &event : events) {
        event.timeMs = AlignInputTime(event.timeMs, tickDurationMs);
        if (event.value.kind == PhysicsSandboxInputValueKind::Analog) {
            event.value.analog =
                    SaturateAnalogInputState(event.value.analog);
        } else if (event.value.kind == PhysicsSandboxInputValueKind::Switch) {
            event.value.switchState =
                    event.value.switchState !=
                                    PhysicsSandboxSwitchState::Released
                    ? PhysicsSandboxSwitchState::Pressed
                    : PhysicsSandboxSwitchState::Released;
        }
    }

    std::stable_sort(events.begin(), events.end(),
                     [](const SandboxInputEvent &left,
                        const SandboxInputEvent &right) {
                         return left.timeMs < right.timeMs;
                     });

    std::vector<SandboxInputEvent> normalized;
    normalized.reserve(events.size());
    for (const SandboxInputEvent &event : events) {
        auto duplicate = std::find_if(
                normalized.rbegin(), normalized.rend(),
                [&event](const SandboxInputEvent &existing) {
                    return existing.timeMs == event.timeMs &&
                           existing.action == event.action;
                });
        if (duplicate != normalized.rend()) {
            *duplicate = event;
        } else {
            normalized.push_back(event);
        }
    }
    events.swap(normalized);
}

void ConvertKeyboardSteeringToAnalog(
        std::vector<SandboxInputEvent> &events) {
    struct SteeringState {
        bool left = false;
        bool right = false;
        std::int32_t leftTimeMs = 0;
        std::int32_t rightTimeMs = 0;
        std::int32_t analogTimeMs = 0;
        AnalogInputState analog = 0;
    } state;

    std::stable_sort(events.begin(), events.end(),
                     [](const SandboxInputEvent &left,
                        const SandboxInputEvent &right) {
                         return left.timeMs < right.timeMs;
                     });

    std::vector<SandboxInputEvent> converted;
    converted.reserve(events.size());
    std::size_t first = 0u;
    while (first < events.size()) {
        std::size_t last = first + 1u;
        while (last < events.size() &&
               events[last].timeMs == events[first].timeMs) {
            ++last;
        }

        bool steeringChanged = false;
        for (std::size_t index = first; index < last; ++index) {
            const SandboxInputEvent &event = events[index];
            if (event.action == SandboxInputAction::Steer &&
                event.value.kind ==
                        PhysicsSandboxInputValueKind::Analog &&
                forevervalidator::IsAnalogInputStateValid(
                        event.value.analog)) {
                state.analog = event.value.analog;
                state.analogTimeMs = event.timeMs;
                steeringChanged = true;
            } else if (
                    event.action == SandboxInputAction::SteerLeft &&
                    event.value.kind ==
                            PhysicsSandboxInputValueKind::Switch) {
                state.left = event.value.switchState !=
                        PhysicsSandboxSwitchState::Released;
                state.leftTimeMs = event.timeMs;
                steeringChanged = true;
            } else if (
                    event.action == SandboxInputAction::SteerRight &&
                    event.value.kind ==
                            PhysicsSandboxInputValueKind::Switch) {
                state.right = event.value.switchState !=
                        PhysicsSandboxSwitchState::Released;
                state.rightTimeMs = event.timeMs;
                steeringChanged = true;
            } else {
                converted.push_back(event);
            }
        }

        if (steeringChanged) {
            const std::int32_t digitalTimeMs =
                    std::max(state.leftTimeMs, state.rightTimeMs);
            const std::int64_t analogMagnitude =
                    state.analog < 0
                    ? -static_cast<std::int64_t>(state.analog)
                    : static_cast<std::int64_t>(state.analog);
            const bool analogWins =
                    state.analogTimeMs > digitalTimeMs ||
                    (state.analogTimeMs == digitalTimeMs &&
                     !state.left && !state.right &&
                     analogMagnitude >
                             kEngineAnalogSteeringDeadZone);
            const AnalogInputState effective = analogWins
                    ? state.analog
                    : state.left
                    ? kAnalogInputMinimum
                    : state.right
                    ? kAnalogInputMaximum
                    : 0;
            SandboxInputEvent analog;
            analog.timeMs = events[first].timeMs;
            analog.action = SandboxInputAction::Steer;
            analog.value.kind = PhysicsSandboxInputValueKind::Analog;
            analog.value.analog = effective;
            converted.push_back(analog);
        }
        first = last;
    }
    events.swap(converted);
}

void NormalizeMutableInputEvents(
        std::vector<SandboxInputEvent> &events,
        const std::vector<SandboxInputEvent> &baseline,
        std::uint32_t tickDurationMs,
        std::int64_t mutableFromTimeMs) {
    std::vector<SandboxInputEvent> mutableEvents;
    mutableEvents.reserve(events.size());
    for (const SandboxInputEvent &event : events) {
        if (event.timeMs >= mutableFromTimeMs) {
            mutableEvents.push_back(event);
        }
    }
    NormalizeInputEvents(mutableEvents, tickDurationMs);

    std::vector<SandboxInputEvent> combined;
    combined.reserve(baseline.size() + mutableEvents.size());
    for (const SandboxInputEvent &event : baseline) {
        if (event.timeMs < mutableFromTimeMs) {
            combined.push_back(event);
        }
    }
    combined.insert(combined.end(),
                    mutableEvents.begin(), mutableEvents.end());
    events.swap(combined);
}

std::size_t EffectiveInputChangeCount(
        const std::vector<SandboxInputEvent> &baseline,
        const std::vector<SandboxInputEvent> &iterationInputs) {
    const std::size_t common = std::min(baseline.size(), iterationInputs.size());
    std::size_t count = baseline.size() > iterationInputs.size()
            ? baseline.size() - iterationInputs.size()
            : iterationInputs.size() - baseline.size();
    for (std::size_t index = 0u; index < common; ++index) {
        if (!SameInputEvent(baseline[index], iterationInputs[index])) ++count;
    }
    return count;
}

std::size_t EffectiveInputChangeCount(
        const std::vector<SandboxInputEvent> &baseline,
        const MutationWindowPatch &patch) {
    const auto first = std::lower_bound(
            baseline.begin(), baseline.end(), patch.minimumTimeMs,
            [](const SandboxInputEvent &event, std::int64_t timeMs) {
                return event.timeMs < timeMs;
            });
    const auto last = std::upper_bound(
            first, baseline.end(), patch.maximumTimeMs,
            [](std::int64_t timeMs, const SandboxInputEvent &event) {
                return timeMs < event.timeMs;
            });
    const std::size_t prefixCount = static_cast<std::size_t>(
            first - baseline.begin());
    const std::size_t replacedCount = static_cast<std::size_t>(last - first);
    const std::size_t resultSize =
            baseline.size() - replacedCount + patch.events.size();
    const std::size_t common = std::min(baseline.size(), resultSize);
    std::size_t count = baseline.size() > resultSize
            ? baseline.size() - resultSize
            : resultSize - baseline.size();
    for (std::size_t index = prefixCount; index < common; ++index) {
        const SandboxInputEvent *iterationEvent = nullptr;
        const std::size_t patchOffset = index - prefixCount;
        if (patchOffset < patch.events.size()) {
            iterationEvent = &patch.events[patchOffset];
        } else {
            const std::size_t baselineIndex =
                    static_cast<std::size_t>(last - baseline.begin()) +
                    patchOffset - patch.events.size();
            iterationEvent = &baseline[baselineIndex];
        }
        if (!SameInputEvent(baseline[index], *iterationEvent)) ++count;
    }
    return count;
}

std::size_t InputCountAfterWindowPatch(
        const std::vector<SandboxInputEvent> &baseline,
        const MutationWindowPatch &patch) {
    const auto first = std::lower_bound(
            baseline.begin(), baseline.end(), patch.minimumTimeMs,
            [](const SandboxInputEvent &event, std::int64_t timeMs) {
                return event.timeMs < timeMs;
            });
    const auto last = std::upper_bound(
            first, baseline.end(), patch.maximumTimeMs,
            [](std::int64_t timeMs, const SandboxInputEvent &event) {
                return timeMs < event.timeMs;
            });
    return baseline.size() -
            static_cast<std::size_t>(last - first) +
            patch.events.size();
}

std::vector<SandboxInputEvent> ApplyInputWindowPatch(
        const std::vector<SandboxInputEvent> &baseline,
        const MutationWindowPatch &patch) {
    std::vector<SandboxInputEvent> result;
    const auto first = std::lower_bound(
            baseline.begin(), baseline.end(), patch.minimumTimeMs,
            [](const SandboxInputEvent &event, std::int64_t timeMs) {
                return event.timeMs < timeMs;
            });
    const auto last = std::upper_bound(
            first, baseline.end(), patch.maximumTimeMs,
            [](std::int64_t timeMs, const SandboxInputEvent &event) {
                return timeMs < event.timeMs;
            });
    result.reserve(
            baseline.size() - static_cast<std::size_t>(last - first) +
            patch.events.size());
    result.insert(result.end(), baseline.begin(), first);
    result.insert(result.end(), patch.events.begin(), patch.events.end());
    result.insert(result.end(), last, baseline.end());
    return result;
}

bool InputEventsAreCanonical(
        const std::vector<SandboxInputEvent> &events,
        std::uint32_t tickDurationMs) {
    using forevervalidator::experimental::PhysicsSandboxInputValueKind;
    using forevervalidator::experimental::PhysicsSandboxSwitchState;
    std::int64_t previousTimeMs = std::numeric_limits<std::int64_t>::min();
    std::size_t groupBegin = 0u;
    for (std::size_t index = 0u; index < events.size(); ++index) {
        const SandboxInputEvent &event = events[index];
        if (event.timeMs < previousTimeMs ||
            AlignInputTime(event.timeMs, tickDurationMs) != event.timeMs) {
            return false;
        }
        if (event.value.kind == PhysicsSandboxInputValueKind::Analog &&
            !forevervalidator::IsAnalogInputStateValid(event.value.analog)) {
            return false;
        }
        if (event.value.kind == PhysicsSandboxInputValueKind::Switch &&
            event.value.switchState != PhysicsSandboxSwitchState::Released &&
            event.value.switchState != PhysicsSandboxSwitchState::Pressed) {
            return false;
        }
        if (index == 0u || event.timeMs != previousTimeMs) {
            groupBegin = index;
        } else {
            for (std::size_t previous = groupBegin;
                 previous < index; ++previous) {
                if (events[previous].action == event.action) return false;
            }
        }
        previousTimeMs = event.timeMs;
    }
    return true;
}

AnalogInputState SteeringStateAt(
        const std::vector<SandboxInputEvent> &events,
        std::int64_t timeMs) {
    AnalogInputState state = 0;
    std::int64_t bestTime = std::numeric_limits<std::int64_t>::min();
    for (const SandboxInputEvent &event : events) {
        if (event.action != SandboxInputAction::Steer ||
            event.value.kind != PhysicsSandboxInputValueKind::Analog ||
            event.timeMs > timeMs || event.timeMs < bestTime) {
            continue;
        }
        state = event.value.analog;
        bestTime = event.timeMs;
    }
    return state;
}

bool SwitchStateAt(const std::vector<SandboxInputEvent> &events,
                   SandboxInputAction action,
                   std::int64_t timeMs) {
    bool state = false;
    std::int64_t bestTime = std::numeric_limits<std::int64_t>::min();
    for (const SandboxInputEvent &event : events) {
        if (event.action != action ||
            event.value.kind != PhysicsSandboxInputValueKind::Switch ||
            event.timeMs > timeMs || event.timeMs < bestTime) {
            continue;
        }
        state = event.value.switchState !=
                PhysicsSandboxSwitchState::Released;
        bestTime = event.timeMs;
    }
    return state;
}

}  // namespace forevertas
