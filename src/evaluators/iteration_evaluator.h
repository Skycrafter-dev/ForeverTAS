#ifndef FOREVERTAS_EVALUATORS_ITERATION_EVALUATOR_H
#define FOREVERTAS_EVALUATORS_ITERATION_EVALUATOR_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <forevervalidator/experimental/physics_sandbox.h>

namespace forevertas {

struct EvaluationPlan {
    std::int64_t startTimeMs = 0;
    std::int64_t endTimeMs = 0;
};

struct EvaluationSample {
    double score = 0.0;
    double timeMs = 0.0;
    std::string description;
};

class IterationEvaluationSession {
public:
    virtual ~IterationEvaluationSession() = default;
    virtual std::optional<EvaluationSample> Observe(
            const std::optional<
                    forevervalidator::experimental::PhysicsSandboxStateView>
                    &previous,
            const forevervalidator::experimental::PhysicsSandboxStateView
                    &current) = 0;
};

class IterationEvaluator {
public:
    virtual ~IterationEvaluator() = default;
    virtual EvaluationPlan Plan(std::int64_t simulationHorizonMs,
                                std::int64_t earliestMutationTimeMs,
                                std::uint32_t tickDurationMs) const = 0;
    virtual std::unique_ptr<IterationEvaluationSession> CreateSession()
            const = 0;
    virtual bool IsBetter(const EvaluationSample &iteration,
                          const EvaluationSample &incumbent) const = 0;
};

inline bool ImprovesSearchResult(
        const IterationEvaluator &evaluator,
        const EvaluationSample &candidate,
        std::size_t candidateInputCount,
        const EvaluationSample &incumbent,
        std::size_t incumbentInputCount) {
    if (evaluator.IsBetter(candidate, incumbent)) {
        return true;
    }
    // EvaluationSample::score is the evaluator ordering key. Once the
    // candidate is not strictly better, a different score is strictly worse;
    // only an equal score is eligible for the input-count tie break.
    if (candidate.score != incumbent.score) {
        return false;
    }
    return candidateInputCount < incumbentInputCount;
}

}  // namespace forevertas

#endif
