#include "mutations/input_event_formatter.h"
#include "mutations/input_event_utils.h"
#include "mutations/replay_input_script.h"
#include "replay_file_io.h"
#include "searches/basic_brute_force_search.h"
#include "searches/search_runner.h"

#include <forevervalidator/native.h>
#if FOREVERVALIDATOR_HAS_CUDA
#include "simulation/backends/cuda/cuda_backend.h"
#include "simulation/backends/cuda/cuda_session_specialization.h"
#endif

#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using forevervalidator::experimental::PhysicsSandbox;
using forevervalidator::experimental::PhysicsSandboxInputEvent;
using forevervalidator::experimental::PhysicsSandboxInputValueKind;
using forevervalidator::experimental::PhysicsSandboxStateView;

class ScopedTemporaryDirectory final {
public:
    ScopedTemporaryDirectory() {
        namespace fs = std::filesystem;
        const std::string stem =
                "forevertas-search-cache-" +
                std::to_string(
                        std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count()) +
                "-";
        for (std::uint32_t attempt = 0u; attempt < 100u; ++attempt) {
            std::error_code error;
            const fs::path candidate = fs::temp_directory_path() /
                    fs::u8path(stem + std::to_string(attempt));
            if (fs::create_directory(candidate, error)) {
                path_ = candidate;
                return;
            }
            if (error && error != std::errc::file_exists) {
                throw std::runtime_error(
                        "could not create temporary cache test directory: " +
                        error.message());
            }
        }
        throw std::runtime_error(
                "could not reserve a unique cache test directory");
    }

    ~ScopedTemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path &Path() const { return path_; }

private:
    std::filesystem::path path_;
};

class ScopedClogCapture final {
public:
    ScopedClogCapture() : previous_(std::clog.rdbuf(output_.rdbuf())) {}

    ~ScopedClogCapture() { std::clog.rdbuf(previous_); }

    std::string Text() const { return output_.str(); }

private:
    std::ostringstream output_;
    std::streambuf *previous_;
};

class IncrementingSteerMutator final : public forevertas::InputMutator {
public:
    forevertas::MutationResult Mutate(
            const forevertas::MutationRequest &request) const override {
        const forevertas::AnalogInputState baseline =
                forevertas::SteeringStateAt(
                        request.baselineInputs, 10);
        observedBaselines.push_back(baseline);
        std::vector<forevertas::SandboxInputEvent> inputs =
                request.baselineInputs;
        PhysicsSandboxInputEvent event;
        event.timeMs = 10;
        event.action = forevertas::SandboxInputAction::Steer;
        event.value.kind = PhysicsSandboxInputValueKind::Analog;
        event.value.analog = forevertas::SaturateAnalogInputState(
                static_cast<std::int64_t>(baseline) + 4096);
        inputs.push_back(event);
        forevertas::NormalizeMutableInputEvents(
                inputs,
                request.baselineInputs,
                request.tickDurationMs,
                request.mutableFromTimeMs);
        return {
                inputs,
                forevertas::EffectiveInputChangeCount(
                        request.baselineInputs, inputs)};
    }

    std::int64_t EarliestMutationTimeMs() const override {
        return 10;
    }

    forevertas::MutationTimeRange AffectedTimeRange() const override {
        return {10, 10};
    }

    mutable std::vector<forevertas::AnalogInputState>
            observedBaselines;
};

class OversizedWindowMutator final : public forevertas::InputMutator {
public:
    forevertas::MutationResult Mutate(
            const forevertas::MutationRequest &) const override {
        forevertas::MutationWindowPatch patch;
        patch.minimumTimeMs = 10;
        patch.maximumTimeMs = 100000;
        PhysicsSandboxInputEvent inReplay;
        inReplay.timeMs = 10;
        inReplay.action = forevertas::SandboxInputAction::Steer;
        inReplay.value.kind = PhysicsSandboxInputValueKind::Analog;
        inReplay.value.analog = 4096;
        PhysicsSandboxInputEvent beyondReplay = inReplay;
        beyondReplay.timeMs = 100000;
        beyondReplay.value.analog = 8192;
        patch.events = {inReplay, beyondReplay};
        return {{}, 2u, std::move(patch)};
    }

    std::int64_t EarliestMutationTimeMs() const override {
        return 10;
    }

    forevertas::MutationTimeRange AffectedTimeRange() const override {
        return {10, 100000};
    }
};

class SteeringSession final
    : public forevertas::IterationEvaluationSession {
public:
    std::optional<forevertas::EvaluationSample> Observe(
            const std::optional<PhysicsSandboxStateView> &,
            const PhysicsSandboxStateView &current) override {
        return forevertas::EvaluationSample{
                static_cast<double>(current.steering),
                static_cast<double>(current.timeMs),
                "steering"};
    }
};

class SteeringEvaluator final : public forevertas::IterationEvaluator {
public:
    forevertas::EvaluationPlan Plan(
            std::int64_t,
            std::int64_t,
            std::uint32_t) const override {
        return {10, 10};
    }

    std::unique_ptr<forevertas::IterationEvaluationSession>
    CreateSession() const override {
        return std::make_unique<SteeringSession>();
    }

    bool IsBetter(
            const forevertas::EvaluationSample &iteration,
            const forevertas::EvaluationSample &incumbent)
            const override {
        return iteration.score > incumbent.score;
    }
};

class NoSampleSession final
    : public forevertas::IterationEvaluationSession {
public:
    std::optional<forevertas::EvaluationSample> Observe(
            const std::optional<PhysicsSandboxStateView> &,
            const PhysicsSandboxStateView &) override {
        return std::nullopt;
    }
};

class NoSampleEvaluator final : public forevertas::IterationEvaluator {
public:
    forevertas::EvaluationPlan Plan(
            std::int64_t,
            std::int64_t,
            std::uint32_t) const override {
        return {10, 10};
    }

    std::unique_ptr<forevertas::IterationEvaluationSession>
    CreateSession() const override {
        return std::make_unique<NoSampleSession>();
    }

    bool IsBetter(
            const forevertas::EvaluationSample &,
            const forevertas::EvaluationSample &) const override {
        return false;
    }
};

template<typename T, typename Error>
T Require(forevervalidator::DiscriminatedResult<T, Error> result,
          const char *operation) {
    if (!result) {
        throw std::runtime_error(std::string(operation) + " failed");
    }
    return std::move(result).Value();
}

PhysicsSandbox CreateEmptyInputSandbox(
        const char *packsDirectory,
        const char *replayPath) {
    const forevervalidator::ReplayIdentity identity{replayPath};
    const forevervalidator::AssetBytes replay = Require(
            forevertas::ReadReplayFileUtf8(replayPath, identity),
            "reading replay for auto-promote test");
    forevervalidator::experimental::PhysicsSandboxOptions options;
    options.backend = forevervalidator::SimulationBackend::Reference;
    options.tickDurationMs = forevertas::kSearchTickDurationMs;
    options.timelineMode = forevervalidator::experimental::
            PhysicsSandboxTimelineMode::Canonical;
    options.simulationHorizonMs =
            forevertas::kDefaultSimulationHorizonMs;
    PhysicsSandbox sandbox = Require(
            forevervalidator::experimental::CreatePhysicsSandbox(
                    Require(
                            forevervalidator::OpenInstalledPackDirectory(
                                    packsDirectory),
                            "opening auto-promote Packs"),
                    options),
            "creating auto-promote sandbox");
    Require(sandbox.LoadReplay(
                    {replay.data(), replay.size()}, identity),
            "loading auto-promote replay");
    return sandbox;
}

bool CheckAutoPromoteSemantics(
        const char *packsDirectory,
        const char *replayPath) {
    SteeringEvaluator evaluator;
    forevertas::SearchRunControl control;
    control.iterationLimit = 3u;
    control.sampleBestTimeline = false;

    PhysicsSandbox fixedSandbox =
            CreateEmptyInputSandbox(packsDirectory, replayPath);
    IncrementingSteerMutator fixedMutator;
    const forevertas::SearchResult fixed =
            forevertas::BasicBruteForceSearch(false).Run(
                    {fixedSandbox,
                     forevertas::kSearchTickDurationMs,
                     fixedMutator,
                     evaluator,
                     &control});

    PhysicsSandbox promotedSandbox =
            CreateEmptyInputSandbox(packsDirectory, replayPath);
    IncrementingSteerMutator promotedMutator;
    const forevertas::SearchResult promoted =
            forevertas::BasicBruteForceSearch(true).Run(
                    {promotedSandbox,
                     forevertas::kSearchTickDurationMs,
                     promotedMutator,
                     evaluator,
                     &control});

    const std::vector<forevertas::AnalogInputState> fixedExpected{
            0, 0, 0};
    const std::vector<forevertas::AnalogInputState> promotedExpected{
            0, 4096, 8192};
    if (fixedMutator.observedBaselines != fixedExpected ||
        promotedMutator.observedBaselines != promotedExpected ||
        fixed.bestState.steering != 0.0625 ||
        promoted.bestState.steering != 0.1875 ||
        fixed.mutationImprovementCount != 1u ||
        promoted.mutationImprovementCount != 3u) {
        std::cerr
                << "auto-promote did not refine each mutation from the "
                   "current best inputs"
                << " fixed=";
        for (const auto value : fixedMutator.observedBaselines) {
            std::cerr << value << ",";
        }
        std::cerr << " promoted=";
        for (const auto value : promotedMutator.observedBaselines) {
            std::cerr << value << ",";
        }
        std::cerr << " states=" << fixed.bestState.steering << "/"
                  << promoted.bestState.steering
                  << " improvements="
                  << fixed.mutationImprovementCount << "/"
                  << promoted.mutationImprovementCount << '\n';
        return false;
    }
    return true;
}

bool CheckNoBestLiveReporting(
        const char *packsDirectory,
        const char *replayPath) {
    PhysicsSandbox sandbox =
            CreateEmptyInputSandbox(packsDirectory, replayPath);
    IncrementingSteerMutator mutator;
    NoSampleEvaluator evaluator;
    std::vector<forevertas::SearchLiveUpdate> liveUpdates;
    forevertas::SearchRunControl control;
    control.iterationLimit = 3u;
    control.sampleBestTimeline = false;
    control.liveChanged =
            [&](const forevertas::SearchLiveUpdate &live) {
                liveUpdates.push_back(live);
            };

    bool rejectedMissingBest = false;
    try {
        static_cast<void>(
                forevertas::BasicBruteForceSearch(false).Run(
                        {sandbox,
                         forevertas::kSearchTickDurationMs,
                         mutator,
                         evaluator,
                         &control}));
    } catch (const std::runtime_error &error) {
        rejectedMissingBest =
                std::string(error.what()) ==
                "no iteration satisfied the selected evaluation target";
    }

    std::uint64_t previousIterations = 0u;
    std::uint64_t previousEvaluatorCalls = 0u;
    std::uint64_t previousMutationCount = 0u;
    bool invalid = liveUpdates.empty();
    for (const forevertas::SearchLiveUpdate &live : liveUpdates) {
        invalid |= live.bestAvailable ||
                live.winnerSource !=
                        forevertas::SearchWinnerSource::Baseline ||
                live.winningIterationIndex.has_value() ||
                live.winningMutationCount != 0u ||
                live.bestScore != 0.0 ||
                live.bestEvaluationTimeMs != 0.0 ||
                !live.bestEvaluationDescription.empty() ||
                !live.bestInputs.empty() ||
                !live.bestTimeline.empty() ||
                live.iterations < previousIterations ||
                live.evaluatorCalls < previousEvaluatorCalls ||
                live.totalMutationCount < previousMutationCount ||
                live.mutationImprovementCount != 0u ||
                live.lastImprovementElapsed.has_value() ||
                live.qualifyingCandidateCount != 0u ||
                live.closestTargetDistance.has_value();
        previousIterations = live.iterations;
        previousEvaluatorCalls = live.evaluatorCalls;
        previousMutationCount = live.totalMutationCount;
    }
    if (!rejectedMissingBest || invalid ||
        previousIterations != 3u ||
        previousEvaluatorCalls != 4u ||
        previousMutationCount != 3u) {
        std::cerr
                << "CPU no-best live reporting did not preserve safe fields "
                   "and monotonic activity counters\n";
        return false;
    }
    return true;
}

bool CheckNoBestRunnerReporting(
        const char *packsDirectory,
        const char *replayPath) {
    const forevertas::InputScriptParseResult parsed =
            forevertas::ParseInputScript(
                    forevertas::ExtractReplayInputScript(
                            packsDirectory, replayPath));
    if (!parsed) {
        throw std::runtime_error(*parsed.error);
    }
    const forevertas::ConditionCompileResult condition =
            forevertas::CompileConditionScript("iterations < 0");
    if (condition.error || !condition.program) {
        throw std::runtime_error(
                condition.error.value_or(
                        "missing compiled no-best condition"));
    }

    const auto checkBackend =
            [&](forevertas::PhysicsBackend backend,
                std::uint32_t workerCount) {
                forevertas::SearchRequest request{
                        packsDirectory, replayPath};
                request.backend = backend;
                request.parallelSampleCount = workerCount;
                request.baseInputCommands = parsed.commands;
                request.condition = condition.program;

                const std::thread::id callerThread =
                        std::this_thread::get_id();
                std::uint64_t previousIterations = 0u;
                std::uint64_t previousEvaluatorCalls = 0u;
                std::uint64_t previousMutationCount = 0u;
                bool sawActivity = false;
                bool invalid = false;
                std::size_t liveUpdateCount = 0u;
                forevertas::SearchRunControl control;
                control.iterationLimit = 4u;
                control.evaluationEndTimeLimitMs = 1020;
                control.sampleBestTimeline = false;
                control.liveChanged =
                        [&](const forevertas::SearchLiveUpdate &live) {
                            invalid |=
                                    std::this_thread::get_id() !=
                                            callerThread ||
                                    live.bestAvailable ||
                                    !live.bestTimeline.empty() ||
                                    !live.bestInputs.empty() ||
                                    !live.bestEvaluationDescription.empty() ||
                                    live.iterations < previousIterations ||
                                    live.evaluatorCalls <
                                            previousEvaluatorCalls ||
                                    live.totalMutationCount <
                                            previousMutationCount ||
                                    live.mutationImprovementCount != 0u ||
                                    live.qualifyingCandidateCount != 0u ||
                                    live.closestTargetDistance.has_value();
                            sawActivity |= live.iterations != 0u ||
                                    live.evaluatorCalls != 0u ||
                                    live.totalMutationCount != 0u;
                            previousIterations = live.iterations;
                            previousEvaluatorCalls =
                                    live.evaluatorCalls;
                            previousMutationCount =
                                    live.totalMutationCount;
                            ++liveUpdateCount;
                        };

                bool rejectedMissingBest = false;
                try {
                    static_cast<void>(
                            forevertas::RunSearch(request, &control));
                } catch (const std::runtime_error &error) {
                    rejectedMissingBest =
                            std::string(error.what()) ==
                            "no iteration satisfied the selected "
                            "evaluation target";
                }
                return rejectedMissingBest &&
                        liveUpdateCount != 0u && sawActivity && !invalid &&
                        (backend !=
                                 forevertas::PhysicsBackend::OptimizedCpu ||
                         previousIterations == 4u);
            };

    if (!checkBackend(forevertas::PhysicsBackend::OptimizedCpu, 1u) ||
        !checkBackend(forevertas::PhysicsBackend::MultiThreadedCpu, 2u)) {
        std::cerr
                << "search runner did not forward no-best activity without "
                   "sampling or selecting it\n";
        return false;
    }
    return true;
}

bool CheckModifierWindowClampedToHorizon(
        const char *packsDirectory,
        const char *replayPath) {
    PhysicsSandbox sandbox =
            CreateEmptyInputSandbox(packsDirectory, replayPath);
    OversizedWindowMutator mutator;
    SteeringEvaluator evaluator;
    forevertas::SearchRunControl control;
    control.iterationLimit = 1u;
    control.sampleBestTimeline = false;
    const forevertas::SearchResult result =
            forevertas::BasicBruteForceSearch(false).Run(
                    {sandbox,
                     forevertas::kSearchTickDurationMs,
                     mutator,
                     evaluator,
                     &control});
    return result.iterations == 1u;
}

#if FOREVERVALIDATOR_HAS_CUDA
bool CheckCudaKernelModeLifecycle(
        const char *packsDirectory,
        const char *replayPath) {
    const forevertas::InputScriptParseResult parsed =
            forevertas::ParseInputScript(
                    forevertas::ExtractReplayInputScript(
                            packsDirectory, replayPath));
    if (!parsed) {
        throw std::runtime_error(*parsed.error);
    }

    forevertas::SearchRequest request{packsDirectory, replayPath};
    request.baseInputCommands = parsed.commands;
    request.backend = forevertas::PhysicsBackend::Cuda;
    request.parallelSampleCount = 64u;
    request.useCudaSessionSpecialization = false;

    forevertas::SearchRunControl control;
    control.iterationLimit = 0u;
    control.sampleBestTimeline = false;
    control.reuseLoadedSandbox = true;

    const std::uint64_t before =
            forevervalidator::simulation::cuda::specialization::
                    SessionModuleBuildCountForTesting();
    const forevertas::SearchResult regular =
            forevertas::RunSearch(request, &control);
    const std::uint64_t afterMapLoad =
            forevervalidator::simulation::cuda::specialization::
                    SessionModuleBuildCountForTesting();

    request.useCudaSessionSpecialization = true;
    const forevertas::SearchResult specialized =
            forevertas::RunSearch(request, &control);
    const std::uint64_t afterModeSwitch =
            forevervalidator::simulation::cuda::specialization::
                    SessionModuleBuildCountForTesting();
    const forevertas::SearchResult specializedAgain =
            forevertas::RunSearch(request, &control);
    const std::uint64_t afterSpecializedReuse =
            forevervalidator::simulation::cuda::specialization::
                    SessionModuleBuildCountForTesting();
    const forevervalidator::CudaBackendDiagnostics cuda =
            forevervalidator::QueryCudaBackendDiagnostics();
    const bool expectsSpecialization = cuda.IsReady() &&
            forevervalidator::simulation::
                    IsCudaSessionSpecializationSupported(
                            cuda.computeCapabilityMajor,
                            cuda.computeCapabilityMinor);
    const std::uint64_t expectedAfterModeSwitch =
            before + (expectsSpecialization ? 1u : 0u);

    const bool sameResult =
            regular.winnerSource == specialized.winnerSource &&
            regular.bestScore == specialized.bestScore &&
            regular.bestEvaluationTimeMs ==
                    specialized.bestEvaluationTimeMs &&
            forevertas::FormatInputScript(regular.bestInputs) ==
                    forevertas::FormatInputScript(
                            specialized.bestInputs) &&
            specialized.bestScore == specializedAgain.bestScore;
    if (afterMapLoad != before ||
        afterModeSwitch != expectedAfterModeSwitch ||
        afterSpecializedReuse != afterModeSwitch || !sameResult) {
        std::cerr
                << "CUDA fast mode did not skip or reuse specialization "
                   "correctly: builds="
                << before << "/" << afterMapLoad << "/"
                << afterModeSwitch << "/" << afterSpecializedReuse
                << " same_result=" << sameResult << '\n';
        return false;
    }
    return true;
}

bool CheckCudaAutoPromoteAcrossBatches(
        const char *packsDirectory,
        const char *replayPath) {
    forevertas::SearchRequest request{packsDirectory, replayPath};
    const forevertas::InputScriptParseResult parsed =
            forevertas::ParseInputScript(
                    forevertas::ExtractReplayInputScript(
                            packsDirectory, replayPath));
    if (!parsed) {
        throw std::runtime_error(*parsed.error);
    }
    request.baseInputCommands = parsed.commands;
    request.backend = forevertas::PhysicsBackend::Cuda;
    request.parallelSampleCount = 128u;
    request.evaluationTarget =
            forevertas::DefaultEvaluationTargetConfiguration();
    request.evaluationTarget.id = forevertas::kPointTargetEvaluationId;
    request.evaluationTarget.settings = {
            {"minTimeMs", "1000"},
            {"maxTimeMs", "6000"},
            {"x", "0"},
            {"y", "0"},
            {"z", "0"}};
    request.searchAlgorithm.settings["autoPromoteBest"] = "true";

    forevertas::SearchRunControl control;
    control.iterationLimit = 256u;
    control.sampleBestTimeline = false;
    const forevertas::SearchResult result =
            forevertas::RunSearch(request, &control);
    const bool mutationWon =
            result.winnerSource ==
            forevertas::SearchWinnerSource::Mutation;
    if (result.iterations != 256u ||
        result.mutationImprovementCount == 0u ||
        mutationWon != (result.mutationImprovementCount > 0u) ||
        result.bestInputs.empty()) {
        std::cerr
                << "CUDA auto-promote lost its global winner across "
                   "multiple batches: improvements="
                << result.mutationImprovementCount << '\n';
        return false;
    }
    return true;
}
#endif

bool RunBackend(const char *packsDirectory,
                const char *replayPath,
                forevertas::PhysicsBackend backend) {
#if FOREVERVALIDATOR_HAS_CUDA
        const bool asynchronousImprovementSampling =
                backend == forevertas::PhysicsBackend::Cuda;
#else
        constexpr bool asynchronousImprovementSampling = false;
#endif
        constexpr std::uint64_t iterationsBeforeStop = 64u;
        bool stopRequested = false;
        bool sawLiveBest = false;
        bool sawFinalSampling = false;
        bool improvementTimelineInvalid = false;
        bool sampledBaseline = false;
        std::uint64_t lastSampledImprovement = 0u;
        std::size_t sampledMutationTimelineCount = 0u;
        std::mutex sampledTimelineMutex;
        std::chrono::steady_clock::duration previousElapsed{};
        std::size_t liveUpdateCount = 0u;
        forevertas::SearchRunControl control;
        control.stopRequested = [&stopRequested]() {
            return stopRequested;
        };
        const auto observeImprovementTimeline =
                [&](const forevertas::SearchLiveUpdate &live) {
                    std::lock_guard<std::mutex> guard(
                            sampledTimelineMutex);
                    if (live.bestTimeline.empty()) {
                        improvementTimelineInvalid = true;
                        return;
                    }
                    const bool baselineTimeline =
                            live.winnerSource ==
                                    forevertas::SearchWinnerSource::Baseline &&
                            !sampledBaseline &&
                            live.mutationImprovementCount == 0u;
                    const bool mutationTimeline =
                            live.winnerSource ==
                                    forevertas::SearchWinnerSource::Mutation &&
                            live.mutationImprovementCount >
                                    lastSampledImprovement;
                    const bool completeTimeline =
                            (baselineTimeline || mutationTimeline) &&
                            live.bestTimeline.front().timeMs == 0 &&
                            (live.bestTimeline.back().raceCompleted ||
                             live.bestTimeline.back().timeMs ==
                                     forevertas::
                                             kDefaultSimulationHorizonMs);
                    bool everyTick = true;
                    for (std::size_t index = 1u;
                         index < live.bestTimeline.size();
                         ++index) {
                        everyTick &=
                                live.bestTimeline[index].timeMs -
                                        live.bestTimeline[index - 1u].timeMs ==
                                10;
                    }
                    improvementTimelineInvalid |=
                            !completeTimeline || !everyTick;
                    if (baselineTimeline) {
                        sampledBaseline = true;
                    } else if (mutationTimeline) {
                        lastSampledImprovement =
                                live.mutationImprovementCount;
                        ++sampledMutationTimelineCount;
                    }
                };
        control.liveChanged = [&](const forevertas::SearchLiveUpdate &live) {
            if (sawFinalSampling) {
                std::cerr << "live search update arrived after final sampling started\n";
                return;
            }
            sawLiveBest |= !live.bestInputs.empty();
            if (!live.bestTimeline.empty()) {
                if (asynchronousImprovementSampling) {
                    std::lock_guard<std::mutex> guard(
                            sampledTimelineMutex);
                    improvementTimelineInvalid = true;
                } else {
                    observeImprovementTimeline(live);
                }
            }
            if (liveUpdateCount != 0u && live.elapsed < previousElapsed) {
                std::cerr << "live elapsed time moved backwards\n";
            }
            previousElapsed = live.elapsed;
            ++liveUpdateCount;
            if (live.iterations >= iterationsBeforeStop) {
                stopRequested = true;
            }
        };
        if (asynchronousImprovementSampling) {
            control.improvementTimelineSampled =
                    observeImprovementTimeline;
        }
        control.progressChanged = [&sawFinalSampling](
                                          const forevertas::SearchProgress
                                                  &progress) {
            sawFinalSampling |= progress.stage ==
                    forevertas::SearchProgressStage::FinalSampling;
        };
        control.reuseLoadedSandbox = true;
        forevertas::SearchRequest request{packsDirectory, replayPath};
        request.searchAlgorithm.settings["autoPromoteBest"] = "true";
        const forevertas::InputScriptParseResult parsed =
                forevertas::ParseInputScript(
                        forevertas::ExtractReplayInputScript(
                                packsDirectory, replayPath));
        if (!parsed) {
            throw std::runtime_error(*parsed.error);
        }
        request.baseInputCommands = parsed.commands;
        request.backend = backend;
#if FOREVERVALIDATOR_HAS_CUDA
        if (backend == forevertas::PhysicsBackend::Cuda) {
            request.parallelSampleCount =
                    forevertas::kDefaultCudaParallelSampleCount;
        }
#endif
        const forevertas::SearchResult result =
                forevertas::RunSearch(request, &control);
        const bool mutationWon =
                result.winnerSource ==
                forevertas::SearchWinnerSource::Mutation;
        if (mutationWon != (result.mutationImprovementCount > 0u)) {
            std::cerr << "winner and mutation improvement count disagree\n";
            return false;
        }
        if (!stopRequested || result.iterations < iterationsBeforeStop) {
            std::cerr << "search returned before the stop request\n";
            return false;
        }
        if (!sawLiveBest || liveUpdateCount < 2u) {
            std::cerr << "search did not publish live updates while running\n";
            return false;
        }
        {
            std::lock_guard<std::mutex> guard(sampledTimelineMutex);
            if (improvementTimelineInvalid || !sampledBaseline ||
                (result.mutationImprovementCount > 0u) !=
                        (sampledMutationTimelineCount > 0u) ||
                (result.mutationImprovementCount > 0u &&
                 lastSampledImprovement !=
                         result.mutationImprovementCount)) {
                std::cerr
                        << "best-run improvements did not publish complete "
                           "timelines\n";
                return false;
            }
        }
        if (result.bestInputs.empty()) {
            std::cerr << "best input timeline was not retained\n";
            return false;
        }
        for (const forevertas::SandboxInputEvent &event :
             result.bestInputs) {
            if (event.value.kind == forevervalidator::experimental::
                            PhysicsSandboxInputValueKind::Analog &&
                !forevervalidator::IsAnalogInputStateValid(
                        event.value.analog)) {
                std::cerr << "best inputs contain an out-of-range analog state\n";
                return false;
            }
        }
        const std::string inputScript =
                forevertas::FormatInputScript(result.bestInputs);
        if (inputScript.rfind("0.00 ", 0u) != 0u ||
            inputScript.find(" release ") != std::string::npos ||
            inputScript.find(" rel ") == std::string::npos) {
            std::cerr << "best inputs were not exported as an input script\n";
            return false;
        }
        if (!sawFinalSampling || result.bestTimeline.empty()) {
            std::cerr << "best run was not sampled after Stop\n";
            return false;
        }
        if (result.bestTimeline.front().timeMs != 0 ||
            (!result.bestTimeline.back().raceCompleted &&
             result.bestTimeline.back().timeMs !=
                     forevertas::kDefaultSimulationHorizonMs)) {
            std::cerr << "best run sampling did not reach completion or the "
                         "Simulation horizon\n";
            return false;
        }
        for (std::size_t index = 1u;
             index < result.bestTimeline.size();
             ++index) {
            if (result.bestTimeline[index].timeMs -
                        result.bestTimeline[index - 1u].timeMs !=
                10) {
                std::cerr << "best run was not sampled every tick\n";
                return false;
            }
        }
        const double searchSeconds =
                std::chrono::duration<double>(previousElapsed).count();
        const double iterationsPerSecond = searchSeconds > 0.0
                ? static_cast<double>(result.iterations) / searchSeconds
                : 0.0;
        std::cout << "backend=" << forevertas::PhysicsBackendId(backend)
                  << " winner="
                  << (mutationWon ? "Mutation" : "Baseline")
                  << " score=" << result.bestScore
                  << " improvements="
                  << result.mutationImprovementCount
                  << " iteration="
                  << (result.winningIterationIndex
                              ? std::to_string(
                                        *result.winningIterationIndex + 1u)
                              : std::string("none"))
                  << " iterations=" << result.iterations
                  << " iterations_per_second=" << iterationsPerSecond
                  << " inputs=" << result.bestInputs.size()
                  << " frames=" << result.bestTimeline.size() << '\n';
        return true;
}

bool CheckStuntTargetBackend(
        const char *packsDirectory,
        const char *replayPath,
        forevertas::PhysicsBackend backend) {
    const forevertas::InputScriptParseResult parsed =
            forevertas::ParseInputScript(
                    forevertas::ExtractReplayInputScript(
                            packsDirectory, replayPath));
    if (!parsed) {
        throw std::runtime_error(*parsed.error);
    }

    forevertas::SearchRunControl control;
    control.iterationLimit = 0u;
    control.sampleBestTimeline = false;
    forevertas::SearchRequest request{packsDirectory, replayPath};
    request.backend = backend;
    request.baseInputCommands = parsed.commands;
#if FOREVERVALIDATOR_HAS_CUDA
    if (backend == forevertas::PhysicsBackend::Cuda) {
        request.useCudaSessionSpecialization = false;
    }
#endif
    request.evaluationTarget = {
            forevertas::kStuntPointsEvaluationId,
            {{"targetTimeMs", "6000"}}};
    const forevertas::SearchResult result =
            forevertas::RunSearch(request, &control);
    const bool valid =
            result.bestEvaluationTimeMs == 6000.0 &&
            result.bestState.timeMs == 6000u &&
            result.bestScore ==
                    static_cast<double>(
                            result.bestState.stuntsScore.value_or(0u)) &&
            result.bestEvaluationDescription.rfind(
                    "Stunt points: ", 0u) == 0u;
    if (!valid) {
        std::cerr
                << "stunt target did not observe the configured deadline "
                << "with backend "
                << forevertas::PhysicsBackendId(backend)
                << " (evaluation=" << result.bestEvaluationTimeMs
                << ", state=" << result.bestState.timeMs << ")\n";
    }
    return valid;
}

bool CheckMultiThreadedCpuBackend(
        const char *packsDirectory,
        const char *replayPath) {
    const forevertas::InputScriptParseResult parsed =
            forevertas::ParseInputScript(
                    forevertas::ExtractReplayInputScript(
                            packsDirectory, replayPath));
    if (!parsed) {
        throw std::runtime_error(*parsed.error);
    }

    forevertas::SearchRequest serialRequest{
            packsDirectory, replayPath};
    serialRequest.backend =
            forevertas::PhysicsBackend::OptimizedCpu;
    serialRequest.baseInputCommands = parsed.commands;
    forevertas::SearchRunControl serialControl;
    serialControl.iterationLimit = 6u;
    serialControl.evaluationEndTimeLimitMs = 1020;
    serialControl.sampleBestTimeline = false;
    const forevertas::SearchResult serial =
            forevertas::RunSearch(serialRequest, &serialControl);

    forevertas::SearchRequest parallelRequest = serialRequest;
    parallelRequest.backend =
            forevertas::PhysicsBackend::MultiThreadedCpu;
    parallelRequest.parallelSampleCount = 3u;
    forevertas::SearchRunControl parallelControl = serialControl;
    const std::thread::id callerThread = std::this_thread::get_id();
    std::uint64_t previousIterations = 0u;
    std::size_t liveUpdateCount = 0u;
    bool aggregateInvalid = false;
    parallelControl.liveChanged =
            [&](const forevertas::SearchLiveUpdate &live) {
                aggregateInvalid |=
                        std::this_thread::get_id() != callerThread ||
                        live.iterations < previousIterations ||
                        live.iterations > 6u;
                previousIterations = live.iterations;
                ++liveUpdateCount;
            };
    const forevertas::SearchResult parallel =
            forevertas::RunSearch(parallelRequest, &parallelControl);
    const bool sameWinner =
            parallel.bestScore == serial.bestScore &&
            parallel.bestEvaluationTimeMs ==
                    serial.bestEvaluationTimeMs &&
            parallel.winnerSource == serial.winnerSource &&
            parallel.winningIterationIndex ==
                    serial.winningIterationIndex &&
            forevertas::FormatInputScript(parallel.bestInputs) ==
                    forevertas::FormatInputScript(serial.bestInputs);
    if (parallel.iterations != 6u || liveUpdateCount == 0u ||
        previousIterations != 6u || aggregateInvalid ||
        !sameWinner ||
        (parallel.winnerSource ==
                 forevertas::SearchWinnerSource::Mutation &&
         parallel.mutationImprovementCount == 0u)) {
        std::cerr
                << "multi-threaded CPU did not aggregate the same six "
                   "independent candidates as serial optimized CPU\n";
        return false;
    }

    forevertas::SearchRequest promotedParallelRequest =
            parallelRequest;
    promotedParallelRequest.searchAlgorithm.settings[
            "autoPromoteBest"] = "true";
    const forevertas::SearchResult promotedParallel =
            forevertas::RunSearch(
                    promotedParallelRequest, &serialControl);
    if (promotedParallel.iterations != 6u ||
        promotedParallel.bestInputs.empty()) {
        std::cerr
                << "multi-threaded CPU did not run with auto-promote "
                   "enabled\n";
        return false;
    }

    bool stop = true;
    forevertas::SearchRunControl stoppedControl;
    stoppedControl.stopRequested = [&]() { return stop; };
    stoppedControl.sampleBestTimeline = false;
    const forevertas::SearchResult stopped =
            forevertas::RunSearch(parallelRequest, &stoppedControl);
    if (stopped.iterations != 0u) {
        std::cerr
                << "multi-threaded CPU ignored a stop before mutations\n";
        return false;
    }

    parallelRequest.parallelSampleCount = 0u;
    try {
        static_cast<void>(
                forevertas::RunSearch(
                        parallelRequest, &serialControl));
        std::cerr << "multi-threaded CPU accepted zero workers\n";
        return false;
    } catch (const std::invalid_argument &) {
    }
    parallelRequest.parallelSampleCount = 2u;
    forevertas::SearchRunControl cancelledControl;
    cancelledControl.cancellationRequested = []() { return true; };
    try {
        static_cast<void>(
                forevertas::RunSearch(
                        parallelRequest, &cancelledControl));
        std::cerr
                << "multi-threaded CPU ignored startup cancellation\n";
        return false;
    } catch (const forevertas::SearchCancelled &) {
    }

    forevertas::SearchRunControl throwingCallbackControl =
            serialControl;
    throwingCallbackControl.liveChanged =
            [](const forevertas::SearchLiveUpdate &) {
                throw std::runtime_error(
                        "expected aggregate callback failure");
            };
    try {
        static_cast<void>(
                forevertas::RunSearch(
                        parallelRequest, &throwingCallbackControl));
        std::cerr
                << "multi-threaded CPU swallowed an aggregate callback "
                   "failure\n";
        return false;
    } catch (const std::runtime_error &error) {
        if (std::string(error.what()) !=
            "expected aggregate callback failure") {
            throw;
        }
    }
    return true;
}

bool CheckCachedScriptIsolation(const char *packsDirectory,
                                const char *replayPath) {
    const std::string replayScript =
            forevertas::ExtractReplayInputScript(
                    packsDirectory, replayPath);
    forevertas::InputScriptParseResult parsed =
            forevertas::ParseInputScript(replayScript);
    if (!parsed) {
        throw std::runtime_error(*parsed.error);
    }

    forevertas::SearchRunControl control;
    control.iterationLimit = 0u;
    control.reuseLoadedSandbox = true;
    control.sampleBestTimeline = false;
    forevertas::SearchRequest request{packsDirectory, replayPath};
    request.baseInputCommands = parsed.commands;
    const forevertas::SearchResult first =
            forevertas::RunSearch(request, &control);

    request.baseInputCommands.clear();
    const forevertas::SearchResult empty =
            forevertas::RunSearch(request, &control);

    request.baseInputCommands = parsed.commands;
    const forevertas::SearchResult restored =
            forevertas::RunSearch(request, &control);
    const std::string firstScript =
            forevertas::FormatInputScript(first.bestInputs);
    const std::string emptyScript =
            forevertas::FormatInputScript(empty.bestInputs);
    const std::string restoredScript =
            forevertas::FormatInputScript(restored.bestInputs);
    if (firstScript.empty() || !emptyScript.empty() ||
        firstScript != restoredScript) {
        std::cerr << "cached searches leaked base scripts between requests\n";
        return false;
    }
    return true;
}

bool CheckCachedReplaySourceIdentity(const char *packsDirectory,
                                     const char *replayPath) {
    namespace fs = std::filesystem;
    ScopedTemporaryDirectory temporary;
    const fs::path replayCopy =
            temporary.Path() / "source-identity.Replay.Gbx";
    std::error_code error;
    fs::copy_file(
            fs::u8path(replayPath),
            replayCopy,
            fs::copy_options::none,
            error);
    if (error) {
        throw std::runtime_error(
                "could not copy replay for cache identity test: " +
                error.message());
    }

    forevertas::SearchRunControl control;
    control.iterationLimit = 0u;
    control.reuseLoadedSandbox = true;
    control.sampleBestTimeline = false;
    forevertas::SearchRequest request{
            packsDirectory, replayCopy.u8string()};
    static_cast<void>(forevertas::RunSearch(request, &control));

    const std::uintmax_t length = fs::file_size(replayCopy, error);
    if (error || length == 0u ||
        length > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error(
                "cache identity replay fixture has an invalid length");
    }
    const fs::file_time_type originalTimestamp =
            fs::last_write_time(replayCopy, error);
    if (error) {
        throw std::runtime_error(
                "could not read cache identity replay timestamp: " +
                error.message());
    }
    {
        std::ofstream stream(
                replayCopy, std::ios::binary | std::ios::trunc);
        const std::vector<char> replacement(
                static_cast<std::size_t>(length), '\0');
        if (!stream ||
            !stream.write(
                    replacement.data(),
                    static_cast<std::streamsize>(replacement.size()))) {
            throw std::runtime_error(
                    "could not replace cache identity replay fixture");
        }
    }
    fs::last_write_time(replayCopy, originalTimestamp, error);
    if (error) {
        throw std::runtime_error(
                "could not preserve cache identity replay timestamp: " +
                error.message());
    }

    ScopedClogCapture logs;
    try {
        static_cast<void>(forevertas::RunSearch(request, &control));
        std::cerr << "cached search reused a replay replaced in place\n";
        return false;
    } catch (const std::runtime_error &) {
    }
    const std::string text = logs.Text();
    if (text.find("reason=source_identity_changed replay_changed=1 "
                  "packs_changed=0") == std::string::npos) {
        std::cerr << "cached replay replacement did not report source "
                     "identity invalidation\n";
        return false;
    }
    return true;
}

bool CheckCachedPackSourceIdentity(const char *packsDirectory,
                                   const char *replayPath) {
    namespace fs = std::filesystem;
    ScopedTemporaryDirectory temporary;
    const fs::path sourceRoot = fs::u8path(packsDirectory);
    std::error_code error;
    for (const char *identifier : {"packlist.dat", "Stadium.pak"}) {
        fs::copy_file(
                sourceRoot / fs::u8path(identifier),
                temporary.Path() / fs::u8path(identifier),
                fs::copy_options::none,
                error);
        if (error) {
            throw std::runtime_error(
                    std::string("could not copy ") + identifier +
                    " for cache identity test: " + error.message());
        }
    }
    for (const char *identifier : {"Game.pak", "Resource.pak"}) {
        std::ofstream stream(
                temporary.Path() / fs::u8path(identifier),
                std::ios::binary);
        stream << "unrelated";
        if (!stream) {
            throw std::runtime_error(
                    std::string("could not create unrelated cache test ") +
                    identifier);
        }
    }

    forevertas::SearchRunControl control;
    control.iterationLimit = 0u;
    control.reuseLoadedSandbox = true;
    control.sampleBestTimeline = false;
    forevertas::SearchRequest request{
            temporary.Path().u8string(), replayPath};
    static_cast<void>(forevertas::RunSearch(request, &control));

    const fs::path relevantPack = temporary.Path() / "Stadium.pak";
    const fs::file_time_type relevantPackTimestamp =
            fs::last_write_time(relevantPack, error);
    if (error) {
        throw std::runtime_error(
                "could not read copied Stadium pack timestamp: " +
                error.message());
    }
    fs::last_write_time(
            relevantPack,
            relevantPackTimestamp + std::chrono::seconds(2),
            error);
    if (error) {
        throw std::runtime_error(
                "could not change copied Stadium pack timestamp: " +
                error.message());
    }
    {
        ScopedClogCapture logs;
        static_cast<void>(forevertas::RunSearch(request, &control));
        if (logs.Text().find(
                    "reason=source_identity_changed replay_changed=0 "
                    "packs_changed=1") == std::string::npos) {
            std::cerr << "cached search ignored relevant Pack metadata "
                         "change\n";
            return false;
        }
    }

    for (const char *identifier : {"Game.pak", "Resource.pak"}) {
        const fs::path unrelated =
                temporary.Path() / fs::u8path(identifier);
        const fs::file_time_type timestamp =
                fs::last_write_time(unrelated, error);
        if (error) {
            throw std::runtime_error(
                    std::string("could not read unrelated ") + identifier +
                    " timestamp: " + error.message());
        }
        fs::last_write_time(
                unrelated, timestamp + std::chrono::seconds(2), error);
        if (error) {
            throw std::runtime_error(
                    std::string("could not change unrelated ") + identifier +
                    " timestamp: " + error.message());
        }
    }
    ScopedClogCapture logs;
    static_cast<void>(forevertas::RunSearch(request, &control));
    if (logs.Text().find("reason=source_identity_changed") !=
        std::string::npos) {
        std::cerr << "unrelated Pack files invalidated cached search\n";
        return false;
    }
    return true;
}

bool CheckCachedHorizonIsolation(const char *packsDirectory,
                                 const char *replayPath) {
    forevertas::SearchRequest request{packsDirectory, replayPath};
    request.backend = forevertas::PhysicsBackend::OptimizedCpu;
    request.modifiers.front().settings["minTimeMs"] = "0";
    request.modifiers.front().settings["maxTimeMs"] = "900";
    request.evaluationTarget.settings["minTimeMs"] = "1000";
    request.evaluationTarget.settings["maxTimeMs"] = "1000";
    request.simulationHorizonMs = 1000u;

    forevertas::SearchRunControl control;
    control.iterationLimit = 0u;
    const forevertas::SearchResult shortRun =
            forevertas::RunSearch(request, &control);
    request.simulationHorizonMs = 1200u;
    const forevertas::SearchResult longRun =
            forevertas::RunSearch(request, &control);
    return !shortRun.bestTimeline.empty() &&
            !longRun.bestTimeline.empty() &&
            shortRun.bestTimeline.back().timeMs == 1000u &&
            longRun.bestTimeline.back().timeMs == 1200u;
}

bool CheckKeyboardSteeringBaseline(const char *packsDirectory,
                                   const char *replayPath) {
    const forevertas::InputScriptParseResult parsed =
            forevertas::ParseInputScript(
                    "0.00 press left\n"
                    "0.00 press right\n"
                    "0.10 rel left\n"
                    "0.20 steer -32768\n"
                    "0.30 press left\n"
                    "0.40 rel left\n"
                    "0.50 rel right");
    if (!parsed) {
        throw std::runtime_error(*parsed.error);
    }

    forevertas::SearchRunControl control;
    control.iterationLimit = 0u;
    control.reuseLoadedSandbox = true;
    control.sampleBestTimeline = false;
    forevertas::SearchRequest request{packsDirectory, replayPath};
    request.baseInputCommands = parsed.commands;
    const forevertas::SearchResult result =
            forevertas::RunSearch(request, &control);
    const std::string script =
            forevertas::FormatInputScript(result.bestInputs);
    const std::string expected =
            "0.00 steer -65536\n"
            "0.10 steer 65536\n"
            "0.20 steer -32768\n"
            "0.30 steer -65536\n"
            "0.40 steer 65536\n"
            "0.50 steer 0";
    if (script != expected) {
        std::cerr << "keyboard steering baseline was not converted exactly\n"
                  << "expected:\n" << expected << "\nactual:\n"
                  << script << '\n';
        return false;
    }
    return true;
}

bool CheckKeyboardSteeringPhysicsParity(const char *packsDirectory,
                                        const char *replayPath) {
    using namespace forevervalidator;
    using namespace forevervalidator::experimental;

    const ReplayIdentity identity{replayPath};
    const AssetBytes replay = Require(
            forevertas::ReadReplayFileUtf8(replayPath, identity),
            "reading replay for keyboard parity");
    PhysicsSandboxOptions options;
    options.backend = SimulationBackend::Reference;
    options.tickDurationMs = forevertas::kSearchTickDurationMs;
    options.timelineMode = PhysicsSandboxTimelineMode::Canonical;
    options.simulationHorizonMs =
            forevertas::kDefaultSimulationHorizonMs;
    PhysicsSandbox keyboard = Require(
            CreatePhysicsSandbox(
                    Require(OpenInstalledPackDirectory(packsDirectory),
                            "opening keyboard parity Packs"),
                    options),
            "creating keyboard parity sandbox");
    PhysicsSandbox analog = Require(
            CreatePhysicsSandbox(
                    Require(OpenInstalledPackDirectory(packsDirectory),
                            "opening analog parity Packs"),
                    options),
            "creating analog parity sandbox");
    Require(keyboard.LoadReplay({replay.data(), replay.size()}, identity),
            "loading keyboard parity replay");
    Require(analog.LoadReplay({replay.data(), replay.size()}, identity),
            "loading analog parity replay");

    const forevertas::InputScriptParseResult parsed =
            forevertas::ParseInputScript(
                    "0.00 press left\n"
                    "0.00 press right\n"
                    "0.10 rel left\n"
                    "0.20 steer -32768\n"
                    "0.20 press right\n"
                    "0.30 press left\n"
                    "0.40 rel left\n"
                    "0.50 rel right\n"
                    "0.50 steer 500\n"
                    "0.60 steer 656");
    if (!parsed) {
        throw std::runtime_error(*parsed.error);
    }
    const std::vector<forevertas::SandboxInputEvent> replayInputs =
            Require(keyboard.ReadInputs(), "reading keyboard parity inputs");
    const forevertas::InputScriptBaselineResult materialized =
            forevertas::BuildInputScriptBaseline(
                    replayInputs,
                    parsed.commands,
                    forevertas::kSearchTickDurationMs);
    if (!materialized) {
        throw std::runtime_error(*materialized.error);
    }
    std::vector<forevertas::SandboxInputEvent> converted =
            materialized.events;
    forevertas::ConvertKeyboardSteeringToAnalog(converted);
    Require(keyboard.ReplaceInputs(materialized.events),
            "applying keyboard parity inputs");
    Require(analog.ReplaceInputs(std::move(converted)),
            "applying analog parity inputs");

    for (std::uint32_t tick = 0u; tick <= 70u; ++tick) {
        const PhysicsSandboxStateView keyboardState =
                Require(keyboard.ReadState(), "reading keyboard parity state");
        const PhysicsSandboxStateView analogState =
                Require(analog.ReadState(), "reading analog parity state");
        const bool equal =
                keyboardState.timeMs == analogState.timeMs &&
                keyboardState.steering == analogState.steering &&
                keyboardState.accelerate == analogState.accelerate &&
                keyboardState.brake == analogState.brake &&
                keyboardState.car.position.x == analogState.car.position.x &&
                keyboardState.car.position.y == analogState.car.position.y &&
                keyboardState.car.position.z == analogState.car.position.z &&
                keyboardState.car.linearSpeed.x ==
                        analogState.car.linearSpeed.x &&
                keyboardState.car.linearSpeed.y ==
                        analogState.car.linearSpeed.y &&
                keyboardState.car.linearSpeed.z ==
                        analogState.car.linearSpeed.z &&
                keyboardState.stuntsScore == analogState.stuntsScore;
        if (!equal) {
            std::cerr << "keyboard and analog simulations diverged at tick "
                      << tick << '\n';
            return false;
        }
        if (tick != 70u) {
            Require(keyboard.AdvanceTicks(1u),
                    "advancing keyboard parity sandbox");
            Require(analog.AdvanceTicks(1u),
                    "advancing analog parity sandbox");
        }
    }
    return true;
}

bool SameSandboxState(const PhysicsSandboxStateView &left,
                      const PhysicsSandboxStateView &right) {
    return left.tick == right.tick && left.timeMs == right.timeMs &&
            left.accelerate == right.accelerate &&
            left.brake == right.brake &&
            left.steering == right.steering &&
            left.car.position.x == right.car.position.x &&
            left.car.position.y == right.car.position.y &&
            left.car.position.z == right.car.position.z &&
            left.car.linearSpeed.x == right.car.linearSpeed.x &&
            left.car.linearSpeed.y == right.car.linearSpeed.y &&
            left.car.linearSpeed.z == right.car.linearSpeed.z &&
            left.car.angularSpeed.x == right.car.angularSpeed.x &&
            left.car.angularSpeed.y == right.car.angularSpeed.y &&
            left.car.angularSpeed.z == right.car.angularSpeed.z &&
            left.checkpointsCollected == right.checkpointsCollected &&
            left.completedLaps == right.completedLaps &&
            left.raceCompleted == right.raceCompleted &&
            left.stuntsScore == right.stuntsScore;
}

bool CheckSandboxCloneAndWindowParity(
        const char *packsDirectory,
        const char *replayPath) {
    using namespace forevervalidator;
    using namespace forevervalidator::experimental;
    const ReplayIdentity identity{replayPath};
    const AssetBytes replay = Require(
            forevertas::ReadReplayFileUtf8(replayPath, identity),
            "reading clone parity replay");
    PhysicsSandboxOptions options;
    options.backend = SimulationBackend::OptimizedCpu;
    options.tickDurationMs = forevertas::kSearchTickDurationMs;
    options.timelineMode = PhysicsSandboxTimelineMode::Canonical;
    options.simulationHorizonMs =
            forevertas::kDefaultSimulationHorizonMs;
    PhysicsSandbox source = Require(
            CreatePhysicsSandbox(
                    Require(OpenInstalledPackDirectory(packsDirectory),
                            "opening clone parity Packs"),
                    options),
            "creating clone parity sandbox");
    Require(source.LoadReplay({replay.data(), replay.size()}, identity),
            "loading clone parity replay");
    const std::vector<PhysicsSandboxInputEvent> canonicalInputs = Require(
            source.ReadInputs(), "reading clone parity canonical inputs");
    const forevertas::InputScriptParseResult parsed =
            forevertas::ParseInputScript(
                    forevertas::ExtractReplayInputScript(
                            packsDirectory, replayPath));
    if (!parsed) throw std::runtime_error(*parsed.error);
    std::vector<forevertas::ParsedInputCommand> commands = parsed.commands;
    forevertas::ParsedInputCommand lateInput;
    lateInput.userTimeMs =
            forevertas::kDefaultSimulationHorizonMs + 1000;
    lateInput.action = forevertas::SandboxInputAction::Steer;
    lateInput.value.kind = PhysicsSandboxInputValueKind::Analog;
    lateInput.value.analog = 12345;
    lateInput.sourceLine = parsed.commands.size() + 1u;
    commands.push_back(lateInput);
    forevertas::InputScriptBaselineResult baselineResult =
            forevertas::BuildInputScriptBaseline(
                    canonicalInputs,
                    commands,
                    forevertas::kSearchTickDurationMs);
    if (!baselineResult) throw std::runtime_error(*baselineResult.error);
    if (baselineResult.events.empty() ||
        baselineResult.events.back().timeMs <=
                static_cast<std::int64_t>(
                        forevertas::kDefaultSimulationHorizonMs)) {
        std::cerr << "input beyond the Simulation horizon was not retained\n";
        return false;
    }
    forevertas::ConvertKeyboardSteeringToAnalog(baselineResult.events);
    Require(source.ReplaceInputs(baselineResult.events),
            "applying clone parity baseline");

    PhysicsSandbox full = Require(
            ClonePhysicsSandbox(source), "cloning full parity sandbox");
    PhysicsSandbox window = Require(
            ClonePhysicsSandbox(source), "cloning window parity sandbox");
    const PhysicsSandboxStateView sourceInitial = Require(
            source.ReadState(), "reading source clone state");
    if (!SameSandboxState(
                sourceInitial,
                Require(full.ReadState(), "reading full clone state")) ||
        !SameSandboxState(
                sourceInitial,
                Require(window.ReadState(), "reading window clone state"))) {
        std::cerr << "cloned sandbox did not preserve its source state\n";
        return false;
    }

    forevertas::MutationWindowPatch patch;
    patch.minimumTimeMs = 0;
    patch.maximumTimeMs = 1000;
    for (const PhysicsSandboxInputEvent &event : baselineResult.events) {
        if (event.timeMs >= patch.minimumTimeMs &&
            event.timeMs <= patch.maximumTimeMs) {
            patch.events.push_back(event);
        }
    }
    const auto steering = std::find_if(
            patch.events.begin(), patch.events.end(),
            [](const PhysicsSandboxInputEvent &event) {
                return event.action ==
                               forevertas::SandboxInputAction::Steer &&
                        event.value.kind ==
                               PhysicsSandboxInputValueKind::Analog;
            });
    if (steering == patch.events.end()) {
        std::cerr << "clone parity replay has no steering event in window\n";
        return false;
    }
    steering->timeMs = std::min<std::int32_t>(
            steering->timeMs + 10, 1000);
    steering->value.analog = forevertas::SaturateAnalogInputState(
            static_cast<std::int64_t>(steering->value.analog) + 12345);
    forevertas::NormalizeInputEvents(
            patch.events, forevertas::kSearchTickDurationMs);
    const std::vector<PhysicsSandboxInputEvent> expectedInputs =
            forevertas::ApplyInputWindowPatch(
                    baselineResult.events, patch);
    Require(full.ReplaceInputs(expectedInputs),
            "applying full clone parity inputs");
    auto windowReplaced = window.ReplaceInputWindow(
            patch.minimumTimeMs,
            patch.maximumTimeMs,
            patch.events);
    if (!windowReplaced) {
        std::cerr << "applying window clone parity inputs failed: "
                  << windowReplaced.Error().diagnostic << '\n';
        return false;
    }
    if (forevertas::FormatInputScript(Require(
                full.ReadInputs(), "reading full parity inputs")) !=
            forevertas::FormatInputScript(Require(
                window.ReadInputs(), "reading window parity inputs")) ||
        forevertas::FormatInputScript(Require(
                source.ReadInputs(), "reading untouched source inputs")) !=
            forevertas::FormatInputScript(baselineResult.events)) {
        std::cerr << "window replacement did not preserve input semantics\n";
        return false;
    }

    std::optional<PhysicsSandboxState> savedWindowState;
    for (std::uint32_t tick = 0u; tick <= 200u; ++tick) {
        const PhysicsSandboxStateView fullState = Require(
                full.ReadState(), "reading full parity state");
        const PhysicsSandboxStateView windowState = Require(
                window.ReadState(), "reading window parity state");
        if (!SameSandboxState(fullState, windowState)) {
            std::cerr << "window control overlay diverged at tick "
                      << tick << '\n';
            return false;
        }
        if (tick == 50u) {
            savedWindowState = Require(
                    window.CaptureState(),
                    "capturing window overlay state");
        }
        if (tick != 200u) {
            Require(full.AdvanceTicks(1u),
                    "advancing full parity sandbox");
            Require(window.AdvanceTicks(1u),
                    "advancing window parity sandbox");
        }
    }
    if (!savedWindowState) return false;
    const PhysicsSandboxStateView restored = Require(
            window.RestoreState(*savedWindowState),
            "restoring window overlay state");
    return restored.timeMs == 500u;
}

bool CheckResizableCanonicalHorizon(const char *packsDirectory,
                                    const char *replayPath) {
    using namespace forevervalidator;
    using namespace forevervalidator::experimental;
    const ReplayIdentity identity{replayPath};
    const AssetBytes replay = Require(
            forevertas::ReadReplayFileUtf8(replayPath, identity),
            "reading resizable-horizon replay");
    const auto create = [&](std::uint32_t horizonMs) {
        PhysicsSandboxOptions options;
        options.backend = SimulationBackend::OptimizedCpu;
        options.tickDurationMs = forevertas::kSearchTickDurationMs;
        options.timelineMode = PhysicsSandboxTimelineMode::Canonical;
        options.simulationHorizonMs = horizonMs;
        PhysicsSandbox sandbox = Require(
                CreatePhysicsSandbox(
                        Require(OpenInstalledPackDirectory(packsDirectory),
                                "opening resizable-horizon Packs"),
                        options),
                "creating resizable-horizon sandbox");
        Require(sandbox.LoadReplay({replay.data(), replay.size()}, identity),
                "loading resizable-horizon replay");
        return sandbox;
    };

    PhysicsSandbox resized = create(6000u);
    const PhysicsSandboxState initial = Require(
            resized.CaptureState(), "capturing resizable initial state");
    Require(resized.AdvanceTicks(200u),
            "advancing to resizable snapshot");
    const PhysicsSandboxState snapshot = Require(
            resized.CaptureState(), "capturing resizable snapshot");
    Require(resized.SetSimulationHorizonMs(8000u),
            "extending canonical horizon");
    const PhysicsSandboxStateView restoredExtended = Require(
            resized.RestoreState(snapshot),
            "restoring snapshot after extending horizon");
    if (restoredExtended.timeMs != 2000u) {
        std::cerr << "extended-horizon snapshot restored at wrong time\n";
        return false;
    }
    const PhysicsSandboxStateView extended = Require(
            resized.AdvanceTicks(600u),
            "advancing extended canonical horizon");
    PhysicsSandbox freshExtended = create(8000u);
    const PhysicsSandboxStateView expectedExtended = Require(
            freshExtended.AdvanceTicks(800u),
            "advancing fresh extended horizon");
    if (!SameSandboxState(extended, expectedExtended)) {
        std::cerr << "extended cached simulation diverged from a fresh run\n";
        return false;
    }

    const auto invalidShrink = resized.SetSimulationHorizonMs(4000u);
    if (invalidShrink) {
        std::cerr << "horizon shrank behind the simulation cursor\n";
        return false;
    }
    Require(resized.RestoreState(initial),
            "restoring initial state before shrinking horizon");
    Require(resized.SetSimulationHorizonMs(4000u),
            "shrinking canonical horizon");
    const PhysicsSandboxStateView restoredShrunk = Require(
            resized.RestoreState(snapshot),
            "restoring snapshot after shrinking horizon");
    if (restoredShrunk.timeMs != 2000u) {
        std::cerr << "shrunk-horizon snapshot restored at wrong time\n";
        return false;
    }
    const PhysicsSandboxStateView shrunk = Require(
            resized.AdvanceTicks(200u),
            "advancing shrunk canonical horizon");
    PhysicsSandbox freshShrunk = create(4000u);
    const PhysicsSandboxStateView expectedShrunk = Require(
            freshShrunk.AdvanceTicks(400u),
            "advancing fresh shrunk horizon");
    if (!SameSandboxState(shrunk, expectedShrunk)) {
        std::cerr << "shrunk cached simulation diverged from a fresh run\n";
        return false;
    }
    return true;
}

bool CheckInputAfterHorizonAccepted(const char *packsDirectory,
                                    const char *replayPath) {
    forevertas::SearchRequest request{packsDirectory, replayPath};
    request.backend = forevertas::PhysicsBackend::OptimizedCpu;
    forevertas::ParsedInputCommand lateInput;
    lateInput.userTimeMs = 100000;
    lateInput.action = forevertas::SandboxInputAction::Steer;
    lateInput.value.kind = PhysicsSandboxInputValueKind::Analog;
    lateInput.value.analog = 12345;
    lateInput.sourceLine = 300u;
    request.baseInputCommands = {lateInput};

    forevertas::SearchRunControl control;
    control.iterationLimit = 0u;
    control.sampleBestTimeline = false;
    static_cast<void>(forevertas::RunSearch(request, &control));
    return true;
}

bool CheckCanonicalHorizonAndLateInputs(const char *packsDirectory,
                                        const char *replayPath) {
    using namespace forevervalidator;
    using namespace forevervalidator::experimental;
    const ReplayIdentity identity{replayPath};
    const AssetBytes replay = Require(
            forevertas::ReadReplayFileUtf8(replayPath, identity),
            "reading canonical horizon replay");

    PhysicsSandboxOptions recordedOptions;
    recordedOptions.backend = SimulationBackend::Reference;
    recordedOptions.tickDurationMs = forevertas::kSearchTickDurationMs;
    PhysicsSandbox recorded = Require(
            CreatePhysicsSandbox(
                    Require(OpenInstalledPackDirectory(packsDirectory),
                            "opening recorded horizon Packs"),
                    recordedOptions),
            "creating recorded horizon sandbox");
    const PhysicsSandboxStateView recordedInitial = Require(
            recorded.LoadReplay({replay.data(), replay.size()}, identity),
            "loading recorded horizon replay");
    const std::uint32_t horizon = static_cast<std::uint32_t>(
            ((recordedInitial.durationMs + 1009u) / 10u) * 10u);

    PhysicsSandboxOptions canonicalOptions;
    canonicalOptions.backend = SimulationBackend::Reference;
    canonicalOptions.tickDurationMs = forevertas::kSearchTickDurationMs;
    canonicalOptions.timelineMode = PhysicsSandboxTimelineMode::Canonical;
    canonicalOptions.simulationHorizonMs = horizon;
    PhysicsSandbox canonical = Require(
            CreatePhysicsSandbox(
                    Require(OpenInstalledPackDirectory(packsDirectory),
                            "opening canonical horizon Packs"),
                    canonicalOptions),
            "creating canonical horizon sandbox");
    const PhysicsSandboxStateView canonicalInitial = Require(
            canonical.LoadReplay({replay.data(), replay.size()}, identity),
            "loading canonical horizon replay");
    const std::vector<PhysicsSandboxInputEvent> fixed = Require(
            canonical.ReadInputs(), "reading canonical fixed inputs");
    if (canonicalInitial.durationMs != horizon || fixed.size() != 1u ||
        fixed.front().timeMs != 0 ||
        fixed.front().action != PhysicsSandboxInputAction::RaceRunning) {
        std::cerr << "canonical sandbox retained recorded timeline data\n";
        return false;
    }

    forevertas::ParsedInputCommand inside;
    inside.userTimeMs = recordedInitial.durationMs + 100;
    inside.action = forevertas::SandboxInputAction::Steer;
    inside.value.kind = PhysicsSandboxInputValueKind::Analog;
    inside.value.analog = 12345;
    inside.sourceLine = 1u;
    forevertas::ParsedInputCommand late = inside;
    late.userTimeMs = horizon + 100;
    late.value.analog = -12345;
    late.sourceLine = 2u;
    const forevertas::InputScriptBaselineResult baseline =
            forevertas::BuildInputScriptBaseline(
                    fixed,
                    {inside, late},
                    forevertas::kSearchTickDurationMs);
    if (!baseline) throw std::runtime_error(*baseline.error);
    Require(canonical.ReplaceInputs(baseline.events),
            "applying canonical late inputs");
    const std::vector<PhysicsSandboxInputEvent> stored = Require(
            canonical.ReadInputs(), "reading stored canonical late inputs");
    if (stored.size() != 3u ||
        stored.back().timeMs <= static_cast<std::int64_t>(horizon)) {
        std::cerr << "late canonical input was not preserved\n";
        return false;
    }
    const PhysicsSandboxStateView finalState = Require(
            canonical.AdvanceTicks(
                    horizon / forevertas::kSearchTickDurationMs),
            "advancing canonical horizon");
    return finalState.timeMs == horizon &&
            finalState.steering ==
                    static_cast<float>(inside.value.analog) /
                            forevervalidator::kAnalogInputScale;
}

PhysicsSandboxStateView RunCanonicalFixture(
        const char *packsDirectory,
        const std::filesystem::path &replayPath,
        forevervalidator::SimulationBackend backend) {
    using namespace forevervalidator;
    using namespace forevervalidator::experimental;
    const ReplayIdentity identity{replayPath.string()};
    const AssetBytes replay = Require(
            forevertas::ReadReplayFileUtf8(replayPath.string(), identity),
            "reading paired canonical fixture");
    PhysicsSandboxOptions options;
    options.backend = backend;
    options.tickDurationMs = forevertas::kSearchTickDurationMs;
    options.timelineMode = PhysicsSandboxTimelineMode::Canonical;
    options.simulationHorizonMs =
            forevertas::kDefaultSimulationHorizonMs;
    PhysicsSandbox sandbox = Require(
            CreatePhysicsSandbox(
                    Require(OpenInstalledPackDirectory(packsDirectory),
                            "opening paired fixture Packs"),
                    options),
            "creating paired canonical sandbox");
    Require(sandbox.LoadScenario({replay.data(), replay.size()}, identity),
            "loading paired canonical scenario fixture");
    std::vector<PhysicsSandboxInputEvent> inputs = Require(
            sandbox.ReadInputs(), "reading paired canonical inputs");
    inputs.push_back({10,
                      PhysicsSandboxInputAction::Accelerate,
                      {PhysicsSandboxInputValueKind::Switch,
                       PhysicsSandboxSwitchState::Pressed,
                       0}});
    inputs.push_back({10,
                      PhysicsSandboxInputAction::Steer,
                      {PhysicsSandboxInputValueKind::Analog,
                       PhysicsSandboxSwitchState::Released,
                       8192}});
    Require(sandbox.ReplaceInputs(std::move(inputs)),
            "applying paired canonical inputs");
    return Require(
            sandbox.AdvanceTicks(
                    forevertas::kDefaultSimulationHorizonMs /
                    forevertas::kSearchTickDurationMs),
            "advancing paired canonical fixture");
}

bool SameCanonicalResult(const PhysicsSandboxStateView &left,
                         const PhysicsSandboxStateView &right) {
    return left.timeMs == right.timeMs &&
            left.car.position.x == right.car.position.x &&
            left.car.position.y == right.car.position.y &&
            left.car.position.z == right.car.position.z &&
            left.car.rotationX == right.car.rotationX &&
            left.car.rotationY == right.car.rotationY &&
            left.car.rotationZ == right.car.rotationZ &&
            left.car.rotationW == right.car.rotationW &&
            left.car.linearSpeed.x == right.car.linearSpeed.x &&
            left.car.linearSpeed.y == right.car.linearSpeed.y &&
            left.car.linearSpeed.z == right.car.linearSpeed.z &&
            left.steering == right.steering &&
            left.accelerate == right.accelerate &&
            left.brake == right.brake &&
            left.checkpointsCollected == right.checkpointsCollected &&
            left.raceCompleted == right.raceCompleted &&
            left.finishTimeMs == right.finishTimeMs;
}

bool CheckPairedCanonicalFixtures(const char *packsDirectory,
                                  const char *replayPath) {
    const std::filesystem::path fixturesRoot =
            std::filesystem::path(replayPath).parent_path().parent_path();
    const std::filesystem::path first =
            fixturesRoot /
            "tmuf_exchange_1000_per_pair/Speed/DesertCar/"
            "7220162.Replay.Gbx";
    const std::filesystem::path second =
            fixturesRoot /
            "tmuf_exchange_4900/Speed/DesertCar/6966118.Replay.Gbx";
    if (!std::filesystem::is_regular_file(first) ||
        !std::filesystem::is_regular_file(second)) {
        return true;
    }
    const std::vector<forevervalidator::SimulationBackend> backends{
            forevervalidator::SimulationBackend::Reference,
            forevervalidator::SimulationBackend::OptimizedCpu,
#if FOREVERVALIDATOR_HAS_CUDA
            forevervalidator::SimulationBackend::Cuda,
#endif
    };
    for (const forevervalidator::SimulationBackend backend : backends) {
        if (!SameCanonicalResult(
                    RunCanonicalFixture(packsDirectory, first, backend),
                    RunCanonicalFixture(packsDirectory, second, backend))) {
            std::cerr << "paired canonical fixtures depended on recorded "
                         "controls, markers, or duration\n";
            return false;
        }
    }
    return true;
}

bool CheckStandaloneChallengeFixture(
        const char *packsDirectory,
        const char *pairedReplayPath,
        const char *challengePath) {
    using namespace forevervalidator;
    using namespace forevervalidator::experimental;
    const auto loadRecorded = [&](const char *path) {
        const ReplayIdentity identity{path};
        const AssetBytes bytes = Require(
                forevertas::ReadReplayFileUtf8(path, identity),
                "reading recorded scenario fixture");
        PhysicsSandboxOptions options;
        options.backend = SimulationBackend::Reference;
        options.tickDurationMs = forevertas::kSearchTickDurationMs;
        options.timelineMode = PhysicsSandboxTimelineMode::RecordedReplay;
        PhysicsSandbox sandbox = Require(
                CreatePhysicsSandbox(
                        Require(OpenInstalledPackDirectory(packsDirectory),
                                "opening recorded scenario Packs"),
                        options),
                "creating recorded scenario sandbox");
        return sandbox.LoadScenario(
                {bytes.data(), bytes.size()}, identity);
    };
    if (!loadRecorded(pairedReplayPath) || loadRecorded(challengePath)) {
        std::cerr << "standalone challenge entered recorded replay mode\n";
        return false;
    }
    const std::vector<forevervalidator::SimulationBackend> backends{
            forevervalidator::SimulationBackend::Reference,
            forevervalidator::SimulationBackend::OptimizedCpu,
#if FOREVERVALIDATOR_HAS_CUDA
            forevervalidator::SimulationBackend::Cuda,
#endif
    };
    for (const forevervalidator::SimulationBackend backend : backends) {
        if (!SameCanonicalResult(
                    RunCanonicalFixture(
                            packsDirectory, pairedReplayPath, backend),
                    RunCanonicalFixture(
                            packsDirectory, challengePath, backend))) {
            std::cerr << "standalone challenge did not match its paired "
                         "replay in canonical mode\n";
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    const bool inputAfterHorizonOnly =
            argc == 4 &&
            std::string_view(argv[1]) == "--input-after-horizon-only";
    if ((!inputAfterHorizonOnly && argc != 3 && argc != 5) ||
        (inputAfterHorizonOnly && argc != 4)) {
        std::cerr << "expected Packs directory, replay path, and optional "
                     "paired replay/challenge paths\n";
        return 2;
    }

    try {
        if (inputAfterHorizonOnly) {
            return CheckInputAfterHorizonAccepted(argv[2], argv[3])
                    ? 0
                    : 1;
        }
        if (!CheckAutoPromoteSemantics(argv[1], argv[2]) ||
            !CheckNoBestLiveReporting(argv[1], argv[2]) ||
            !CheckNoBestRunnerReporting(argv[1], argv[2]) ||
            !CheckModifierWindowClampedToHorizon(argv[1], argv[2]) ||
            !CheckCanonicalHorizonAndLateInputs(argv[1], argv[2]) ||
            !CheckPairedCanonicalFixtures(argv[1], argv[2]) ||
            (argc == 5 && !CheckStandaloneChallengeFixture(
                    argv[1], argv[3], argv[4])) ||
#if FOREVERVALIDATOR_HAS_CUDA
            !CheckCudaKernelModeLifecycle(argv[1], argv[2]) ||
            !CheckCudaAutoPromoteAcrossBatches(argv[1], argv[2]) ||
#endif
            !CheckCachedScriptIsolation(argv[1], argv[2]) ||
            !CheckCachedReplaySourceIdentity(argv[1], argv[2]) ||
            !CheckCachedPackSourceIdentity(argv[1], argv[2]) ||
            !CheckCachedHorizonIsolation(argv[1], argv[2]) ||
            !CheckKeyboardSteeringBaseline(argv[1], argv[2]) ||
            !CheckKeyboardSteeringPhysicsParity(argv[1], argv[2]) ||
            !CheckSandboxCloneAndWindowParity(argv[1], argv[2]) ||
            !CheckResizableCanonicalHorizon(argv[1], argv[2]) ||
            !CheckMultiThreadedCpuBackend(argv[1], argv[2]) ||
            !CheckStuntTargetBackend(
                    argv[1],
                    argv[2],
                    forevertas::PhysicsBackend::Reference) ||
            !CheckStuntTargetBackend(
                    argv[1],
                    argv[2],
                    forevertas::PhysicsBackend::OptimizedCpu) ||
            !RunBackend(argv[1],
                        argv[2],
                        forevertas::PhysicsBackend::Reference) ||
            !RunBackend(argv[1],
                        argv[2],
                        forevertas::PhysicsBackend::OptimizedCpu)
#if FOREVERVALIDATOR_HAS_CUDA
            || !CheckStuntTargetBackend(
                    argv[1],
                    argv[2],
                    forevertas::PhysicsBackend::Cuda)
            || !RunBackend(argv[1],
                           argv[2],
                           forevertas::PhysicsBackend::Cuda)
#endif
        ) {
            return 1;
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
