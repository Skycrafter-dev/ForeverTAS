#include "searches/basic_brute_force_search.h"
#include "diagnostics/diagnostics_log.h"
#include "searches/gpu_segment_escalation.h"

#include "evaluators/evaluator_utils.h"
#include "mutations/input_event_utils.h"
#include "physics_backend.h"
#include "searches/cuda_batch_calibrator.h"
#include "searches/cuda_calibration_safety.h"
#if FOREVERVALIDATOR_HAS_HIP
#include "searches/hip_calibration_limits.h"
#endif
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
#if FOREVERVALIDATOR_HAS_TWIN
#include <forevervalidator/experimental/twin.h>

#include <condition_variable>
#include <exception>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#endif

namespace forevertas {
namespace {

using forevervalidator::DiscriminatedResult;
using forevervalidator::Vector3;
using forevervalidator::experimental::PhysicsSandbox;
using forevervalidator::experimental::PhysicsSandboxCarState;
using forevervalidator::experimental::PhysicsSandboxInputEvent;
using forevervalidator::experimental::PhysicsSandboxInputAction;
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

#if FOREVERVALIDATOR_HAS_CUDA || FOREVERVALIDATOR_HAS_HIP || \
        FOREVERVALIDATOR_HAS_TWIN
void ReportCudaBatchSize(const SearchRunControl *control,
                         std::uint32_t batchSize) {
    if (control != nullptr && control->cudaBatchSizeChanged) {
        control->cudaBatchSizeChanged(batchSize);
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

#if FOREVERVALIDATOR_HAS_TWIN
// FOREVERTAS_APPROXIMATE_DUMP=DIR: each approximate rebuild's twin inputs saved
// as DIR/rebuild-N.txt (for tools/twin/twin_bridge api --inputs).
std::string ApproximateDumpDirectory() {
#if defined(_WIN32)
    char *value = nullptr;
    std::size_t valueSize = 0u;
    if (_dupenv_s(&value, &valueSize, "FOREVERTAS_APPROXIMATE_DUMP") != 0 ||
        value == nullptr) {
        return {};
    }
    std::string directory(value);
    std::free(value);
    return directory;
#else
    const char *value = std::getenv("FOREVERTAS_APPROXIMATE_DUMP");
    return value != nullptr ? std::string(value) : std::string();
#endif
}
#endif

// The log gets a search's first GPU batches, then one every 30 s.
thread_local std::uint32_t loggedGpuBatches = 0u;
thread_local std::chrono::steady_clock::time_point lastLoggedGpuBatch{};

void ReportCudaBatchProfile(
        const char *phase,
        const forevervalidator::experimental::
                PhysicsSandboxCudaSearchBatch &batch,
        std::uint64_t timelineTickCount,
        std::chrono::steady_clock::duration wallElapsed) {
    const auto now = std::chrono::steady_clock::now();
    if (diagnostics::LogOpen() &&
        (loggedGpuBatches < 4u || now - lastLoggedGpuBatch >= std::chrono::seconds(30))) {
        ++loggedGpuBatches;
        lastLoggedGpuBatch = now;
        diagnostics::Log("gpu batch (%s) at candidate %llu: %u candidates, %.1f ms (kernels %.1f ms), "
                         "%.0f MiB on the device%s%s",
                         phase, static_cast<unsigned long long>(batch.firstCandidateId),
                         batch.candidateCount,
                         std::chrono::duration<double, std::milli>(wallElapsed).count(),
                         batch.metrics.kernelMilliseconds,
                         static_cast<double>(batch.metrics.residentDeviceBytes) / (1024.0 * 1024.0),
                         batch.bestChanged ? ", new best" : "", batch.cancelled ? ", cancelled" : "");
    }
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

CudaCalibrationDeviceLimits QueryGpuCalibrationDeviceLimits(
        forevervalidator::SimulationBackend backend) {
    static_cast<void>(backend);   // (a build without CUDA or HIP: the twin's CPU threads only)
#if FOREVERVALIDATOR_HAS_HIP
    if (backend == forevervalidator::SimulationBackend::Hip) {
        return QueryHipCalibrationDeviceLimits();
    }
#endif
#if FOREVERVALIDATOR_HAS_CUDA
    if (backend == forevervalidator::SimulationBackend::Cuda) {
        int device = 0;
        cudaDeviceProp properties{};
        std::size_t freeMemory = 0u;
        std::size_t totalMemory = 0u;
        cudaError_t error = cudaGetDevice(&device);
        if (error == cudaSuccess) {
            error = cudaGetDeviceProperties(&properties, device);
        }
        if (error == cudaSuccess) {
            error = cudaMemGetInfo(&freeMemory, &totalMemory);
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
        return limits;
    }
#endif
    throw std::runtime_error("selected GPU backend is not compiled");
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
    profile.reservationBytesPerCandidate = batch.metrics.reservationBytesPerCandidate;
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
    diagnostics::Log("gpu calibration: %u candidates refused: %s", batchSize, decision.reason.c_str());
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
    diagnostics::Log("gpu calibration: %u candidates%s, %.0f per second, %.1f s measuring",
                     calibrator.CurrentBatchSize(), calibrator.Confirming() ? " (confirming)" : "",
                     calibrator.BestThroughput(), calibrator.SpentSeconds());
    std::clog << "forevertas_cuda_calibration_selected"
              << " candidates="
              << calibrator.CurrentBatchSize()
              << (calibrator.Confirming() ? " confirming=1" : "")
              << " sustained_iterations_per_second="
              << calibrator.BestThroughput()
              << " measured_sizes="
              << calibrator.ReliableMeasurementCount()
              << " calibration_seconds="
              << calibrator.SpentSeconds()
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
    // Approximate physics: the fields above stay the twin's best, which the
    // search goes on from; what is reported is its newest rebuild on exact
    // physics.
    struct Exact {
        EvaluationSample evaluation;
        SearchWinnerSource source = SearchWinnerSource::Baseline;
        std::optional<std::uint64_t> iterationIndex;
        std::size_t mutationCount = 0u;
        std::uint64_t mutationImprovementCount = 0u;
        PhysicsSandboxStateView view;
        PhysicsSandboxState snapshot;
        std::vector<PhysicsSandboxInputEvent> inputs;
        ApproximateResult approximate;
    };
    bool approximatePhysics = false;
    std::optional<Exact> exact;
};

void ReportLive(
        const SearchRunControl *control,
        const BestIteration &best,
        std::uint64_t iterations,
        std::uint64_t evaluatorCalls,
        std::uint64_t mutationImprovementCount,
        std::uint64_t totalMutationCount,
        std::chrono::steady_clock::duration elapsed,
        const std::optional<std::chrono::steady_clock::duration>
                &lastImprovementElapsed) {
    if (control == nullptr) {
        return;
    }
    if (control->statisticsChanged) {
        control->statisticsChanged({iterations, elapsed});
    }
    if (!control->liveChanged || !best.evaluation) {
        return;
    }
    if (best.approximatePhysics) {
        if (!best.exact) {
            return;
        }
        const BestIteration::Exact &exact = *best.exact;
        control->liveChanged({
                exact.source,
                exact.iterationIndex,
                exact.mutationCount,
                exact.evaluation.score,
                exact.evaluation.timeMs,
                exact.evaluation.description,
                exact.view,
                exact.inputs,
                iterations,
                evaluatorCalls,
                exact.mutationImprovementCount,
                totalMutationCount,
                elapsed,
                lastImprovementElapsed,
                {},
                exact.evaluation.objectiveScores,
                exact.evaluation.metricValues,
                exact.approximate});
        return;
    }
    control->liveChanged({
            best.source,
            best.iterationIndex,
            best.mutationCount,
            best.evaluation->score,
            best.evaluation->timeMs,
            best.evaluation->description,
            best.view,
            best.inputs,
            iterations,
            evaluatorCalls,
            mutationImprovementCount,
            totalMutationCount,
            elapsed,
            lastImprovementElapsed,
            {},
            best.evaluation->objectiveScores,
            best.evaluation->metricValues,
            std::nullopt});
}

#if FOREVERVALIDATOR_HAS_CUDA || FOREVERVALIDATOR_HAS_HIP || \
        FOREVERVALIDATOR_HAS_TWIN
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
                            << FormatRaceTimeMilliseconds(
                                       batch.bestTimeMs);
                    return description.str();
                } else if constexpr (std::is_same_v<
                                             T,
                                             PhysicsSandboxCudaVolumeEntryEvaluator>) {
                    if (configured.entrySpeed) {
                        return MetricDescription("Volume entry speed",
                                                 batch.bestScore, "m/s",
                                                 batch.bestTimeMs);
                    }
                    return TimeMetricDescription(
                            "Volume entry time", batch.bestTimeMs);
                } else if constexpr (std::is_same_v<
                                             T,
                                             PhysicsSandboxCudaCustomVolumeEntryEvaluator>) {
                    if (configured.entrySpeed) {
                        return MetricDescription("Custom volume entry speed",
                                                 batch.bestScore, "m/s",
                                                 batch.bestTimeMs);
                    }
                    return TimeMetricDescription(
                            "Custom volume entry time", batch.bestTimeMs);
                } else if constexpr (std::is_same_v<
                                             T,
                                             PhysicsSandboxCudaDistanceEvaluator>) {
                    std::string target = configured.finish
                            ? std::string("the finish")
                            : "checkpoint " +
                                      std::to_string(std::uint64_t{configured.checkpointIndex} + 1u);
                    target += " of lap " + std::to_string(configured.lap);
                    if (batch.bestScore < 1e12) {
                        return "Reached " + target + " at " +
                               FormatRaceTimeMilliseconds(batch.bestTimeMs);
                    }
                    // 1e12 + 1e6 per checkpoint still before it + metres.
                    return MetricDescription(
                            ((configured.aroundWalls.empty() ? "Distance to " : "Distance around walls to ") + target).c_str(),
                            std::fmod(batch.bestScore - 1e12, 1e6), "m",
                            batch.bestTimeMs);
                } else if constexpr (std::is_same_v<
                                             T,
                                             PhysicsSandboxCudaCheckpointEvaluator>) {
                    return TimeMetricDescription(
                            configured.finish ? "Finish checkpoint time" : "Checkpoint time",
                            batch.bestTimeMs);
                } else if constexpr (std::is_same_v<
                                             T,
                                             PhysicsSandboxCudaStuntPointsEvaluator>) {
                    return "Stunt points: " +
                            std::to_string(
                                    static_cast<std::uint32_t>(
                                            batch.bestScore)) +
                            " at " +
                            FormatRaceTimeMilliseconds(
                                    batch.bestTimeMs);
                } else if constexpr (std::is_same_v<
                                             T,
                                             PhysicsSandboxCudaScriptedEvaluator>) {
                    std::ostringstream description;
                    description.precision(8);
                    for (std::size_t i = 0u;
                         i < batch.bestMetricValues.size(); ++i) {
                        if (i != 0u) description << "; ";
                        description << "objective " << i + 1u << "="
                                    << batch.bestMetricValues[i];
                    }
                    return description.str();
                } else if constexpr (std::is_same_v<
                                             T,
                                             PhysicsSandboxCudaConditionTimeEvaluator>) {
                    return "Conditions first met at " + FormatRaceTimeMilliseconds(batch.bestTimeMs);
                } else {
                    return "Precise finish time: " +
                            FormatHumanDurationNanoseconds(
                                    static_cast<std::uint64_t>(
                                            batch.bestScore));
                }
            },
            evaluator);
}

// Approximate physics: one of the twin's bests to rebuild on exact physics.
struct ApproximateJob {
    BestIteration twin;
    std::uint64_t mutationImprovementCount = 0u;
    std::uint32_t followTicks = 0u;       // the branch to the twin's best tick
    std::uint32_t bestTick = 0u;          // absolute, as the resolver counts
    std::uint32_t evaluationEndTick = 0u;
    std::int64_t evaluationStartTimeMs = 0;
    ConditionExecutionContext condition;
};

struct ApproximateRebuilt {
    ApproximateJob job;
    std::vector<PhysicsSandboxInputEvent> inputs;
    SearchExecutionContext::ResolvedCudaWinner exact;
    double driftMeters = 0.0;
};

using ApproximateResolver = std::function<SearchExecutionContext::ResolvedCudaWinner(
        const std::vector<PhysicsSandboxInputEvent> &,
        std::int64_t,
        std::uint32_t,
        const ConditionExecutionContext &)>;

#if FOREVERVALIDATOR_HAS_TWIN
// The twin's new bests rebuilt on exact physics on the CPU while the GPU
// searches on. One rebuild runs at a time; a best that arrives meanwhile
// replaces the waiting one, so only the newest is rebuilt.
class ApproximateRebuilder final {
public:
    using Job = ApproximateJob;
    using Rebuilt = ApproximateRebuilt;

    ApproximateRebuilder(PhysicsSandbox branch, ApproximateResolver resolve)
        : branch_(std::move(branch)), resolve_(std::move(resolve)) {
        worker_ = std::thread([this]() { Run(); });
    }

    ~ApproximateRebuilder() {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            stop_ = true;
        }
        changed_.notify_all();
        worker_.join();
    }

    void Submit(Job job) {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            waiting_ = std::move(job);
        }
        changed_.notify_all();
    }

    // The newest rebuild not taken yet; rethrows a failed rebuild.
    std::optional<Rebuilt> Take() {
        std::lock_guard<std::mutex> guard(mutex_);
        if (failure_) std::rethrow_exception(failure_);
        std::optional<Rebuilt> taken = std::move(finished_);
        finished_.reset();
        return taken;
    }

    // Waits until the waiting and running rebuilds are done.
    void Drain() {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [this]() {
            return failure_ || (!waiting_ && !running_);
        });
    }

private:
    void Run() noexcept {
        // the twin world compiles while the GPU session starts
        try {
            bridge_ = Require(
                    forevervalidator::experimental::twin::CreateTwinBridge(branch_),
                    "preparing the exact rebuild");
        } catch (...) {
            std::lock_guard<std::mutex> guard(mutex_);
            failure_ = std::current_exception();
        }
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                changed_.wait(lock, [this]() { return stop_ || waiting_; });
                if (stop_) return;
                job = std::move(*waiting_);
                waiting_.reset();
                running_ = true;
            }
            try {
                std::optional<Rebuilt> rebuilt = Rebuild(std::move(job));
                std::lock_guard<std::mutex> guard(mutex_);
                if (rebuilt) finished_ = std::move(rebuilt);
                running_ = false;
            } catch (...) {
                std::lock_guard<std::mutex> guard(mutex_);
                if (!stop_) failure_ = std::current_exception();
                running_ = false;
            }
            changed_.notify_all();
        }
    }

    std::optional<Rebuilt> Rebuild(Job job) {
        namespace twin = forevervalidator::experimental::twin;
        if (!bridge_) throw std::runtime_error("the exact rebuild is unavailable");
        twin::TwinBridgeOptions options;
        options.threads = std::clamp(
                std::thread::hardware_concurrency() / 4u, 1u, 4u);
        options.cancelled = [this]() {
            std::lock_guard<std::mutex> guard(mutex_);
            return stop_;
        };
        if (const std::string dump = ApproximateDumpDirectory(); !dump.empty()) {
            std::ofstream out(dump + "/rebuild-" + std::to_string(++dumped_) + ".txt");
            out << "# follow " << job.followTicks << " ticks\n";
            for (const PhysicsSandboxInputEvent &e : job.twin.inputs) {
                out << e.timeMs << ' ' << static_cast<int>(e.action) << ' '
                    << static_cast<int>(e.value.kind) << ' '
                    << static_cast<int>(e.value.switchState) << ' '
                    << e.value.analog << '\n';
            }
        }
        const auto started = std::chrono::steady_clock::now();
        twin::TwinBridgeResult bridged = Require(
                bridge_->Run(job.twin.inputs, job.followTicks, options),
                "rebuilding the best on exact physics");
        const double bridgeSeconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - started).count();
        // Scored like a CUDA winner: through the twin's best tick, or to the
        // end of the evaluation when the exact run has no result by then.
        SearchExecutionContext::ResolvedCudaWinner exact = resolve_(
                bridged.inputs, job.evaluationStartTimeMs, job.bestTick,
                job.condition);
        if (!exact.evaluation && job.evaluationEndTick > job.bestTick) {
            exact = resolve_(bridged.inputs, job.evaluationStartTimeMs,
                             job.evaluationEndTick, job.condition);
        }
        if (CudaBatchProfilingEnabled()) {
            std::cerr << "approximate rebuild: " << bridged.ticks << " ticks in "
                      << bridgeSeconds << " s, pass 1 max "
                      << bridged.passOneMaxDriftMeters * 1000.0 << " mm, result max "
                      << bridged.maxDriftMeters * 1000.0 << " mm, final "
                      << bridged.finalDriftMeters * 1000.0 << " mm ("
                      << (bridged.solved ? "solved" : "pass 1 kept") << ", "
                      << bridged.steerEdits << " steer edits, "
                      << bridged.steerShifts << " steer shifts, "
                      << bridged.pedalShifts << " pedal shifts); approximate "
                      << job.twin.evaluation->score << ", exact "
                      << (exact.evaluation ? std::to_string(exact.evaluation->score)
                                           : std::string("none"))
                      << '\n' << std::flush;
        }
        if (!exact.evaluation) return std::nullopt;
        Rebuilt rebuilt{std::move(job), std::move(bridged.inputs),
                        std::move(exact), bridged.maxDriftMeters};
        return rebuilt;
    }

    PhysicsSandbox branch_;
    ApproximateResolver resolve_;
    std::unique_ptr<forevervalidator::experimental::twin::TwinBridge> bridge_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<Job> waiting_;
    std::optional<Rebuilt> finished_;
    std::exception_ptr failure_;
    bool running_ = false;
    bool stop_ = false;
    std::uint32_t dumped_ = 0u;
    std::thread worker_;
};
#else
class ApproximateRebuilder final {
public:
    ApproximateRebuilder(PhysicsSandbox, ApproximateResolver) {
        throw std::invalid_argument(
                "this build has no approximate physics");
    }
    void Submit(ApproximateJob) {}
    std::optional<ApproximateRebuilt> Take() { return std::nullopt; }
    void Drain() {}
};
#endif

SearchResult RunGpuBasicBruteForce(
        const SearchExecutionContext &context,
        const EvaluationPlan &evaluationPlan,
        std::int64_t earliestMutationTimeMs,
        const PhysicsSandboxState &branch,
        const std::vector<PhysicsSandboxInputEvent>
                &originalBaselineInputs,
        bool autoPromoteBest,
        const TreeSearchSettings *treeSettings,
        std::chrono::steady_clock::time_point started) {
    using namespace forevervalidator::experimental;
    if (context.cudaModifiers == nullptr ||
        context.cudaEvaluator == nullptr ||
        (!context.calibrateCudaBatchSize &&
         context.cudaBatchSize == 0u)) {
        throw std::invalid_argument(
                "GPU search configuration is unavailable");
    }

    constexpr std::uint32_t calibrationInitialBatchSize = 1u;
    const std::uint32_t initialBatchSize = calibrationInitialBatchSize;
    const auto queryLimits = [&]() {
        return QueryGpuCalibrationDeviceLimits(CandidateBackend(context));
    };
    if (context.approximateCpuThreads == 0u) {
        const auto initialLimits = queryLimits();
        const auto headroom = std::max<std::uint64_t>(512ull * 1024 * 1024,
                initialLimits.totalMemoryBytes / 100 * 15);
        if (initialLimits.freeMemoryBytes <= headroom)
            throw std::runtime_error("GPU search rejected before probe: insufficient memory headroom; reduce GPU memory use or choose Optimized CPU");
    }
    PhysicsSandboxCudaSearchConfiguration configuration;
    configuration.maximumBatchSize = initialBatchSize;
    configuration.earliestMutationTimeMs = earliestMutationTimeMs;
    configuration.evaluationStartTimeMs = evaluationPlan.startTimeMs;
    configuration.evaluationEndTimeMs = evaluationPlan.endTimeMs;
    configuration.modifiers = *context.cudaModifiers;
    configuration.evaluator = *context.cudaEvaluator;
    std::optional<GpuSegmentEscalation> segmentEscalation;
    if (treeSettings != nullptr) {
        std::int64_t first = std::numeric_limits<std::int64_t>::max();
        std::int64_t last = 0;
        for (const auto &modifier : configuration.modifiers) {
            std::visit([&](const auto &value) {
                first = std::min(first, value.window.minimumTimeMs);
                last = std::max(last, value.window.maximumTimeMs);
            }, modifier);
        }
        for (const auto &range : TreeSegmentRanges(
                     first, std::min(last, evaluationPlan.endTimeMs),
                     treeSettings->segmentCount, context.tickDurationMs)) {
            configuration.mutationSegments.push_back(
                    {range.minimumTimeMs, range.maximumTimeMs});
        }
        if (configuration.mutationSegments.empty()) {
            throw std::invalid_argument("GPU tree search has no mutable segments");
        }
        segmentEscalation.emplace(
                static_cast<std::uint32_t>(configuration.mutationSegments.size()),
                treeSettings->branchedSegmentCount,
                treeSettings->escalateAfterTrees);
        configuration.changedSegmentCount = segmentEscalation->ChangedSegmentCount();
    }
    if (context.condition != nullptr) {
        configuration.condition = context.condition->cuda;
        configuration.condition->lastImprovementTimeSeconds =
                context.searchStartedTimeSeconds;
        configuration.condition->lastRestartTimeSeconds =
                context.searchStartedTimeSeconds;
    } else if (std::holds_alternative<
                       PhysicsSandboxCudaScriptedEvaluator>(
                               configuration.evaluator)) {
        configuration.condition.emplace();
        PhysicsSandboxCudaConditionInstruction always;
        always.opcode = PhysicsSandboxCudaConditionOpcode::Constant;
        always.x = 1.0;
        configuration.condition->instructions.push_back(always);
        configuration.condition->lastImprovementTimeSeconds =
                context.searchStartedTimeSeconds;
        configuration.condition->lastRestartTimeSeconds =
                context.searchStartedTimeSeconds;
    }
    configuration.captureBestState = !context.resolveCudaWinner;
    // Approximate physics: no batch calibration (its measurements and safety
    // limits are FV CUDA's); a calibrated batch takes a size the twin runs well
    const std::uint32_t fixedBatchSize =
            context.approximatePhysics && context.calibrateCudaBatchSize
            ? 32768u : context.cudaBatchSize;
    std::optional<ApproximateRebuilder> rebuilder;
    const std::int64_t branchTimeMs = static_cast<std::int64_t>(
            Require(context.sandbox.ReadState(), "reading the branch").timeMs);
    if (context.approximatePhysics) {
        if (!context.resolveCudaWinner) {
            throw std::invalid_argument(
                    "approximate physics needs the optimized CPU winner "
                    "resolver");
        }
#if FOREVERVALIDATOR_HAS_TWIN
        PhysicsSandboxTwinSearchOptions twin;
        twin.gpu = context.approximateCpuThreads == 0u;
        twin.verifyWinners = false;
        twin.threads = context.approximateCpuThreads != 0u
                ? context.approximateCpuThreads
                : std::max(1u, std::thread::hardware_concurrency() / 2u);
        configuration.twin = twin;
#endif
        configuration.maximumBatchSize = fixedBatchSize;
        rebuilder.emplace(
                Require(ClonePhysicsSandbox(context.sandbox),
                        "copying the branch for the exact rebuild"),
                context.resolveCudaWinner);
    }
    std::optional<PhysicsSandboxCudaSearchSession> session;
    diagnostics::Log("gpu session: creating, batches up to %u%s",
                     configuration.maximumBatchSize,
                     context.approximatePhysics ? ", approximate physics" : "");
    session.emplace(Require(
            CreatePhysicsSandboxCudaSearchSession(
                    context.sandbox, configuration),
            "creating resident GPU search session"));
    diagnostics::Log("gpu session: created");
    loggedGpuBatches = 0u;
    std::optional<CudaBatchCalibrator> calibrator;
    CudaCalibrationSafetyPlanner calibrationSafety;
    if (context.calibrateCudaBatchSize && !context.approximatePhysics) {
        calibrator.emplace();
    }
    // Keep calibration from consuming a bounded search before mutations begin.
    constexpr std::uint64_t maximumCalibrationCandidates = 1'000'000u;
    const std::uint64_t calibrationCandidateBudget =
            context.control != nullptr &&
                            context.control->iterationLimit
                    ? std::min<std::uint64_t>(
                              maximumCalibrationCandidates,
                              std::max<std::uint64_t>(
                                      1u,
                                      *context.control->iterationLimit / 10u))
                    : maximumCalibrationCandidates;
    std::uint32_t sessionCapacity = configuration.maximumBatchSize;
    const std::uint64_t timelineTickCount =
            static_cast<std::uint64_t>(
                    (evaluationPlan.endTimeMs -
                     earliestMutationTimeMs) /
                    context.tickDurationMs) +
            1u;

    BestIteration best;
    best.approximatePhysics = context.approximatePhysics;
    std::uint64_t iterations = 0u;
    std::uint64_t evaluatorCalls = 0u;
    std::uint64_t mutationImprovementCount = 0u;
    std::uint64_t totalMutationCount = 0u;
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
                   now - started,
                   lastImprovementElapsed);
        lastLiveReport = now;
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
                                *context.cudaEvaluator, batch),
                        batch.bestObjectiveScores,
                        batch.bestMetricValues};
                const auto absoluteBestTick = [&]() {
                    const std::uint64_t absoluteTick =
                            static_cast<std::uint64_t>(
                                    evaluationPlan.startTimeMs /
                                    context.tickDurationMs) +
                            batch.bestEvaluationTick;
                    if (absoluteTick >
                        std::numeric_limits<std::uint32_t>::max()) {
                        throw std::overflow_error(
                                "GPU winner tick is out of range");
                    }
                    return static_cast<std::uint32_t>(absoluteTick);
                };
                const auto conditionContext = [&]() {
                    return ConditionExecutionContext{
                            batch.bestIsMutation
                                    ? batch.bestCandidateId.value_or(0u) + 1u
                                    : 0u,
                            configuration.condition
                                    ? configuration.condition->lastImprovementTimeSeconds
                                    : 0.0,
                            configuration.condition
                                    ? configuration.condition->lastRestartTimeSeconds
                                    : 0.0,
                            batch.evaluationCurrentTimeSeconds};
                };
                if (batch.bestSnapshot) {
                    best.view = batch.bestState;
                    best.snapshot = std::move(*batch.bestSnapshot);
                } else if (context.approximatePhysics) {
                    // The twin's best stays the search's; FV scores the
                    // baseline's inputs as they are and rebuilds the others.
                    const std::uint32_t tick = absoluteBestTick();
                    if (!batch.bestIsMutation) {
                        SearchExecutionContext::ResolvedCudaWinner exact =
                                context.resolveCudaWinner(
                                        batch.bestInputs,
                                        evaluationPlan.startTimeMs,
                                        tick,
                                        conditionContext());
                        if (exact.evaluation) {
                            best.exact = BestIteration::Exact{
                                    std::move(*exact.evaluation),
                                    SearchWinnerSource::Baseline,
                                    std::nullopt,
                                    0u,
                                    0u,
                                    exact.view,
                                    std::move(exact.snapshot),
                                    batch.bestInputs,
                                    ApproximateResult{
                                            batch.bestScore,
                                            best.evaluation->description,
                                            std::nullopt}};
                        }
                    } else {
                        ApproximateJob job;
                        job.twin = best;
                        job.twin.exact.reset();
                        job.twin.inputs = batch.bestInputs;
                        job.mutationImprovementCount = mutationImprovementCount;
                        job.followTicks = static_cast<std::uint32_t>(
                                tick - branchTimeMs / context.tickDurationMs);
                        job.bestTick = tick;
                        job.evaluationEndTick = static_cast<std::uint32_t>(
                                evaluationPlan.endTimeMs /
                                context.tickDurationMs);
                        job.evaluationStartTimeMs = evaluationPlan.startTimeMs;
                        job.condition = conditionContext();
                        rebuilder->Submit(std::move(job));
                    }
                } else if (context.resolveCudaWinner) {
                    const std::uint64_t absoluteTick =
                            static_cast<std::uint64_t>(
                                    evaluationPlan.startTimeMs /
                                    context.tickDurationMs) +
                            batch.bestEvaluationTick;
                    if (absoluteTick >
                        std::numeric_limits<std::uint32_t>::max()) {
                        throw std::overflow_error(
                                "GPU winner tick is out of range");
                    }
                    SearchExecutionContext::ResolvedCudaWinner resolved =
                            context.resolveCudaWinner(
                                    batch.bestInputs,
                                    evaluationPlan.startTimeMs,
                                    static_cast<std::uint32_t>(absoluteTick),
                                    {batch.bestIsMutation
                                             ? batch.bestCandidateId.value_or(0u) + 1u
                                             : 0u,
                                     configuration.condition
                                             ? configuration.condition->lastImprovementTimeSeconds
                                             : 0.0,
                                     configuration.condition
                                             ? configuration.condition->lastRestartTimeSeconds
                                             : 0.0,
                                     batch.evaluationCurrentTimeSeconds});
                    best.view = resolved.view;
                    best.snapshot = std::move(resolved.snapshot);
                    const bool preciseFinish = std::holds_alternative<
                                PhysicsSandboxCudaFinishTimeEvaluator>(
                                *context.cudaEvaluator);
                    if (preciseFinish) {
                        if (!best.view.finishTime ||
                            static_cast<double>(
                                    best.view.finishTime->estimatedNs) !=
                                    batch.bestScore) {
                            throw std::runtime_error(
                                "GPU/optimized CPU parity error: winner "
                                "finish time disagrees");
                        }
                    }
                    if (!resolved.evaluation) {
                        throw std::runtime_error(
                                "GPU/optimized CPU parity error: winner "
                                "has no optimized CPU evaluation");
                    }
                    const auto agrees = [](double cpu, double gpu) {
                        return std::isfinite(cpu) && std::isfinite(gpu) &&
                                std::abs(cpu - gpu) <=
                                        std::max(1e-5, std::abs(gpu) * 1e-5);
                    };
                    if (!preciseFinish &&
                        (!agrees(resolved.evaluation->score,
                                 batch.bestScore) ||
                         !agrees(resolved.evaluation->timeMs,
                                 batch.bestTimeMs))) {
                        throw std::runtime_error(
                                "GPU/optimized CPU parity error: winner "
                                "metric disagrees");
                    }
                    if (std::holds_alternative<
                                PhysicsSandboxCudaScriptedEvaluator>(
                                    *context.cudaEvaluator)) {
                        if (resolved.evaluation->objectiveScores.size() !=
                                    batch.bestObjectiveScores.size() ||
                            resolved.evaluation->metricValues.size() !=
                                    batch.bestMetricValues.size()) {
                            throw std::runtime_error(
                                    "GPU/optimized CPU parity error: scripted "
                                    "objective count disagrees");
                        }
                        for (std::size_t i = 0u;
                             i < batch.bestObjectiveScores.size(); ++i) {
                            if (!agrees(
                                        resolved.evaluation->objectiveScores[i],
                                        batch.bestObjectiveScores[i]) ||
                                !agrees(
                                        resolved.evaluation->metricValues[i],
                                        batch.bestMetricValues[i])) {
                                throw std::runtime_error(
                                        "GPU/optimized CPU parity error: scripted "
                                        "objective metric disagrees");
                            }
                        }
                    }
                    best.evaluation = std::move(resolved.evaluation);
                } else {
                    throw std::runtime_error(
                            "GPU winner state was not captured");
                }
                best.inputs = std::move(batch.bestInputs);
            };
    // Approximate physics: the newest finished rebuild becomes what is
    // reported, unless exact physics scores it worse than the reported one
    // (the rebuild could not follow the twin's path closely enough).
    const auto adoptRebuilt = [&]() {
        if (!rebuilder) return false;
        std::optional<ApproximateRebuilt> rebuilt = rebuilder->Take();
        if (!rebuilt) return false;
        if (best.exact && context.evaluator.IsBetter(
                                  best.exact->evaluation,
                                  *rebuilt->exact.evaluation)) {
            return false;
        }
        const BestIteration &twin = rebuilt->job.twin;
        const std::size_t changes = EffectiveInputChangeCount(
                originalBaselineInputs, rebuilt->inputs);
        best.exact = BestIteration::Exact{
                std::move(*rebuilt->exact.evaluation),
                twin.source,
                twin.iterationIndex,
                changes,
                rebuilt->job.mutationImprovementCount,
                rebuilt->exact.view,
                std::move(rebuilt->exact.snapshot),
                std::move(rebuilt->inputs),
                ApproximateResult{twin.evaluation->score,
                                  twin.evaluation->description,
                                  rebuilt->driftMeters}};
        return true;
    };

    CheckCancellation(context.control);
    ReportProgress(context.control, SearchProgressStage::Baseline, 0u);
    const auto baselineStarted = std::chrono::steady_clock::now();
    const auto cancelSubmission = [&]() {
        return context.control != nullptr &&
                context.control->cancellationRequested && context.control->cancellationRequested();
    };
    PhysicsSandboxCudaSearchBatch baseline = Require(
            session->EvaluateBaseline(
                    cancelSubmission),
            "evaluating GPU baseline");
    ReportCudaBatchProfile(
            "baseline",
            baseline,
            timelineTickCount,
            std::chrono::steady_clock::now() - baselineStarted);
    const auto baselineElapsed = std::chrono::steady_clock::now() - baselineStarted;
    calibrationSafety.Observe(CudaCalibrationProfile(baseline, sessionCapacity));
    if (calibrator) {
        // From the device's resident threads (one wave), the baseline's single candidate as the latency.
        const CudaCalibrationBatchProfile profile = CudaCalibrationProfile(baseline, sessionCapacity);
        const std::uint64_t resident = static_cast<std::uint64_t>(profile.simulationActiveBlocksPerMultiprocessor) *
                profile.simulationThreadsPerBlock * queryLimits().multiprocessorCount;
        std::uint32_t start = 1024u;
        while (start < 65536u && start < resident) start *= 2u;
        if (resident == 0u) start = 4096u;
        CudaBatchCalibrator::Settings settings;
        settings.startBatchSize = start;
        settings.latencySeconds = std::chrono::duration<double>(baselineElapsed).count();
        calibrator.emplace(settings);
    }
    if (baseline.cancelled) {
        throw SearchCancelled();
    }
    // (calibration too: it starts at the device's resident threads, not at one candidate)
    if (!context.approximatePhysics &&
        baseline.metrics.reservationBytesPerCandidate == 0 && (calibrator || context.cudaBatchSize > 1u) &&
        (context.control == nullptr || !context.control->iterationLimit ||
         *context.control->iterationLimit > 1u)) {
        // A second bounded measurement separates fixed scene storage from
        // per-candidate storage. The disposable session cannot promote a probe
        // winner or consume the real search's candidate sequence.
        const auto decision = calibrationSafety.Evaluate(2u, 0u,
                queryLimits());
        if (!decision.safe)
            throw std::runtime_error("Manual GPU safety probe rejected: " + decision.reason +
                    "; choose one parallel sample or Optimized CPU");
        auto probeConfiguration = configuration;
        probeConfiguration.maximumBatchSize = 2u;
        auto probe = Require(CreatePhysicsSandboxCudaSearchSession(context.sandbox, probeConfiguration),
                             "creating bounded manual GPU safety probe");
        const auto probeBaseline = Require(probe.EvaluateBaseline(cancelSubmission),
                                           "evaluating manual GPU safety probe baseline");
        if (probeBaseline.cancelled) throw SearchCancelled();
        const auto measurement = Require(probe.RunBatch(0u, 2u, cancelSubmission),
                                         "measuring bounded manual GPU safety probe");
        if (measurement.cancelled) throw SearchCancelled();
        calibrationSafety.Observe(CudaCalibrationProfile(measurement, 2u));
    }
    evaluatorCalls += baseline.evaluatorCalls;
    adoptBest(baseline);
    reportLive(true);
    if (calibrator) {
        ReportCudaBatchSize(
                context.control, calibrator->CurrentBatchSize());
        ReportProgress(
                context.control, SearchProgressStage::Calibration, 0u);
    } else {
        ReportProgress(
                context.control, SearchProgressStage::Mutations, 0u);
    }

    // (a machine of a group starts at its own range; GPUs sharing a sequence claim instead)
    std::uint64_t iterationIndex = context.control != nullptr ? context.control->iterationIndexOffset : 0u;
    bool sessionChanged = true;   // the next batch is the first since the session was made or grown
    // What the session mutates: the base run, then each promoted best.
    std::vector<PhysicsSandboxInputEvent> mutationBaseline = originalBaselineInputs;
    // Several GPUs: this one's candidates, claimed from the shared sequence a block at a time.
    std::uint64_t claimedNext = 0u;
    std::uint64_t claimedEnd = 0u;
    while (!StopRequested(context.control) &&
           !IterationLimitReached(context.control, iterations)) {
        CheckCancellation(context.control);
        // Several GPUs on one search: another GPU's better run becomes what
        // this one mutates and has to beat. This GPU's own best stays what it
        // reports, so the run is credited to the GPU that found it.
        // (approximate physics: the twin's session carries over to the new baseline)
        if (autoPromoteBest && context.control != nullptr && context.control->promotedBaselineInputs) {
            std::optional<std::vector<PhysicsSandboxInputEvent>> shared =
                    context.control->promotedBaselineInputs();
            if (shared && !std::equal(shared->begin(), shared->end(), mutationBaseline.begin(),
                                      mutationBaseline.end(), SameInputEvent)) {
                std::optional<PhysicsSandboxCudaSearchSession> previous;
                if (context.approximatePhysics) previous = std::move(session);
                session.reset();
                Require(context.sandbox.RestoreState(branch),
                        "restoring GPU branch for a shared best");
                Require(context.sandbox.ReplaceInputs(*shared),
                        "taking another GPU's best as baseline");
                configuration.maximumBatchSize = sessionCapacity;
                configuration.incumbent.reset();
                session.emplace(Require(
                        previous ? PromotePhysicsSandboxCudaSearchSession(
                                           context.sandbox, std::move(*previous), configuration)
                                 : CreatePhysicsSandboxCudaSearchSession(
                                           context.sandbox, configuration),
                        "creating GPU search session from a shared best"));
                const PhysicsSandboxCudaSearchBatch adopted = Require(
                        session->EvaluateBaseline(cancelSubmission),
                        "evaluating another GPU's best");
                if (adopted.cancelled) throw SearchCancelled();
                evaluatorCalls += adopted.evaluatorCalls;
                mutationBaseline = std::move(*shared);
                sessionChanged = true;
            }
        }
        if (calibrator && !calibrator->Complete() &&
            iterations >= calibrationCandidateBudget &&
            calibrator->CompleteWithBestMeasurement()) {
            ReportCudaBatchSize(context.control,
                                calibrator->CurrentBatchSize());
            ReportCudaCalibrationSelection(*calibrator);
            ReportProgress(context.control,
                           SearchProgressStage::Mutations, iterations);
        }
        std::uint32_t batchSize = calibrator
                ? calibrator->CurrentBatchSize()
                : fixedBatchSize;
        if (context.control != nullptr && context.control->iterationLimit)
            batchSize = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                    batchSize, *context.control->iterationLimit - iterations));
        if (!context.approximatePhysics) {
            const CudaCalibrationSafetyDecision decision =
                    calibrationSafety.Evaluate(
                            batchSize,
                            sessionCapacity,
                            queryLimits());
            if (!decision.safe) {
                if (!calibrator)
                    throw std::runtime_error("Manual GPU batch rejected before allocation/dispatch (" +
                            std::to_string(batchSize) + " samples): " + decision.reason +
                            "; reduce parallel samples, shorten the horizon, or use Optimized CPU");
                ReportRejectedCudaCalibrationBatch(
                        batchSize, decision);
                if (calibrator->Complete()) {
                    throw std::runtime_error(
                            "the selected GPU calibration batch is no "
                            "longer inside verified safe limits: " +
                            decision.reason);
                }
                if (batchSize == 1u) {
                    throw std::runtime_error(
                            "no GPU calibration batch is inside "
                            "verified safe limits: " +
                            decision.reason);
                }
                calibrator->RejectUnsafeCurrent();
                ReportCudaBatchSize(
                        context.control,
                        calibrator->CurrentBatchSize());
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
                            "reserving calibrated GPU batch capacity failed";
                    if (!reserved.Error().diagnostic.empty()) {
                        message += ": " + reserved.Error().diagnostic;
                    }
                    throw std::runtime_error(std::move(message));
                }
                calibrator->CapacityUnavailable();
                ReportCudaBatchSize(
                        context.control,
                        calibrator->CurrentBatchSize());
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
            sessionChanged = true;
        }
        if (context.control != nullptr &&
            context.control->iterationLimit) {
            batchSize = static_cast<std::uint32_t>(
                    std::min<std::uint64_t>(
                            batchSize,
                            *context.control->iterationLimit -
                                    iterations));
        }
        if (context.control != nullptr && context.control->claimIterations) {
            // a block of batches at a time, taken in order: an approximate session runs the next batch
            // ahead (the candidates after this batch's) and drops it when the next batch starts elsewhere
            if (claimedEnd - claimedNext < batchSize) {
                constexpr std::uint64_t batchesClaimed = 64u;
                claimedNext = context.control->claimIterations(
                        static_cast<std::uint32_t>(std::min<std::uint64_t>(
                                std::uint64_t{batchSize} * batchesClaimed,
                                std::numeric_limits<std::uint32_t>::max())));
                claimedEnd = claimedNext + std::min<std::uint64_t>(std::uint64_t{batchSize} * batchesClaimed,
                                                                   std::numeric_limits<std::uint32_t>::max());
            }
            iterationIndex = claimedNext;
            claimedNext += batchSize;
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
        const auto batchStarted = std::chrono::steady_clock::now();
        BeginIteration(context.control);
        PhysicsSandboxCudaSearchBatch batch = Require(
                session->RunBatch(
                        iterationIndex,
                        batchSize,
                        cancelSubmission),
                "executing GPU search batch");
        const auto batchElapsed =
                std::chrono::steady_clock::now() - batchStarted;
        ReportCudaBatchProfile(
                "mutations", batch, timelineTickCount, batchElapsed);
        std::optional<CudaCalibrationSafetyDecision>
                executedCalibrationSafety;
        {
            calibrationSafety.Observe(
                    CudaCalibrationProfile(
                            batch, sessionCapacity));
            if (calibrator && !calibrator->Complete()) {
                executedCalibrationSafety =
                        calibrationSafety.Evaluate(
                                batch.candidateCount,
                                sessionCapacity,
                                queryLimits());
            }
        }
        if (batch.cancelled) {
            throw SearchCancelled();
        }
        iterations += batch.candidateCount;
        evaluatorCalls += batch.evaluatorCalls;
        totalMutationCount += batch.totalMutationCount;
        mutationImprovementCount +=
                batch.mutationImprovementCount;
        const bool promote =
                autoPromoteBest &&
                batch.mutationImprovementCount != 0u &&
                batch.bestValid;
        if (segmentEscalation) {
            segmentEscalation->RecordBatch(batch.mutationImprovementCount != 0u);
            configuration.changedSegmentCount = segmentEscalation->ChangedSegmentCount();
            if (!promote) {
                Require(session->UpdateChangedSegmentCount(configuration.changedSegmentCount),
                        "updating GPU segment escalation");
            }
        }
        if (batch.bestChanged && batch.bestValid) {
            adoptBest(batch);
            best.mutationCount = EffectiveInputChangeCount(
                    originalBaselineInputs, best.inputs);
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
                        "updating GPU condition times");
            }
        }
        if (calibrator && !calibrator->Complete()) {
            if (executedCalibrationSafety &&
                !executedCalibrationSafety->safe) {
                ReportRejectedCudaCalibrationBatch(
                        batch.candidateCount,
                        *executedCalibrationSafety);
                if (batch.candidateCount == 1u) {
                    throw std::runtime_error(
                            "the minimum GPU calibration batch "
                            "exceeded verified safe limits: " +
                            executedCalibrationSafety->reason);
                }
                calibrator->RejectUnsafeCurrent();
            } else {
                const auto calibrationElapsed = batchElapsed;
                // a new best's state capture runs one candidate again: not the batch size's cost
                const auto capture = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double, std::milli>(
                                std::isfinite(batch.metrics.winnerStateCaptureKernelMilliseconds)
                                        ? batch.metrics.winnerStateCaptureKernelMilliseconds
                                        : 0.0));
                calibrator->Observe(
                        batch.candidateCount,
                        capture < calibrationElapsed ? calibrationElapsed - capture : calibrationElapsed,
                        batch.metrics.aroundWallsReruns != 0u ? CudaBatchCalibrator::Disturbance::Certain
                        : sessionChanged                     ? CudaBatchCalibrator::Disturbance::Possible
                                                             : CudaBatchCalibrator::Disturbance::None);
                if (calibrator->Confirming()) {   // chosen: the search proper from here
                    ReportCudaCalibrationSelection(*calibrator);
                    ReportProgress(context.control, SearchProgressStage::Mutations, iterations);
                }
            }
            if (calibrator->Complete() &&
                !calibrator->HasReliableMeasurement()) {
                throw std::runtime_error(
                        "GPU calibration could not obtain a "
                        "repeatable throughput measurement");
            }
            ReportCudaBatchSize(
                    context.control, calibrator->CurrentBatchSize());
            if (calibrator->Complete()) {
                ReportCudaCalibrationSelection(*calibrator);
                ReportProgress(
                        context.control,
                        SearchProgressStage::Mutations,
                        iterations);
            }
        }
        sessionChanged = false;
        if (promote) {
            const auto promoteStarted = std::chrono::steady_clock::now();
            // an approximate session takes the new baseline in place (its world, threads and device
            // buffers stay); an exact one is closed before it is made again
            std::optional<PhysicsSandboxCudaSearchSession> previous;
            if (context.approximatePhysics) previous.emplace(std::move(*session));
            session.reset();
            const auto sessionClosed = std::chrono::steady_clock::now();
            Require(context.sandbox.RestoreState(branch),
                    "restoring GPU branch for promoted baseline");
            Require(context.sandbox.ReplaceInputs(best.inputs),
                    "promoting GPU best inputs to baseline");
            const auto baselineReplaced = std::chrono::steady_clock::now();
            mutationBaseline = best.inputs;
            const std::uint32_t recreatedCapacity =
                    calibrator
                    ? (calibrator->Complete()
                               ? calibrator->BestBatchSize()
                               : calibrator->CurrentBatchSize())
                    : sessionCapacity;
            if (!context.approximatePhysics) {
                const CudaCalibrationSafetyDecision decision =
                        calibrationSafety.Evaluate(
                                recreatedCapacity,
                                0u,
                                queryLimits());
                if (!decision.safe) {
                    throw std::runtime_error(
                            "recreating the promoted GPU baseline would "
                            "leave verified safe limits: " +
                            decision.reason);
                }
            }
            configuration.maximumBatchSize = recreatedCapacity;
            if (!best.evaluation) {
                throw std::runtime_error(
                        "promoted GPU incumbent is unavailable");
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
            incumbent.objectiveScores =
                    best.evaluation->objectiveScores;
            incumbent.metricValues =
                    best.evaluation->metricValues;
            configuration.incumbent = incumbent;
            session.emplace(Require(
                    previous ? PromotePhysicsSandboxCudaSearchSession(
                                       context.sandbox, std::move(*previous), configuration)
                             : CreatePhysicsSandboxCudaSearchSession(
                                       context.sandbox, configuration),
                    "recreating promoted GPU search session"));
            sessionChanged = true;
            sessionCapacity = recreatedCapacity;
            if (CudaBatchProfilingEnabled()) {
                const auto ms = [](auto from, auto to) {
                    return std::chrono::duration<double, std::milli>(to - from).count();
                };
                const auto created = std::chrono::steady_clock::now();
                std::clog << "forevertas_cuda_promote close_ms=" << ms(promoteStarted, sessionClosed)
                          << " baseline_ms=" << ms(sessionClosed, baselineReplaced)
                          << " create_ms=" << ms(baselineReplaced, created) << '\n'
                          << std::flush;
            }
        }
        const bool rebuilt = adoptRebuilt();
        if (batch.mutationImprovementCount != 0u) {
            lastImprovementElapsed =
                    std::chrono::steady_clock::now() - started;
        }
        reportLive(batch.mutationImprovementCount != 0u || rebuilt);
        if (exhaustsSequence) {
            throw std::overflow_error("iteration sequence exhausted");
        }
        iterationIndex += batchSize;
    }

    CheckCancellation(context.control);
    if (rebuilder) {
        // the twin's last best, rebuilt before the search reports its result
        ReportProgress(context.control, SearchProgressStage::ExactRebuild,
                       iterations);
        rebuilder->Drain();
        adoptRebuilt();
        CheckCancellation(context.control);
    }
    reportLive(true);
    if (context.approximatePhysics) {
        if (!best.exact) {
            throw NoEligibleEvaluation();
        }
        BestIteration::Exact &exact = *best.exact;
        return SearchResult{
                exact.source,
                exact.iterationIndex,
                exact.mutationCount,
                exact.evaluation.score,
                exact.evaluation.timeMs,
                exact.evaluation.description,
                exact.view,
                std::move(exact.inputs),
                {},
                iterations,
                evaluatorCalls,
                exact.mutationImprovementCount,
                totalMutationCount,
                std::chrono::steady_clock::now() - started,
                lastImprovementElapsed,
                exact.snapshot,
                exact.evaluation.objectiveScores,
                exact.evaluation.metricValues,
                exact.approximate};
    }
    if (!best.evaluation || !best.snapshot) {
        throw NoEligibleEvaluation();
    }
    if (!SameState(best.snapshot->View(), best.view)) {
        throw std::runtime_error(
                "resolved GPU global best does not match its captured state");
    }
    const bool mutationWon =
            best.source == SearchWinnerSource::Mutation;
    if (mutationWon != (mutationImprovementCount > 0u)) {
        throw std::runtime_error(
                "GPU mutation winner and improvement count are inconsistent");
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
            best.evaluation->objectiveScores,
            best.evaluation->metricValues,
            std::nullopt};
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
        return kUnreadableSetting;
    }
    if (tickDurationMs == 0u) {
        return "the tick length is 0";
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

BasicBruteForceSearch::BasicBruteForceSearch(TreeSearchSettings gpuTreeSettings)
    : autoPromoteBest_(true), gpuTreeSettings_(std::move(gpuTreeSettings)) {}

SearchResult BasicBruteForceSearch::Run(
        const SearchExecutionContext &context) const {
    const auto started = std::chrono::steady_clock::now();
    if (gpuTreeSettings_ && !context.approximatePhysics &&
        CandidateBackend(context) != forevervalidator::SimulationBackend::Cuda &&
        CandidateBackend(context) != forevervalidator::SimulationBackend::Hip) {
        throw std::invalid_argument("GPU segment search requires CUDA or HIP");
    }
    if (context.tickDurationMs == 0u) {
        throw std::invalid_argument(
                "tick duration must be greater than zero");
    }

    const std::int64_t earliestMutationTimeMs = std::max<std::int64_t>(
            context.control != nullptr && context.control->iterationLimit == 0u ? context.tickDurationMs : 0u,
            std::min(
            context.mutator.EarliestMutationTimeMs(),
            static_cast<std::int64_t>(context.simulationHorizonMs)));
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
    if (evaluationPlan.startTimeMs < context.tickDurationMs ||
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
            static_cast<std::uint64_t>(std::min(
                    earliestMutationTimeMs, evaluationPlan.startTimeMs)) -
            context.tickDurationMs;
    current = AdvanceTo(context.sandbox,
                        current.timeMs,
                        branchTimeMs,
                        context.tickDurationMs,
                        context.control);

    const std::vector<PhysicsSandboxInputEvent> baselineInputs = Require(
            context.sandbox.ReadInputs(), "reading baseline inputs");
    const bool pruneRedundantAnalogInsertions = !current.stuntsScore &&
            std::none_of(baselineInputs.begin(), baselineInputs.end(), [](const auto &event) {
                return event.action == PhysicsSandboxInputAction::SteerLeft ||
                        event.action == PhysicsSandboxInputAction::SteerRight;
            });
    const PhysicsSandboxState branch = Require(
            context.sandbox.CaptureState(), "capturing branch state");

#if FOREVERVALIDATOR_HAS_CUDA || FOREVERVALIDATOR_HAS_HIP || \
        FOREVERVALIDATOR_HAS_TWIN
    // Approximate searches run as a session too (the twin's), on the GPU or
    // on CPU threads.
    if ((IsGpuSimulationBackend(CandidateBackend(context)) ||
         context.approximatePhysics) &&
        context.cudaEvaluator != nullptr) {
        return RunGpuBasicBruteForce(
                context,
                evaluationPlan,
                // The GPU session starts at the branch even when a custom
                // target observes the unmodified prefix before the first pass.
                std::min(earliestMutationTimeMs, evaluationPlan.startTimeMs),
                branch,
                baselineInputs,
                autoPromoteBest_,
                gpuTreeSettings_ ? &*gpuTreeSettings_ : nullptr,
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
                   now - started,
                   lastImprovementElapsed);
        lastLiveReport = now;
    };

    const auto evaluateTimeline = [&](SearchWinnerSource source,
                                      std::optional<std::uint64_t> iterationIndex,
                                      std::size_t mutationCount,
                                      std::size_t inputCount) {
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
        std::optional<EvaluationSample> cumulativeSample;
        PhysicsSandboxStateView cumulativeView;
        for (std::uint64_t tick = 0u;
             tick < evaluationTicks && !session->IsComplete(); ++tick) {
            // Targets whose samples only get worse with time cannot improve
            // past the incumbent's sample tick, so the attempt ends there
            // with exactly the result a full evaluation would give.
            if (source == SearchWinnerSource::Mutation && best.evaluation) {
                if (const std::optional<double> latest =
                            context.evaluator.LatestImprovingTimeMs(
                                    *best.evaluation)) {
                    const double ticks =
                            std::ceil(*latest / context.tickDurationMs);
                    if (static_cast<double>(state.timeMs) >=
                        ticks * context.tickDurationMs) {
                        break;
                    }
                }
            }
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
            session->SetExecutionContext(
                    {conditionIterations,
                     lastImprovementTimeSeconds,
                     context.searchStartedTimeSeconds,
                     currentTimeSeconds});
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
            if (context.evaluator.CompareAtEndOnly()) {
                cumulativeSample = sample;
                cumulativeView = state;
                if (state.raceCompleted) break;
                continue;
            }
            if (best.evaluation &&
                !ImprovesSearchResult(
                        context.evaluator,
                        *sample,
                        inputCount,
                        *best.evaluation,
                        best.inputs.size())) {
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
        if (cumulativeSample &&
            (!best.evaluation || ImprovesSearchResult(
                    context.evaluator, *cumulativeSample, inputCount,
                    *best.evaluation, best.inputs.size()))) {
            best.evaluation = *cumulativeSample;
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
                lastImprovementTimeSeconds =
                        std::chrono::duration<double>(
                                std::chrono::system_clock::now()
                                        .time_since_epoch()).count();
                reportLive(true);
            } else {
                reportLive(false);
            }
        }
        if (context.control != nullptr &&
            context.control->attemptCompleted) {
            std::optional<SearchLiveUpdate> attempt;
            if (cumulativeSample) {
                attempt.emplace();
                attempt->winnerSource = source;
                attempt->winningIterationIndex = iterationIndex;
                attempt->winningMutationCount = mutationCount;
                attempt->bestScore = cumulativeSample->score;
                attempt->bestEvaluationTimeMs =
                        cumulativeSample->timeMs;
                attempt->bestEvaluationDescription =
                        cumulativeSample->description;
                attempt->bestState = cumulativeView;
                attempt->bestInputs = Require(
                        context.sandbox.ReadInputs(),
                        "reading completed attempt inputs");
                attempt->objectiveScores =
                        cumulativeSample->objectiveScores;
                attempt->metricValues =
                        cumulativeSample->metricValues;
            }
            context.control->attemptCompleted(
                    iterationIndex, std::move(attempt));
        }
        return improved;
    };

    ReportProgress(context.control, SearchProgressStage::Baseline, 0u);
    evaluateTimeline(
            SearchWinnerSource::Baseline,
            std::nullopt,
            0u,
            baselineInputs.size());
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
    // Several devices on one search (this thread one of a CPU device's): the
    // candidates come from the search's one sequence, a block at a time.
    const bool claiming = context.control != nullptr &&
            static_cast<bool>(context.control->claimIterations);
    std::uint64_t claimedNext = 0u, claimedEnd = 0u;
    while (!StopRequested(context.control) &&
           !IterationLimitReached(context.control, iterations)) {
        BeginIteration(context.control);
        if (claiming) {
            if (claimedNext == claimedEnd) {
                constexpr std::uint32_t kClaimedBlock = 256u;
                claimedNext = context.control->claimIterations(kClaimedBlock);
                claimedEnd = claimedNext + kClaimedBlock;
            }
            iterationIndex = claimedNext++;
        }
        Require(context.sandbox.RestoreState(branch),
                "restoring branch state");
        MutationResult mutation = context.mutator.Mutate(
                {mutationBaselineInputs,
                 iterationIndex,
                 0u,
                 context.tickDurationMs,
                 earliestMutationTimeMs,
                 true,
                 mutationBaselineGeneration,
                 pruneRedundantAnalogInsertions});
        CheckCancellation(context.control);
        ++iterations;
        bool improved = false;
        if (mutation.mutationCount != 0u) {
            totalMutationCount += mutation.mutationCount;
            const std::size_t inputCount = mutation.windowPatch
                    ? InputCountAfterWindowPatch(
                              mutationBaselineInputs,
                              *mutation.windowPatch)
                    : mutation.inputs.size();
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
                    mutation.mutationCount,
                    inputCount);
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
        if (claiming) continue;
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
        throw NoEligibleEvaluation();
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
            best.evaluation->objectiveScores,
            best.evaluation->metricValues,
            std::nullopt};
}

}  // namespace forevertas
