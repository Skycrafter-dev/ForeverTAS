#include "conditions/condition_program.h"
#include "mutations/input_event_utils.h"
#include "mutations/input_event_formatter.h"
#include "mutations/replay_input_script.h"
#include "physics_backend.h"
#include "replay_file_io.h"
#include "searches/algorithm_registry.h"
#include "searches/result_input_script.h"
#include "searches/search_runner.h"

#include <forevervalidator/experimental/physics_sandbox.h>
#include <forevervalidator/native.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <exception>
#include <fstream>
#include <future>
#include <iostream>
#include <iomanip>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#endif

namespace {

using forevertas::OptionConfiguration;
using forevertas::SearchRequest;
using forevertas::SearchResult;

const std::vector<forevertas::ParsedInputCommand> &ReplayInputCommands(
        const char *packs,
        const char *replay) {
    static std::mutex mutex;
    static std::map<std::pair<std::string, std::string>,
                    std::vector<forevertas::ParsedInputCommand>> cache;
    const std::pair<std::string, std::string> key{packs, replay};
    std::scoped_lock lock(mutex);
    const auto existing = cache.find(key);
    if (existing != cache.end()) {
        return existing->second;
    }
    forevertas::InputScriptParseResult parsed =
            forevertas::ParseInputScript(
                    forevertas::ExtractReplayInputScript(packs, replay));
    if (!parsed) {
        throw std::runtime_error(*parsed.error);
    }
    return cache.emplace(key, std::move(parsed.commands)).first->second;
}

bool SameInputs(
        const std::vector<forevertas::SandboxInputEvent> &left,
        const std::vector<forevertas::SandboxInputEvent> &right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0u; index < left.size(); ++index) {
        if (!forevertas::SameInputEvent(left[index], right[index])) {
            return false;
        }
    }
    return true;
}

SearchResult Run(const char *packs,
                 const char *replay,
                 forevertas::PhysicsBackend backend,
                 std::uint32_t batchSize,
                 std::uint64_t iterations,
                 std::vector<OptionConfiguration> modifiers,
                 OptionConfiguration evaluator,
                 bool calibrate = false,
                 std::vector<std::uint32_t> *calibrationUpdates = nullptr,
                 bool sampleBestTimeline = false,
                 std::optional<std::int64_t>
                         evaluationEndTimeLimitMs = std::nullopt,
                 bool *calibrationCompleted = nullptr,
                 bool stopAfterCalibration = false,
                 bool autoPromoteBest = false,
                 std::uint32_t simulationHorizonMs =
                         forevertas::kDefaultSimulationHorizonMs,
                 const std::string &conditionScript = {},
                 std::uint64_t *winnerResolutionCount = nullptr) {
    SearchRequest request{packs, replay};
    request.baseInputCommands = ReplayInputCommands(packs, replay);
    request.backend = backend;
    request.parallelSampleCount = batchSize;
    request.calibrateCudaParallelSampleCount = calibrate;
    request.simulationHorizonMs = simulationHorizonMs;
    request.searchAlgorithm.settings["autoPromoteBest"] =
            autoPromoteBest ? "true" : "false";
    request.modifiers = std::move(modifiers);
    request.evaluationTarget = std::move(evaluator);
    const forevertas::ConditionCompileResult condition =
            forevertas::CompileConditionScript(conditionScript);
    if (condition.error) {
        throw std::runtime_error(*condition.error);
    }
    request.condition = condition.program;
    forevertas::SearchRunControl control;
    control.iterationLimit = iterations;
    control.sampleBestTimeline = sampleBestTimeline;
    control.evaluationEndTimeLimitMs = evaluationEndTimeLimitMs;
    control.reuseLoadedSandbox = true;
    control.cudaBatchSizeChanged =
            [calibrationUpdates](std::uint32_t value) {
                if (calibrationUpdates != nullptr &&
                    (calibrationUpdates->empty() ||
                     calibrationUpdates->back() != value)) {
                    calibrationUpdates->push_back(value);
                }
            };
    control.cudaWinnerResolved = [winnerResolutionCount]() {
        if (winnerResolutionCount != nullptr) {
            ++*winnerResolutionCount;
        }
    };
    bool calibrationFinished = false;
    control.progressChanged =
            [calibrationCompleted, &calibrationFinished](
                    const forevertas::SearchProgress &progress) {
                if (progress.stage ==
                            forevertas::SearchProgressStage::
                                    Mutations) {
                    calibrationFinished = true;
                    if (calibrationCompleted != nullptr) {
                        *calibrationCompleted = true;
                    }
                }
            };
    control.stopRequested =
            [stopAfterCalibration, &calibrationFinished]() {
                return stopAfterCalibration &&
                        calibrationFinished;
            };
    return forevertas::RunSearch(request, &control);
}

bool SameAuthoritativeResult(const SearchResult &reference,
                             const SearchResult &cuda,
                             const std::string &label) {
    const bool same =
            reference.winnerSource == cuda.winnerSource &&
            reference.winningIterationIndex ==
                    cuda.winningIterationIndex &&
            reference.winningMutationCount ==
                    cuda.winningMutationCount &&
            reference.bestScore == cuda.bestScore &&
            reference.bestEvaluationTimeMs ==
                    cuda.bestEvaluationTimeMs &&
            reference.objectiveScores == cuda.objectiveScores &&
            reference.metricValues == cuda.metricValues &&
            reference.iterations == cuda.iterations &&
            (reference.mutationImprovementCount > 0u) ==
                    (cuda.mutationImprovementCount > 0u) &&
            SameInputs(reference.bestInputs, cuda.bestInputs);
    if (!same) {
        std::cerr << label
                  << " parity failed: reference winner="
                  << (reference.winningIterationIndex
                              ? std::to_string(
                                        *reference.winningIterationIndex)
                              : "baseline")
                  << " CUDA winner="
                  << (cuda.winningIterationIndex
                              ? std::to_string(
                                        *cuda.winningIterationIndex)
                              : "baseline")
                  << " reference score=" << reference.bestScore
                  << " CUDA score=" << cuda.bestScore
                  << " reference mutations="
                  << reference.totalMutationCount
                  << " CUDA mutations=" << cuda.totalMutationCount
                  << " sameWinner="
                  << (reference.winnerSource == cuda.winnerSource)
                  << " sameIteration="
                  << (reference.winningIterationIndex ==
                      cuda.winningIterationIndex)
                  << " sameMutationCount="
                  << (reference.winningMutationCount ==
                      cuda.winningMutationCount)
                  << " sameScore=" << (reference.bestScore == cuda.bestScore)
                  << " sameEvaluationTime="
                  << (reference.bestEvaluationTimeMs ==
                      cuda.bestEvaluationTimeMs)
                  << " sameIterations="
                  << (reference.iterations == cuda.iterations)
                  << " sameImprovementPresence="
                  << ((reference.mutationImprovementCount > 0u) ==
                      (cuda.mutationImprovementCount > 0u))
                  << "(" << reference.mutationImprovementCount
                  << "/" << cuda.mutationImprovementCount << ")"
                  << " sameInputs="
                  << SameInputs(reference.bestInputs, cuda.bestInputs)
                  << '\n';
    }
    return same;
}

OptionConfiguration DefaultModifier(const std::string &id) {
    const auto *registration = forevertas::FindModifier(id);
    if (registration == nullptr) {
        throw std::runtime_error("missing modifier registration: " + id);
    }
    return {registration->id, registration->defaultSettings};
}

OptionConfiguration DefaultEvaluator(const std::string &id) {
    const auto *registration =
            forevertas::FindEvaluationTarget(id);
    if (registration == nullptr) {
        throw std::runtime_error(
                "missing evaluator registration: " + id);
    }
    return {registration->id, registration->defaultSettings};
}

bool CheckParity(const char *packs,
                 const char *replay,
                 const std::string &label,
                 std::uint32_t batchSize,
                 std::uint64_t iterations,
                 const std::vector<OptionConfiguration> &modifiers,
                 const OptionConfiguration &evaluator,
                 bool requireMutationWinner = false,
                 double *cudaSeconds = nullptr,
                 std::optional<std::int64_t>
                         evaluationEndTimeLimitMs = std::nullopt,
                 const std::string &conditionScript = {},
                 std::uint32_t simulationHorizonMs =
                         forevertas::kDefaultSimulationHorizonMs) {
    const auto started = std::chrono::steady_clock::now();
    std::cout << "checking " << label << std::endl;
    std::future<SearchResult> reference = std::async(
            std::launch::async,
            [=]() {
                return Run(
                        packs,
                        replay,
                        forevertas::PhysicsBackend::Reference,
                        1u,
                        iterations,
                        modifiers,
                        evaluator,
                        false,
                        nullptr,
                        false,
                        evaluationEndTimeLimitMs,
                        nullptr,
                        false,
                        false,
                        simulationHorizonMs,
                        conditionScript);
            });
    const auto cudaStarted = std::chrono::steady_clock::now();
    const SearchResult cuda = Run(
            packs, replay, forevertas::PhysicsBackend::Cuda,
            batchSize, iterations, modifiers, evaluator,
            false, nullptr, false, evaluationEndTimeLimitMs,
            nullptr, false, false,
            simulationHorizonMs,
            conditionScript);
    const SearchResult authoritative = reference.get();
    if (cudaSeconds != nullptr) {
        *cudaSeconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - cudaStarted)
                               .count();
    }
    std::cout << label << " winner="
              << (cuda.winningIterationIndex
                          ? std::to_string(
                                    *cuda.winningIterationIndex)
                          : "baseline")
              << " seconds="
              << std::chrono::duration<double>(
                         std::chrono::steady_clock::now() - started)
                         .count()
              << std::endl;
    const bool mutationWinner =
            cuda.winningIterationIndex.has_value();
    if (requireMutationWinner && !mutationWinner) {
        std::cerr << label
                  << " did not exercise winning candidate data\n";
    }
    return SameAuthoritativeResult(authoritative, cuda, label) &&
            (!requireMutationWinner || mutationWinner);
}

bool CheckEquivalentDeletionPrefersFewerInputs(
        const char *packs,
        const char *replay) {
    OptionConfiguration deletion = DefaultModifier(
            forevertas::kInputDeletionModifierId);
    deletion.settings["minTimeMs"] = "10";
    deletion.settings["maxTimeMs"] = "5990";
    deletion.settings["steerEnabled"] = "true";
    deletion.settings["steerMaxCount"] = "4";
    deletion.settings["accelerateEnabled"] = "true";
    deletion.settings["accelerateMaxCount"] = "2";
    deletion.settings["brakeEnabled"] = "true";
    deletion.settings["brakeMaxCount"] = "2";

    OptionConfiguration stunt = DefaultEvaluator(
            forevertas::kStuntPointsEvaluationId);
    stunt.settings["targetTimeMs"] = "6000";

    const SearchResult baseline = Run(
            packs,
            replay,
            forevertas::PhysicsBackend::Reference,
            1u,
            0u,
            {deletion},
            stunt,
            false,
            nullptr,
            false);
    if (baseline.bestInputs.empty()) {
        std::cerr << "deletion equivalence fixture has no baseline inputs\n";
        return false;
    }

    constexpr std::uint64_t iterations = 128u;
    const SearchResult reference = Run(
            packs,
            replay,
            forevertas::PhysicsBackend::Reference,
            1u,
            iterations,
            {deletion},
            stunt,
            false,
            nullptr,
            false);
    const SearchResult optimized = Run(
            packs,
            replay,
            forevertas::PhysicsBackend::OptimizedCpu,
            1u,
            iterations,
            {deletion},
            stunt,
            false,
            nullptr,
            false);
    const SearchResult multiThreaded = Run(
            packs,
            replay,
            forevertas::PhysicsBackend::MultiThreadedCpu,
            4u,
            iterations,
            {deletion},
            stunt,
            false,
            nullptr,
            false);
    const SearchResult cuda = Run(
            packs,
            replay,
            forevertas::PhysicsBackend::Cuda,
            64u,
            iterations,
            {deletion},
            stunt,
            false,
            nullptr,
            false,
            std::nullopt,
            nullptr,
            false);

    const bool simplified =
            reference.winnerSource ==
                    forevertas::SearchWinnerSource::Mutation &&
            reference.bestScore == baseline.bestScore &&
            reference.bestInputs.size() < baseline.bestInputs.size() &&
            reference.mutationImprovementCount > 0u;
    if (!simplified) {
        std::cerr
                << "equivalent deletion did not simplify the reference run: "
                << "baseline_inputs=" << baseline.bestInputs.size()
                << " best_inputs=" << reference.bestInputs.size()
                << " baseline_score=" << baseline.bestScore
                << " best_score=" << reference.bestScore
                << " winner="
                << (reference.winnerSource ==
                                    forevertas::SearchWinnerSource::Mutation
                            ? "mutation" : "baseline")
                << '\n';
        return false;
    }

    bool okay = true;
    okay &= SameAuthoritativeResult(
            reference, optimized,
            "equivalent deletion optimized CPU");
    okay &= SameAuthoritativeResult(
            reference, multiThreaded,
            "equivalent deletion multi-threaded CPU");
    okay &= SameAuthoritativeResult(
            reference, cuda,
            "equivalent deletion CUDA");
    return okay;
}

bool CheckCancellation(const char *packs, const char *replay) {
    SearchRequest request{packs, replay};
    request.baseInputCommands = ReplayInputCommands(packs, replay);
    request.backend = forevertas::PhysicsBackend::Cuda;
    request.parallelSampleCount = 4096u;
    forevertas::SearchRunControl control;
    control.reuseLoadedSandbox = true;
    std::chrono::steady_clock::time_point mutationStarted{};
    control.progressChanged =
            [&](const forevertas::SearchProgress &progress) {
                if (progress.stage ==
                    forevertas::SearchProgressStage::Mutations) {
                    mutationStarted = std::chrono::steady_clock::now();
                }
            };
    control.cancellationRequested = [&]() {
        return mutationStarted.time_since_epoch().count() != 0 &&
                std::chrono::steady_clock::now() - mutationStarted >
                        std::chrono::milliseconds(5);
    };
    try {
        static_cast<void>(forevertas::RunSearch(request, &control));
    } catch (const forevertas::SearchCancelled &) {
        return true;
    }
    std::cerr << "running CUDA batch ignored cancellation\n";
    return false;
}

// The Distance target around walls on the GPU (tables built on the CPU,
// queried in the kernel) against the optimized CPU: the same winners and
// scores, to a checkpoint the window reaches or to the finish.
bool CheckDistanceAroundWallsParity(const char *packs, const char *replay,
                                    const std::string &event) {
    OptionConfiguration insertion = DefaultModifier(
            forevertas::kInputInsertionModifierId);
    insertion.settings["minTimeMs"] = "2000";
    insertion.settings["maxTimeMs"] = "6000";
    insertion.settings["steerMinCount"] = "1";
    insertion.settings["steerMaxCount"] = "2";
    OptionConfiguration distance = DefaultEvaluator(
            forevertas::kDistanceTargetEvaluationId);
    distance.settings["route"] =
            std::getenv("FTAS_DISTANCE_ROUTE") ? std::getenv("FTAS_DISTANCE_ROUTE") : "around";
    distance.settings["eventType"] = event;
    distance.settings["checkpointIndex"] = "2";
    distance.settings["minTimeMs"] = "1000";
    const std::uint32_t horizon = event == "finish" ? 70000u : 12000u;
    distance.settings["maxTimeMs"] = std::to_string(horizon);
    const auto run = [&](forevertas::PhysicsBackend backend, std::uint32_t batch) {
        const char *count = std::getenv("FTAS_DISTANCE_ITERATIONS");
        return Run(packs, replay, backend, batch, count ? std::stoull(count) : 6u, {insertion},
                   distance, false, nullptr,
                   false, std::nullopt, nullptr, false, false, horizon);
    };
    const auto started = std::chrono::steady_clock::now();
    const SearchResult cpu = run(forevertas::PhysicsBackend::OptimizedCpu, 1u);
    const auto cudaStarted = std::chrono::steady_clock::now();
    std::cout << "CPU done in " << std::chrono::duration<double>(cudaStarted - started).count()
              << " s, score " << cpu.bestScore << std::endl;
    const SearchResult cuda = run(forevertas::PhysicsBackend::Cuda, 4u);
    std::cout << "distance around walls to " << event << ": score " << cuda.bestScore
              << " (CPU " << cpu.bestScore << "), CPU "
              << std::chrono::duration<double>(cudaStarted - started).count() << " s, CUDA "
              << std::chrono::duration<double>(std::chrono::steady_clock::now() - cudaStarted)
                         .count()
              << " s" << std::endl;
    return SameAuthoritativeResult(cpu, cuda, "distance around walls to " + event);
}

bool CheckCalibration(const char *packs, const char *replay) {
    OptionConfiguration insertion = DefaultModifier(
            forevertas::kInputInsertionModifierId);
    insertion.settings["minTimeMs"] = "1000";
    insertion.settings["maxTimeMs"] = "1000";
    insertion.settings["steerMinCount"] = "1";
    insertion.settings["steerMaxCount"] = "1";
    insertion.settings["steerMaxHoldMs"] = "0";
    insertion.settings["steerOffsetMin"] = "0.1";
    insertion.settings["steerOffsetMax"] = "0.1";
    OptionConfiguration velocity = DefaultEvaluator(
            forevertas::kVelocityEvaluationId);
    velocity.settings["minTimeMs"] = "1010";
    velocity.settings["maxTimeMs"] = "1010";

    std::vector<std::uint32_t> updates;
    bool calibrationCompleted = false;
    const SearchResult calibrated = Run(
            packs,
            replay,
            forevertas::PhysicsBackend::Cuda,
            64u,
            3000000u,
            {insertion},
            velocity,
            true,
            &updates,
            false,
            std::nullopt,
            &calibrationCompleted,
            true);
    const bool grew = std::any_of(
            updates.begin(),
            updates.end(),
            [](std::uint32_t value) {
                return value > 1u;
            });
    if (updates.size() < 3u || updates.front() != 1u || !grew ||
        !calibrationCompleted) {
        std::cerr
                << "real CUDA calibration depended on the configured "
                   "batch size, did not grow, or did not complete; "
                   "completed="
                << calibrationCompleted << " updates=";
        for (std::uint32_t update : updates) {
            std::cerr << update << ',';
        }
        std::cerr << '\n';
        return false;
    }
    return calibrated.iterations != 0u &&
            calibrated.evaluatorCalls != 0u;
}

// One search on two GPUs (here twice this one): each takes its own candidates,
// both search, and what they report adds up.
bool CheckGpuPool(const char *packs, const char *replay) {
    OptionConfiguration velocity = DefaultEvaluator(forevertas::kVelocityEvaluationId);
    velocity.settings["minTimeMs"] = "2500";
    velocity.settings["maxTimeMs"] = "2500";
    OptionConfiguration steering = DefaultModifier(forevertas::kRandomSteeringModifierId);
    steering.settings["minTimeMs"] = "500";
    steering.settings["maxTimeMs"] = "2000";
    SearchRequest request{packs, replay};
    request.simulationHorizonMs = 2600u;
    request.baseInputCommands = ReplayInputCommands(packs, replay);
    request.backend = forevertas::PhysicsBackend::Cuda;
    request.parallelSampleCount = 2048u;
    request.searchAlgorithm.settings["autoPromoteBest"] = "true";
    request.modifiers = {steering};
    request.evaluationTarget = velocity;
    const std::vector<forevertas::SearchDevice> devices{
            {forevertas::PhysicsBackend::Cuda, 2048u, false},
            {forevertas::PhysicsBackend::Cuda, 1024u, false}};
    std::mutex mutex;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> claimed;
    std::vector<std::uint64_t> workerIterations;
    std::uint64_t sharedBests = 0u;
    std::size_t smallestStack = SIZE_MAX;
    const forevertas::DeviceSearchRunner runner =
            [&](const SearchRequest &deviceRequest, const forevertas::SearchRunControl *control) {
                // a search thread's stack (Windows gives a thread 1 MB: a GPU
                // session's set-up overflowed it on an RX 9070 XT)
                std::size_t stack = forevertas::kSearchThreadStackBytes;
#if defined(__linux__)
                pthread_attr_t attributes;
                if (pthread_getattr_np(pthread_self(), &attributes) == 0) {
                    pthread_attr_getstacksize(&attributes, &stack);
                    pthread_attr_destroy(&attributes);
                }
#endif
                {
                    std::scoped_lock lock(mutex);
                    smallestStack = std::min(smallestStack, stack);
                }
                forevertas::SearchRunControl watched = *control;
                watched.claimIterations = [&, claim = control->claimIterations](std::uint32_t count) {
                    const std::uint64_t first = claim(count);
                    std::scoped_lock lock(mutex);
                    claimed.emplace_back(first, count);
                    return first;
                };
                watched.promotedBaselineInputs = [&, promoted = control->promotedBaselineInputs]() {
                    auto inputs = promoted();
                    if (inputs) {
                        std::scoped_lock lock(mutex);
                        ++sharedBests;
                    }
                    return inputs;
                };
                SearchResult result = forevertas::RunSearch(deviceRequest, &watched);
                std::scoped_lock lock(mutex);
                workerIterations.push_back(result.iterations);
                return result;
            };
    forevertas::SearchRunControl control;
    control.iterationLimit = 100000u;
    const auto started = std::chrono::steady_clock::now();
    const SearchResult result = forevertas::RunSearchOnDevices(request, devices, runner, &control);
    std::sort(claimed.begin(), claimed.end());
    bool disjoint = !claimed.empty();
    for (std::size_t i = 1u; i < claimed.size(); ++i) {
        disjoint &= claimed[i - 1u].first + claimed[i - 1u].second <= claimed[i].first;
    }
    std::uint64_t total = 0u;
    for (const std::uint64_t iterations : workerIterations) total += iterations;
    std::cout << "gpu pool: " << result.iterations << " attempts (" << workerIterations.size()
              << " workers), " << claimed.size() << " batches, " << result.mutationImprovementCount
              << " improvements, " << sharedBests << " shared bests offered, score " << result.bestScore
              << ", " << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()
              << " s" << std::endl;
    const bool okay = smallestStack >= forevertas::kSearchThreadStackBytes &&
            workerIterations.size() == 2u && workerIterations[0] != 0u && workerIterations[1] != 0u &&
            total == result.iterations && disjoint &&
            result.winnerSource == forevertas::SearchWinnerSource::Mutation &&
            result.mutationImprovementCount != 0u;
    if (!okay) {
        std::cerr << "GPU pool: workers " << workerIterations.size() << ", total " << total << " vs "
                  << result.iterations << ", disjoint " << disjoint << ", worker stack " << smallestStack
                  << " bytes\n";
    }
    return okay;
}

// Attempts per second for a few seconds, and the time outside the batches
// (with FOREVERTAS_CUDA_PROFILE=1 every batch's own time is printed).
bool MeasureThroughput(const char *packs, const char *replay, bool approximate, bool cpu, double seconds) {
    OptionConfiguration velocity = DefaultEvaluator(forevertas::kVelocityEvaluationId);
    velocity.settings["minTimeMs"] = "1000";
    velocity.settings["maxTimeMs"] = "1000";
    SearchRequest request{packs, replay};
    request.baseInputCommands = ReplayInputCommands(packs, replay);
    request.backend = cpu ? forevertas::PhysicsBackend::MultiThreadedCpu : forevertas::PhysicsBackend::Cuda;
    request.parallelSampleCount = cpu ? forevertas::DefaultCpuWorkerCount() : 12000u;
    request.approximatePhysics = approximate;
    request.simulationHorizonMs = 5000u;
    const char *promote = std::getenv("FTAS_PROMOTE");
    request.searchAlgorithm.settings["autoPromoteBest"] =
            promote == nullptr || std::string(promote) != "0" ? "true" : "false";
    request.modifiers = {DefaultModifier(forevertas::kInputDeletionModifierId),
                         DefaultModifier(forevertas::kSmoothSteeringModifierId)};
    for (OptionConfiguration &modifier : request.modifiers) {
        modifier.settings["minTimeMs"] = "10";
        modifier.settings["maxTimeMs"] = "990";
    }
    request.evaluationTarget = velocity;
    forevertas::SearchRunControl control;
    std::chrono::steady_clock::time_point searching{};
    control.progressChanged = [&](const forevertas::SearchProgress &progress) {
        if (progress.stage == forevertas::SearchProgressStage::Mutations &&
            searching == std::chrono::steady_clock::time_point{}) {
            searching = std::chrono::steady_clock::now();
        }
    };
    std::uint64_t attemptsAtStop = 0u;
    std::uint64_t attemptsSoFar = 0u;
    std::chrono::steady_clock::time_point stoppedAt{};
    control.statisticsChanged = [&](const forevertas::SearchStatisticsUpdate &statistics) {
        attemptsSoFar = statistics.iterations;
    };
    control.liveChanged = [&](const forevertas::SearchLiveUpdate &live) { attemptsSoFar = live.iterations; };
    control.stopRequested = [&]() {
        const bool stop = searching != std::chrono::steady_clock::time_point{} &&
                std::chrono::steady_clock::now() - searching > std::chrono::duration<double>(seconds);
        if (stop && stoppedAt == std::chrono::steady_clock::time_point{}) {
            stoppedAt = std::chrono::steady_clock::now();
            attemptsAtStop = attemptsSoFar;
        }
        return stop;
    };
    // FTAS_BASE_SECONDS=s: first a promoted search for s seconds; its best is this search's base run
    if (const char *baseSeconds = std::getenv("FTAS_BASE_SECONDS"); baseSeconds != nullptr && std::atof(baseSeconds) > 0.0) {
        SearchRequest first = request;
        first.searchAlgorithm.settings["autoPromoteBest"] = "true";
        forevertas::SearchRunControl firstControl;
        const auto firstStarted = std::chrono::steady_clock::now();
        const double firstSeconds = std::atof(baseSeconds);
        firstControl.stopRequested = [&]() {
            return std::chrono::steady_clock::now() - firstStarted > std::chrono::duration<double>(firstSeconds);
        };
        // FTAS_BASE_ATTEMPTS=n: that many attempts instead (the same base run every time)
        if (const char *attempts = std::getenv("FTAS_BASE_ATTEMPTS"); attempts != nullptr) {
            firstControl.stopRequested = {};
            firstControl.iterationLimit = std::strtoull(attempts, nullptr, 10);
        }
        const SearchResult based = forevertas::RunSearch(first, &firstControl);
        const forevertas::InputScriptParseResult parsed = forevertas::ParseInputScript(
                forevertas::FormatResultInputScript(based.bestInputs, request.evaluationTarget.id, based.bestState));
        if (!parsed) throw std::runtime_error(*parsed.error);
        request.baseInputCommands = parsed.commands;
        std::cout << "base run: the best of a promoted search, " << based.mutationImprovementCount << " improvements, "
                  << based.bestEvaluationDescription << std::endl;
    }
    // FTAS_ATTEMPTS=n: that many attempts instead of SECONDS (the same search every time)
    if (const char *attempts = std::getenv("FTAS_ATTEMPTS"); attempts != nullptr) {
        control.stopRequested = {};
        control.iterationLimit = std::strtoull(attempts, nullptr, 10);
    }
    const SearchResult result = forevertas::RunSearch(request, &control);
    if (std::getenv("FTAS_ATTEMPTS") != nullptr) {
        std::cout.precision(17);
        std::cout << "result: score " << result.bestScore << ", " << result.bestInputs.size() << " inputs, "
                  << result.mutationImprovementCount << " improvements, iteration "
                  << result.winningIterationIndex.value_or(0u) << std::endl;
    }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - searching).count();
    std::cout << (cpu ? "cpu " : "gpu ") << (approximate ? "approximate" : "exact") << ": " << result.iterations << " attempts in "
              << elapsed << " s searching, " << result.iterations / elapsed << " per second, "
              << result.mutationImprovementCount << " improvements; while searching: "
              << attemptsAtStop / std::chrono::duration<double>(stoppedAt - searching).count() << " per second" << std::endl;
    return result.iterations != 0u;
}

bool CheckPreciseFinishParity(const char *packs, const char *replay) {
    constexpr std::uint64_t iterations = 32u;
    const std::vector<OptionConfiguration> modifiers{
            DefaultModifier(
                    forevertas::kRandomSteeringModifierId)};
    const OptionConfiguration evaluator = DefaultEvaluator(
            forevertas::kPreciseFinishTimeEvaluationId);
    std::future<SearchResult> referenceFuture = std::async(
            std::launch::async,
            [=]() {
                return Run(
                        packs,
                        replay,
                        forevertas::PhysicsBackend::Reference,
                        1u,
                        iterations,
                        modifiers,
                        evaluator,
                        false, nullptr, false, std::nullopt,
                        nullptr, false, true, 30000u);
            });
    std::future<SearchResult> optimizedFuture = std::async(
            std::launch::async,
            [=]() {
                return Run(
                        packs,
                        replay,
                        forevertas::PhysicsBackend::OptimizedCpu,
                        1u,
                        iterations,
                        modifiers,
                        evaluator,
                        false, nullptr, false, std::nullopt,
                        nullptr, false, true, 30000u);
            });
    const SearchResult cuda = Run(
            packs,
            replay,
            forevertas::PhysicsBackend::Cuda,
            static_cast<std::uint32_t>(iterations),
            iterations,
            modifiers,
            evaluator,
            false, nullptr, false, std::nullopt,
            nullptr, false, true, 30000u);
    const SearchResult reference = referenceFuture.get();
    const SearchResult optimized = optimizedFuture.get();
    OptionConfiguration promotionModifier = DefaultModifier(
            forevertas::kExistingEventPerturbationModifierId);
    promotionModifier.settings["minTimeMs"] = "4000";
    promotionModifier.settings["maxTimeMs"] = "5720";
    promotionModifier.settings["minCount"] = "1";
    promotionModifier.settings["maxCount"] = "3";
    const SearchResult promotionProbe = Run(
            packs,
            replay,
            forevertas::PhysicsBackend::Cuda,
            40000u,
            40001u,
            {promotionModifier},
            evaluator,
            false, nullptr, false, std::nullopt,
            nullptr, false, true, 30000u);
    if (promotionProbe.iterations != 40001u ||
        promotionProbe.mutationImprovementCount == 0u ||
        !promotionProbe.winningIterationIndex) {
        std::cerr << "precise finish CUDA did not exercise auto-promotion "
                     "and its seeded follow-up batch\n";
        return false;
    }
    const auto exactFinishResult =
            [](const SearchResult &result, const char *label) {
                if (!result.bestState.raceCompleted ||
                    !result.bestState.finishTime.has_value() ||
                    !result.bestState.finishTime->IsValid()) {
                    std::cerr << label
                              << " did not expose an exact finish interval\n";
                    return false;
                }
                const std::uint64_t upperBoundNs =
                        result.bestState.finishTime->upperBoundNs;
                const bool exactScore =
                        result.bestScore ==
                                static_cast<double>(upperBoundNs) &&
                        result.bestEvaluationTimeMs ==
                                static_cast<double>(upperBoundNs) /
                                        1000000.0;
                if (!exactScore) {
                    std::cerr << label
                              << " score does not match its finish interval\n";
                }
                return exactScore;
            };
    return exactFinishResult(reference, "precise finish reference") &&
            exactFinishResult(
                    optimized, "precise finish optimized CPU") &&
            exactFinishResult(cuda, "precise finish CUDA") &&
            SameAuthoritativeResult(
                   reference, optimized, "precise finish optimized CPU") &&
            SameAuthoritativeResult(
                    reference, cuda, "precise finish CUDA");
}

bool CheckUnchangedIncumbentIsNotReconstructed(const char *packs,
                                                const char *replay) {
    std::uint64_t resolutions = 0u;
    static_cast<void>(Run(
            packs,
            replay,
            forevertas::PhysicsBackend::Cuda,
            32u,
            64u,
            {DefaultModifier(
                    forevertas::kExistingEventPerturbationModifierId)},
            DefaultEvaluator(forevertas::kVelocityEvaluationId),
            false,
            nullptr,
            false,
            std::nullopt,
            nullptr,
            false,
            false,
            forevertas::kDefaultSimulationHorizonMs,
            "iterations = 0",
            &resolutions));
    if (resolutions != 1u) {
        std::cerr << "unchanged CUDA incumbent was reconstructed "
                  << resolutions << " times instead of once\n";
        return false;
    }
    return true;
}

bool CheckCheckpointConditionParity(const char *packs,
                                    const char *replay) {
    OptionConfiguration velocity = DefaultEvaluator(
            forevertas::kVelocityEvaluationId);
    velocity.settings["minTimeMs"] = "1000";
    velocity.settings["maxTimeMs"] = "1000";
    OptionConfiguration random = DefaultModifier(
            forevertas::kRandomSteeringModifierId);
    random.settings["minTimeMs"] = "0";
    random.settings["maxTimeMs"] = "0";
    constexpr std::uint32_t probeHorizonMs = 60000u;
    const SearchResult probe = Run(
            packs,
            replay,
            forevertas::PhysicsBackend::Reference,
            1u,
            0u,
            {random},
            velocity,
            false,
            nullptr,
            true,
            std::nullopt,
            nullptr,
            false,
            false,
            probeHorizonMs);
    const auto checkpoint = std::find_if(
            probe.bestTimeline.begin(),
            probe.bestTimeline.end(),
            [](const forevertas::SearchTimelineFrame &frame) {
                return frame.checkpointsCollected > 0u;
            });
    if (checkpoint == probe.bestTimeline.end()) {
        std::cerr << "checkpoint condition fixture did not collect a checkpoint\n";
        return false;
    }

    OptionConfiguration atCheckpoint = velocity;
    atCheckpoint.settings["minTimeMs"] =
            std::to_string(checkpoint->timeMs);
    atCheckpoint.settings["maxTimeMs"] =
            std::to_string(checkpoint->timeMs);
    const std::string condition =
            "car.cps = " + std::to_string(checkpoint->checkpointsCollected);
    const std::uint32_t horizonMs = static_cast<std::uint32_t>(
            std::max<std::int64_t>(
                    checkpoint->timeMs +
                            static_cast<std::int64_t>(
                                    forevertas::kSearchTickDurationMs),
                    1000));
    const auto run = [&](forevertas::PhysicsBackend backend) {
        return Run(
                packs,
                replay,
                backend,
                backend == forevertas::PhysicsBackend::Cuda ? 32u : 1u,
                0u,
                {random},
                atCheckpoint,
                false,
                nullptr,
                false,
                std::nullopt,
                nullptr,
                false,
                false,
                horizonMs,
                condition);
    };
    const SearchResult reference = run(
            forevertas::PhysicsBackend::Reference);
    const SearchResult optimized = run(
            forevertas::PhysicsBackend::OptimizedCpu);
    const SearchResult cudaRegular = run(
            forevertas::PhysicsBackend::Cuda);
    return SameAuthoritativeResult(
                   reference, optimized,
                   "checkpoint condition optimized CPU") &&
            SameAuthoritativeResult(
                    reference, cudaRegular,
                    "checkpoint condition CUDA");
}

SearchResult RunFixedScript(const char *packs,
                            const char *scenario,
                            const std::string &script,
                            forevertas::PhysicsBackend backend,
                            std::uint32_t horizonMs) {
    const forevertas::InputScriptParseResult parsed =
            forevertas::ParseInputScript(script);
    if (!parsed) throw std::runtime_error(*parsed.error);
    SearchRequest request{packs, scenario};
    request.baseInputCommands = parsed.commands;
    request.backend = backend;
    request.parallelSampleCount = 1u;
    request.simulationHorizonMs = horizonMs;
    request.evaluationTarget =
            DefaultEvaluator(forevertas::kPreciseFinishTimeEvaluationId);
    forevertas::SearchRunControl control;
    control.iterationLimit = 0u;
    control.sampleBestTimeline = true;
    return forevertas::RunSearch(request, &control);
}

struct RawFrame {
    std::uint64_t timeMs = 0u;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    bool completed = false;
    std::optional<std::uint32_t> finishTimeMs;
};

std::vector<RawFrame> SimulateFixedScript(
        const char *packs,
        const char *scenario,
        const std::vector<forevertas::SandboxInputEvent> &inputs,
        forevertas::PhysicsBackend backend,
        std::uint32_t horizonMs) {
    using namespace forevervalidator;
    using namespace forevervalidator::experimental;
    PhysicsSandboxOptions options;
    options.backend = forevertas::ToForeverValidatorBackend(backend);
    options.tickDurationMs = forevertas::kSearchTickDurationMs;
    options.timelineMode = PhysicsSandboxTimelineMode::Canonical;
    options.simulationHorizonMs = horizonMs;
    auto source = OpenInstalledPackDirectory(packs);
    if (!source) throw std::runtime_error("could not open Packs directory");
    auto created = CreatePhysicsSandbox(std::move(source).Value(), options);
    if (!created) throw std::runtime_error("could not create raw sandbox");
    PhysicsSandbox sandbox = std::move(created).Value();
    const ReplayIdentity identity{scenario};
    auto bytes = forevertas::ReadReplayFileUtf8(scenario, identity);
    if (!bytes) throw std::runtime_error("could not read raw scenario");
    AssetBytes scenarioBytes = std::move(bytes).Value();
    if (!sandbox.LoadScenario(
                {scenarioBytes.data(), scenarioBytes.size()}, identity)) {
        throw std::runtime_error("could not load raw scenario");
    }
    if (!sandbox.ReplaceInputs(inputs)) {
        throw std::runtime_error("could not install raw sandbox inputs");
    }
    auto read = sandbox.ReadState();
    if (!read) throw std::runtime_error("could not read raw sandbox state");
    PhysicsSandboxStateView state = read.Value();
    std::vector<RawFrame> frames;
    frames.reserve(horizonMs / forevertas::kSearchTickDurationMs + 1u);
    const auto append = [&]() {
        frames.push_back({state.timeMs,
                          state.car.position.x,
                          state.car.position.y,
                          state.car.position.z,
                          state.raceCompleted,
                          state.finishTimeMs});
    };
    append();
    while (state.timeMs < horizonMs && !state.raceCompleted) {
        auto advanced = sandbox.AdvanceTicks(1u);
        if (!advanced) throw std::runtime_error("raw simulation failed");
        state = advanced.Value();
        append();
    }
    return frames;
}

bool DiagnoseFixedScript(const char *packs,
                         const char *scenario,
                         const char *scriptPath,
                         std::uint32_t horizonMs) {
    std::ifstream file(scriptPath, std::ios::binary);
    if (!file) throw std::runtime_error("could not read input script");
    const std::string script(std::istreambuf_iterator<char>(file), {});
    const std::array<std::pair<const char *, SearchResult>, 3> results{{
            {"reference", RunFixedScript(packs, scenario, script,
                    forevertas::PhysicsBackend::Reference, horizonMs)},
            {"optimized", RunFixedScript(packs, scenario, script,
                    forevertas::PhysicsBackend::OptimizedCpu, horizonMs)},
            {"cuda", RunFixedScript(packs, scenario, script,
                    forevertas::PhysicsBackend::Cuda, horizonMs)},
    }};
    for (const auto &[name, result] : results) {
        const auto finish = result.bestTimeline.empty()
                ? std::optional<std::uint32_t>{}
                : result.bestTimeline.back().finishTimeMs;
        std::cout << name << " finish="
                  << (finish ? std::to_string(*finish) : "none")
                  << " completed=" << result.bestState.raceCompleted
                  << " score=" << std::setprecision(17) << result.bestScore
                  << " evaluation_time=" << result.bestEvaluationTimeMs
                  << " time=" << result.bestState.timeMs
                  << " position=" << result.bestState.car.position.x << ","
                  << result.bestState.car.position.y << ","
                  << result.bestState.car.position.z << "\n";
    }
    const std::array<std::pair<const char *, std::vector<RawFrame>>, 3>
            rawResults{{
                    {"reference-raw", SimulateFixedScript(
                         packs, scenario, results.front().second.bestInputs,
                         forevertas::PhysicsBackend::Reference, horizonMs)},
                    {"optimized-raw", SimulateFixedScript(
                         packs, scenario, results.front().second.bestInputs,
                         forevertas::PhysicsBackend::OptimizedCpu, horizonMs)},
                    {"cuda-raw", SimulateFixedScript(
                         packs, scenario, results.front().second.bestInputs,
                         forevertas::PhysicsBackend::Cuda, horizonMs)},
            }};
    const auto &reference = rawResults.front().second;
    bool okay = true;
    for (std::size_t resultIndex = 1u; resultIndex < rawResults.size();
         ++resultIndex) {
        const auto &candidate = rawResults[resultIndex].second;
        const std::size_t common = std::min(reference.size(), candidate.size());
        std::size_t first = common;
        for (std::size_t index = 0u; index < common; ++index) {
            const auto &left = reference[index];
            const auto &right = candidate[index];
            if (left.x != right.x || left.y != right.y || left.z != right.z ||
                left.completed != right.completed ||
                left.finishTimeMs != right.finishTimeMs) {
                first = index;
                break;
            }
        }
        if (first != common || reference.size() != candidate.size()) {
            okay = false;
            std::cerr << rawResults[resultIndex].first
                      << " first divergence tick="
                      << first << " time="
                      << (first < common ? reference[first].timeMs : -1)
                      << " reference_frames=" << reference.size()
                      << " candidate_frames=" << candidate.size();
            if (first < common) {
                std::cerr << " reference_position="
                          << reference[first].x << ","
                          << reference[first].y << ","
                          << reference[first].z
                          << " candidate_position="
                          << candidate[first].x << ","
                          << candidate[first].y << ","
                          << candidate[first].z;
            }
            std::cerr << "\n";
        }
    }
    return okay;
}

SearchResult RunMismatchMutation(const char *packs,
                                 const char *scenario,
                                 const std::string &script,
                                 forevertas::PhysicsBackend backend,
                                 std::uint64_t iterations = 1u) {
    const forevertas::InputScriptParseResult parsed =
            forevertas::ParseInputScript(script);
    if (!parsed) throw std::runtime_error(*parsed.error);
    SearchRequest request{packs, scenario};
    request.baseInputCommands = parsed.commands;
    request.backend = backend;
    request.parallelSampleCount = backend == forevertas::PhysicsBackend::Cuda
            ? static_cast<std::uint32_t>(
                      std::min<std::uint64_t>(iterations, 40000u))
            : 1u;
    request.simulationHorizonMs = 25000u;
    request.searchAlgorithm.settings["autoPromoteBest"] = "true";
    OptionConfiguration modifier = DefaultModifier(
            forevertas::kExistingEventPerturbationModifierId);
    modifier.settings = {{"minTimeMs", "4000"},
                         {"maxTimeMs", "8500"},
                         {"minCount", "1"},
                         {"maxCount", "12"},
                         {"maxTimeShiftMs", "100"},
                         {"steerMode", "delta"},
                         {"steerDeltaMin", "-1"},
                         {"steerDeltaMax", "1"},
                         {"steerAbsoluteMin", "-1"},
                         {"steerAbsoluteMax", "1"},
                         {"toggleAccelerate", "false"},
                         {"toggleBrake", "true"},
                         {"seed", "3837657081"}};
    request.modifiers = {std::move(modifier)};
    request.evaluationTarget =
            DefaultEvaluator(forevertas::kVelocityEvaluationId);
    request.evaluationTarget.settings["minTimeMs"] = "10000";
    request.evaluationTarget.settings["maxTimeMs"] = "10000";
    const forevertas::ConditionCompileResult condition =
            forevertas::CompileConditionScript("iterations > 0");
    if (condition.error) throw std::runtime_error(*condition.error);
    request.condition = condition.program;
    forevertas::SearchRunControl control;
    control.iterationLimit = iterations;
    control.sampleBestTimeline = true;
    control.reuseLoadedSandbox = true;
    return forevertas::RunSearch(request, &control);
}

bool DiagnoseMutationBackend(const char *packs,
                             const char *scenario,
                             const char *scriptPath,
                             const char *backendName,
                             std::uint64_t iterations) {
    std::ifstream file(scriptPath, std::ios::binary);
    if (!file) throw std::runtime_error("could not read input script");
    const std::string script(std::istreambuf_iterator<char>(file), {});
    const std::string name(backendName);
    const forevertas::PhysicsBackend backend = name == "reference"
            ? forevertas::PhysicsBackend::Reference
            : name == "optimized"
            ? forevertas::PhysicsBackend::OptimizedCpu
            : forevertas::PhysicsBackend::Cuda;
    const SearchResult result = RunMismatchMutation(
            packs, scenario, script, backend, iterations);
    std::cout << name << " winner="
              << (result.winnerSource ==
                                  forevertas::SearchWinnerSource::Mutation
                          ? "mutation" : "baseline")
              << " iteration="
              << (result.winningIterationIndex
                          ? std::to_string(*result.winningIterationIndex)
                          : "none")
              << " score=" << std::setprecision(17) << result.bestScore
              << "\n" << forevertas::FormatInputScript(result.bestInputs);
    return true;
}

bool DiagnoseMismatchMutation(const char *packs,
                              const char *scenario,
                              const char *scriptPath) {
    std::ifstream file(scriptPath, std::ios::binary);
    if (!file) throw std::runtime_error("could not read input script");
    const std::string script(std::istreambuf_iterator<char>(file), {});
    const std::array<std::pair<const char *, SearchResult>, 3> results{{
            {"reference", RunMismatchMutation(packs, scenario, script,
                    forevertas::PhysicsBackend::Reference)},
            {"optimized", RunMismatchMutation(packs, scenario, script,
                    forevertas::PhysicsBackend::OptimizedCpu)},
            {"cuda", RunMismatchMutation(packs, scenario, script,
                    forevertas::PhysicsBackend::Cuda)},
    }};
    bool okay = true;
    const std::string expected = forevertas::FormatInputScript(
            results.front().second.bestInputs);
    for (const auto &[name, result] : results) {
        const std::string formatted =
                forevertas::FormatInputScript(result.bestInputs);
        std::cout << name << " winner="
                  << (result.winnerSource ==
                                      forevertas::SearchWinnerSource::Mutation
                              ? "mutation" : "baseline")
                  << " score=" << std::setprecision(17) << result.bestScore
                  << " finish="
                  << (result.bestTimeline.empty() ||
                              !result.bestTimeline.back().finishTimeMs
                              ? "none"
                              : std::to_string(*result.bestTimeline.back()
                                                       .finishTimeMs))
                  << " same_inputs=" << (formatted == expected) << "\n";
        if (formatted != expected ||
            result.bestScore != results.front().second.bestScore) {
            okay = false;
            std::cerr << name << " inputs:\n" << formatted;
        }
    }
    return okay;
}

}  // namespace

int main(int argc, char **argv) {
    const bool scriptParity = argc == 6 &&
            std::string(argv[1]) == "--script-parity";
    const bool mutationParity = argc == 5 &&
            std::string(argv[1]) == "--mutation-parity";
    const bool mutationBackend = argc == 7 &&
            std::string(argv[1]) == "--mutation-backend";
    const bool calibrationOnly =
            argc == 4 &&
            std::string(argv[1]) == "--calibration-only";
    const bool distanceOnly =
            argc == 4 &&
            (std::string(argv[1]) == "--distance-around-walls" ||
             std::string(argv[1]) == "--distance-around-walls-finish");
    const bool preciseFinishOnly =
            argc == 4 &&
            std::string(argv[1]) == "--precise-finish-only";
    const bool gpuPoolOnly = argc == 4 && std::string(argv[1]) == "--gpu-pool";
    // --throughput [cpu-]exact|[cpu-]approximate SECONDS PACKS REPLAY (FTAS_PROMOTE=0: no promotion)
    if (argc == 6 && std::string(argv[1]) == "--throughput") {
        const std::string mode = argv[2];
        return MeasureThroughput(argv[4], argv[5], mode.find("approximate") != std::string::npos,
                                 mode.rfind("cpu-", 0) == 0, std::atof(argv[3])) ? 0 : 1;
    }
    const bool ordinaryParity =
            argc == 3 ||
            (argc == 4 && std::string_view(argv[1]).find("--") != 0u);
    if ((!scriptParity && !mutationParity && !mutationBackend &&
         !calibrationOnly && !distanceOnly &&
         !preciseFinishOnly && !gpuPoolOnly &&
         !ordinaryParity) ||
        ((calibrationOnly || preciseFinishOnly || distanceOnly || gpuPoolOnly) && argc != 4)) {
        std::cerr << "expected Packs directory and replay path\n";
        return 2;
    }
    const bool focusedMode = calibrationOnly || preciseFinishOnly || distanceOnly || gpuPoolOnly;
    const char *const packs = argv[focusedMode ? 2 : 1];
    const char *const replay = argv[focusedMode ? 3 : 2];
    try {
        if (scriptParity) {
            return DiagnoseFixedScript(
                    argv[2], argv[3], argv[4],
                    static_cast<std::uint32_t>(std::stoul(argv[5])))
                    ? 0 : 1;
        }
        if (mutationParity) {
            return DiagnoseMismatchMutation(argv[2], argv[3], argv[4])
                    ? 0 : 1;
        }
        if (mutationBackend) {
            return DiagnoseMutationBackend(
                    argv[2], argv[3], argv[4], argv[5],
                    std::stoull(argv[6])) ? 0 : 1;
        }
        if (calibrationOnly) {
            return CheckCalibration(packs, replay) ? 0 : 1;
        }
        if (distanceOnly) {
            return CheckDistanceAroundWallsParity(
                           packs, replay,
                           std::string(argv[1]) == "--distance-around-walls-finish"
                                   ? "finish" : "checkpoint")
                    ? 0 : 1;
        }
        if (preciseFinishOnly) {
            return CheckPreciseFinishParity(packs, replay) ? 0 : 1;
        }
        if (gpuPoolOnly) {
            return CheckGpuPool(packs, replay) ? 0 : 1;
        }
        bool okay = CheckEquivalentDeletionPrefersFewerInputs(packs, replay);
        if (ordinaryParity && argc == 4) {
            okay &= CheckCheckpointConditionParity(packs, argv[3]);
        }
        okay &= CheckUnchangedIncumbentIsNotReconstructed(packs, replay);
        const OptionConfiguration velocity =
                DefaultEvaluator(forevertas::kVelocityEvaluationId);
        OptionConfiguration coverageVelocity = velocity;
        coverageVelocity.settings["minTimeMs"] = "1010";
        coverageVelocity.settings["maxTimeMs"] = "1010";
        constexpr std::int64_t shortEvaluationEndTimeMs = 1020;

        std::vector<OptionConfiguration> completeModifierPipeline;
        for (const auto &registration :
             forevertas::ModifierRegistry()) {
            OptionConfiguration modifier{
                    registration.id,
                    registration.defaultSettings};
            modifier.settings["minTimeMs"] = "1000";
            modifier.settings["maxTimeMs"] = "1000";
            completeModifierPipeline.push_back(std::move(modifier));
        }
        okay &= CheckParity(
                argv[1],
                argv[2],
                "complete modifier pipeline",
                2u,
                3u,
                completeModifierPipeline,
                coverageVelocity,
                false,
                nullptr,
                shortEvaluationEndTimeMs);

        const OptionConfiguration random = DefaultModifier(
                forevertas::kRandomSteeringModifierId);
        okay &= CheckParity(
                argv[1],
                argv[2],
                "condition excludes baseline by iteration count",
                4u,
                4u,
                {random},
                coverageVelocity,
                true,
                nullptr,
                shortEvaluationEndTimeMs,
                "iterations > 0");
        const SearchResult conditionReference = Run(
                argv[1], argv[2],
                forevertas::PhysicsBackend::Reference,
                1u, 4u, {random}, coverageVelocity,
                false, nullptr, false, shortEvaluationEndTimeMs,
                nullptr, false, false,
                forevertas::kDefaultSimulationHorizonMs,
                "iterations > 0");
        const SearchResult conditionOptimized = Run(
                argv[1], argv[2],
                forevertas::PhysicsBackend::OptimizedCpu,
                1u, 4u, {random}, coverageVelocity,
                false, nullptr, false, shortEvaluationEndTimeMs,
                nullptr, false, false,
                forevertas::kDefaultSimulationHorizonMs,
                "iterations > 0");
        okay &= SameAuthoritativeResult(
                conditionReference,
                conditionOptimized,
                "condition optimized CPU");
        const SearchResult baselineProbe = Run(
                packs,
                replay,
                forevertas::PhysicsBackend::Reference,
                1u,
                0u,
                {random},
                velocity,
                false,
                nullptr,
                true);
        if (baselineProbe.bestTimeline.size() < 3u) {
            throw std::runtime_error(
                    "baseline sampling did not produce a timeline");
        }
        const auto volumeTargetPosition = std::find_if(
                baselineProbe.bestTimeline.begin(),
                baselineProbe.bestTimeline.end(),
                [](const forevertas::SearchTimelineFrame &frame) {
                    return frame.timeMs == 1020;
                });
        if (volumeTargetPosition ==
            baselineProbe.bestTimeline.end()) {
            throw std::runtime_error(
                    "baseline sampling missed the short parity target");
        }
        const forevertas::SearchTimelineFrame &volumeTarget =
                *volumeTargetPosition;
        const auto decimal = [](float value) {
            std::ostringstream stream;
            stream << std::setprecision(17)
                   << static_cast<double>(value);
            return stream.str();
        };
        const forevertas::SearchTimelineFrame &steeringTarget =
                baselineProbe.bestTimeline[
                        std::min<std::size_t>(
                                500u,
                                baselineProbe.bestTimeline.size() - 1u)];
        const forevertas::SearchTimelineFrame &steeringPrevious =
                baselineProbe.bestTimeline[
                        std::min<std::size_t>(
                                499u,
                                baselineProbe.bestTimeline.size() - 1u)];
        const double tangentX =
                steeringTarget.positionX -
                steeringPrevious.positionX;
        const double tangentZ =
                steeringTarget.positionZ -
                steeringPrevious.positionZ;
        const double tangentLength =
                std::hypot(tangentX, tangentZ);
        const double lateralX = tangentLength == 0.0
                ? 20.0
                : -20.0 * tangentZ / tangentLength;
        const double lateralZ = tangentLength == 0.0
                ? 0.0
                : 20.0 * tangentX / tangentLength;
        OptionConfiguration offLinePoint = DefaultEvaluator(
                forevertas::kPointTargetEvaluationId);
        offLinePoint.settings["minTimeMs"] = "4000";
        offLinePoint.settings["maxTimeMs"] = "6000";
        offLinePoint.settings["x"] =
                decimal(static_cast<float>(
                        steeringTarget.positionX + lateralX));
        offLinePoint.settings["y"] =
                decimal(steeringTarget.positionY);
        offLinePoint.settings["z"] =
                decimal(static_cast<float>(
                        steeringTarget.positionZ + lateralZ));
        okay &= CheckParity(
                argv[1],
                argv[2],
                "random-steering winning candidate",
                32u,
                64u,
                {random},
                offLinePoint,
                false);
        okay &= CheckParity(
                argv[1],
                argv[2],
                "existing-event winning candidate",
                32u,
                64u,
                {DefaultModifier(
                        forevertas::
                                kExistingEventPerturbationModifierId)},
                offLinePoint,
                true);
        for (const auto &registration :
             forevertas::EvaluationTargetRegistry()) {
            if (registration.id ==
                        forevertas::kCustomVolumeEntryEvaluationId ||
                registration.id == forevertas::kCheckpointTimeEvaluationId ||
                registration.id ==
                        forevertas::kPreciseFinishTimeEvaluationId) {
                continue;
            }
            OptionConfiguration configured{
                    registration.id,
                    registration.defaultSettings};
            const auto minimum =
                    configured.settings.find("minTimeMs");
            const auto maximum =
                    configured.settings.find("maxTimeMs");
            if (minimum != configured.settings.end() &&
                maximum != configured.settings.end()) {
                minimum->second = "1010";
                maximum->second = "1010";
            }
            if (registration.id ==
                forevertas::kStuntPointsEvaluationId) {
                configured.settings["targetTimeMs"] = "1010";
            }
            if (registration.id ==
                forevertas::kScriptedTargetEvaluationId) {
                configured.settings["script"] =
                        "max car.speed\nmin car.y\ntarget 0 car.x";
            }
            if (registration.id ==
                forevertas::kVolumeEntryEvaluationId) {
                configured.settings["centerX"] =
                        decimal(volumeTarget.positionX);
                configured.settings["centerY"] =
                        decimal(volumeTarget.positionY);
                configured.settings["centerZ"] =
                        decimal(volumeTarget.positionZ);
                configured.settings["sizeX"] = "0.01";
                configured.settings["sizeY"] = "0.01";
                configured.settings["sizeZ"] = "0.01";
            }
            okay &= CheckParity(
                    argv[1],
                    argv[2],
                    "evaluator " + registration.id,
                    2u,
                    2u,
                    {random},
                    configured,
                    false,
                    nullptr,
                    registration.id ==
                                    forevertas::
                                            kPreciseFinishTimeEvaluationId
                            ? std::nullopt
                            : std::optional<std::int64_t>(
                                      shortEvaluationEndTimeMs));
        }

        double batchOneSeconds = 0.0;
        okay &= CheckParity(
                argv[1], argv[2], "batch size one",
                1u, 4u, {random}, velocity, false,
                &batchOneSeconds);
        const auto largeStarted = std::chrono::steady_clock::now();
        const SearchResult large = Run(
                argv[1],
                argv[2],
                forevertas::PhysicsBackend::Cuda,
                256u,
                257u,
                {random},
                velocity);
        const double largeSeconds =
                std::chrono::duration<double>(
                        std::chrono::steady_clock::now() -
                        largeStarted)
                        .count();
        const double batchOneRate =
                batchOneSeconds > 0.0
                ? 4.0 / batchOneSeconds
                : 0.0;
        const double largeRate =
                largeSeconds > 0.0
                ? static_cast<double>(large.iterations) /
                          largeSeconds
                : 0.0;
        if (large.iterations != 257u ||
            largeRate <= batchOneRate) {
            std::cerr << "large partial CUDA batch did not complete\n";
            okay = false;
        }
        std::cout << "stadium_cuda_batch_one_candidates_per_second="
                  << batchOneRate << '\n';
        std::cout << "realistic_stadium_cuda_candidates_per_second="
                  << largeRate
                  << '\n';

        OptionConfiguration shortInsertion = DefaultModifier(
                forevertas::kInputInsertionModifierId);
        shortInsertion.settings["minTimeMs"] = "1000";
        shortInsertion.settings["maxTimeMs"] = "1000";
        shortInsertion.settings["steerMinCount"] = "1";
        shortInsertion.settings["steerMaxCount"] = "1";
        shortInsertion.settings["steerMaxHoldMs"] = "0";
        shortInsertion.settings["steerOffsetMin"] = "0.1";
        shortInsertion.settings["steerOffsetMax"] = "0.1";
        OptionConfiguration shortVelocity = velocity;
        shortVelocity.settings["minTimeMs"] = "1010";
        shortVelocity.settings["maxTimeMs"] = "1010";
        const auto aboveOldCapStarted =
                std::chrono::steady_clock::now();
        const SearchResult aboveOldCap = Run(
                argv[1],
                argv[2],
                forevertas::PhysicsBackend::Cuda,
                8192u,
                8192u,
                {shortInsertion},
                shortVelocity);
        const double aboveOldCapSeconds =
                std::chrono::duration<double>(
                        std::chrono::steady_clock::now() -
                        aboveOldCapStarted)
                        .count();
        if (aboveOldCap.iterations != 8192u ||
            aboveOldCap.evaluatorCalls != 8193u) {
            std::cerr
                    << "CUDA did not fully evaluate a batch above the "
                       "old cap\n";
            okay = false;
        }
        std::cout << "cuda_8192_batch_candidates_per_second="
                  << 8192.0 / aboveOldCapSeconds << '\n';

        const SearchResult replayA = Run(
                argv[1], argv[2],
                forevertas::PhysicsBackend::Cuda,
                13u, 31u, {random}, velocity);
        const SearchResult replayB = Run(
                argv[1], argv[2],
                forevertas::PhysicsBackend::Cuda,
                13u, 31u, {random}, velocity);
        okay &= SameAuthoritativeResult(
                replayA, replayB, "deterministic CUDA replay");
        okay &= CheckCancellation(argv[1], argv[2]);
        return okay ? 0 : 1;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
