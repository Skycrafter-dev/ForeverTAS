#ifndef FOREVERTAS_SEARCHES_RESULT_INPUT_SCRIPT_H
#define FOREVERTAS_SEARCHES_RESULT_INPUT_SCRIPT_H

#include "mutations/input_event_formatter.h"
#include "searches/algorithm_registry.h"

#include <string_view>

namespace forevertas {

inline std::string FormatResultInputScript(
        const std::vector<SandboxInputEvent> &inputs,
        std::string_view targetId,
        const forevervalidator::experimental::PhysicsSandboxStateView &winningState) {
    std::optional<std::uint64_t> cutoff;
    if (targetId == kPreciseFinishTimeEvaluationId && winningState.raceCompleted &&
        winningState.finishTime && winningState.finishTime->IsValid()) {
        // Keep the controls consumed by the finish-triggering physics tick.
        // State time is absolute; the formatter applies the input-time origin.
        cutoff = winningState.timeMs;
    }
    return FormatInputScript(inputs, cutoff);
}

}  // namespace forevertas

#endif
