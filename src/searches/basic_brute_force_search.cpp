#include "searches/basic_brute_force_search.h"

#include "evaluators/evaluator_utils.h"
#include "searches/cuda_batch_calibrator.h"
#include "searches/cuda_calibration_safety.h"
#include "searches/option_settings_utils.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#if FOREVERVALIDATOR_HAS_CUDA
#include <cuda_runtime_api.h>
#endif

namespace forevertas {
namespace {

using forevervalidator::DiscriminatedResult;
using forevervalidator::Vector3;
using forevervalidator::experimental::PhysicsSandbox;
using forevervalidator::experimental::PhysicsSandboxCarState;
using forevervalidator::experimental::PhysicsSandboxInputEvent;
using forevervalidator::experimental::PhysicsSandboxState;
using forevervalidator::experimental::PhysicsSandboxStateView;

template<typename T, typename Error>
T Require(DiscriminatedResult<T, Error> result, const char *operation) {
    if (!result) {
        std::string message = std::string(operation) + " failed";
        if (!result.Error().diagnostic.empty()) {
            message += ": " + result.Error().diagnostic;
        }
        throw std::runtime_error(std::move(message));
    }
    return std::move(result).Value();
}

bool SameVector(const Vector3 &left, const Vector3 &right) {
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

bool SameCar(const PhysicsSandboxCarState &left,
             const PhysicsSandboxCarState &right) {
    return left.rotationX == right.rotationX &&
           left.rotationY == right.rotationY &&
           left.rotationZ == right.rotationZ &&
           left.rotationW == right.rotationW &&
           SameVector(left.position, right.position) &&
           SameVector(left.linearSpeed, right.linearSpeed) &&
           SameVector(left.angularSpeed, right.angularSpeed) &&
           SameVector(left.force, right.force) &&
           SameVector(left.torque, right.torque);
}

bool SameState(const PhysicsSandboxStateView &left,
               const PhysicsSandboxStateView &right) {
    return left.tick == right.tick && left.timeMs == right.timeMs &&
           left.mapEnvironment == right.mapEnvironment &&
           left.vehicleModel == right.vehicleModel &&
           left.playMode == right.playMode && SameCar(left.car, right.car) &&
           left.accelerate == right.accelerate &&
           left.brake == right.brake && left.steering == right.steering &&
           left.checkpointsCollected == right.checkpointsCollected &&
           left.checkpointsTotal == right.checkpointsTotal &&
           left.completedLaps == right.completedLaps &&
           left.totalLaps == right.totalLaps &&
           left.raceCompleted == right.raceCompleted &&
           left.finishTimeMs == right.finishTimeMs &&
           left.finishTime == right.finishTime &&
           left.respawnCount == right.respawnCount &&
           left.stuntsScore == right.stuntsScore;
}

void CheckCancellation(const SearchRunControl *control) {
    if (control != nullptr && control->cancellationRequested &&
        control->cancellationRequested()) {
        throw SearchCancelled();
    }
}

void BeginIteration(const SearchRunControl *control) {
    CheckCancellation(control);
    if (control != nullptr && control->beginIteration &&
        !control->beginIteration()) {
        throw SearchCancelled();
    }
}

bool StopRequested(const SearchRunControl *control) {
    return control != nullptr && control->stopRequested &&
            control->stopRequested();
}

bool IterationLimitReached(const SearchRunControl *control,
                           std::uint64_t iterations) {
    return control != nullptr && control->iterationLimit &&
            iterations >= *control->iterationLimit;
}

void ReportProgress(const SearchRunControl *control,
                    SearchProgressStage stage,
                    std::uint64_t completedWork,
                    std::uint64_t totalWork = 0u) {
    if (control != nullptr && control->progressChanged) {
        control->progressChanged({stage, completedWork, totalWork});
    }
}

#if FOREVERVALIDATOR_HAS_CUDA
void ReportCudaBatchSize(const SearchRunControl *control,
                         std::uint32_t batchSize) {
    if (control != nullptr && control->cudaBatchSizeChanged) {
        control->cudaBatchSizeChanged(batchSize);
    }
}

void ReportCudaBatchCapacity(const SearchRunControl *control,
                             std::uint32_t batchCapacity) {
    if (control != nullptr && control->cudaBatchCapacityChanged) {
        control->cudaBatchCapacityChanged(batchCapacity);
    }
}

void ReportCudaBatchExecution(const SearchRunControl *control,
                              std::uint64_t firstCandidateId,
                              std::uint32_t candidateCount) {
    if (control != nullptr && control->cudaBatchExecuted) {
        control->cudaBatchExecuted(firstCandidateId, candidateCount);
    }
}

bool CudaBatchProfilingEnabled() {
    static const bool enabled = []() {
#if defined(_WIN32)
        char *value = nullptr;
        std::size_t valueSize = 0u;
        if (_dupenv_s(&value, &valueSize,
                      "FOREVERTAS_CUDA_PROFILE") != 0) {
            return false;
        }
        const bool result = value != nullptr && value[0] != '\0' &&
                !(value[0] == '0' && value[1] == '\0');
        std::free(value);
        return result;
#else
        const char *value = std::getenv("FOREVERTAS_CUDA_PROFILE");
        return value != nullptr && value[0] != '\0' &&
                !(value[0] == '0' && value[1] == '\0');
#endif
    }();
    return enabled;
}

void ReportCudaBatchProfile(
        const char *phase,
        const forevervalidator::experimental::
                PhysicsSandboxCudaSearchBatch &batch,
        std::uint64_t timelineTickCount,
        std::chrono::steady_clock::duration wallElapsed) {
    if (!CudaBatchProfilingEnabled()) {
        return;
    }
    const double wallMilliseconds =
            std::chrono::duration<double, std::milli>(
                    wallElapsed)
                    .count();
    const double attemptsPerSecond =
            wallMilliseconds > 0.0
            ? static_cast<double>(batch.candidateCount) *
                      1000.0 / wallMilliseconds
            : 0.0;
    const double simulatedTicks =
            static_cast<double>(batch.evaluatedCandidateCount) *
            static_cast<double>(timelineTickCount);
    const double physicsTicksPerSecond =
            batch.metrics.simulationKernelMilliseconds > 0.0
            ? simulatedTicks * 1000.0 /
                      batch.metrics.simulationKernelMilliseconds
            : 0.0;
    std::clog << "forevertas_cuda_batch"
              << " phase=" << phase
              << " first_candidate=" << batch.firstCandidateId
              << " candidates=" << batch.candidateCount
              << " active=" << batch.evaluatedCandidateCount
              << " timeline_ticks=" << timelineTickCount
              << " attempts_per_second=" << attemptsPerSecond
              << " physics_ticks_per_second="
              << physicsTicksPerSecond
              << " wall_ms=" << wallMilliseconds
              << " kernel_ms=" << batch.metrics.kernelMilliseconds
              << " mutation_ms="
              << batch.metrics.mutationKernelMilliseconds
              << " simulation_ms="
              << batch.metrics.simulationKernelMilliseconds
              << " finish_refinement_ms="
              << batch.metrics.finishRefinementKernelMilliseconds
              << " winner_capture_ms="
              << batch.metrics.winnerStateCaptureKernelMilliseconds
              << " finalization_ms="
              << batch.metrics.finalizationKernelMilliseconds
              << " best_changed=" << batch.bestChanged
              << " resident_mib="
              << static_cast<double>(
                         batch.metrics.residentDeviceBytes) /
                         (1024.0 * 1024.0)
              << '\n'
              << std::flush;
}

CudaCalibrationDeviceLimits QueryCudaCalibrationDeviceLimits() {
    int device = 0;
    cudaDeviceProp properties{};
    std::size_t freeMemory = 0u;
    std::size_t totalMemory = 0u;
    int kernelExecutionTimeoutEnabled = 0;
    cudaError_t error = cudaGetDevice(&device);
    if (error == cudaSuccess) {
        error = cudaGetDeviceProperties(&properties, device);
    }
    if (error == cudaSuccess) {
        error = cudaMemGetInfo(&freeMemory, &totalMemory);
    }
    if (error == cudaSuccess) {
        error = cudaDeviceGetAttribute(
                &kernelExecutionTimeoutEnabled,
                cudaDevAttrKernelExecTimeout,
                device);
    }
    if (error != cudaSuccess) {
        throw std::runtime_error(
                std::string("querying CUDA calibration safety limits "
                            "failed: ") +
                cudaGetErrorString(error));
    }

    CudaCalibrationDeviceLimits limits;
    limits.totalMemoryBytes = totalMemory;
    limits.freeMemoryBytes = freeMemory;
    limits.maximumThreadsPerBlock =
            static_cast<std::uint32_t>(
                    properties.maxThreadsPerBlock);
    limits.maximumGridDimensionX =
            static_cast<std::uint32_t>(
                    properties.maxGridSize[0]);
    limits.registersPerBlock =
            static_cast<std::uint32_t>(
                    properties.regsPerBlock);
    limits.registersPerMultiprocessor =
            static_cast<std::uint32_t>(
                    properties.regsPerMultiprocessor);
    limits.maximumThreadsPerMultiprocessor =
            static_cast<std::uint32_t>(
                    properties.maxThreadsPerMultiProcessor);
    limits.maximumBlocksPerMultiprocessor =
            static_cast<std::uint32_t>(
                    properties.maxBlocksPerMultiProcessor);
    limits.multiprocessorCount =
            static_cast<std::uint32_t>(
                    properties.multiProcessorCount);
    limits.kernelExecutionTimeoutEnabled =
            kernelExecutionTimeoutEnabled != 0;
    return limits;
}

CudaCalibrationBatchProfile CudaCalibrationProfile(
        const forevervalidator::experimental::
                PhysicsSandboxCudaSearchBatch &batch,
        std::uint32_t batchCapacity) {
    CudaCalibrationBatchProfile profile;
    profile.batchSize = std::max(batch.candidateCount, 1u);
    profile.batchCapacity = batchCapacity;
    profile.residentDeviceBytes =
            batch.metrics.residentDeviceBytes;
    profile.kernelMilliseconds = std::max(
            {batch.metrics.scoreInitializationKernelMilliseconds,
             batch.metrics.mutationKernelMilliseconds,
             batch.metrics.simulationKernelMilliseconds,
             batch.metrics.finishRefinementKernelMilliseconds,
             batch.metrics.winnerKernelMilliseconds,
             batch.metrics.winnerReductionKernelMilliseconds,
             batch.metrics.winnerStateCaptureKernelMilliseconds,
             batch.metrics.finalizationKernelMilliseconds});
    profile.simulationThreadsPerBlock =
            batch.metrics.simulationThreadsPerBlock;
    profile.simulationRegistersPerThread =
            batch.metrics.simulationRegistersPerThread;
    profile.simulationLocalBytesPerThread =
            batch.metrics.simulationLocalBytesPerThread;
    profile.simulationActiveBlocksPerMultiprocessor =
            batch.metrics
                    .simulationActiveBlocksPerMultiprocessor;
    profile.simulationTheoreticalOccupancy =
            batch.metrics.simulationTheoreticalOccupancy;
    return profile;
}

void ReportRejectedCudaCalibrationBatch(
        std::uint32_t batchSize,
        const CudaCalibrationSafetyDecision &decision) {
    std::clog << "forevertas_cuda_calibration_rejected"
              << " candidates=" << batchSize
              << " reason=\"" << decision.reason << '"'
              << " transient_mib="
              << static_cast<double>(
                         decision.requiredTransientBytes) /
                         (1024.0 * 1024.0)
              << " reserved_headroom_mib="
              << static_cast<double>(
                         decision.reservedMemoryHeadroomBytes) /
                         (1024.0 * 1024.0)
              << " predicted_kernel_ms="
              << decision.predictedKernelMilliseconds
              << '\n'
              << std::flush;
}

void ReportCudaCalibrationSelection(
        const CudaBatchCalibrator &calibrator) {
    std::clog << "forevertas_cuda_calibration_selected"
              << " candidates="
              << calibrator.BestBatchSize()
              << " sustained_iterations_per_second="
              << calibrator.BestThroughput()
              << " reliable_candidates="
              << calibrator.ReliableMeasurementCount()
              << '\n'
              << std::flush;
}
#endif

std::uint64_t TickCount(std::uint64_t durationMs,
                        std::uint32_t tickDurationMs) {
    if (durationMs % tickDurationMs != 0u) {
        throw std::runtime_error(
                "sandbox state time is not aligned to the tick duration");
    }
    return durationMs / tickDurationMs;
}

PhysicsSandboxStateView AdvanceTo(PhysicsSandbox &sandbox,
                                  std::uint64_t currentTimeMs,
                                  std::uint64_t targetTimeMs,
                                  std::uint32_t tickDurationMs,
                                  const SearchRunControl *control) {
    if (currentTimeMs > targetTimeMs) {
        throw std::runtime_error("sandbox is already past the requested time");
    }
    if (currentTimeMs == targetTimeMs) {
        return Require(sandbox.ReadState(), "reading sandbox state");
    }

    constexpr std::uint32_t maxTicksPerAdvance = 128u;
    std::uint64_t ticksRemaining =
            TickCount(targetTimeMs - currentTimeMs, tickDurationMs);
    PhysicsSandboxStateView state;
    while (ticksRemaining != 0u) {
        CheckCancellation(control);
        const std::uint32_t ticks =
                static_cast<std::uint32_t>(
                        std::min<std::uint64_t>(ticksRemaining,
                                                maxTicksPerAdvance));
        state = Require(sandbox.AdvanceTicks(ticks), "advancing sandbox");
        ticksRemaining -= ticks;
    }
    CheckCancellation(control);
    return state;
}

struct BestIteration {
    std::optional<EvaluationSample> evaluation;
    SearchWinnerSource source = SearchWinnerSource::Baseline;
    std::optional<std::uint64_t> iterationIndex;
    std::size_t mutationCount = 0u;
    std::uint32_t evaluationTick = 0u;
    double detail0 = 0.0;
    double detail1 = 0.0;
    PhysicsSandboxStateView view;
    std::optional<PhysicsSandboxState> snapshot;
    std::vector<PhysicsSandboxInputEvent> inputs;
};

void ReportLive(
        const SearchRunControl *control,
        const BestIteration &best,
        std::uint64_t iterations,
        std::uint64_t evaluatorCalls,
        std::uint64_t mutationImprovementCount,
        std::uint64_t totalMutationCount,
        std::uint64_t qualifyingCandidateCount,
        const std::optional<double> &closestTargetDistance,
        std::chrono::steady_clock::duration elapsed,
        const std::optional<std::chrono::steady_clock::duration>
                &lastImprovementElapsed) {
    if (control == nullptr || !control->liveChanged) {
        return;
    }
    SearchLiveUpdate live;
    live.bestAvailable = best.evaluation.has_value();
    if (best.evaluation) {
        live.winnerSource = best.source;
        live.winningIterationIndex = best.iterationIndex;
        live.winningMutationCount = best.mutationCount;
        live.bestScore = best.evaluation->score;
        live.bestEvaluationTimeMs = best.evaluation->timeMs;
        live.bestEvaluationDescription = best.evaluation->description;
        live.bestState = best.view;
        live.bestInputs = best.inputs;
    }
    live.iterations = iterations;
    live.evaluatorCalls = evaluatorCalls;
    live.mutationImprovementCount = mutationImprovementCount;
    live.totalMutationCount = totalMutationCount;
    live.elapsed = elapsed;
    live.lastImprovementElapsed = lastImprovementElapsed;
    live.qualifyingCandidateCount = qualifyingCandidateCount;
    live.closestTargetDistance = closestTargetDistance;
    control->liveChanged(live);
}

#if FOREVERVALIDATOR_HAS_CUDA
std::string CudaEvaluationDescription(
        const forevervalidator::experimental::
                PhysicsSandboxCudaEvaluator &evaluator,
        const forevervalidator::experimental::
                PhysicsSandboxCudaSearchBatch &batch) {
    using namespace forevervalidator::experimental;
    return std::visit(
            [&](const auto &configured) -> std::string {
                using T = std::decay_t<decltype(configured)>;
                if constexpr (std::is_same_v<
                                      T,
                                      PhysicsSandboxCudaVelocityEvaluator>) {
                    return MetricDescription(
                            configured.projected
                                    ? "Projected velocity"
                                    : "Velocity",
                            batch.bestScore,
                            "m/s",
                            batch.bestTimeMs);
                } else if constexpr (std::is_same_v<
                                             T,
                                             PhysicsSandboxCudaPointEvaluator>) {
                    return MetricDescription(
                            "Point distance",
                            batch.bestScore,
                            "m",
                            batch.bestTimeMs);
                } else if constexpr (std::is_same_v<
                                             T,
                                             PhysicsSandboxCudaPoseEvaluator>) {
                    constexpr double radiansToDegrees =
                            180.0 / 3.14159265358979323846;
                    std::ostringstream description;
                    description.precision(8);
                    description
                            << "Pose error: " << batch.bestScore
                            << " (position " << batch.bestDetail0
                            << " m, rotation "
                            << batch.bestDetail1 * radiansToDegrees
                            << " deg) at "
                            << FormatHumanDurationMilliseconds(
                                       batch.bestTimeMs);
                    return description.str();
                } else if constexpr (std::is_same_v<
                                             T,
                                             PhysicsSandboxCudaVolumeEntryEvaluator>) {
                    return TimeMetricDescription(
                            "Volume entry time", batch.bestTimeMs);
                } else if constexpr (std::is_same_v<
                                             T,
                                             PhysicsSandboxCudaStuntPointsEvaluator>) {
                    return "Stunt points: " +
                            std::to_string(
                                    static_cast<std::uint32_t>(
                                            batch.bestScore)) +
                            " at " +
                            FormatHumanDurationMilliseconds(
                                    batch.bestTimeMs);
                } else {
                    return "Precise finish time: " +
                            FormatHumanDurationNanoseconds(
                                    static_cast<std::uint64_t>(
                                            batch.bestScore));
                }
            },
            evaluator);
}

SearchResult RunCudaBasicBruteForce(
        const SearchExecutionContext &context,
        const EvaluationPlan &evaluationPlan,
        std::int64_t earliestMutationTimeMs,
        const PhysicsSandboxState &branch,
        const std::vector<PhysicsSandboxInputEvent>
                &originalBaselineInputs,
        bool autoPromoteBest,
        std::chrono::steady_clock::time_point started) {
    using namespace forevervalidator::experimental;
    if (context.cudaModifiers == nullptr ||
        context.cudaEvaluator == nullptr ||
        (!context.calibrateCudaBatchSize &&
         context.cudaBatchSize == 0u) ||
        (context.calibrateCudaBatchSize &&
         context.cudaCalibrationStartBatchSize == 0u)) {
        throw std::invalid_argument(
                "CUDA search configuration is unavailable");
    }

    constexpr std::uint32_t calibrationBootstrapBatchSize = 1u;
    const std::uint32_t requestedBatchSize =
            context.calibrateCudaBatchSize
            ? context.cudaCalibrationStartBatchSize
            : context.cudaBatchSize;
    const std::uint32_t initialSessionCapacity =
            context.calibrateCudaBatchSize
            ? calibrationBootstrapBatchSize
            : requestedBatchSize;
    PhysicsSandboxCudaSearchConfiguration configuration;
    configuration.maximumBatchSize = initialSessionCapacity;
    configuration.earliestMutationTimeMs = earliestMutationTimeMs;
    configuration.evaluationStartTimeMs = evaluationPlan.startTimeMs;
    configuration.evaluationEndTimeMs = evaluationPlan.endTimeMs;
    configuration.modifiers = *context.cudaModifiers;
    configuration.evaluator = *context.cudaEvaluator;
    if (context.condition != nullptr) {
        configuration.condition = context.condition->cuda;
        configuration.condition->lastImprovementTimeSeconds =
                context.searchStartedTimeSeconds;
        configuration.condition->lastRestartTimeSeconds =
                context.searchStartedTimeSeconds;
    }
    configuration.useSessionSpecialization =
            context.useCudaSessionSpecialization;
    configuration.captureBestState = !context.resolveCudaWinner;
    std::optional<PhysicsSandboxCudaSearchSession> session;
    session.emplace(Require(
            CreatePhysicsSandboxCudaSearchSession(
                    context.sandbox, configuration),
            "creating resident CUDA search session"));
    ReportCudaBatchCapacity(context.control, initialSessionCapacity);
    std::optional<CudaBatchCalibrator> calibrator;
    CudaCalibrationSafetyPlanner calibrationSafety;
    if (context.calibrateCudaBatchSize) {
        calibrator.emplace(requestedBatchSize);
    }
    std::uint32_t sessionCapacity = initialSessionCapacity;
    const std::uint64_t timelineTickCount =
            static_cast<std::uint64_t>(
                    (evaluationPlan.endTimeMs -
                     earliestMutationTimeMs) /
                    context.tickDurationMs) +
            1u;

    BestIteration best;
    std::uint64_t iterations = 0u;
    std::uint64_t evaluatorCalls = 0u;
    std::uint64_t mutationImprovementCount = 0u;
    std::uint64_t totalMutationCount = 0u;
    std::uint64_t qualifyingCandidateCount = 0u;
    std::optional<double> closestTargetDistance;
    std::optional<std::chrono::steady_clock::duration>
            lastImprovementElapsed;
    auto lastLiveReport = started - std::chrono::milliseconds(100);
    const auto reportLive = [&](bool force) {
        const auto now = std::chrono::steady_clock::now();
        if (!force &&
            now - lastLiveReport < std::chrono::milliseconds(100)) {
            return;
        }
        ReportLive(context.control,
                   best,
                   iterations,
                   evaluatorCalls,
                   mutationImprovementCount,
                   totalMutationCount,
                   qualifyingCandidateCount,
                   closestTargetDistance,
                   now - started,
                   lastImprovementElapsed);
        lastLiveReport = now;
    };
    const auto accumulateTargetProgress =
            [&](const PhysicsSandboxCudaSearchBatch &batch) {
                qualifyingCandidateCount +=
                        batch.qualifyingCandidateCount;
                if (batch.closestTargetDistance &&
                    (!closestTargetDistance ||
                     *batch.closestTargetDistance <
                             *closestTargetDistance)) {
                    closestTargetDistance =
                            batch.closestTargetDistance;
                }
            };
    const auto adoptBest =
            [&](PhysicsSandboxCudaSearchBatch &batch) {
                if (!batch.bestValid ||
                    (!batch.bestChanged && best.evaluation.has_value())) {
                    return;
                }
                best.source = batch.bestIsMutation
                        ? SearchWinnerSource::Mutation
                        : SearchWinnerSource::Baseline;
                best.iterationIndex = batch.bestCandidateId;
                best.mutationCount = batch.bestMutationCount;
                best.evaluationTick = batch.bestEvaluationTick;
                best.detail0 = batch.bestDetail0;
                best.detail1 = batch.bestDetail1;
                best.evaluation = EvaluationSample{
                        batch.bestScore,
                        batch.bestTimeMs,
                        CudaEvaluationDescription(
                                *context.cudaEvaluator, batch)};
                if (batch.bestSnapshot) {
                    best.view = batch.bestState;
                    best.snapshot = std::move(*batch.bestSnapshot);
                } else if (context.resolveCudaWinner) {
                    const std::uint64_t absoluteTick =
                            static_cast<std::uint64_t>(
                                    evaluationPlan.startTimeMs /
                                    context.tickDurationMs) +
                            batch.bestEvaluationTick;
                    if (absoluteTick >
                        std::numeric_limits<std::uint32_t>::max()) {
                        throw std::overflow_error(
                                "CUDA winner tick is out of range");
                    }
                    SearchExecutionContext::ResolvedCudaWinner resolved =
                            context.resolveCudaWinner(
                                    batch.bestInputs,
                                    static_cast<std::uint32_t>(absoluteTick));
                    best.view = resolved.view;
                    best.snapshot = std::move(resolved.snapshot);
                    if (std::holds_alternative<
                                PhysicsSandboxCudaFinishTimeEvaluator>(
                                *context.cudaEvaluator)) {
                        if (!best.view.finishTime ||
                            static_cast<double>(
                                    best.view.finishTime->estimatedNs) !=
                                    batch.bestScore) {
                            throw std::runtime_error(
                                "reference winner finish time does not match "
                                "CUDA");
                        }
                    }
                } else {
                    throw std::runtime_error(
                            "CUDA winner state was not captured");
                }
                best.inputs = std::move(batch.bestInputs);
            };

    CheckCancellation(context.control);
    ReportProgress(context.control, SearchProgressStage::Baseline, 0u);
    const auto baselineStarted = std::chrono::steady_clock::now();
    PhysicsSandboxCudaSearchBatch baseline = Require(
            session->EvaluateBaseline(
                    [control = context.control]() {
                        return control != nullptr &&
                                control->cancellationRequested &&
                                control->cancellationRequested();
                    }),
            "evaluating CUDA baseline");
    ReportCudaBatchProfile(
            "baseline",
            baseline,
            timelineTickCount,
            std::chrono::steady_clock::now() - baselineStarted);
    if (calibrator) {
        calibrationSafety.Observe(
                CudaCalibrationProfile(
                        baseline, sessionCapacity));
    }
    if (baseline.cancelled) {
        throw SearchCancelled();
    }
    evaluatorCalls += baseline.evaluatorCalls;
    accumulateTargetProgress(baseline);
    adoptBest(baseline);
    reportLive(true);
    if (calibrator) {
        ReportProgress(
                context.control, SearchProgressStage::Calibration, 0u);
    } else {
        ReportCudaBatchSize(context.control, requestedBatchSize);
        ReportProgress(
                context.control, SearchProgressStage::Mutations, 0u);
    }

    std::uint64_t iterationIndex = 0u;
    std::optional<std::uint32_t> reportedCalibrationBatchSize;
    while (!StopRequested(context.control) &&
           !IterationLimitReached(context.control, iterations)) {
        CheckCancellation(context.control);
        std::optional<CudaCalibrationDeviceLimits> calibrationLimits;
        std::optional<std::uint32_t> stagedProbe;
        if (calibrator) {
            calibrationLimits = QueryCudaCalibrationDeviceLimits();
            if (!calibrator->Complete()) {
                stagedProbe = calibrationSafety.NextStagedProbe(
                        calibrator->CurrentBatchSize(),
                        *calibrationLimits);
            }
        }
        const bool calibrationBootstrap = stagedProbe.has_value();
        std::uint32_t batchSize = calibrationBootstrap
                ? *stagedProbe
                : calibrator
                ? calibrator->CurrentBatchSize()
                : context.cudaBatchSize;
        if (context.control != nullptr &&
            context.control->iterationLimit) {
            batchSize = static_cast<std::uint32_t>(
                    std::min<std::uint64_t>(
                            batchSize,
                            *context.control->iterationLimit -
                                    iterations));
        }
        const bool requiresCalibrationSafety = calibrator &&
                ((calibrationBootstrap && batchSize > 2u) ||
                 (!calibrationBootstrap &&
                  (!calibrator->Complete() ||
                   batchSize > sessionCapacity)));
        if (requiresCalibrationSafety) {
            const CudaCalibrationSafetyDecision decision =
                    calibrationSafety.Evaluate(
                            batchSize,
                            sessionCapacity,
                            *calibrationLimits);
            if (!decision.safe) {
                ReportRejectedCudaCalibrationBatch(
                        batchSize, decision);
                if (calibrator->Complete()) {
                    throw std::runtime_error(
                            "the selected CUDA calibration batch is no "
                            "longer inside verified safe limits: " +
                            decision.reason);
                }
                if (batchSize == 1u) {
                    throw std::runtime_error(
                            "no CUDA calibration batch is inside "
                            "verified safe limits: " +
                            decision.reason);
                }
                calibrator->RejectUnsafeCurrent();
                if (calibrator->Complete()) {
                    ReportCudaCalibrationSelection(
                            *calibrator);
                    ReportProgress(
                            context.control,
                            SearchProgressStage::Mutations,
                            iterations);
                }
                continue;
            }
        }
        if (batchSize > sessionCapacity) {
            PhysicsSandboxResult<std::uint32_t> reserved =
                    session->ReserveBatchCapacity(batchSize);
            if (!reserved) {
                if (!calibrator ||
                    calibrator->Complete() ||
                    reserved.Error().code !=
                            PhysicsSandboxErrorCode::AllocationFailed) {
                    std::string message =
                            "reserving calibrated CUDA batch capacity failed";
                    if (!reserved.Error().diagnostic.empty()) {
                        message += ": " + reserved.Error().diagnostic;
                    }
                    throw std::runtime_error(std::move(message));
                }
                calibrator->CapacityUnavailable();
                if (calibrator->Complete()) {
                    ReportCudaCalibrationSelection(
                            *calibrator);
                    ReportProgress(
                            context.control,
                            SearchProgressStage::Mutations,
                            iterations);
                }
                continue;
            }
            sessionCapacity = reserved.Value();
            ReportCudaBatchCapacity(
                    context.control, sessionCapacity);
        }
        const bool exhaustsSequence =
                batchSize != 0u &&
                iterationIndex >
                        std::numeric_limits<std::uint64_t>::max() -
                                (batchSize - 1u);
        if (exhaustsSequence) {
            batchSize = static_cast<std::uint32_t>(
                    std::numeric_limits<std::uint64_t>::max() -
                    iterationIndex + 1u);
        }
        if (calibrator &&
            reportedCalibrationBatchSize != batchSize) {
            ReportCudaBatchSize(context.control, batchSize);
            reportedCalibrationBatchSize = batchSize;
        }
        const auto batchStarted = std::chrono::steady_clock::now();
        BeginIteration(context.control);
        PhysicsSandboxCudaSearchBatch batch = Require(
                session->RunBatch(
                        iterationIndex,
                        batchSize,
                        [control = context.control]() {
                            return control != nullptr &&
                                    control->cancellationRequested &&
                                    control->cancellationRequested();
                        }),
                "executing CUDA search batch");
        const auto batchElapsed =
                std::chrono::steady_clock::now() - batchStarted;
        ReportCudaBatchProfile(
                calibrationBootstrap
                        ? "calibration-bootstrap"
                        : "mutations",
                batch,
                timelineTickCount,
                batchElapsed);
        std::optional<CudaCalibrationSafetyDecision>
                executedCalibrationSafety;
        if (calibrator) {
            calibrationSafety.Observe(
                    CudaCalibrationProfile(
                            batch, sessionCapacity));
            if (!calibrationBootstrap &&
                !calibrator->Complete()) {
                executedCalibrationSafety =
                        calibrationSafety.Evaluate(
                                batch.candidateCount,
                                sessionCapacity,
                                QueryCudaCalibrationDeviceLimits());
            }
        }
        if (batch.cancelled) {
            throw SearchCancelled();
        }
        ReportCudaBatchExecution(
                context.control,
                batch.firstCandidateId,
                batch.candidateCount);
        iterations += batch.candidateCount;
        evaluatorCalls += batch.evaluatorCalls;
        totalMutationCount += batch.totalMutationCount;
        mutationImprovementCount +=
                batch.mutationImprovementCount;
        accumulateTargetProgress(batch);
        const bool promote =
                autoPromoteBest &&
                batch.mutationImprovementCount != 0u &&
                batch.bestValid;
        if (batch.bestChanged && batch.bestValid) {
            adoptBest(batch);
            if (autoPromoteBest) {
                best.mutationCount = EffectiveInputChangeCount(
                        originalBaselineInputs, best.inputs);
            }
        }
        if (configuration.condition &&
            batch.mutationImprovementCount != 0u) {
            configuration.condition->lastImprovementTimeSeconds =
                    std::chrono::duration<double>(
                            std::chrono::system_clock::now()
                                    .time_since_epoch())
                            .count();
            if (!promote) {
                Require(session->UpdateConditionTimes(
                                configuration.condition
                                        ->lastImprovementTimeSeconds,
                                configuration.condition
                                        ->lastRestartTimeSeconds),
                        "updating CUDA condition times");
            }
        }
        if (calibrator && !calibrationBootstrap &&
            !calibrator->Complete()) {
            if (executedCalibrationSafety &&
                !executedCalibrationSafety->safe) {
                ReportRejectedCudaCalibrationBatch(
                        batch.candidateCount,
                        *executedCalibrationSafety);
                if (batch.candidateCount == 1u) {
                    throw std::runtime_error(
                            "the minimum CUDA calibration batch "
                            "exceeded verified safe limits: " +
                            executedCalibrationSafety->reason);
                }
                calibrator->RejectUnsafeCurrent();
            } else {
                calibrator->Observe(
                        batch.candidateCount, batchElapsed);
            }
            if (calibrator->Complete() &&
                !calibrator->HasReliableMeasurement()) {
                throw std::runtime_error(
                        "CUDA calibration could not obtain a "
                        "repeatable throughput measurement");
            }
            if (calibrator->Complete()) {
                ReportCudaCalibrationSelection(*calibrator);
                ReportProgress(
                        context.control,
                        SearchProgressStage::Mutations,
                        iterations);
            }
        }
        if (promote) {
            session.reset();
            Require(context.sandbox.RestoreState(branch),
                    "restoring CUDA branch for promoted baseline");
            Require(context.sandbox.ReplaceInputs(best.inputs),
                    "promoting CUDA best inputs to baseline");
            const std::uint32_t recreatedCapacity =
                    calibrator
                    ? (calibrator->Complete()
                               ? calibrator->BestBatchSize()
                               : sessionCapacity)
                    : sessionCapacity;
            if (calibrator) {
                const CudaCalibrationSafetyDecision decision =
                        calibrationSafety.Evaluate(
                                recreatedCapacity,
                                0u,
                                QueryCudaCalibrationDeviceLimits());
                if (!decision.safe) {
                    throw std::runtime_error(
                            "recreating the promoted CUDA baseline would "
                            "leave verified safe limits: " +
                            decision.reason);
                }
            }
            configuration.maximumBatchSize = recreatedCapacity;
            if (!best.evaluation) {
                throw std::runtime_error(
                        "promoted CUDA incumbent is unavailable");
            }
            PhysicsSandboxCudaSearchIncumbent incumbent;
            incumbent.mutation =
                    best.source == SearchWinnerSource::Mutation;
            incumbent.candidateId = best.iterationIndex;
            incumbent.mutationCount = best.mutationCount;
            incumbent.evaluationTick = best.evaluationTick;
            incumbent.score = best.evaluation->score;
            incumbent.timeMs = best.evaluation->timeMs;
            incumbent.detail0 = best.detail0;
            incumbent.detail1 = best.detail1;
            incumbent.preciseFinish = std::holds_alternative<
                    PhysicsSandboxCudaFinishTimeEvaluator>(
                    *context.cudaEvaluator);
            configuration.incumbent = incumbent;
            session.emplace(Require(
                    CreatePhysicsSandboxCudaSearchSession(
                            context.sandbox, configuration),
                    "recreating promoted CUDA search session"));
            sessionCapacity = recreatedCapacity;
            ReportCudaBatchCapacity(
                    context.control, sessionCapacity);
        }
        if (batch.mutationImprovementCount != 0u) {
            lastImprovementElapsed =
                    std::chrono::steady_clock::now() - started;
            reportLive(true);
        } else {
            reportLive(false);
        }
        if (exhaustsSequence) {
            throw std::overflow_error("iteration sequence exhausted");
        }
        iterationIndex += batchSize;
    }

    CheckCancellation(context.control);
    reportLive(true);
    if (!best.evaluation || !best.snapshot) {
        throw std::runtime_error(
                "no iteration satisfied the selected evaluation target");
    }
    if (!SameState(best.snapshot->View(), best.view)) {
        throw std::runtime_error(
                "resolved CUDA global best does not match its captured state");
    }
    const bool mutationWon =
            best.source == SearchWinnerSource::Mutation;
    if (mutationWon != (mutationImprovementCount > 0u)) {
        throw std::runtime_error(
                "CUDA mutation winner and improvement count are inconsistent");
    }
    return SearchResult{
            best.source,
            best.iterationIndex,
            best.mutationCount,
            best.evaluation->score,
            best.evaluation->timeMs,
            best.evaluation->description,
            best.view,
            std::move(best.inputs),
            {},
            iterations,
            evaluatorCalls,
            mutationImprovementCount,
            totalMutationCount,
            std::chrono::steady_clock::now() - started,
            lastImprovementElapsed,
            *best.snapshot,
            qualifyingCandidateCount,
            closestTargetDistance};
}
#endif

}  // namespace

OptionSettings DefaultBasicBruteForceOptionSettings() {
    return {{"autoPromoteBest", "false"}};
}

std::optional<std::string> ValidateBasicBruteForceOptionSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) {
    if (const auto keyError = ValidateOptionSettingKeys(
                settings, DefaultBasicBruteForceOptionSettings())) {
        return keyError;
    }
    if (!ParseBoolean(settings.at("autoPromoteBest"))) {
        return "auto-promote best must be true or false";
    }
    if (tickDurationMs == 0u) {
        return "tick duration must be greater than zero";
    }
    return std::nullopt;
}

std::unique_ptr<SearchAlgorithm> CreateBasicBruteForceSearch(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) {
    if (const auto error =
                ValidateBasicBruteForceOptionSettings(settings, tickDurationMs)) {
        throw std::invalid_argument(*error);
    }
    return std::make_unique<BasicBruteForceSearch>(
            *ParseBoolean(settings.at("autoPromoteBest")));
}

BasicBruteForceSearch::BasicBruteForceSearch(bool autoPromoteBest)
    : autoPromoteBest_(autoPromoteBest) {}

SearchResult BasicBruteForceSearch::Run(
        const SearchExecutionContext &context) const {
    const auto started = std::chrono::steady_clock::now();
    if (context.tickDurationMs == 0u) {
        throw std::invalid_argument(
                "tick duration must be greater than zero");
    }

    const std::int64_t earliestMutationTimeMs = std::min(
            context.mutator.EarliestMutationTimeMs(),
            static_cast<std::int64_t>(context.simulationHorizonMs));
    if (earliestMutationTimeMs <
                static_cast<std::int64_t>(context.tickDurationMs) ||
        earliestMutationTimeMs % context.tickDurationMs != 0) {
        throw std::invalid_argument(
                "modifier pipeline must begin on or after the first whole "
                "tick");
    }
    CheckCancellation(context.control);
    PhysicsSandboxStateView current = Require(
            context.sandbox.ReadState(), "reading initial sandbox state");
    EvaluationPlan evaluationPlan = context.evaluator.Plan(
            context.simulationHorizonMs,
            earliestMutationTimeMs,
            context.tickDurationMs);
    if (context.control != nullptr &&
        context.control->evaluationEndTimeLimitMs) {
        evaluationPlan.endTimeMs = std::min(
                evaluationPlan.endTimeMs,
                *context.control->evaluationEndTimeLimitMs);
    }
    if (evaluationPlan.startTimeMs < earliestMutationTimeMs ||
        evaluationPlan.endTimeMs < evaluationPlan.startTimeMs ||
        evaluationPlan.endTimeMs > context.simulationHorizonMs ||
        evaluationPlan.startTimeMs % context.tickDurationMs != 0 ||
        evaluationPlan.endTimeMs % context.tickDurationMs != 0) {
        throw std::invalid_argument(
                "evaluation target returned an invalid observation plan: "
                "mutation=" +
                std::to_string(earliestMutationTimeMs) +
                " start=" +
                std::to_string(evaluationPlan.startTimeMs) +
                " end=" +
                std::to_string(evaluationPlan.endTimeMs) +
                " Simulation horizon=" +
                std::to_string(context.simulationHorizonMs));
    }

    const std::uint64_t branchTimeMs =
            static_cast<std::uint64_t>(earliestMutationTimeMs) -
            context.tickDurationMs;
    current = AdvanceTo(context.sandbox,
                        current.timeMs,
                        branchTimeMs,
                        context.tickDurationMs,
                        context.control);

    const std::vector<PhysicsSandboxInputEvent> baselineInputs = Require(
            context.sandbox.ReadInputs(), "reading baseline inputs");
    const PhysicsSandboxState branch = Require(
            context.sandbox.CaptureState(), "capturing branch state");

#if FOREVERVALIDATOR_HAS_CUDA
    if (context.sandbox.Backend() ==
                forevervalidator::SimulationBackend::Cuda &&
        context.cudaEvaluator != nullptr) {
        return RunCudaBasicBruteForce(
                context,
                evaluationPlan,
                earliestMutationTimeMs,
                branch,
                baselineInputs,
                autoPromoteBest_,
                started);
    }
#endif

    const std::uint64_t preEvaluationTimeMs =
            static_cast<std::uint64_t>(evaluationPlan.startTimeMs) -
            context.tickDurationMs;
    const std::uint64_t evaluationTicks =
            static_cast<std::uint64_t>(
                    (evaluationPlan.endTimeMs - evaluationPlan.startTimeMs) /
                    context.tickDurationMs) +
            1u;

    BestIteration best;
    std::uint64_t iterations = 0u;
    std::uint64_t evaluatorCalls = 0u;
    std::uint64_t mutationImprovementCount = 0u;
    std::uint64_t totalMutationCount = 0u;
    std::optional<std::chrono::steady_clock::duration>
            lastImprovementElapsed;
    double lastImprovementTimeSeconds = context.searchStartedTimeSeconds;
    auto lastLiveReport = started - std::chrono::milliseconds(100);
    const auto reportLive = [&](bool force) {
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - lastLiveReport < std::chrono::milliseconds(100)) {
            return;
        }
        ReportLive(context.control,
                   best,
                   iterations,
                   evaluatorCalls,
                   mutationImprovementCount,
                   totalMutationCount,
                   0u,
                   std::nullopt,
                   now - started,
                   lastImprovementElapsed);
        lastLiveReport = now;
    };

    const auto evaluateTimeline = [&](SearchWinnerSource source,
                                      std::optional<std::uint64_t> iterationIndex,
                                      std::size_t mutationCount) {
        bool improved = false;
        PhysicsSandboxStateView state = AdvanceTo(
                context.sandbox,
                branchTimeMs,
                preEvaluationTimeMs,
                context.tickDurationMs,
                context.control);
        std::optional<PhysicsSandboxStateView> previous = state;
        std::unique_ptr<IterationEvaluationSession> session =
                context.evaluator.CreateSession();
        for (std::uint64_t tick = 0u; tick < evaluationTicks; ++tick) {
            CheckCancellation(context.control);
            state = Require(context.sandbox.AdvanceTicks(1u),
                            "advancing evaluation tick");
            const double currentTimeSeconds =
                    std::chrono::duration<double>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
            const std::uint64_t conditionIterations =
                    source == SearchWinnerSource::Baseline
                    ? 0u
                    : iterationIndex.value_or(0u) + 1u;
            const bool eligible = context.condition == nullptr ||
                    context.condition->Evaluate(
                            *previous, state,
                            {conditionIterations,
                             lastImprovementTimeSeconds,
                             context.searchStartedTimeSeconds,
                             currentTimeSeconds});
            const std::optional<EvaluationSample> sample = eligible
                    ? session->Observe(previous, state)
                    : std::nullopt;
            previous = state;
            if (eligible) ++evaluatorCalls;
            if (!sample) {
                if (state.raceCompleted) break;
                continue;
            }
            if (!std::isfinite(sample->score) ||
                !std::isfinite(sample->timeMs)) {
                throw std::runtime_error(
                        "iteration evaluator returned a non-finite result");
            }
            if (best.evaluation &&
                !context.evaluator.IsBetter(*sample, *best.evaluation)) {
                if (state.raceCompleted) break;
                continue;
            }

            best.evaluation = sample;
            best.source = source;
            best.iterationIndex = iterationIndex;
            best.mutationCount = mutationCount;
            best.view = state;
            best.snapshot = Require(context.sandbox.CaptureState(),
                                    "capturing improved state");
            best.inputs = Require(context.sandbox.ReadInputs(),
                                  "reading improved inputs");
            improved = true;
            if (source == SearchWinnerSource::Mutation) {
                ++mutationImprovementCount;
                lastImprovementElapsed =
                        std::chrono::steady_clock::now() - started;
                lastImprovementTimeSeconds = currentTimeSeconds;
                reportLive(true);
            } else {
                reportLive(false);
            }
            if (state.raceCompleted) break;
        }
        return improved;
    };

    ReportProgress(context.control, SearchProgressStage::Baseline, 0u);
    evaluateTimeline(SearchWinnerSource::Baseline, std::nullopt, 0u);
    reportLive(true);
    ReportProgress(context.control, SearchProgressStage::Mutations, 0u);

    if (context.control != nullptr &&
        context.control->iterationIndexStride == 0u) {
        throw std::invalid_argument(
                "iteration index stride must be greater than zero");
    }
    std::uint64_t iterationIndex = context.control == nullptr
            ? 0u
            : context.control->iterationIndexOffset;
    const std::uint64_t iterationIndexStride = context.control == nullptr
            ? 1u
            : context.control->iterationIndexStride;
    std::vector<PhysicsSandboxInputEvent> mutationBaselineInputs =
            baselineInputs;
    std::uint64_t mutationBaselineGeneration = 0u;
    while (!StopRequested(context.control) &&
           !IterationLimitReached(context.control, iterations)) {
        BeginIteration(context.control);
        Require(context.sandbox.RestoreState(branch),
                "restoring branch state");
        MutationResult mutation = context.mutator.Mutate(
                {mutationBaselineInputs,
                 iterationIndex,
                 0u,
                 context.tickDurationMs,
                 earliestMutationTimeMs,
                 true,
                 mutationBaselineGeneration});
        CheckCancellation(context.control);
        ++iterations;
        bool improved = false;
        if (mutation.mutationCount != 0u) {
            totalMutationCount += mutation.mutationCount;
            if (mutation.windowPatch) {
                mutation.windowPatch->minimumTimeMs = std::min(
                        mutation.windowPatch->minimumTimeMs,
                        static_cast<std::int64_t>(
                                context.simulationHorizonMs));
                mutation.windowPatch->maximumTimeMs = std::min(
                        mutation.windowPatch->maximumTimeMs,
                        static_cast<std::int64_t>(
                                context.simulationHorizonMs));
                for (PhysicsSandboxInputEvent &event :
                     mutation.windowPatch->events) {
                    event.timeMs = std::min<std::int64_t>(
                            event.timeMs, context.simulationHorizonMs);
                }
                Require(context.sandbox.ReplaceInputWindow(
                                mutation.windowPatch->minimumTimeMs,
                                mutation.windowPatch->maximumTimeMs,
                                std::move(mutation.windowPatch->events)),
                        "replacing iteration input window");
            } else {
                Require(context.sandbox.ReplaceInputs(
                                std::move(mutation.inputs)),
                        "replacing iteration inputs");
            }
            improved = evaluateTimeline(
                    SearchWinnerSource::Mutation,
                    iterationIndex,
                    mutation.mutationCount);
            if (improved && autoPromoteBest_) {
                best.mutationCount = EffectiveInputChangeCount(
                        baselineInputs, best.inputs);
            }
        }
        if (autoPromoteBest_) {
            std::optional<std::vector<PhysicsSandboxInputEvent>>
                    sharedBaseline;
            if (context.control != nullptr &&
                context.control->promotedBaselineInputs) {
                sharedBaseline =
                        context.control->promotedBaselineInputs();
            }
            if (sharedBaseline) {
                mutationBaselineInputs =
                        std::move(*sharedBaseline);
                ++mutationBaselineGeneration;
            } else if (improved) {
                mutationBaselineInputs = best.inputs;
                ++mutationBaselineGeneration;
            }
        }
        reportLive(false);
        if (iterationIndex >
            std::numeric_limits<std::uint64_t>::max() -
                    iterationIndexStride) {
            throw std::overflow_error("iteration sequence exhausted");
        }
        iterationIndex += iterationIndexStride;
    }

    CheckCancellation(context.control);
    reportLive(true);
    if (!best.evaluation || !best.snapshot) {
        throw std::runtime_error(
                "no iteration satisfied the selected evaluation target");
    }
    const PhysicsSandboxStateView restored = Require(
            context.sandbox.RestoreState(*best.snapshot),
            "restoring global best state");
    if (!SameState(restored, best.view)) {
        throw std::runtime_error(
                "restored global best does not match its captured state");
    }
    const bool mutationWon = best.source == SearchWinnerSource::Mutation;
    if (mutationWon != (mutationImprovementCount > 0u)) {
        throw std::runtime_error(
                "mutation winner and improvement count are inconsistent");
    }

    return SearchResult{
            best.source,
            best.iterationIndex,
            best.mutationCount,
            best.evaluation->score,
            best.evaluation->timeMs,
            best.evaluation->description,
            best.view,
            std::move(best.inputs),
            {},
            iterations,
            evaluatorCalls,
            mutationImprovementCount,
            totalMutationCount,
            std::chrono::steady_clock::now() - started,
            lastImprovementElapsed,
            *best.snapshot,
            0u,
            std::nullopt};
}

}  // namespace forevertas
