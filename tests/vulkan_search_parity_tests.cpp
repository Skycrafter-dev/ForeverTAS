#include "conditions/condition_program.h"
#include "mutations/input_event_utils.h"
#include "mutations/input_event_formatter.h"
#include "mutations/replay_input_script.h"
#include "physics_backend.h"
#include "replay_file_io.h"
#include "searches/algorithm_registry.h"
#include "searches/search_runner.h"

#include <forevervalidator/experimental/physics_sandbox.h>
#include <forevervalidator/native.h>

#include <algorithm>
#include <array>
#include <chrono>
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
                 [[maybe_unused]] bool useSessionSpecialization = true,
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
                             const SearchResult &vulkan,
                             const std::string &label) {
    const bool same =
            reference.winnerSource == vulkan.winnerSource &&
            reference.winningIterationIndex ==
                    vulkan.winningIterationIndex &&
            reference.winningMutationCount ==
                    vulkan.winningMutationCount &&
            reference.bestScore == vulkan.bestScore &&
            reference.bestEvaluationTimeMs ==
                    vulkan.bestEvaluationTimeMs &&
            reference.iterations == vulkan.iterations &&
            (reference.mutationImprovementCount > 0u) ==
                    (vulkan.mutationImprovementCount > 0u) &&
            SameInputs(reference.bestInputs, vulkan.bestInputs);
    if (!same) {
        std::cerr << label
                  << " parity failed: reference winner="
                  << (reference.winningIterationIndex
                              ? std::to_string(
                                        *reference.winningIterationIndex)
                              : "baseline")
                  << " Vulkan winner="
                  << (vulkan.winningIterationIndex
                              ? std::to_string(
                                        *vulkan.winningIterationIndex)
                              : "baseline")
                  << " reference score=" << reference.bestScore
                  << " Vulkan score=" << vulkan.bestScore
                  << " reference mutations="
                  << reference.totalMutationCount
                  << " Vulkan mutations=" << vulkan.totalMutationCount
                  << " sameWinner="
                  << (reference.winnerSource == vulkan.winnerSource)
                  << " sameIteration="
                  << (reference.winningIterationIndex ==
                      vulkan.winningIterationIndex)
                  << " sameMutationCount="
                  << (reference.winningMutationCount ==
                      vulkan.winningMutationCount)
                  << " sameScore=" << (reference.bestScore == vulkan.bestScore)
                  << " sameEvaluationTime="
                  << (reference.bestEvaluationTimeMs ==
                      vulkan.bestEvaluationTimeMs)
                  << " sameIterations="
                  << (reference.iterations == vulkan.iterations)
                  << " sameImprovementPresence="
                  << ((reference.mutationImprovementCount > 0u) ==
                      (vulkan.mutationImprovementCount > 0u))
                  << "(" << reference.mutationImprovementCount
                  << "/" << vulkan.mutationImprovementCount << ")"
                  << " sameInputs="
                  << SameInputs(reference.bestInputs, vulkan.bestInputs)
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
                 double *vulkanSeconds = nullptr,
                 std::optional<std::int64_t>
                         evaluationEndTimeLimitMs = std::nullopt,
                 const std::string &conditionScript = {}) {
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
                        true,
                        false,
                        forevertas::kDefaultSimulationHorizonMs,
                        conditionScript);
            });
    const auto vulkanStarted = std::chrono::steady_clock::now();
    const SearchResult vulkan = Run(
            packs, replay, forevertas::PhysicsBackend::Vulkan,
            batchSize, iterations, modifiers, evaluator,
            false, nullptr, false, evaluationEndTimeLimitMs,
            nullptr, false, true, false,
            forevertas::kDefaultSimulationHorizonMs,
            conditionScript);
    const SearchResult authoritative = reference.get();
    if (vulkanSeconds != nullptr) {
        *vulkanSeconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - vulkanStarted)
                               .count();
    }
    std::cout << label << " winner="
              << (vulkan.winningIterationIndex
                          ? std::to_string(
                                    *vulkan.winningIterationIndex)
                          : "baseline")
              << " seconds="
              << std::chrono::duration<double>(
                         std::chrono::steady_clock::now() - started)
                         .count()
              << std::endl;
    const bool mutationWinner =
            vulkan.winningIterationIndex.has_value();
    if (requireMutationWinner && !mutationWinner) {
        std::cerr << label
                  << " did not exercise winning candidate data\n";
    }
    return SameAuthoritativeResult(authoritative, vulkan, label) &&
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
    const SearchResult vulkan = Run(
            packs,
            replay,
            forevertas::PhysicsBackend::Vulkan,
            64u,
            iterations,
            {deletion},
            stunt,
            false,
            nullptr,
            false,
            std::nullopt,
            nullptr);

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
            reference, vulkan,
            "equivalent deletion Vulkan");
    return okay;
}

bool CheckCancellation(const char *packs, const char *replay) {
    SearchRequest request{packs, replay};
    request.baseInputCommands = ReplayInputCommands(packs, replay);
    request.backend = forevertas::PhysicsBackend::Vulkan;
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
    std::cerr << "running Vulkan batch ignored cancellation\n";
    return false;
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
            forevertas::PhysicsBackend::Vulkan,
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
                << "real Vulkan calibration depended on the configured "
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
                        nullptr, false, true, true, 30000u);
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
                        nullptr, false, true, true, 30000u);
            });
    const SearchResult vulkan = Run(
            packs,
            replay,
            forevertas::PhysicsBackend::Vulkan,
            static_cast<std::uint32_t>(iterations),
            iterations,
            modifiers,
            evaluator,
            false, nullptr, false, std::nullopt,
            nullptr, false, true, true, 30000u);
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
            forevertas::PhysicsBackend::Vulkan,
            1024u,
            1025u,
            {promotionModifier},
            evaluator,
            false, nullptr, false, std::nullopt,
            nullptr, false, true, true, 30000u);
    if (promotionProbe.iterations != 1025u ||
        promotionProbe.mutationImprovementCount == 0u ||
        !promotionProbe.winningIterationIndex) {
        std::cerr << "precise finish Vulkan did not exercise auto-promotion "
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
            exactFinishResult(vulkan, "precise finish Vulkan") &&
            SameAuthoritativeResult(
                   reference, optimized, "precise finish optimized CPU") &&
            SameAuthoritativeResult(
                    reference, vulkan, "precise finish Vulkan");
}

bool CheckUnchangedIncumbentIsNotReconstructed(const char *packs,
                                                const char *replay) {
    std::uint64_t resolutions = 0u;
    static_cast<void>(Run(
            packs,
            replay,
            forevertas::PhysicsBackend::Vulkan,
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
            false,
            forevertas::kDefaultSimulationHorizonMs,
            "iterations = 0",
            &resolutions));
    if (resolutions != 1u) {
        std::cerr << "unchanged Vulkan incumbent was reconstructed "
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
            true,
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
                backend == forevertas::PhysicsBackend::Vulkan ? 32u : 1u,
                0u,
                {random},
                atCheckpoint,
                false,
                nullptr,
                false,
                std::nullopt,
                nullptr,
                false,
                true,
                false,
                horizonMs,
                condition);
    };
    const SearchResult reference = run(
            forevertas::PhysicsBackend::Reference);
    const SearchResult optimized = run(
            forevertas::PhysicsBackend::OptimizedCpu);
    const SearchResult vulkan = run(
            forevertas::PhysicsBackend::Vulkan);
    return SameAuthoritativeResult(
                   reference, optimized,
                   "checkpoint condition optimized CPU") &&
            SameAuthoritativeResult(
                    reference, vulkan,
                    "checkpoint condition Vulkan");
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
                    forevertas::PhysicsBackend::OptimizedCpu,
                    horizonMs)},
            {"vulkan", RunFixedScript(packs, scenario, script,
                    forevertas::PhysicsBackend::Vulkan, horizonMs)},
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
                    {"vulkan-raw", SimulateFixedScript(
                         packs, scenario, results.front().second.bestInputs,
                         forevertas::PhysicsBackend::Vulkan, horizonMs)},
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
    request.parallelSampleCount = backend == forevertas::PhysicsBackend::Vulkan
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
            : forevertas::PhysicsBackend::Vulkan;
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
            {"vulkan", RunMismatchMutation(packs, scenario, script,
                    forevertas::PhysicsBackend::Vulkan)},
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
    const bool preciseFinishOnly =
            argc == 4 &&
            std::string(argv[1]) == "--precise-finish-only";
    const bool ordinaryParity =
            argc == 3 ||
            (argc == 4 && std::string_view(argv[1]).find("--") != 0u);
    if ((!scriptParity && !mutationParity && !mutationBackend &&
         !calibrationOnly &&
         !preciseFinishOnly &&
         !ordinaryParity) ||
        ((calibrationOnly || preciseFinishOnly) && argc != 4)) {
        std::cerr << "expected Packs directory and replay path\n";
        return 2;
    }
    const bool focusedMode = calibrationOnly || preciseFinishOnly;
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
        if (preciseFinishOnly) {
            return CheckPreciseFinishParity(packs, replay) ? 0 : 1;
        }
        bool okay = true;
        okay &= CheckEquivalentDeletionPrefersFewerInputs(packs, replay);
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
                nullptr, false, true, false,
                forevertas::kDefaultSimulationHorizonMs,
                "iterations > 0");
        const SearchResult conditionOptimized = Run(
                argv[1], argv[2],
                forevertas::PhysicsBackend::OptimizedCpu,
                1u, 4u, {random}, coverageVelocity,
                false, nullptr, false, shortEvaluationEndTimeMs,
                nullptr, false, true, false,
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
                forevertas::PhysicsBackend::Vulkan,
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
            std::cerr << "large partial Vulkan batch did not complete\n";
            okay = false;
        }
        std::cout << "stadium_vulkan_batch_one_candidates_per_second="
                  << batchOneRate << '\n';
        std::cout << "realistic_stadium_vulkan_candidates_per_second="
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
                forevertas::PhysicsBackend::Vulkan,
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
                    << "Vulkan did not fully evaluate a batch above the "
                       "old cap\n";
            okay = false;
        }
        std::cout << "vulkan_8192_batch_candidates_per_second="
                  << 8192.0 / aboveOldCapSeconds << '\n';

        const SearchResult replayA = Run(
                argv[1], argv[2],
                forevertas::PhysicsBackend::Vulkan,
                13u, 31u, {random}, velocity);
        const SearchResult replayB = Run(
                argv[1], argv[2],
                forevertas::PhysicsBackend::Vulkan,
                13u, 31u, {random}, velocity);
        okay &= SameAuthoritativeResult(
                replayA, replayB, "deterministic Vulkan replay");
        okay &= CheckCancellation(argv[1], argv[2]);
        return okay ? 0 : 1;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
