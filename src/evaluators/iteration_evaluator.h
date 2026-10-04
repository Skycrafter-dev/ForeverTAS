#ifndef FOREVERTAS_EVALUATORS_ITERATION_EVALUATOR_H
#define FOREVERTAS_EVALUATORS_ITERATION_EVALUATOR_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <forevervalidator/experimental/physics_sandbox.h>

namespace forevertas {

struct ConditionExecutionContext;

struct EvaluationPlan {
    std::int64_t startTimeMs = 0;
    std::int64_t endTimeMs = 0;
};

struct EvaluationSample {
    EvaluationSample() = default;
    EvaluationSample(double scoreValue, double timeValue,
                     std::string descriptionValue,
                     std::vector<double> objectiveScoreValues = {},
                     std::vector<double> metricValueValues = {})
        : score(scoreValue), timeMs(timeValue),
          description(std::move(descriptionValue)),
          objectiveScores(std::move(objectiveScoreValues)),
          metricValues(std::move(metricValueValues)) {}

    double score = 0.0;
    double timeMs = 0.0;
    std::string description;
    std::vector<double> objectiveScores;
    std::vector<double> metricValues;
};

class IterationEvaluationSession {
public:
    virtual ~IterationEvaluationSession() = default;
    virtual void SetExecutionContext(const ConditionExecutionContext &) {}
    // True only after all samples relevant to this candidate have been consumed.
    virtual bool IsComplete() const { return false; }
    virtual std::optional<EvaluationSample> Observe(
            const std::optional<
                    forevervalidator::experimental::PhysicsSandboxStateView>
                    &previous,
            const forevervalidator::experimental::PhysicsSandboxStateView
                    &current) = 0;
    // Tree search continues several branches from one observed prefix, so
    // every session must copy its complete timeline state.
    virtual std::unique_ptr<IterationEvaluationSession> Clone() const = 0;
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
    virtual bool IsEquivalent(const EvaluationSample &iteration,
                              const EvaluationSample &incumbent) const {
        return iteration.score == incumbent.score;
    }
    virtual bool CompareAtEndOnly() const { return false; }
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
    if (!evaluator.IsEquivalent(candidate, incumbent)) {
        return false;
    }
    return candidateInputCount < incumbentInputCount;
}

}  // namespace forevertas

#endif
