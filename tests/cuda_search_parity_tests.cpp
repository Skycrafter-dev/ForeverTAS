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
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
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
                 bool useSessionSpecialization = true,
                 bool autoPromoteBest = false,
                 std::uint32_t simulationHorizonMs =
                         forevertas::kDefaultSimulationHorizonMs,
                 const std::string &conditionScript = {},
                 std::uint64_t *winnerResolutionCount = nullptr,
                 std::uint32_t calibrationStartBatchSize =
                         forevertas::
                                 kDefaultCudaCalibrationStartSampleCount,
                 std::vector<std::uint32_t> *capacityUpdates = nullptr,
                 std::vector<std::pair<std::uint64_t, std::uint32_t>>
                         *batchExecutions = nullptr,
                 std::vector<forevertas::SearchLiveUpdate>
                         *liveUpdates = nullptr) {
    SearchRequest request{packs, replay};
    request.baseInputCommands = ReplayInputCommands(packs, replay);
    request.backend = backend;
    request.parallelSampleCount = batchSize;
    request.calibrateCudaParallelSampleCount = calibrate;
    request.cudaCalibrationStartSampleCount =
            calibrationStartBatchSize;
    request.useCudaSessionSpecialization = useSessionSpecialization;
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
    control.cudaBatchCapacityChanged =
            [capacityUpdates](std::uint32_t value) {
                if (capacityUpdates != nullptr &&
                    (capacityUpdates->empty() ||
                     capacityUpdates->back() != value)) {
                    capacityUpdates->push_back(value);
                }
            };
    control.cudaBatchExecuted =
            [batchExecutions](std::uint64_t firstCandidateId,
                              std::uint32_t candidateCount) {
                if (batchExecutions != nullptr) {
                    batchExecutions->emplace_back(
                            firstCandidateId, candidateCount);
                }
            };
    control.cudaWinnerResolved = [winnerResolutionCount]() {
        if (winnerResolutionCount != nullptr) {
            ++*winnerResolutionCount;
        }
    };
    if (liveUpdates != nullptr) {
        control.sampleImprovementTimelines = false;
        control.liveChanged =
                [liveUpdates](
                        const forevertas::SearchLiveUpdate &live) {
                    liveUpdates->push_back(live);
                };
    }
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

bool CheckCudaTargetProgressPropagation(
        const char *packs,
        const char *replay,
        const forevertas::SearchTimelineFrame &targetFrame) {
    const auto decimal = [](double value) {
        std::ostringstream stream;
        stream << std::setprecision(17) << value;
        return stream.str();
    };
    OptionConfiguration modifier = DefaultModifier(
            forevertas::kInputInsertionModifierId);
    modifier.settings["minTimeMs"] = "1000";
    modifier.settings["maxTimeMs"] = "1000";
    modifier.settings["steerEnabled"] = "true";
    modifier.settings["steerMode"] = "offset";
    modifier.settings["steerOffsetMin"] = "0.0001";
    modifier.settings["steerOffsetMax"] = "0.0001";
    modifier.settings["steerMinCount"] = "1";
    modifier.settings["steerMaxCount"] = "1";
    modifier.settings["steerMaxHoldMs"] = "0";
    modifier.settings["accelerateEnabled"] = "false";
    modifier.settings["brakeEnabled"] = "false";

    OptionConfiguration hit = DefaultEvaluator(
            forevertas::kVolumeEntryEvaluationId);
    hit.settings["centerX"] = decimal(targetFrame.positionX);
    hit.settings["centerY"] = decimal(targetFrame.positionY);
    hit.settings["centerZ"] = decimal(targetFrame.positionZ);
    hit.settings["sizeX"] = "0.1";
    hit.settings["sizeY"] = "0.1";
    hit.settings["sizeZ"] = "0.1";

    std::vector<forevertas::SearchLiveUpdate> hitLive;
    const SearchResult hitResult = Run(
            packs,
            replay,
            forevertas::PhysicsBackend::Cuda,
            2u,
            5u,
            {modifier},
            hit,
            false,
            nullptr,
            false,
            1040,
            nullptr,
            false,
            true,
            false,
            forevertas::kDefaultSimulationHorizonMs,
            std::string{},
            nullptr,
            forevertas::kDefaultCudaCalibrationStartSampleCount,
            nullptr,
            nullptr,
            &hitLive);
    std::uint64_t previousQualifyingCount = 0u;
    bool hitLiveInvalid = hitLive.empty();
    for (const forevertas::SearchLiveUpdate &live : hitLive) {
        hitLiveInvalid |= !live.bestAvailable ||
                live.qualifyingCandidateCount <
                        previousQualifyingCount ||
                !live.closestTargetDistance ||
                *live.closestTargetDistance != 0.0;
        previousQualifyingCount =
                live.qualifyingCandidateCount;
    }
    constexpr std::uint64_t expectedQualifyingCount = 6u;
    if (hitLiveInvalid ||
        hitResult.qualifyingCandidateCount !=
                expectedQualifyingCount ||
        !hitResult.closestTargetDistance ||
        *hitResult.closestTargetDistance != 0.0 ||
        hitLive.back().iterations != 5u ||
        hitLive.back().qualifyingCandidateCount !=
                hitResult.qualifyingCandidateCount ||
        hitLive.back().closestTargetDistance !=
                hitResult.closestTargetDistance) {
        std::cerr
                << "CUDA hit progress did not accumulate baseline and "
                   "mutation batches\n";
        return false;
    }

    OptionConfiguration miss = hit;
    miss.settings["centerX"] = decimal(
            static_cast<double>(targetFrame.positionX) + 1000000.0);
    miss.settings["centerY"] = decimal(
            static_cast<double>(targetFrame.positionY) + 1000000.0);
    miss.settings["centerZ"] = decimal(
            static_cast<double>(targetFrame.positionZ) + 1000000.0);
    miss.settings["sizeX"] = "0.01";
    miss.settings["sizeY"] = "0.01";
    miss.settings["sizeZ"] = "0.01";

    std::vector<forevertas::SearchLiveUpdate> missLive;
    bool rejectedMissingBest = false;
    try {
        static_cast<void>(Run(
                packs,
                replay,
                forevertas::PhysicsBackend::Cuda,
                2u,
                5u,
                {modifier},
                miss,
                false,
                nullptr,
                false,
                1040,
                nullptr,
                false,
                true,
                false,
                forevertas::kDefaultSimulationHorizonMs,
                std::string{},
                nullptr,
                forevertas::kDefaultCudaCalibrationStartSampleCount,
                nullptr,
                nullptr,
                &missLive));
    } catch (const std::runtime_error &error) {
        rejectedMissingBest =
                std::string(error.what()) ==
                "no iteration satisfied the selected evaluation target";
    }

    previousQualifyingCount = 0u;
    std::optional<double> previousClosest;
    bool missLiveInvalid = missLive.empty();
    for (const forevertas::SearchLiveUpdate &live : missLive) {
        missLiveInvalid |= live.bestAvailable ||
                live.qualifyingCandidateCount <
                        previousQualifyingCount ||
                live.qualifyingCandidateCount != 0u ||
                !live.closestTargetDistance ||
                !std::isfinite(*live.closestTargetDistance) ||
                *live.closestTargetDistance <= 0.0 ||
                (previousClosest &&
                 *live.closestTargetDistance > *previousClosest) ||
                !live.bestTimeline.empty();
        previousQualifyingCount =
                live.qualifyingCandidateCount;
        previousClosest = live.closestTargetDistance;
    }
    if (!rejectedMissingBest || missLiveInvalid ||
        missLive.back().iterations != 5u) {
        std::cerr
                << "CUDA miss progress did not publish no-best counters "
                   "and closest distance\n";
        return false;
    }
    return true;
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
    const auto cudaStarted = std::chrono::steady_clock::now();
    const SearchResult cuda = Run(
            packs, replay, forevertas::PhysicsBackend::Cuda,
            batchSize, iterations, modifiers, evaluator,
            false, nullptr, false, evaluationEndTimeLimitMs,
            nullptr, false, true, false,
            forevertas::kDefaultSimulationHorizonMs,
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

bool CheckCudaKernelModeParity(
        const char *packs,
        const char *replay) {
    OptionConfiguration modifier = DefaultModifier(
            forevertas::kRandomSteeringModifierId);
    modifier.settings["minTimeMs"] = "1000";
    modifier.settings["maxTimeMs"] = "1000";
    OptionConfiguration evaluator = DefaultEvaluator(
            forevertas::kVelocityEvaluationId);
    evaluator.settings["minTimeMs"] = "1020";
    evaluator.settings["maxTimeMs"] = "1020";

    const SearchResult regular = Run(
            packs,
            replay,
            forevertas::PhysicsBackend::Cuda,
            8u,
            8u,
            {modifier},
            evaluator,
            false,
            nullptr,
            false,
            std::nullopt,
            nullptr,
            false,
            false);
    const SearchResult specialized = Run(
            packs,
            replay,
            forevertas::PhysicsBackend::Cuda,
            8u,
            8u,
            {modifier},
            evaluator,
            false,
            nullptr,
            false,
            std::nullopt,
            nullptr,
            false,
            true);
    return SameAuthoritativeResult(
            regular, specialized, "regular/specialized CUDA");
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
    std::vector<std::uint32_t> capacityUpdates;
    std::vector<std::pair<std::uint64_t, std::uint32_t>>
            batchExecutions;
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
            true,
            true,
            false,
            forevertas::kDefaultSimulationHorizonMs,
            {},
            nullptr,
            std::numeric_limits<std::uint32_t>::max(),
            &capacityUpdates,
            &batchExecutions);
    const bool grew = std::any_of(
            updates.begin(),
            updates.end(),
            [](std::uint32_t value) {
                return value > 1u;
            });
    const bool boundedBootstrap =
            !capacityUpdates.empty() &&
            capacityUpdates.front() == 1u &&
            std::none_of(
                    capacityUpdates.begin(),
                    capacityUpdates.end(),
                    [](std::uint32_t value) {
                        return value ==
                                std::numeric_limits<
                                        std::uint32_t>::max();
                    });
    std::vector<std::uint32_t> executedBatchChanges;
    for (const auto &[firstCandidateId, candidateCount] :
         batchExecutions) {
        static_cast<void>(firstCandidateId);
        if (executedBatchChanges.empty() ||
            executedBatchChanges.back() != candidateCount) {
            executedBatchChanges.push_back(candidateCount);
        }
    }
    const bool truthfulStagedProbes =
            updates.size() >= 2u &&
            updates.front() == 2u &&
            updates[1] > updates.front() &&
            updates == executedBatchChanges &&
            std::none_of(
                    updates.begin(),
                    updates.end(),
                    [](std::uint32_t value) {
                        return value ==
                                std::numeric_limits<
                                        std::uint32_t>::max();
                    });
    const bool nonOverlappingBootstrap =
            batchExecutions.size() >= 2u &&
            batchExecutions[0].first == 0u &&
            batchExecutions[0].second == 2u &&
            batchExecutions[1].first == 2u &&
            batchExecutions[1].second == updates[1] &&
            std::none_of(
                    batchExecutions.begin(),
                    batchExecutions.end(),
                    [](const auto &execution) {
                        return execution.second ==
                                std::numeric_limits<
                                        std::uint32_t>::max();
                    });
    if (updates.size() < 3u || !truthfulStagedProbes || !grew ||
        !boundedBootstrap || !nonOverlappingBootstrap ||
        !calibrationCompleted) {
        std::cerr
                << "real CUDA calibration depended on the configured "
                   "batch size, allocated/executed the unsafe first probe, "
                   "did not grow, or did not complete; "
                   "completed="
                << calibrationCompleted << " updates=";
        for (std::uint32_t update : updates) {
            std::cerr << update << ',';
        }
        std::cerr << " capacities=";
        for (std::uint32_t capacity : capacityUpdates) {
            std::cerr << capacity << ',';
        }
        std::cerr << " executions=";
        for (const auto &[firstCandidateId, candidateCount] :
             batchExecutions) {
            std::cerr << firstCandidateId << '+' << candidateCount << ',';
        }
        std::cerr << '\n';
        return false;
    }

    std::vector<std::pair<std::uint64_t, std::uint32_t>>
            oneIterationExecutions;
    std::vector<std::uint32_t> oneIterationBatches;
    std::vector<std::uint32_t> oneIterationCapacities;
    bool oneIterationCalibrationCompleted = false;
    const SearchResult oneIteration = Run(
            packs,
            replay,
            forevertas::PhysicsBackend::Cuda,
            64u,
            1u,
            {insertion},
            velocity,
            true,
            &oneIterationBatches,
            false,
            std::nullopt,
            &oneIterationCalibrationCompleted,
            false,
            true,
            false,
            forevertas::kDefaultSimulationHorizonMs,
            {},
            nullptr,
            std::numeric_limits<std::uint32_t>::max(),
            &oneIterationCapacities,
            &oneIterationExecutions);
    std::vector<std::pair<std::uint64_t, std::uint32_t>>
            zeroIterationExecutions;
    std::vector<std::uint32_t> zeroIterationBatches;
    std::vector<std::uint32_t> zeroIterationCapacities;
    const SearchResult zeroIteration = Run(
            packs,
            replay,
            forevertas::PhysicsBackend::Cuda,
            64u,
            0u,
            {insertion},
            velocity,
            true,
            &zeroIterationBatches,
            false,
            std::nullopt,
            nullptr,
            false,
            true,
            false,
            forevertas::kDefaultSimulationHorizonMs,
            {},
            nullptr,
            std::numeric_limits<std::uint32_t>::max(),
            &zeroIterationCapacities,
            &zeroIterationExecutions);
    const bool boundedIterationBudgets =
            oneIteration.iterations == 1u &&
            oneIterationExecutions.size() == 1u &&
            oneIterationExecutions.front() ==
                    std::pair<std::uint64_t, std::uint32_t>{0u, 1u} &&
            oneIterationBatches == std::vector<std::uint32_t>{1u} &&
            oneIterationCapacities ==
                    std::vector<std::uint32_t>{1u} &&
            !oneIterationCalibrationCompleted &&
            zeroIteration.iterations == 0u &&
            zeroIterationExecutions.empty() &&
            zeroIterationBatches.empty() &&
            zeroIterationCapacities ==
                    std::vector<std::uint32_t>{1u};
    if (!boundedIterationBudgets) {
        std::cerr
                << "CUDA calibration bootstrap exceeded the zero/one "
                   "iteration budget\n";
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
    const SearchResult cuda = Run(
            packs,
            replay,
            forevertas::PhysicsBackend::Cuda,
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
            forevertas::PhysicsBackend::Cuda,
            40000u,
            40001u,
            {promotionModifier},
            evaluator,
            false, nullptr, false, std::nullopt,
            nullptr, false, true, true, 30000u);
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

SearchResult RunFixedScript(const char *packs,
                            const char *scenario,
                            const std::string &script,
                            forevertas::PhysicsBackend backend,
                            bool specialize,
                            std::uint32_t horizonMs) {
    const forevertas::InputScriptParseResult parsed =
            forevertas::ParseInputScript(script);
    if (!parsed) throw std::runtime_error(*parsed.error);
    SearchRequest request{packs, scenario};
    request.baseInputCommands = parsed.commands;
    request.backend = backend;
    request.parallelSampleCount = 1u;
    request.useCudaSessionSpecialization = specialize;
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
    const std::array<std::pair<const char *, SearchResult>, 4> results{{
            {"reference", RunFixedScript(packs, scenario, script,
                    forevertas::PhysicsBackend::Reference, false, horizonMs)},
            {"optimized", RunFixedScript(packs, scenario, script,
                    forevertas::PhysicsBackend::OptimizedCpu, false,
                    horizonMs)},
            {"cuda-regular", RunFixedScript(packs, scenario, script,
                    forevertas::PhysicsBackend::Cuda, false, horizonMs)},
            {"cuda-specialized", RunFixedScript(packs, scenario, script,
                    forevertas::PhysicsBackend::Cuda, true, horizonMs)},
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
                                 bool specialize,
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
    request.useCudaSessionSpecialization = specialize;
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
    const bool specialize = name == "cuda-specialized";
    const SearchResult result = RunMismatchMutation(
            packs, scenario, script, backend, specialize, iterations);
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
    const std::array<std::pair<const char *, SearchResult>, 4> results{{
            {"reference", RunMismatchMutation(packs, scenario, script,
                    forevertas::PhysicsBackend::Reference, false)},
            {"optimized", RunMismatchMutation(packs, scenario, script,
                    forevertas::PhysicsBackend::OptimizedCpu, false)},
            {"cuda-regular", RunMismatchMutation(packs, scenario, script,
                    forevertas::PhysicsBackend::Cuda, false)},
            {"cuda-specialized", RunMismatchMutation(packs, scenario, script,
                    forevertas::PhysicsBackend::Cuda, true)},
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
    if ((!scriptParity && !mutationParity && !mutationBackend &&
         !calibrationOnly &&
         !preciseFinishOnly &&
         argc != 3) ||
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
        bool okay = CheckCudaKernelModeParity(packs, replay);
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
        okay &= CheckCudaTargetProgressPropagation(
                argv[1], argv[2], volumeTarget);
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
