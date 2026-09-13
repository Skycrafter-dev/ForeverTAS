#include "searches/search_runner.h"
#include "blocks/visual_runtime.h"
#if FOREVERVALIDATOR_HAS_CUDA
#include "blocks/program_cuda.h"
#endif
#include "blocks/block_value.h"

#include "input_timeline_time.h"
#include "mutations/composite_input_mutator.h"
#include "mutations/input_event_utils.h"
#include "replay_file_io.h"
#include "searches/algorithm_registry.h"
#include "searches/cuda_search_configuration.h"
#include "searches/option_settings_utils.h"

#include <forevervalidator/experimental/physics_sandbox.h>
#include <forevervalidator/native.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <deque>
#include <exception>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>

namespace forevertas {
namespace {

using forevervalidator::DiscriminatedResult;

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

void CheckCancellation(const SearchRunControl *control) {
    if (control != nullptr && control->cancellationRequested &&
        control->cancellationRequested()) {
        throw SearchCancelled();
    }
}

void ReportProgress(const SearchRunControl *control,
                    SearchProgressStage stage,
                    std::uint64_t completedWork,
                    std::uint64_t totalWork) {
    if (control != nullptr && control->progressChanged) {
        control->progressChanged(
                {stage, completedWork, totalWork});
    }
}

forevervalidator::experimental::PhysicsSandboxOptions CanonicalOptions(
        const SearchRequest &request,
        forevervalidator::SimulationBackend backend) {
    forevervalidator::experimental::PhysicsSandboxOptions options;
    options.backend = backend;
    options.tickDurationMs = kSearchTickDurationMs;
    options.timelineMode = forevervalidator::experimental::
            PhysicsSandboxTimelineMode::Canonical;
    options.simulationHorizonMs = request.simulationHorizonMs;
    return options;
}

SearchTimelineFrame ToTimelineFrame(
        const forevervalidator::experimental::PhysicsSandboxStateView &view) {
    return {
            static_cast<std::int64_t>(view.timeMs),
            view.car.position.x,
            view.car.position.y,
            view.car.position.z,
            view.car.rotationX,
            view.car.rotationY,
            view.car.rotationZ,
            view.car.rotationW,
            view.accelerate,
            view.brake,
            view.steering,
            view.checkpointsCollected,
            view.checkpointsTotal,
            view.completedLaps,
            view.totalLaps,
            view.raceCompleted,
            view.finishTimeMs,
            view.car.linearSpeed.x,
            view.car.linearSpeed.y,
            view.car.linearSpeed.z,
            view.car.signedSpeed,
            view.car.turbo,
            view.car.cameraFlightTransition,
            view.car.burning,
            view.car.gearChanged,
            view.car.wheelContact,
            view.car.wheelHasSurface,
            view.car.cameraSupportUp.x,
            view.car.cameraSupportUp.y,
            view.car.cameraSupportUp.z};
}

struct TimelineSamplingRuntime {
    forevervalidator::experimental::PhysicsSandbox sandbox;
    forevervalidator::experimental::PhysicsSandboxState initialState;
    std::uint64_t finalTickCount = 0u;
};

TimelineSamplingRuntime CreateTimelineSamplingRuntime(
        const SearchRequest &request,
        const forevervalidator::AssetBytes &replay,
        const forevervalidator::ReplayIdentity &identity) {
    using namespace forevervalidator;
    using namespace forevervalidator::experimental;

    AssetSource source = Require(
            OpenInstalledPackDirectory(request.packDirectory),
            "opening pack directory for timeline sampling");
    PhysicsSandboxOptions options = CanonicalOptions(
            request, ToForeverValidatorBackend(request.backend));
    PhysicsSandbox sandbox = Require(
            CreatePhysicsSandbox(std::move(source), options),
            "creating timeline-sampling sandbox");
    Require(sandbox.LoadScenario({replay.data(), replay.size()}, identity),
            "loading scenario for timeline sampling");
    PhysicsSandboxState initialState = Require(
            sandbox.CaptureState(),
            "capturing timeline-sampling initial state");
    return {
            std::move(sandbox),
            std::move(initialState),
            request.simulationHorizonMs / kSearchTickDurationMs};
}

TimelineSamplingRuntime CloneTimelineSamplingRuntime(
        const forevervalidator::experimental::PhysicsSandbox &source,
        std::uint64_t simulationHorizonMs) {
    using namespace forevervalidator::experimental;
    if (simulationHorizonMs % kSearchTickDurationMs != 0u) {
        throw std::runtime_error(
                "Simulation horizon is not aligned to the search tick duration");
    }
    PhysicsSandbox sandbox = Require(
            ClonePhysicsSandbox(source),
            "cloning timeline-sampling sandbox");
    PhysicsSandboxState initialState = Require(
            sandbox.CaptureState(),
            "capturing cloned timeline-sampling initial state");
    return {std::move(sandbox),
            std::move(initialState),
            simulationHorizonMs / kSearchTickDurationMs};
}

std::vector<SearchTimelineFrame> SampleTimeline(
        TimelineSamplingRuntime &runtime,
        const std::vector<
                forevervalidator::experimental::PhysicsSandboxInputEvent>
                &inputs,
        const SearchRunControl *control,
        bool reportProgress) {
    using namespace forevervalidator;
    using namespace forevervalidator::experimental;

    CheckCancellation(control);
    PhysicsSandboxStateView state = Require(
            runtime.sandbox.RestoreState(runtime.initialState),
            "restoring timeline-sampling initial state");
    Require(runtime.sandbox.ReplaceInputs(inputs),
            "replacing inputs for timeline sampling");
    std::vector<SearchTimelineFrame> frames;
    if (runtime.finalTickCount >= frames.max_size()) {
        throw std::length_error("timeline contains too many ticks");
    }
    frames.reserve(
            static_cast<std::size_t>(runtime.finalTickCount) + 1u);
    frames.push_back(ToTimelineFrame(state));
    if (reportProgress) {
        ReportProgress(control,
                       SearchProgressStage::FinalSampling,
                       0u,
                       runtime.finalTickCount);
    }

    for (std::uint64_t tick = 1u;
         tick <= runtime.finalTickCount && !state.raceCompleted;
         ++tick) {
        CheckCancellation(control);
        state = Require(runtime.sandbox.AdvanceTicks(1u),
                        "sampling run timeline");
        frames.push_back(ToTimelineFrame(state));
        if (reportProgress &&
            (tick == runtime.finalTickCount || tick % 128u == 0u)) {
            ReportProgress(control,
                           SearchProgressStage::FinalSampling,
                           tick,
                           runtime.finalTickCount);
        }
    }
    return frames;
}

std::vector<SearchTimelineFrame> SampleBestTimeline(
        const SearchRequest &request,
        const forevervalidator::AssetBytes &replay,
        const forevervalidator::ReplayIdentity &identity,
        const std::vector<
                forevervalidator::experimental::PhysicsSandboxInputEvent>
                &inputs,
        const SearchRunControl *control) {
    ReportProgress(
            control, SearchProgressStage::FinalSamplingSetup, 0u, 0u);
    SearchRequest samplingRequest = request;
#if FOREVERVALIDATOR_HAS_CUDA
    if (samplingRequest.backend == PhysicsBackend::Cuda) {
        samplingRequest.backend = PhysicsBackend::Reference;
    }
#endif
    TimelineSamplingRuntime runtime =
            CreateTimelineSamplingRuntime(samplingRequest, replay, identity);
    return SampleTimeline(runtime, inputs, control, true);
}

class AsyncImprovementTimelineSampler final {
public:
    AsyncImprovementTimelineSampler(
            SearchRequest request,
            forevervalidator::AssetBytes replay,
            forevervalidator::ReplayIdentity identity,
            const SearchRunControl *control,
            std::function<void(const SearchLiveUpdate &)> callback)
        : request_(std::move(request)),
          replay_(std::move(replay)),
          identity_(std::move(identity)),
          control_(control),
          callback_(std::move(callback)) {
        request_.backend = PhysicsBackend::Reference;
        samplingControl_.cancellationRequested = [this]() {
            return discardRequested_.load(std::memory_order_relaxed) ||
                    (control_ != nullptr &&
                     control_->cancellationRequested &&
                     control_->cancellationRequested());
        };
        worker_ = std::thread([this]() { Run(); });
    }

    ~AsyncImprovementTimelineSampler() {
        static_cast<void>(Stop(true));
    }

    AsyncImprovementTimelineSampler(
            const AsyncImprovementTimelineSampler &) = delete;
    AsyncImprovementTimelineSampler &operator=(
            const AsyncImprovementTimelineSampler &) = delete;

    void Submit(const SearchLiveUpdate &live) {
        std::exception_ptr failure;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            failure = failure_;
            if (!failure && !stopRequested_) {
                if (live.winnerSource == SearchWinnerSource::Baseline ||
                    pending_.empty() ||
                    pending_.back().winnerSource ==
                            SearchWinnerSource::Baseline) {
                    pending_.push_back(live);
                } else {
                    pending_.back() = live;
                }
            }
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
        ready_.notify_one();
    }

    std::exception_ptr Stop(bool discardPending) noexcept {
        if (discardPending) {
            discardRequested_.store(true, std::memory_order_relaxed);
        }
        {
            std::lock_guard<std::mutex> guard(mutex_);
            stopRequested_ = true;
            if (discardPending) {
                pending_.clear();
            }
        }
        ready_.notify_one();
        if (worker_.joinable()) {
            worker_.join();
        }
        std::lock_guard<std::mutex> guard(mutex_);
        return failure_;
    }

private:
    void Run() noexcept {
        try {
            std::optional<TimelineSamplingRuntime> runtime;
            for (;;) {
                std::optional<SearchLiveUpdate> task;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    ready_.wait(lock, [this]() {
                        return stopRequested_ || !pending_.empty();
                    });
                    if (pending_.empty()) {
                        return;
                    }
                    task = std::move(pending_.front());
                    pending_.pop_front();
                }
                if (!runtime) {
                    runtime.emplace(CreateTimelineSamplingRuntime(
                            request_, replay_, identity_));
                }
                task->bestTimeline = SampleTimeline(
                        *runtime,
                        task->bestInputs,
                        &samplingControl_,
                        false);
                callback_(*task);
            }
        } catch (...) {
            std::lock_guard<std::mutex> guard(mutex_);
            failure_ = std::current_exception();
            pending_.clear();
            stopRequested_ = true;
        }
    }

    SearchRequest request_;
    forevervalidator::AssetBytes replay_;
    forevervalidator::ReplayIdentity identity_;
    const SearchRunControl *control_ = nullptr;
    SearchRunControl samplingControl_;
    std::function<void(const SearchLiveUpdate &)> callback_;
    std::atomic_bool discardRequested_{false};
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<SearchLiveUpdate> pending_;
    std::exception_ptr failure_;
    bool stopRequested_ = false;
    std::thread worker_;
};

class CudaWinnerReferenceWorker final {
public:
    CudaWinnerReferenceWorker(
            SearchRequest request,
            forevervalidator::AssetBytes replay,
            forevervalidator::ReplayIdentity identity)
        : request_(std::move(request)),
          replay_(std::move(replay)),
          identity_(std::move(identity)) {
        request_.backend = PhysicsBackend::Reference;
        worker_ = std::thread([this]() { Run(); });
    }

    ~CudaWinnerReferenceWorker() {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            stopRequested_ = true;
        }
        ready_.notify_one();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    SearchExecutionContext::ResolvedCudaWinner Resolve(
            const std::vector<forevervalidator::experimental::
                                      PhysicsSandboxInputEvent> &inputs,
            std::uint32_t tick) {
        Task task;
        task.inputs = inputs;
        task.tick = tick;
        std::future<SearchExecutionContext::ResolvedCudaWinner> future =
                task.result.get_future();
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (stopRequested_ || pending_) {
                throw std::runtime_error(
                        "reference winner worker is unavailable");
            }
            pending_.emplace(std::move(task));
        }
        ready_.notify_one();
        return future.get();
    }

private:
    struct Task {
        std::vector<forevervalidator::experimental::
                            PhysicsSandboxInputEvent>
                inputs;
        std::uint32_t tick = 0u;
        std::promise<SearchExecutionContext::ResolvedCudaWinner> result;
    };

    void Run() noexcept {
        std::optional<TimelineSamplingRuntime> runtime;
        for (;;) {
            std::optional<Task> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [this]() {
                    return stopRequested_ || pending_.has_value();
                });
                if (!pending_) {
                    return;
                }
                task.emplace(std::move(*pending_));
                pending_.reset();
            }
            try {
                if (!runtime) {
                    runtime.emplace(CreateTimelineSamplingRuntime(
                            request_, replay_, identity_));
                }
                if (task->tick > runtime->finalTickCount) {
                    throw std::out_of_range(
                            "CUDA winner tick exceeds the Simulation horizon");
                }
                Require(runtime->sandbox.RestoreState(runtime->initialState),
                        "restoring reference winner worker");
                Require(runtime->sandbox.ReplaceInputs(task->inputs),
                        "replacing reference winner inputs");
                forevervalidator::experimental::PhysicsSandboxStateView view =
                        task->tick == 0u
                        ? Require(runtime->sandbox.ReadState(),
                                  "reading reference winner state")
                        : Require(runtime->sandbox.AdvanceTicks(task->tick),
                                  "simulating reference winner");
                forevervalidator::experimental::PhysicsSandboxState snapshot =
                        Require(
                                runtime->sandbox.CaptureState(),
                                "capturing reference winner state");
                task->result.set_value({view, std::move(snapshot)});
            } catch (...) {
                task->result.set_exception(std::current_exception());
            }
        }
    }

    SearchRequest request_;
    forevervalidator::AssetBytes replay_;
    forevervalidator::ReplayIdentity identity_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::optional<Task> pending_;
    bool stopRequested_ = false;
    std::thread worker_;
};

struct CachedSearchSandbox {
    std::mutex lock;
    forevervalidator::AssetBytes replay;
    std::optional<
            forevervalidator::experimental::PhysicsSandbox>
            sandbox;
    std::optional<
            forevervalidator::experimental::PhysicsSandboxState>
            initialState;
    std::vector<
            forevervalidator::experimental::PhysicsSandboxInputEvent>
            initialInputs;
};

std::shared_ptr<CachedSearchSandbox> CachedSandboxFor(
        const SearchRequest &request) {
    using Key = std::tuple<std::string,
                           std::string,
                           PhysicsBackend,
                           std::uint32_t,
                           bool>;
    static std::mutex cacheLock;
    static std::map<Key, std::shared_ptr<CachedSearchSandbox>> cache;
    const Key key{
            request.packDirectory,
            request.replayPath,
            request.backend,
            request.simulationHorizonMs,
            request.useCudaSessionSpecialization};
    std::lock_guard<std::mutex> guard(cacheLock);
    auto &entry = cache[key];
    if (!entry) {
        entry = std::make_shared<CachedSearchSandbox>();
    }
    return entry;
}

SearchResult RunLoadedSearch(
        const SearchRequest &request,
        const forevervalidator::AssetBytes &replay,
        const forevervalidator::ReplayIdentity &identity,
        forevervalidator::experimental::PhysicsSandbox &sandbox,
        const SearchAlgorithmRegistration &searchRegistration,
        const EvaluationTargetRegistration &evaluationRegistration,
        const SearchRunControl *control) {
    using namespace forevervalidator::experimental;

    std::unique_ptr<SearchAlgorithm> search =
            searchRegistration.create(
                    request.searchAlgorithm.settings,
                    kSearchTickDurationMs);
    std::vector<std::unique_ptr<InputMutator>> modifierPasses;
    modifierPasses.reserve(request.modifiers.size());
    for (const OptionConfiguration &modifier : request.modifiers) {
        const ModifierRegistration *const registration =
                FindModifier(modifier.id);
        modifierPasses.push_back(registration->create(
                modifier.settings, kSearchTickDurationMs));
    }
    const CompositeInputMutator mutator(std::move(modifierPasses));
    std::unique_ptr<IterationEvaluator> evaluator =
            evaluationRegistration.create(
                    request.evaluationTarget.settings,
                    kSearchTickDurationMs);
    std::vector<PhysicsSandboxCudaModifier> cudaModifiers;
    std::optional<PhysicsSandboxCudaEvaluator> cudaEvaluator;
    ReportProgress(control, SearchProgressStage::PreparingSearch, 0u, 0u);
#if FOREVERVALIDATOR_HAS_CUDA
    if (request.backend == PhysicsBackend::Cuda) {
        if (request.searchAlgorithm.id != kBasicBruteForceSearchId) {
            throw std::invalid_argument(
                    "CUDA does not support search algorithm: " +
                    request.searchAlgorithm.id);
        }
        cudaModifiers = BuildCudaModifiers(
                request.modifiers, kSearchTickDurationMs);
        cudaEvaluator = BuildCudaEvaluator(
                {evaluationRegistration.id,
                 request.evaluationTarget.settings},
                kSearchTickDurationMs);
    }
#endif
    const SearchRunControl *executionControl = control;
    SearchRunControl instrumentedControl;
    std::unique_ptr<TimelineSamplingRuntime> improvementSampler;
    std::unique_ptr<AsyncImprovementTimelineSampler>
            asyncImprovementSampler;
    std::uint64_t sampledImprovementCount = 0u;
    bool sampledBaseline = false;
#if FOREVERVALIDATOR_HAS_CUDA
    std::unique_ptr<CudaWinnerReferenceWorker> cudaWinnerWorker;
    if (request.backend == PhysicsBackend::Cuda) {
        cudaWinnerWorker = std::make_unique<CudaWinnerReferenceWorker>(
                request, replay, identity);
    }
#endif
    if (control != nullptr && control->liveChanged &&
        control->sampleImprovementTimelines) {
        instrumentedControl = *control;
        const auto downstreamLiveChanged = control->liveChanged;
#if FOREVERVALIDATOR_HAS_CUDA
        const bool asynchronousCpuSampling =
                request.backend == PhysicsBackend::Cuda &&
                static_cast<bool>(control->improvementTimelineSampled);
#else
        constexpr bool asynchronousCpuSampling = false;
#endif
        if (asynchronousCpuSampling) {
            asyncImprovementSampler =
                    std::make_unique<AsyncImprovementTimelineSampler>(
                            request,
                            replay,
                            identity,
                            control,
                            control->improvementTimelineSampled);
        }
        instrumentedControl.liveChanged =
                [&, downstreamLiveChanged](
                        const SearchLiveUpdate &live) {
                    const bool initialBaseline =
                            live.winnerSource ==
                                    SearchWinnerSource::Baseline &&
                            !sampledBaseline;
                    const bool improvedMutation =
                            live.winnerSource ==
                                    SearchWinnerSource::Mutation &&
                            live.mutationImprovementCount >
                                    sampledImprovementCount;
                    if (asyncImprovementSampler) {
                        downstreamLiveChanged(live);
                        if (!initialBaseline && !improvedMutation) {
                            return;
                        }
                        asyncImprovementSampler->Submit(live);
                        if (initialBaseline) {
                            sampledBaseline = true;
                        } else {
                            sampledImprovementCount =
                                    live.mutationImprovementCount;
                        }
                        return;
                    }
                    if (!initialBaseline && !improvedMutation) {
                        downstreamLiveChanged(live);
                        return;
                    }
                    if (!improvementSampler) {
                        improvementSampler =
                                std::make_unique<TimelineSamplingRuntime>(
                                        CreateTimelineSamplingRuntime(
                                                request,
                                                replay,
                                                identity));
                    }
                    SearchLiveUpdate enriched = live;
                    enriched.bestTimeline = SampleTimeline(
                            *improvementSampler,
                            live.bestInputs,
                            control,
                            false);
                    downstreamLiveChanged(enriched);
                    if (initialBaseline) {
                        sampledBaseline = true;
                    } else {
                        sampledImprovementCount =
                                live.mutationImprovementCount;
                    }
                };
        executionControl = &instrumentedControl;
    }

    SearchResult result = [&]() {
        try {
            const double searchStartedTimeSeconds =
                    std::chrono::duration<double>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
            SearchResult completed = search->Run({
                    sandbox,
                    kSearchTickDurationMs,
                    mutator,
                    *evaluator,
                    executionControl,
                    request.parallelSampleCount,
                    request.calibrateCudaParallelSampleCount,
                    request.useCudaSessionSpecialization,
                    cudaModifiers.empty() ? nullptr : &cudaModifiers,
                    cudaEvaluator ? &*cudaEvaluator : nullptr,
#if FOREVERVALIDATOR_HAS_CUDA
                    cudaWinnerWorker
                            ? [worker = cudaWinnerWorker.get(), control](
                                      const std::vector<
                                              PhysicsSandboxInputEvent>
                                              &inputs,
                                      std::uint32_t tick) {
                                  if (control != nullptr &&
                                      control->cudaWinnerResolved) {
                                      control->cudaWinnerResolved();
                                  }
                                  return worker->Resolve(inputs, tick);
                              }
                            : std::function<SearchExecutionContext::
                                      ResolvedCudaWinner(
                                              const std::vector<
                                                      PhysicsSandboxInputEvent>
                                                      &,
                                              std::uint32_t)>{},
#else
                    {},
#endif
                    request.simulationHorizonMs,
                    request.condition ? &*request.condition : nullptr,
                    searchStartedTimeSeconds
            });
            if (asyncImprovementSampler) {
                const std::exception_ptr failure =
                        asyncImprovementSampler->Stop(false);
                if (failure) {
                    std::rethrow_exception(failure);
                }
            }
            return completed;
        } catch (...) {
            if (asyncImprovementSampler) {
                static_cast<void>(
                        asyncImprovementSampler->Stop(true));
            }
            throw;
        }
    }();
    CheckCancellation(control);
    if (control == nullptr || control->sampleBestTimeline) {
        result.bestTimeline = SampleBestTimeline(
                request, replay, identity, result.bestInputs, control);
    }
    return result;
}

std::vector<forevervalidator::experimental::PhysicsSandboxInputEvent>
BuildBaselineOrThrow(
        const SearchRequest &request,
        const std::vector<
                forevervalidator::experimental::PhysicsSandboxInputEvent>
                &canonicalInputs) {
    InputScriptBaselineResult baseline = BuildInputScriptBaseline(
            canonicalInputs,
            request.baseInputCommands,
            kSearchTickDurationMs);
    if (!baseline) {
        throw std::invalid_argument(*baseline.error);
    }
    ConvertKeyboardSteeringToAnalog(baseline.events);
    return std::move(baseline.events);
}

struct CpuWorkerState {
    std::optional<SearchStatisticsUpdate> statistics;
    std::optional<SearchLiveUpdate> live;
    std::optional<SearchResult> result;
    std::exception_ptr failure;
    bool done = false;
};

bool BetterEvaluation(const IterationEvaluator &evaluator,
                      double candidateScore,
                      double candidateTimeMs,
                      std::size_t candidateInputCount,
                      double incumbentScore,
                      double incumbentTimeMs,
                      std::size_t incumbentInputCount) {
    return ImprovesSearchResult(
            evaluator,
            {candidateScore, candidateTimeMs, {}},
            candidateInputCount,
            {incumbentScore, incumbentTimeMs, {}},
            incumbentInputCount);
}

bool PreferEvaluation(
        const IterationEvaluator &evaluator,
        double candidateScore,
        double candidateTimeMs,
        std::size_t candidateInputCount,
        SearchWinnerSource candidateSource,
        std::optional<std::uint64_t> candidateIteration,
        double incumbentScore,
        double incumbentTimeMs,
        std::size_t incumbentInputCount,
        SearchWinnerSource incumbentSource,
        std::optional<std::uint64_t> incumbentIteration) {
    const EvaluationSample candidate{
            candidateScore, candidateTimeMs, {}};
    const EvaluationSample incumbent{
            incumbentScore, incumbentTimeMs, {}};
    if (evaluator.IsBetter(candidate, incumbent)) {
        return true;
    }
    if (candidateScore != incumbentScore) {
        return false;
    }
    if (candidateInputCount != incumbentInputCount) {
        return candidateInputCount < incumbentInputCount;
    }
    if (candidateSource != incumbentSource) {
        return candidateSource == SearchWinnerSource::Baseline;
    }
    return candidateIteration.value_or(0u) <
            incumbentIteration.value_or(0u);
}

SearchResult RunMultiThreadedCpuSearch(
        const SearchRequest &request,
        const SearchAlgorithmRegistration &searchRegistration,
        const EvaluationTargetRegistration &evaluationRegistration,
        const SearchRunControl *control) {
    CheckCancellation(control);
    if (request.parallelSampleCount == 0u ||
        request.parallelSampleCount > kMaximumCpuWorkerCount) {
        throw std::invalid_argument(
                "CPU worker count must be between 1 and " +
                std::to_string(kMaximumCpuWorkerCount));
    }
    if (request.searchAlgorithm.id != kBasicBruteForceSearchId) {
        throw std::invalid_argument(
                "multi-threaded CPU does not support search algorithm: " +
                request.searchAlgorithm.id);
    }

    const auto started = std::chrono::steady_clock::now();
    const std::uint32_t workerCount = request.parallelSampleCount;
    using namespace forevervalidator;
    using namespace forevervalidator::experimental;

    const ReplayIdentity identity{request.replayPath};
    ReportProgress(
            control, SearchProgressStage::OpeningPacksDirectory, 0u, 0u);
    AssetSource source = Require(
            OpenInstalledPackDirectory(request.packDirectory),
            "opening shared CPU pack directory");
    ReportProgress(control, SearchProgressStage::ReadingScenario, 0u, 0u);
    AssetBytes replay = Require(
            ReadReplayFileUtf8(request.replayPath, identity),
            "reading shared CPU replay");
    PhysicsSandboxOptions options = CanonicalOptions(
            request, SimulationBackend::OptimizedCpu);
    ReportProgress(control, SearchProgressStage::CreatingSimulation, 0u, 0u);
    PhysicsSandbox sourceSandbox = Require(
            CreatePhysicsSandbox(std::move(source), options),
            "creating shared CPU sandbox");
    ReportProgress(control, SearchProgressStage::LoadingScenario, 0u, 0u);
    Require(sourceSandbox.LoadScenario(
                    {replay.data(), replay.size()}, identity),
            "loading shared CPU scenario");
    const std::vector<SandboxInputEvent> canonicalInputs = Require(
            sourceSandbox.ReadInputs(),
            "reading shared CPU canonical inputs");
    ReportProgress(
            control, SearchProgressStage::ApplyingBaselineInputs, 0u, 0u);
    Require(sourceSandbox.ReplaceInputs(BuildBaselineOrThrow(
                    request,
                    canonicalInputs)),
            "applying shared CPU baseline");

    std::vector<PhysicsSandbox> workerSandboxes;
    workerSandboxes.reserve(workerCount);
    for (std::uint32_t workerIndex = 0u;
         workerIndex < workerCount; ++workerIndex) {
        workerSandboxes.push_back(Require(
                ClonePhysicsSandbox(sourceSandbox),
                "cloning shared CPU sandbox"));
        ReportProgress(control,
                       SearchProgressStage::PreparingSearch,
                       workerIndex + 1u,
                       workerCount);
    }
    std::unique_ptr<IterationEvaluator> evaluator =
            evaluationRegistration.create(
                    request.evaluationTarget.settings,
                    kSearchTickDurationMs);
    std::mutex evaluatorMutex;
    const auto betterShared =
            [&](double candidateScore,
                double candidateTimeMs,
                std::size_t candidateInputCount,
                double incumbentScore,
                double incumbentTimeMs,
                std::size_t incumbentInputCount) {
                std::lock_guard<std::mutex> guard(evaluatorMutex);
                return BetterEvaluation(
                        *evaluator,
                        candidateScore,
                        candidateTimeMs,
                        candidateInputCount,
                        incumbentScore,
                        incumbentTimeMs,
                        incumbentInputCount);
            };
    const auto preferShared =
            [&](double candidateScore,
                double candidateTimeMs,
                std::size_t candidateInputCount,
                SearchWinnerSource candidateSource,
                std::optional<std::uint64_t> candidateIteration,
                double incumbentScore,
                double incumbentTimeMs,
                std::size_t incumbentInputCount,
                SearchWinnerSource incumbentSource,
                std::optional<std::uint64_t> incumbentIteration) {
                std::lock_guard<std::mutex> guard(evaluatorMutex);
                return PreferEvaluation(
                        *evaluator,
                        candidateScore,
                        candidateTimeMs,
                        candidateInputCount,
                        candidateSource,
                        candidateIteration,
                        incumbentScore,
                        incumbentTimeMs,
                        incumbentInputCount,
                        incumbentSource,
                        incumbentIteration);
            };
    std::mutex stateMutex;
    std::mutex upstreamControlMutex;
    std::condition_variable stateChanged;
    std::vector<CpuWorkerState> states(workerCount);
    std::atomic_bool internalCancellation{false};
    const bool autoPromoteBest = *ParseBoolean(
            request.searchAlgorithm.settings.at(
                    "autoPromoteBest"));
    std::optional<EvaluationSample> promotedEvaluation;
    std::vector<SandboxInputEvent> promotedInputs;
    std::uint64_t revision = 0u;
    std::size_t finishedWorkerCount = 0u;
    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    try {
        for (std::uint32_t workerIndex = 0u;
             workerIndex < workerCount;
             ++workerIndex) {
            workers.emplace_back([&, workerIndex]() {
                try {
                    SearchRequest workerRequest = request;
                    workerRequest.backend = PhysicsBackend::OptimizedCpu;
                    workerRequest.parallelSampleCount = 1u;
                    workerRequest.calibrateCudaParallelSampleCount = false;

                    SearchRunControl workerControl;
                    if (control != nullptr) {
                        workerControl = *control;
                    }
                    const auto upstreamStop = workerControl.stopRequested;
                    workerControl.stopRequested =
                            [&, upstreamStop]() {
                                if (internalCancellation.load(
                                            std::memory_order_relaxed)) {
                                    return true;
                                }
                                std::lock_guard<std::mutex> guard(
                                        upstreamControlMutex);
                                return upstreamStop && upstreamStop();
                            };
                    const auto upstreamCancellation =
                            workerControl.cancellationRequested;
                    workerControl.cancellationRequested =
                            [&, upstreamCancellation]() {
                                if (internalCancellation.load(
                                            std::memory_order_relaxed)) {
                                    return true;
                                }
                                std::lock_guard<std::mutex> guard(
                                        upstreamControlMutex);
                                return upstreamCancellation &&
                                        upstreamCancellation();
                            };
                    const auto upstreamBegin =
                            workerControl.beginIteration;
                    workerControl.beginIteration =
                            [&, upstreamBegin]() {
                                std::lock_guard<std::mutex> guard(
                                        upstreamControlMutex);
                                return !upstreamBegin || upstreamBegin();
                            };
                    workerControl.progressChanged = {};
                    workerControl.cudaBatchSizeChanged = {};
                    workerControl.statisticsChanged =
                            [&, workerIndex](
                                    const SearchStatisticsUpdate &statistics) {
                                std::lock_guard<std::mutex> guard(stateMutex);
                                states[workerIndex].statistics = statistics;
                                ++revision;
                                stateChanged.notify_one();
                            };
                    workerControl.liveChanged =
                            [&, workerIndex](
                                    const SearchLiveUpdate &live) {
                                std::lock_guard<std::mutex> guard(
                                        stateMutex);
                                if (autoPromoteBest &&
                                    live.winnerSource ==
                                            SearchWinnerSource::Mutation &&
                                    (!promotedEvaluation ||
                                     betterShared(
                                             live.bestScore,
                                             live.bestEvaluationTimeMs,
                                             live.bestInputs.size(),
                                             promotedEvaluation->score,
                                             promotedEvaluation->timeMs,
                                             promotedInputs.size()))) {
                                    promotedEvaluation = {
                                            live.bestScore,
                                            live.bestEvaluationTimeMs,
                                            {}};
                                    promotedInputs =
                                            live.bestInputs;
                                }
                                states[workerIndex].live = live;
                                ++revision;
                                stateChanged.notify_one();
                            };
                    workerControl.promotedBaselineInputs =
                            [&]() -> std::optional<
                                    std::vector<SandboxInputEvent>> {
                                if (!autoPromoteBest) {
                                    return std::nullopt;
                                }
                                std::lock_guard<std::mutex> guard(
                                        stateMutex);
                                if (!promotedEvaluation) {
                                    return std::nullopt;
                                }
                                return promotedInputs;
                            };
                    if (control != nullptr && control->iterationLimit) {
                        const std::uint64_t total =
                                *control->iterationLimit;
                        workerControl.iterationLimit =
                                total / workerCount +
                                (workerIndex < total % workerCount
                                         ? 1u
                                         : 0u);
                    }
                    workerControl.iterationIndexOffset = workerIndex;
                    workerControl.iterationIndexStride = workerCount;
                    workerControl.sampleImprovementTimelines = false;
                    workerControl.sampleBestTimeline = false;
                    workerControl.reuseLoadedSandbox = false;

                    SearchResult result = RunLoadedSearch(
                            workerRequest,
                            replay,
                            identity,
                            workerSandboxes[workerIndex],
                            searchRegistration,
                            evaluationRegistration,
                            &workerControl);
                    std::lock_guard<std::mutex> guard(stateMutex);
                    states[workerIndex].result = std::move(result);
                    states[workerIndex].done = true;
                    ++finishedWorkerCount;
                    ++revision;
                    stateChanged.notify_one();
                } catch (...) {
                    internalCancellation.store(
                            true, std::memory_order_relaxed);
                    std::lock_guard<std::mutex> guard(stateMutex);
                    states[workerIndex].failure =
                            std::current_exception();
                    states[workerIndex].done = true;
                    ++finishedWorkerCount;
                    ++revision;
                    stateChanged.notify_one();
                }
            });
        }
    } catch (...) {
        internalCancellation.store(true, std::memory_order_relaxed);
        for (std::thread &worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        throw;
    }

    std::optional<SearchLiveUpdate> aggregateBest;
    std::uint64_t aggregateImprovementCount = 0u;
    std::optional<std::chrono::steady_clock::duration>
            lastImprovementElapsed;
    std::uint64_t publishedRevision = 0u;
    std::size_t completedWorkers = 0u;
    bool searchingReported = false;
    std::unique_ptr<TimelineSamplingRuntime> timelineSampler;
    auto nextReduction =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(100);
    try {
        while (completedWorkers < workerCount) {
            std::vector<std::optional<SearchStatisticsUpdate>>
                    statisticsUpdates;
            std::vector<std::optional<SearchLiveUpdate>> liveUpdates;
            {
                std::unique_lock<std::mutex> lock(stateMutex);
                stateChanged.wait_until(
                        lock,
                        nextReduction,
                        [&]() {
                            return finishedWorkerCount == workerCount;
                        });
                if (revision == publishedRevision) {
                    nextReduction =
                            std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(100);
                    continue;
                }
                publishedRevision = revision;
                completedWorkers = finishedWorkerCount;
                statisticsUpdates.reserve(states.size());
                liveUpdates.reserve(states.size());
                for (const CpuWorkerState &state : states) {
                    statisticsUpdates.push_back(state.statistics);
                    liveUpdates.push_back(state.live);
                }
                nextReduction =
                        std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(100);
            }

            std::uint64_t statisticsIterations = 0u;
            bool haveStatistics = false;
            for (const auto &statistics : statisticsUpdates) {
                if (!statistics) {
                    continue;
                }
                haveStatistics = true;
                statisticsIterations += statistics->iterations;
            }
            if (haveStatistics && control != nullptr &&
                control->statisticsChanged) {
                control->statisticsChanged({
                        statisticsIterations,
                        std::chrono::steady_clock::now() - started});
            }

            std::optional<SearchLiveUpdate> candidate;
            std::uint64_t iterations = 0u;
            std::uint64_t evaluatorCalls = 0u;
            std::uint64_t totalMutationCount = 0u;
            for (const auto &live : liveUpdates) {
                if (!live) {
                    continue;
                }
                iterations += live->iterations;
                evaluatorCalls += live->evaluatorCalls;
                totalMutationCount += live->totalMutationCount;
                if (!candidate ||
                    preferShared(
                            live->bestScore,
                            live->bestEvaluationTimeMs,
                            live->bestInputs.size(),
                            live->winnerSource,
                            live->winningIterationIndex,
                            candidate->bestScore,
                            candidate->bestEvaluationTimeMs,
                            candidate->bestInputs.size(),
                            candidate->winnerSource,
                            candidate->winningIterationIndex)) {
                    candidate = live;
                }
            }
            if (!candidate) {
                continue;
            }
            if (!searchingReported) {
                ReportProgress(
                        control,
                        SearchProgressStage::Mutations,
                        iterations,
                        0u);
                searchingReported = true;
            }

            bool improved = false;
            const bool improvesResult = aggregateBest &&
                    betterShared(
                        candidate->bestScore,
                        candidate->bestEvaluationTimeMs,
                        candidate->bestInputs.size(),
                        aggregateBest->bestScore,
                        aggregateBest->bestEvaluationTimeMs,
                        aggregateBest->bestInputs.size());
            if (!aggregateBest ||
                preferShared(
                        candidate->bestScore,
                        candidate->bestEvaluationTimeMs,
                        candidate->bestInputs.size(),
                        candidate->winnerSource,
                        candidate->winningIterationIndex,
                        aggregateBest->bestScore,
                        aggregateBest->bestEvaluationTimeMs,
                        aggregateBest->bestInputs.size(),
                        aggregateBest->winnerSource,
                        aggregateBest->winningIterationIndex)) {
                improved = candidate->winnerSource ==
                                SearchWinnerSource::Mutation &&
                        (!aggregateBest || improvesResult);
                aggregateBest = candidate;
                if (improved) {
                    ++aggregateImprovementCount;
                    lastImprovementElapsed =
                            std::chrono::steady_clock::now() - started;
                }
            }
            if (control != nullptr && control->liveChanged &&
                aggregateBest) {
                SearchLiveUpdate aggregate = *aggregateBest;
                if (improved &&
                    aggregate.winnerSource == SearchWinnerSource::Mutation) {
                    if (!timelineSampler) {
                        timelineSampler =
                                std::make_unique<TimelineSamplingRuntime>(
                                        CloneTimelineSamplingRuntime(
                                                sourceSandbox,
                                                request.simulationHorizonMs));
                    }
                    aggregate.bestTimeline = SampleTimeline(
                            *timelineSampler,
                            aggregate.bestInputs,
                            control,
                            false);
                } else {
                    aggregate.bestTimeline.clear();
                }
                aggregate.iterations = iterations;
                aggregate.evaluatorCalls = evaluatorCalls;
                aggregate.mutationImprovementCount =
                        aggregateImprovementCount;
                aggregate.totalMutationCount = totalMutationCount;
                aggregate.elapsed =
                        std::chrono::steady_clock::now() - started;
                aggregate.lastImprovementElapsed =
                        lastImprovementElapsed;
                control->liveChanged(aggregate);
            }
        }
    } catch (...) {
        internalCancellation.store(true, std::memory_order_relaxed);
        stateChanged.notify_all();
        for (std::thread &worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        throw;
    }

    for (std::thread &worker : workers) {
        worker.join();
    }
    CheckCancellation(control);

    for (const CpuWorkerState &state : states) {
        if (!state.failure) {
            continue;
        }
        try {
            std::rethrow_exception(state.failure);
        } catch (const SearchCancelled &) {
            continue;
        }
    }
    for (const CpuWorkerState &state : states) {
        if (state.failure) {
            std::rethrow_exception(state.failure);
        }
    }

    std::optional<std::size_t> bestWorker;
    std::uint64_t iterations = 0u;
    std::uint64_t evaluatorCalls = 0u;
    std::uint64_t totalMutationCount = 0u;
    for (std::size_t index = 0u; index < states.size(); ++index) {
        if (!states[index].result) {
            throw std::runtime_error(
                    "CPU worker completed without a search result");
        }
        const SearchResult &result = *states[index].result;
        iterations += result.iterations;
        evaluatorCalls += result.evaluatorCalls;
        totalMutationCount += result.totalMutationCount;
        if (!bestWorker ||
            preferShared(
                    result.bestScore,
                    result.bestEvaluationTimeMs,
                    result.bestInputs.size(),
                    result.winnerSource,
                    result.winningIterationIndex,
                    states[*bestWorker].result->bestScore,
                    states[*bestWorker].result
                            ->bestEvaluationTimeMs,
                    states[*bestWorker].result->bestInputs.size(),
                    states[*bestWorker].result->winnerSource,
                    states[*bestWorker].result
                            ->winningIterationIndex)) {
            bestWorker = index;
        }
    }

    SearchResult result =
            std::move(*states[*bestWorker].result);
    if (((!aggregateBest ||
         betterShared(
                 result.bestScore,
                 result.bestEvaluationTimeMs,
                 result.bestInputs.size(),
                 aggregateBest->bestScore,
                 aggregateBest->bestEvaluationTimeMs,
                 aggregateBest->bestInputs.size())) ||
         aggregateImprovementCount == 0u) &&
        result.winnerSource == SearchWinnerSource::Mutation) {
        ++aggregateImprovementCount;
        lastImprovementElapsed =
                std::chrono::steady_clock::now() - started;
    }
    result.iterations = iterations;
    result.evaluatorCalls = evaluatorCalls;
    result.mutationImprovementCount =
            aggregateImprovementCount;
    result.totalMutationCount = totalMutationCount;
    result.elapsed = std::chrono::steady_clock::now() - started;
    result.lastImprovementElapsed = lastImprovementElapsed;

    CheckCancellation(control);
    if (control == nullptr || control->sampleBestTimeline) {
        ReportProgress(
                control, SearchProgressStage::FinalSamplingSetup, 0u, 0u);
        if (!timelineSampler) {
            timelineSampler = std::make_unique<TimelineSamplingRuntime>(
                    CloneTimelineSamplingRuntime(
                            sourceSandbox, request.simulationHorizonMs));
        }
        result.bestTimeline = SampleTimeline(
                *timelineSampler,
                result.bestInputs,
                control,
                true);
    }
    return result;
}

class VisualPhysicsHost final : public blocks::VisualSimulationHost {
public:
    using Snapshot=blocks::VisualPhysicsSnapshot;
    using Sandbox = forevervalidator::experimental::PhysicsSandbox;
    using Factory = std::function<Sandbox()>;
    struct HistoryReplay {
        Factory factory;
        std::mutex mutex;
        std::unique_ptr<Sandbox> sandbox;
        explicit HistoryReplay(Factory factory) : factory(std::move(factory)) {}
        std::vector<blocks::VisualState> sample(const forevervalidator::experimental::PhysicsSandboxState &origin,
                const blocks::VisualInputs &inputs,std::uint32_t skip,std::uint32_t ticks,std::uint32_t horizon) {
            std::lock_guard<std::mutex> lock(mutex);
            if (!sandbox) sandbox=std::make_unique<Sandbox>(factory());
            Require(sandbox->SetSimulationHorizonMs(std::max({horizon,static_cast<std::uint32_t>(origin.View().timeMs),
                static_cast<std::uint32_t>(Require(sandbox->ReadState(),"reading history cursor").timeMs)})),"setting history horizon");
            Require(sandbox->RestoreState(origin),"restoring history origin");
            Require(sandbox->SetSimulationHorizonMs(horizon),"setting history horizon");
            Require(sandbox->ReplaceInputs(inputs),"restoring history inputs");
            if (skip) Require(sandbox->AdvanceTicks(skip),"advancing to history span");
            std::vector<blocks::VisualState> states; states.reserve(ticks);
            for (std::uint32_t i=0;i<ticks;++i) states.push_back(Require(sandbox->AdvanceTicks(1),"sampling history"));
            return states;
        }
    };
    struct DeferredSnapshot final : blocks::VisualHostSnapshot {
        forevervalidator::experimental::PhysicsSandboxState origin;
        blocks::VisualInputs inputs;
        std::uint32_t ticks,horizon;
        std::shared_ptr<const unsigned char> identity;
        mutable std::once_flag once;
        mutable std::optional<forevervalidator::experimental::PhysicsSandboxState> resolved;
        DeferredSnapshot(forevervalidator::experimental::PhysicsSandboxState origin,blocks::VisualInputs inputs,
                std::uint32_t ticks,std::uint32_t horizon,std::shared_ptr<const unsigned char> identity)
            : origin(std::move(origin)),inputs(std::move(inputs)),ticks(ticks),horizon(horizon),identity(std::move(identity)) {}
    };
    VisualPhysicsHost(Sandbox &sandbox, Factory factory, std::uint32_t horizon,bool sourceAcceleration=true,
            std::function<void(const std::string &)> sourceMode={})
        : sandbox_(sandbox), factory_(std::move(factory)), horizon_(horizon),historyReplay_(std::make_shared<HistoryReplay>(factory_)),
          sourceAcceleration_(sourceAcceleration),sourceMode_(std::move(sourceMode)) {}
    VisualPhysicsHost(std::unique_ptr<Sandbox> sandbox, Factory factory, std::uint32_t horizon,bool sourceAcceleration=true,
            std::function<void(const std::string &)> sourceMode={})
        : owned_(std::move(sandbox)), sandbox_(*owned_), factory_(std::move(factory)), horizon_(horizon),historyReplay_(std::make_shared<HistoryReplay>(factory_)),
          sourceAcceleration_(sourceAcceleration),sourceMode_(std::move(sourceMode)) {}
    ~VisualPhysicsHost() override {
        if (profile_) std::fprintf(stderr,"BLOCK_SOURCE_HOST_PROFILE advances=%llu advanceMilliseconds=%.6f captures=%llu captureMilliseconds=%.6f\n",
            static_cast<unsigned long long>(advanceCount_),std::chrono::duration<double,std::milli>(advanceTime_).count(),
            static_cast<unsigned long long>(captureCount_),std::chrono::duration<double,std::milli>(captureTime_).count());
    }
    std::unique_ptr<blocks::VisualSimulationHost> fork() const override {
#if FOREVERVALIDATOR_HAS_CUDA
        synchronizeCursor(false);
#endif
        // The engine has a prepared-scene clone for optimized CPU. Reference
        // and CUDA branches load the same scenario on the same backend.
        const bool prepared=sandbox_.Backend()==forevervalidator::SimulationBackend::OptimizedCpu;
        auto clone=prepared ? Require(ClonePhysicsSandbox(sandbox_),"cloning simulation branch") : factory_();
        if (!prepared) {
            Require(clone.SetSimulationHorizonMs(horizon_), "setting branch horizon");
            Require(clone.RestoreState(Require(sandbox_.CaptureState(), "capturing branch")), "restoring cloned branch");
        }
        auto result=std::make_unique<VisualPhysicsHost>(std::make_unique<Sandbox>(std::move(clone)), factory_, horizon_,sourceAcceleration_,sourceMode_);
        result->historyReplay_=historyReplay_;
        result->prefixes_=prefixes_;
        result->recordPrefixes_=false;
        return result;
    }
    blocks::VisualState read() const override {
#if FOREVERVALIDATOR_HAS_CUDA
        if (cudaCursorActive_) return cudaCursor_->read();
#endif
        return Require(sandbox_.ReadState(), "reading simulation state");
    }
    std::uint32_t workerCapacity() const override {
        // GPU programs use the batched executor. Source-interpreter fallback
        // must not load hundreds of independent GPU scenes/contexts.
        return sandbox_.Backend()==forevervalidator::SimulationBackend::Cuda ? 1u : 256u;
    }
    std::shared_ptr<blocks::VisualBatchExecutor> batchExecutor() override {
#if FOREVERVALIDATOR_HAS_CUDA
        if (sandbox_.Backend()==forevervalidator::SimulationBackend::Cuda) {
            synchronizeCursor();
            sourceCursorReported_=false;
            if (!executor_) {
                executor_=blocks::CreateCudaProgramExecutor(sandbox_,[replay=historyReplay_](const blocks::VisualSnapshot &base,
                        const blocks::VisualInputs &inputs,std::uint32_t ticks) {
                    const auto *physical=dynamic_cast<const Snapshot *>(base.native.get());
                    if (!physical) throw std::runtime_error("Missing CUDA history origin.");
                    return replay->sample(physical->state,inputs,0,ticks,base.horizonMs);
                },cudaPrefixes_);
            }
            return executor_;
        }
#endif
        return {};
    }
    blocks::VisualState advance() override {
        const auto started=profile_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const auto previous=recording_ && (recording_->ticks+1)%32==0 ? std::optional<blocks::VisualState>(read()) : std::nullopt;
        captured_.reset();
        identity_.reset();
        cacheEligible_=false;
        std::optional<blocks::VisualState> resident;
#if FOREVERVALIDATOR_HAS_CUDA
        const auto *cursorPolicy=std::getenv("FOREVERTAS_CUDA_SOURCE_CURSOR");
        if (sourceAcceleration_ && !sourceCursorDisabled_ && (!cursorPolicy || std::string(cursorPolicy)!="0") &&
                sandbox_.Backend()==forevervalidator::SimulationBackend::Cuda) {
            try {
                if (!cudaCursorActive_) {
                    if (cudaCursor_) cudaCursor_->reset(sandbox_);
                    else cudaCursor_=std::make_unique<blocks::CudaProgramPhysicsCursor>(sandbox_);
                    cudaCursorActive_=true;
                }
                if (auto advanced=cudaCursor_->advance(1)) {
                    resident=advanced->state;
                    if (!sourceCursorReported_ && sourceMode_) sourceMode_("CUDA resident physics with source block control");
                    sourceCursorReported_=true;
                }
            } catch (const std::bad_alloc &) {
            } catch (const blocks::BytecodeNeedsInterpreter &) {
            }
            if (!resident) {
                synchronizeCursor();
                cudaCursor_.reset();
                sourceCursorDisabled_=true;
                if (sourceMode_) sourceMode_("CUDA physics with source block control: resident cursor unavailable");
            }
        }
#endif
        auto state=resident ? *resident : Require(sandbox_.AdvanceTicks(1), "advancing simulation");
        recordSourceStep(state,previous);
#if FOREVERVALIDATOR_HAS_CUDA
        if (cudaPrefixes_) {
            if (cudaCursorActive_) cudaPrefixes_->observe(*cudaCursor_);
            else cudaPrefixes_->observe(sandbox_,state);
        }
#endif
        if (profile_) { ++advanceCount_; advanceTime_+=std::chrono::steady_clock::now()-started; }
        return state;
    }
    blocks::VisualAdvance advanceMany(std::uint32_t ticks) override {
        if (ticks<=1) return blocks::VisualSimulationHost::advanceMany(ticks);
#if FOREVERVALIDATOR_HAS_CUDA
        synchronizeCursor();
#endif
        recording_.reset();
        captured_.reset();
        const bool cacheable=cacheEligible_ && ticks>=8 && sandbox_.Backend()==forevervalidator::SimulationBackend::OptimizedCpu;
        const auto startIdentity=identity();
        const auto inputs=Require(sandbox_.ReadInputs(),"capturing history inputs");
        const auto origin=historyOrigin_ ? *historyOrigin_ : Require(sandbox_.CaptureState(),"capturing history origin");
        const auto firstTime=Require(sandbox_.ReadState(),"reading history cursor").timeMs;
        const auto prefixEnd=[](const blocks::VisualInputs &sequence,std::uint64_t endTime) {
            return std::upper_bound(sequence.begin(),sequence.end(),endTime,
                [](std::uint64_t time,const SandboxInputEvent &event) { return static_cast<std::int64_t>(time)<event.timeMs; });
        };
        auto selected=prefixes_.end();
        if (cacheable) for (auto it=prefixes_.begin();it!=prefixes_.end();++it) {
            const auto &entry=*it;
            if (entry.origin!=startIdentity || entry.ticks>ticks || entry.horizon!=horizon_ ||
                (selected!=prefixes_.end() && entry.ticks<=selected->ticks)) continue;
            // Pointer identity proves the complete starting physics state is
            // identical; compare every consumed input, not a lossy state hash.
            const auto endTime=entry.advanced.state.timeMs;
            if (!std::equal(inputs.begin(),prefixEnd(inputs,endTime),entry.inputs.begin(),prefixEnd(entry.inputs,endTime),SameInputEvent)) continue;
            selected=it;
        }
        std::uint32_t reusedTicks=0;
        if (selected!=prefixes_.end()) {
            const auto &entry=*selected;
            Require(sandbox_.RestoreState(entry.end->state),"restoring shared input prefix");
            Require(sandbox_.ReplaceInputs(inputs),"applying branch suffix");
            identity_=entry.end->identity;
            reusedTicks=entry.ticks;
            // Keep shared prefixes hot instead of evicting them behind every
            // novel suffix. The cache remains bounded and equality is exact.
            auto hit=std::move(*selected);
            prefixes_.erase(selected);
            prefixes_.push_back(std::move(hit));
            if (reusedTicks==ticks) return prefixes_.back().advanced;
        }
        const auto skip=static_cast<std::uint32_t>((firstTime-origin.View().timeMs)/kSearchTickDurationMs);
        const auto remaining=ticks-reusedTicks;
        const auto previous=remaining==1 ? Require(sandbox_.ReadState(),"reading previous simulation state") :
            Require(sandbox_.AdvanceTicks(remaining-1),"advancing simulation");
        const auto state=Require(sandbox_.AdvanceTicks(1),"advancing simulation");
#if FOREVERVALIDATOR_HAS_CUDA
        if (cudaPrefixes_) cudaPrefixes_->observe(sandbox_,state,&previous);
#endif
        identity_.reset();
        const auto horizon=horizon_;
        const auto history=blocks::DeferredVisualHistory([replay=historyReplay_,origin,inputs,ticks,horizon,skip] {
            return replay->sample(origin,inputs,skip,ticks,horizon);
        });
        blocks::VisualAdvance advanced{state,previous,history};
        if (cacheable) {
            if (prefixes_.size()==64) prefixes_.erase(prefixes_.begin());
            prefixes_.push_back({startIdentity,std::make_shared<Snapshot>(Require(sandbox_.CaptureState(),"caching input prefix"),identity()),
                inputs,ticks,horizon_,advanced});
        }
        // Learn one new checkpoint after a restored/shared prefix. Recording
        // every never-reused suffix would only spend memory and clone time.
        cacheEligible_=false;
        return advanced;
    }
    std::shared_ptr<const blocks::VisualHostSnapshot> capture() const override {
        if (captured_) return captured_;
        if (historyOrigin_ && sandbox_.Backend()==forevervalidator::SimulationBackend::OptimizedCpu) {
            const auto time=Require(sandbox_.ReadState(),"reading saved cursor").timeMs;
            const auto ticks=static_cast<std::uint32_t>((time-historyOrigin_->View().timeMs)/kSearchTickDurationMs);
            if (ticks && ticks<=4096)
                return captured_=std::make_shared<DeferredSnapshot>(*historyOrigin_,inputs(),ticks,horizon_,identity());
        }
        const auto started=profile_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        auto native=
#if FOREVERVALIDATOR_HAS_CUDA
            cudaCursorActive_ ? cudaCursor_->capture() :
#endif
            Require(sandbox_.CaptureState(), "saving simulation");
        captured_=std::make_shared<Snapshot>(std::move(native),identity());
        if (profile_) { ++captureCount_; captureTime_+=std::chrono::steady_clock::now()-started; }
        if (recordPrefixes_ && !recording_ && sandbox_.Backend()==forevervalidator::SimulationBackend::OptimizedCpu) {
            const auto &native=static_cast<const Snapshot &>(*captured_);
            recording_=Recording{native.state,native.identity,0};
        }
#if FOREVERVALIDATOR_HAS_CUDA
        if (sandbox_.Backend()==forevervalidator::SimulationBackend::Cuda) {
            if (!cudaPrefixes_) cudaPrefixes_=std::make_shared<blocks::CudaProgramPrefixCache>();
            cudaPrefixes_->start(static_cast<const Snapshot &>(*captured_),horizon_);
        }
#endif
        return captured_;
    }
    blocks::VisualState restore(const blocks::VisualHostSnapshot &snapshot) override {
#if FOREVERVALIDATOR_HAS_CUDA
        cudaCursorActive_=false;
#endif
        captured_.reset();
        recording_.reset();
        if (const auto *deferred=dynamic_cast<const DeferredSnapshot *>(&snapshot)) {
            // Inputs before the cursor are immutable. Replaying from the exact
            // native origin reconstructs the full state, not just public fields.
            std::call_once(deferred->once,[&] {
                Require(sandbox_.RestoreState(deferred->origin),"restoring deferred origin");
                Require(sandbox_.SetSimulationHorizonMs(deferred->horizon),"setting deferred horizon");
                Require(sandbox_.ReplaceInputs(deferred->inputs),"restoring deferred inputs");
                Require(sandbox_.AdvanceTicks(deferred->ticks),"reconstructing deferred snapshot");
                deferred->resolved=Require(sandbox_.CaptureState(),"capturing deferred snapshot");
            });
            const auto state=Require(sandbox_.RestoreState(*deferred->resolved),"restoring deferred snapshot");
            identity_=deferred->identity;
            historyOrigin_=*deferred->resolved;
            cacheEligible_=true;
            if (recordPrefixes_) recording_=Recording{*deferred->resolved,deferred->identity,0};
            return state;
        }
        const auto *native = dynamic_cast<const Snapshot *>(&snapshot);
        if (!native) throw std::invalid_argument("Snapshot belongs to another simulation host.");
        const auto state=Require(sandbox_.RestoreState(native->state), "restoring simulation");
#if FOREVERVALIDATOR_HAS_CUDA
        if (cudaPrefixes_) cudaPrefixes_->start(*native,horizon_,true);
#endif
        identity_=native->identity;
        historyOrigin_=native->state;
        cacheEligible_=true;
        if (recordPrefixes_ && sandbox_.Backend()==forevervalidator::SimulationBackend::OptimizedCpu)
            recording_=Recording{native->state,native->identity,0};
        return state;
    }
    blocks::VisualInputs inputs() const override { return Require(sandbox_.ReadInputs(), "reading inputs"); }
    void replaceInputs(blocks::VisualInputs inputs) override {
#if FOREVERVALIDATOR_HAS_CUDA
        synchronizeCursor(false);
#endif
        captured_.reset();
        Require(sandbox_.ReplaceInputs(std::move(inputs)), "replacing inputs");
#if FOREVERVALIDATOR_HAS_CUDA
        if (cudaCursorActive_) {
            cudaCursorActive_=false;
            try {
                cudaCursor_->replaceInputs(sandbox_);
                cudaCursorActive_=true;
                return;
            } catch (const std::bad_alloc &) {
            } catch (const blocks::BytecodeNeedsInterpreter &) {
            }
            // The authoritative sandbox already contains the validated edit.
            cudaCursor_.reset();
            sourceCursorDisabled_=true;
            if (sourceMode_) sourceMode_("CUDA physics with source block control: resident cursor unavailable");
        }
#endif
    }
    blocks::VisualState setHorizon(std::uint32_t milliseconds) override {
        if (milliseconds==horizon_) return read();
#if FOREVERVALIDATOR_HAS_CUDA
        synchronizeCursor();
#endif
        captured_.reset();
        recording_.reset();
        const auto state=Require(sandbox_.SetSimulationHorizonMs(milliseconds), "setting simulation horizon");
#if FOREVERVALIDATOR_HAS_CUDA
        if (cudaPrefixes_) cudaPrefixes_->invalidate();
#endif
        horizon_=milliseconds;
        identity_.reset();
        return state;
    }
private:
#if FOREVERVALIDATOR_HAS_CUDA
    void synchronizeCursor(bool discard=true) const {
        if (!cudaCursorActive_) return;
        Require(sandbox_.RestoreState(cudaCursor_->capture()),"synchronizing resident CUDA physics");
        if (discard) cudaCursorActive_=false;
    }
#endif
    std::shared_ptr<const unsigned char> identity() const {
        if (!identity_) identity_=std::make_shared<const unsigned char>(0);
        return identity_;
    }
    struct Prefix {
        std::shared_ptr<const unsigned char> origin;
        std::shared_ptr<const Snapshot> end;
        blocks::VisualInputs inputs;
        std::uint32_t ticks,horizon;
        blocks::VisualAdvance advanced;
    };
    struct Recording {
        forevervalidator::experimental::PhysicsSandboxState origin;
        std::shared_ptr<const unsigned char> identity;
        std::uint32_t ticks;
    };
    void recordSourceStep(const blocks::VisualState &state,const std::optional<blocks::VisualState> &previous) try {
        if (!recording_) return;
        if (state.timeMs!=recording_->origin.View().timeMs+(++recording_->ticks)*kSearchTickDurationMs) {
            recording_.reset(); return;
        }
        if (recording_->ticks%32!=0 || !previous) return;
        auto inputs=sandbox_.ReadInputs();
        auto saved=sandbox_.CaptureState();
        if (!inputs || !saved) { recording_.reset(); return; }
        const auto end=std::make_shared<Snapshot>(std::move(saved.Value()),identity());
        const auto history=blocks::DeferredVisualHistory([replay=historyReplay_,origin=recording_->origin,
                events=inputs.Value(),ticks=recording_->ticks,horizon=horizon_] { return replay->sample(origin,events,0,ticks,horizon); });
        Prefix prefix{recording_->identity,end,std::move(inputs.Value()),recording_->ticks,horizon_,{state,*previous,history}};
        if (prefixes_.size()==64) prefixes_.erase(prefixes_.begin());
        prefixes_.push_back(std::move(prefix));
        captured_=end;
        if (recording_->ticks==128) recording_=Recording{end->state,end->identity,0};
    } catch (const std::bad_alloc &) {
        recording_.reset();
    }
    mutable std::shared_ptr<const unsigned char> identity_;
    const bool profile_=std::getenv("FOREVERTAS_CUDA_VM_PROFILE")!=nullptr;
    std::uint64_t advanceCount_=0;
    mutable std::uint64_t captureCount_=0;
    std::chrono::steady_clock::duration advanceTime_{};
    mutable std::chrono::steady_clock::duration captureTime_{};
    // Repeated maps can share an unchanged native origin. Every sandbox
    // mutation invalidates this cache, including future-only input edits.
    mutable std::shared_ptr<const blocks::VisualHostSnapshot> captured_;
    mutable std::optional<Recording> recording_;
    bool recordPrefixes_=true;
    std::optional<forevervalidator::experimental::PhysicsSandboxState> historyOrigin_;
    bool cacheEligible_=true;
    std::vector<Prefix> prefixes_;
    std::unique_ptr<Sandbox> owned_;
    forevervalidator::experimental::PhysicsSandbox &sandbox_;
    Factory factory_;
    std::uint32_t horizon_;
    std::shared_ptr<HistoryReplay> historyReplay_;
    std::shared_ptr<blocks::VisualBatchExecutor> executor_;
    bool sourceAcceleration_=true;
    std::function<void(const std::string &)> sourceMode_;
#if FOREVERVALIDATOR_HAS_CUDA
    mutable std::shared_ptr<blocks::CudaProgramPrefixCache> cudaPrefixes_;
    mutable std::unique_ptr<blocks::CudaProgramPhysicsCursor> cudaCursor_;
    mutable bool cudaCursorActive_=false;
    bool sourceCursorDisabled_=false,sourceCursorReported_=false;
#endif
};

std::vector<SearchTimelineFrame> VisualTimeline(const blocks::VisualSnapshot &snapshot) {
    std::vector<SearchTimelineFrame> frames;
    const auto history = blocks::VisualHistoryStates(snapshot.history);
    frames.reserve(history.size());
    for (const auto &state : history) frames.push_back(ToTimelineFrame(state));
    return frames;
}

SearchResult RunVisualSearch(const SearchRequest &request, const SearchRunControl *control) {
    using namespace forevervalidator;
    using namespace forevervalidator::experimental;
    const auto validation = blocks::ValidateExecutableVisualProgram(*request.executable);
    if (!validation.ok) throw std::invalid_argument(validation.errors.front());
    if (request.simulationHorizonMs < kSearchTickDurationMs || request.simulationHorizonMs > kMaximumSimulationHorizonMs ||
        request.simulationHorizonMs % kSearchTickDurationMs)
        throw std::invalid_argument("Invalid block program simulation horizon.");
    CheckCancellation(control);
    ReportProgress(control, SearchProgressStage::OpeningPacksDirectory, 0, 0);
    auto source = Require(OpenInstalledPackDirectory(request.packDirectory), "opening packs");
    CheckCancellation(control);
    ReportProgress(control, SearchProgressStage::ReadingScenario, 0, 0);
    const ReplayIdentity identity{request.replayPath};
    auto replay = Require(ReadReplayFileUtf8(request.replayPath, identity), "reading scenario");
    ReportProgress(control, SearchProgressStage::CreatingSimulation, 0, 0);
    auto sandbox = Require(CreatePhysicsSandbox(std::move(source),
        CanonicalOptions(request, ToForeverValidatorBackend(request.backend))), "creating simulation");
    CheckCancellation(control);
    ReportProgress(control, SearchProgressStage::LoadingScenario, 0, 0);
    Require(sandbox.LoadScenario({replay.data(), replay.size()}, identity), "loading scenario");
    const auto initialInputs=Require(sandbox.ReadInputs(), "reading scenario inputs");
    ReportProgress(control, SearchProgressStage::ApplyingBaselineInputs, 0, 0);
    Require(sandbox.ReplaceInputs(BuildBaselineOrThrow(request,initialInputs)), "applying starting inputs");
    CheckCancellation(control);
    if (control && control->beginIteration && !control->beginIteration()) throw SearchCancelled();
    ReportProgress(control, SearchProgressStage::VisualProgram, 0, 0);
    VisualPhysicsHost host(sandbox, [request,replay,identity] {
        auto branch = Require(CreatePhysicsSandbox(
            Require(OpenInstalledPackDirectory(request.packDirectory), "opening branch packs"),
            CanonicalOptions(request, ToForeverValidatorBackend(request.backend))), "creating branch");
        Require(branch.LoadScenario({replay.data(), replay.size()}, identity), "loading branch scenario");
        return branch;
    }, request.simulationHorizonMs,!control || control->compileVisualPrograms,
        control ? control->visualExecutionModeChanged : std::function<void(const std::string &)>{});
    blocks::VisualRuntimeControl runtime;
    runtime.horizonMs = request.simulationHorizonMs;
    runtime.tickMs = kSearchTickDurationMs;
    runtime.workerCount = request.backend == PhysicsBackend::Reference || request.backend == PhysicsBackend::OptimizedCpu
            ? 1u : request.parallelSampleCount;
    runtime.batchSize = std::min(1024u,runtime.workerCount * 32u);
#if FOREVERVALIDATOR_HAS_CUDA
    if (request.backend==PhysicsBackend::Cuda) runtime.batchSize=1024u;
#endif
    if (request.backend==PhysicsBackend::Reference) runtime.batchSize=1;
    if (request.backend==PhysicsBackend::OptimizedCpu) runtime.batchSize=1;
    runtime.debugger = request.debugger;
    if (control) {
        runtime.candidateLimit = control->iterationLimit;
        runtime.compilePrograms = control->compileVisualPrograms;
        runtime.executionModeChanged = control->visualExecutionModeChanged;
    }
    const auto started = std::chrono::steady_clock::now();
    auto lastStatistics = started;
    std::mutex statisticsMutex;
    std::optional<std::chrono::steady_clock::duration> lastPublication;
    std::uint64_t publications = 0;
    std::atomic<std::uint64_t> candidates{0};
    runtime.candidateCountChanged = [&](std::uint64_t count) { candidates.store(count,std::memory_order_relaxed); };
    runtime.stopRequested = [&] {
        CheckCancellation(control);
        return control && control->stopRequested && control->stopRequested();
    };
    runtime.progress = [&] {
        std::lock_guard<std::mutex> lock(statisticsMutex);
        const auto now = std::chrono::steady_clock::now();
        if (control && control->statisticsChanged && now-lastStatistics >= std::chrono::milliseconds(100)) {
            control->statisticsChanged({candidates.load(std::memory_order_relaxed), now-started});
            lastStatistics = now;
        }
    };
    runtime.published = [&](const blocks::VisualPublishedRun &published) {
        ++publications;
        lastPublication = std::chrono::steady_clock::now()-started;
        if (!control || !control->liveChanged) return;
        SearchLiveUpdate live;
        live.winnerSource = SearchWinnerSource::Program;
        if (published.candidate) live.winningIterationIndex = published.candidate-1;
        live.bestScore = published.score;
        live.bestState = published.evaluationState;
        live.bestEvaluationTimeMs = static_cast<double>(published.evaluationState.timeMs);
        live.bestEvaluationDescription = "Program-selected score: " + blocks::FormatNumberValue(published.score);
        live.bestInputs = *published.snapshot->inputs;
        live.iterations = candidates.load(std::memory_order_relaxed);
        live.evaluatorCalls = publications;
        live.mutationImprovementCount = publications;
        live.elapsed = *lastPublication;
        live.lastImprovementElapsed = lastPublication;
        if (control->sampleImprovementTimelines) live.bestTimeline = VisualTimeline(*published.snapshot);
        control->liveChanged(live);
    };
    const auto execution = blocks::ExecuteVisualProgram(*request.executable, host, runtime);
    CheckCancellation(control);
    const auto selected = execution.published ? execution.published->snapshot : execution.finalSnapshot;
    const auto evaluationState = execution.published ? execution.published->evaluationState : selected->state;
    const double score = execution.published ? execution.published->score : 0;
    const auto elapsed = std::chrono::steady_clock::now()-started;
    if (control && control->statisticsChanged) control->statisticsChanged({execution.candidates, elapsed});
    auto timeline = !control || control->sampleBestTimeline ? VisualTimeline(*selected) : std::vector<SearchTimelineFrame>{};
    std::optional<PhysicsSandboxState> physical;
    if (const auto *native=dynamic_cast<const VisualPhysicsHost::Snapshot *>(selected->native.get())) physical=native->state;
    else if (const auto *deferred=dynamic_cast<const VisualPhysicsHost::DeferredSnapshot *>(selected->native.get())) {
        host.setHorizon(std::max(selected->horizonMs,static_cast<std::uint32_t>(host.read().timeMs)));
        host.restore(*deferred);
        physical=*deferred->resolved;
    }
    if (!physical) throw std::runtime_error("Missing physical snapshot after block execution.");
    return {SearchWinnerSource::Program,
            execution.published && execution.published->candidate
                ? std::optional<std::uint64_t>(execution.published->candidate-1) : std::nullopt,
            0, score, static_cast<double>(evaluationState.timeMs),
            execution.published ? "Program-selected score: " + blocks::FormatNumberValue(score)
                                : "Program ended without keeping a result; showing its final simulation state.",
            evaluationState, *selected->inputs, std::move(timeline), execution.candidates,
            publications, publications, 0, elapsed, lastPublication, *physical};
}

}  // namespace

SearchResult RunSearch(const SearchRequest &request,
                       const SearchRunControl *control) {
    using namespace forevervalidator;
    using namespace forevervalidator::experimental;

    if (request.executable) return RunVisualSearch(request, control);

    const SearchAlgorithmRegistration *const searchRegistration =
            FindSearchAlgorithm(request.searchAlgorithm.id);
    const EvaluationTargetRegistration *const evaluationRegistration =
            FindEvaluationTarget(request.evaluationTarget.id);
    if (searchRegistration == nullptr) {
        throw std::invalid_argument("unknown search algorithm: " +
                                    request.searchAlgorithm.id);
    }
    if (request.modifiers.empty()) {
        throw std::invalid_argument(
                "modifier pipeline must contain at least one pass");
    }
    if (evaluationRegistration == nullptr) {
        throw std::invalid_argument("unknown evaluation target: " +
                                    request.evaluationTarget.id);
    }
    if (request.simulationHorizonMs < kSearchTickDurationMs ||
        request.simulationHorizonMs > kMaximumSimulationHorizonMs ||
        request.simulationHorizonMs % kSearchTickDurationMs != 0u) {
        throw std::invalid_argument(
                "Simulation horizon must be between 10 and " +
                std::to_string(kMaximumSimulationHorizonMs) +
                " ms and aligned to 10 ms");
    }
    if (const auto error = searchRegistration->validateSettings(
                request.searchAlgorithm.settings, kSearchTickDurationMs)) {
        throw std::invalid_argument(*error);
    }
    if (const auto error = evaluationRegistration->validateSettings(
                request.evaluationTarget.settings, kSearchTickDurationMs)) {
        throw std::invalid_argument(*error);
    }
    std::int64_t earliestMutationTimeMs =
            std::numeric_limits<std::int64_t>::max();
    std::vector<OptionConfiguration> executionModifiers;
    executionModifiers.reserve(request.modifiers.size());
    for (const OptionConfiguration &modifier : request.modifiers) {
        const ModifierRegistration *const registration =
                FindModifier(modifier.id);
        if (registration == nullptr) {
            throw std::invalid_argument("unknown modifier: " + modifier.id);
        }
        if (const auto error = registration->validateSettings(
                    modifier.settings, kSearchTickDurationMs)) {
            throw std::invalid_argument(*error);
        }
        OptionSettings executionSettings =
                ClampInputWindowToSimulationHorizon(
                        modifier.settings,
                        kSearchTickDurationMs,
                        request.simulationHorizonMs);
        const std::unique_ptr<InputMutator> mutator = registration->create(
                executionSettings, kSearchTickDurationMs);
        earliestMutationTimeMs = std::min(
                earliestMutationTimeMs,
                mutator->EarliestMutationTimeMs());
        executionModifiers.push_back(
                {registration->id, std::move(executionSettings)});
    }
    const std::unique_ptr<IterationEvaluator> evaluator =
            evaluationRegistration->create(
                    request.evaluationTarget.settings,
                    kSearchTickDurationMs);
    const EvaluationPlan evaluationPlan = evaluator->Plan(
            request.simulationHorizonMs,
            earliestMutationTimeMs,
            kSearchTickDurationMs);
    if (evaluationPlan.startTimeMs < earliestMutationTimeMs ||
        evaluationPlan.endTimeMs < evaluationPlan.startTimeMs ||
        evaluationPlan.endTimeMs > request.simulationHorizonMs ||
        evaluationPlan.startTimeMs % kSearchTickDurationMs != 0 ||
        evaluationPlan.endTimeMs % kSearchTickDurationMs != 0) {
        throw std::invalid_argument(
                "evaluation plan [" +
                std::to_string(evaluationPlan.startTimeMs) + ", " +
                std::to_string(evaluationPlan.endTimeMs) +
                "] ms does not fit the Simulation horizon of " +
                std::to_string(request.simulationHorizonMs) + " ms");
    }

    SearchRequest executionRequest = request;
    executionRequest.modifiers = std::move(executionModifiers);

    if (request.backend == PhysicsBackend::MultiThreadedCpu) {
        return RunMultiThreadedCpuSearch(
                executionRequest,
                *searchRegistration,
                *evaluationRegistration,
                control);
    }

    CheckCancellation(control);
    const ReplayIdentity identity{request.replayPath};
    PhysicsSandboxOptions options = CanonicalOptions(
            request, ToForeverValidatorBackend(request.backend));
#if FOREVERVALIDATOR_HAS_CUDA
    options.prepareCudaSearchSpecialization =
            request.backend == PhysicsBackend::Cuda &&
            request.useCudaSessionSpecialization;
#endif
    if (control != nullptr && control->reuseLoadedSandbox) {
        const std::shared_ptr<CachedSearchSandbox> cached =
                CachedSandboxFor(request);
        std::lock_guard<std::mutex> guard(cached->lock);
        CheckCancellation(control);
        if (!cached->sandbox) {
            ReportProgress(
                    control,
                    SearchProgressStage::OpeningPacksDirectory,
                    0u,
                    0u);
            AssetSource source = Require(
                    OpenInstalledPackDirectory(request.packDirectory),
                    "opening cached pack directory");
            ReportProgress(
                    control, SearchProgressStage::ReadingScenario, 0u, 0u);
            cached->replay = Require(
                    ReadReplayFileUtf8(request.replayPath, identity),
                    "reading cached replay");
            ReportProgress(
                    control,
                    SearchProgressStage::CreatingSimulation,
                    0u,
                    0u);
            cached->sandbox.emplace(Require(
                    CreatePhysicsSandbox(std::move(source), options),
                    "creating cached sandbox"));
            ReportProgress(
                    control, SearchProgressStage::LoadingScenario, 0u, 0u);
            Require(
                    cached->sandbox->LoadScenario(
                            {cached->replay.data(),
                             cached->replay.size()},
                            identity),
                    "loading scenario into cached sandbox");
            cached->initialState = Require(
                    cached->sandbox->CaptureState(),
                    "capturing cached initial state");
            cached->initialInputs = Require(
                    cached->sandbox->ReadInputs(),
                    "reading cached initial inputs");
        } else {
            ReportProgress(
                    control,
                    SearchProgressStage::RestoringSimulation,
                    0u,
                    0u);
            Require(
                    cached->sandbox->RestoreState(
                            *cached->initialState),
                    "restoring cached initial state");
            Require(
                    cached->sandbox->ReplaceInputs(
                            cached->initialInputs),
                    "restoring cached initial inputs");
        }
        ReportProgress(
                control,
                SearchProgressStage::ApplyingBaselineInputs,
                0u,
                0u);
        Require(
                cached->sandbox->ReplaceInputs(BuildBaselineOrThrow(
                        request,
                        cached->initialInputs)),
                "applying base input script");
        CheckCancellation(control);
        return RunLoadedSearch(
                executionRequest,
                cached->replay,
                identity,
                *cached->sandbox,
                *searchRegistration,
                *evaluationRegistration,
                control);
    }

    ReportProgress(
            control, SearchProgressStage::OpeningPacksDirectory, 0u, 0u);
    AssetSource source = Require(
            OpenInstalledPackDirectory(request.packDirectory),
            "opening pack directory");
    CheckCancellation(control);
    ReportProgress(control, SearchProgressStage::ReadingScenario, 0u, 0u);
    AssetBytes replay = Require(
            ReadReplayFileUtf8(request.replayPath, identity),
            "reading replay");
    CheckCancellation(control);
    ReportProgress(
            control, SearchProgressStage::CreatingSimulation, 0u, 0u);
    PhysicsSandbox sandbox = Require(
            CreatePhysicsSandbox(std::move(source), options),
            "creating sandbox");
    CheckCancellation(control);
    ReportProgress(control, SearchProgressStage::LoadingScenario, 0u, 0u);
    Require(sandbox.LoadScenario({replay.data(), replay.size()}, identity),
            "loading scenario");
    const std::vector<PhysicsSandboxInputEvent> canonicalInputs = Require(
            sandbox.ReadInputs(), "reading canonical inputs");
    ReportProgress(
            control,
            SearchProgressStage::ApplyingBaselineInputs,
            0u,
            0u);
    Require(
            sandbox.ReplaceInputs(BuildBaselineOrThrow(
                    request,
                    canonicalInputs)),
            "applying base input script");
    CheckCancellation(control);
    return RunLoadedSearch(
            executionRequest,
            replay,
            identity,
            sandbox,
            *searchRegistration,
            *evaluationRegistration,
            control);
}

}  // namespace forevertas
