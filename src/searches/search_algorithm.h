#ifndef FOREVERTAS_SEARCHES_SEARCH_ALGORITHM_H
#define FOREVERTAS_SEARCHES_SEARCH_ALGORITHM_H

#include "evaluators/iteration_evaluator.h"
#include "conditions/condition_program.h"
#include "mutations/input_mutator.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <forevervalidator/experimental/physics_sandbox.h>

namespace forevertas {

enum class SearchWinnerSource : std::uint8_t {
    Baseline,
    Mutation,
};

enum class SearchIterationPhase : std::uint8_t {
    Pending,
    Started,
    Cancelled,
};

enum class SearchProgressStage : std::uint8_t {
    OpeningPacksDirectory,
    ReadingScenario,
    CreatingSimulation,
    LoadingScenario,
    RestoringSimulation,
    ApplyingBaselineInputs,
    PreparingSearch,
    Baseline,
    Calibration,
    Mutations,
    FinalSamplingSetup,
    FinalSampling,
};

struct SearchProgress {
    SearchProgressStage stage = SearchProgressStage::OpeningPacksDirectory;
    std::uint64_t completedWork = 0u;
    std::uint64_t totalWork = 0u;
};

struct SearchTimelineFrame {
    std::int64_t timeMs = 0;
    float positionX = 0.0f;
    float positionY = 0.0f;
    float positionZ = 0.0f;
    float rotationX = 0.0f;
    float rotationY = 0.0f;
    float rotationZ = 0.0f;
    float rotationW = 1.0f;
    float accelerate = 0.0f;
    float brake = 0.0f;
    float steering = 0.0f;
    std::uint32_t checkpointsCollected = 0u;
    std::uint32_t checkpointsTotal = 0u;
    std::uint32_t completedLaps = 0u;
    std::uint32_t totalLaps = 1u;
    bool raceCompleted = false;
    std::optional<std::uint32_t> finishTimeMs;
    float linearSpeedX = 0.0f;
    float linearSpeedY = 0.0f;
    float linearSpeedZ = 0.0f;
    float signedSpeed = 0.0f;
    float turbo = 0.0f;
    float cameraFlightTransition = 0.0f;
    bool burning = false;
    bool gearChanged = false;
    std::array<bool, 4> wheelContact{{true, true, true, true}};
    std::array<bool, 4> wheelHasSurface{{true, true, true, true}};
    float cameraSupportUpX = 0.0f;
    float cameraSupportUpY = 1.0f;
    float cameraSupportUpZ = 0.0f;
};

struct SearchLiveUpdate {
    SearchWinnerSource winnerSource = SearchWinnerSource::Baseline;
    std::optional<std::uint64_t> winningIterationIndex;
    std::size_t winningMutationCount = 0u;
    double bestScore = 0.0;
    double bestEvaluationTimeMs = 0.0;
    std::string bestEvaluationDescription;
    forevervalidator::experimental::PhysicsSandboxStateView bestState;
    std::vector<SandboxInputEvent> bestInputs;
    std::uint64_t iterations = 0u;
    std::uint64_t evaluatorCalls = 0u;
    std::uint64_t mutationImprovementCount = 0u;
    std::uint64_t totalMutationCount = 0u;
    std::chrono::steady_clock::duration elapsed{};
    std::optional<std::chrono::steady_clock::duration>
            lastImprovementElapsed;
    std::vector<SearchTimelineFrame> bestTimeline;
};

struct SearchRunControl {
    std::function<bool()> stopRequested;
    std::function<bool()> cancellationRequested;
    std::function<void(const SearchProgress &)> progressChanged;
    std::function<bool()> beginIteration;
    std::function<void(const SearchLiveUpdate &)> liveChanged;
    std::function<void(const SearchLiveUpdate &)>
            improvementTimelineSampled;
    std::function<void(std::uint32_t)> cudaBatchSizeChanged;
    std::function<void()> cudaWinnerResolved;
    std::function<std::optional<std::vector<SandboxInputEvent>>()>
            promotedBaselineInputs;
    std::optional<std::uint64_t> iterationLimit;
    std::optional<std::int64_t> evaluationEndTimeLimitMs;
    std::uint64_t iterationIndexOffset = 0u;
    std::uint64_t iterationIndexStride = 1u;
    bool sampleImprovementTimelines = true;
    bool sampleBestTimeline = true;
    bool reuseLoadedSandbox = false;
};

class SearchCancelled final : public std::exception {
public:
    const char *what() const noexcept override {
        return "search cancelled";
    }
};

struct SearchExecutionContext {
    struct ResolvedCudaWinner {
        forevervalidator::experimental::PhysicsSandboxStateView view;
        forevervalidator::experimental::PhysicsSandboxState snapshot;
    };

    forevervalidator::experimental::PhysicsSandbox &sandbox;
    std::uint32_t tickDurationMs;
    const InputMutator &mutator;
    const IterationEvaluator &evaluator;
    const SearchRunControl *control = nullptr;
    std::uint32_t cudaBatchSize = 1u;
    bool calibrateCudaBatchSize = false;
    bool useCudaSessionSpecialization = false;
    const std::vector<forevervalidator::experimental::
                              PhysicsSandboxCudaModifier>
            *cudaModifiers = nullptr;
    const forevervalidator::experimental::PhysicsSandboxCudaEvaluator
            *cudaEvaluator = nullptr;
    std::function<ResolvedCudaWinner(
            const std::vector<forevervalidator::experimental::
                                      PhysicsSandboxInputEvent> &,
            std::uint32_t)>
            resolveCudaWinner = {};
    std::uint32_t simulationHorizonMs = 6000u;
    const ConditionProgram *condition = nullptr;
    double searchStartedTimeSeconds = 0.0;
};

struct SearchResult {
    SearchWinnerSource winnerSource = SearchWinnerSource::Baseline;
    std::optional<std::uint64_t> winningIterationIndex;
    std::size_t winningMutationCount = 0u;
    double bestScore = 0.0;
    double bestEvaluationTimeMs = 0.0;
    std::string bestEvaluationDescription;
    forevervalidator::experimental::PhysicsSandboxStateView bestState;
    std::vector<SandboxInputEvent> bestInputs;
    std::vector<SearchTimelineFrame> bestTimeline;
    std::uint64_t iterations = 0u;
    std::uint64_t evaluatorCalls = 0u;
    std::uint64_t mutationImprovementCount = 0u;
    std::uint64_t totalMutationCount = 0u;
    std::chrono::steady_clock::duration elapsed{};
    std::optional<std::chrono::steady_clock::duration>
            lastImprovementElapsed;
    forevervalidator::experimental::PhysicsSandboxState bestSnapshot;
};

class SearchAlgorithm {
public:
    virtual ~SearchAlgorithm() = default;
    virtual SearchResult Run(const SearchExecutionContext &context) const = 0;
};

}  // namespace forevertas

#endif
