#include "mutations/input_event_utils.h"
#include "mutations/replay_input_script.h"
#include "physics_backend.h"
#include "replay_file_io.h"
#include "searches/algorithm_registry.h"
#include "searches/search_runner.h"

#include <forevervalidator/experimental/physics_sandbox.h>
#include <forevervalidator/native.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using forevertas::OptionConfiguration;
using forevertas::PhysicsBackend;
using forevertas::SearchResult;

OptionConfiguration Modifier(const char *id) {
    const auto *registration = forevertas::FindModifier(id);
    if (registration == nullptr) throw std::runtime_error("missing modifier");
    return {registration->id, registration->defaultSettings};
}

OptionConfiguration Evaluator(const char *id) {
    const auto *registration = forevertas::FindEvaluationTarget(id);
    if (registration == nullptr) throw std::runtime_error("missing evaluator");
    return {registration->id, registration->defaultSettings};
}

SearchResult Run(const char *packs,
                 const char *replay,
                 PhysicsBackend backend,
                 std::uint64_t iterations,
                 std::uint32_t batchSize,
                 OptionConfiguration modifier,
                 OptionConfiguration evaluator,
                 std::uint32_t horizonMs,
                 bool autoPromote = false,
                 bool calibrate = false,
                 std::vector<std::uint32_t> *updates = nullptr,
                 bool *calibrationCompleted = nullptr,
                 const char *conditionScript = nullptr) {
    forevertas::SearchRequest request{packs, replay};
    const auto parsed = forevertas::ParseInputScript(
            forevertas::ExtractReplayInputScript(packs, replay));
    if (!parsed) throw std::runtime_error(*parsed.error);
    request.baseInputCommands = parsed.commands;
    request.backend = backend;
    request.parallelSampleCount = batchSize;
    request.modifiers = {std::move(modifier)};
    request.evaluationTarget = std::move(evaluator);
    request.simulationHorizonMs = horizonMs;
    request.searchAlgorithm.settings["autoPromoteBest"] =
            autoPromote ? "true" : "false";
    request.calibrateCudaParallelSampleCount = calibrate;
    if (conditionScript != nullptr) {
        const auto condition = forevertas::CompileConditionScript(
                conditionScript);
        if (condition.error) throw std::runtime_error(*condition.error);
        request.condition = condition.program;
    }
    forevertas::SearchRunControl control;
    control.iterationLimit = iterations;
    control.reuseLoadedSandbox = true;
    control.cudaBatchSizeChanged = [updates](std::uint32_t value) {
        if (updates != nullptr &&
            (updates->empty() || updates->back() != value)) {
            updates->push_back(value);
        }
    };
    bool mutationStarted = false;
    control.progressChanged = [calibrationCompleted, &mutationStarted](
                                      const forevertas::SearchProgress &progress) {
        if (progress.stage ==
            forevertas::SearchProgressStage::Mutations) {
            mutationStarted = true;
            if (calibrationCompleted != nullptr) *calibrationCompleted = true;
        }
    };
    control.stopRequested = [calibrate, &mutationStarted]() {
        return calibrate && mutationStarted;
    };
    return forevertas::RunSearch(request, &control);
}

bool SameResult(const SearchResult &expected,
                const SearchResult &actual,
                const char *label,
                bool compareGpuCounters = false) {
    bool sameInputs = expected.bestInputs.size() == actual.bestInputs.size();
    for (std::size_t i = 0; sameInputs && i < expected.bestInputs.size(); ++i) {
        sameInputs = forevertas::SameInputEvent(
                expected.bestInputs[i], actual.bestInputs[i]);
    }
    const bool same =
            expected.winnerSource == actual.winnerSource &&
            expected.winningIterationIndex == actual.winningIterationIndex &&
            expected.winningMutationCount == actual.winningMutationCount &&
            expected.bestScore == actual.bestScore &&
            expected.bestEvaluationTimeMs == actual.bestEvaluationTimeMs &&
            expected.iterations == actual.iterations &&
            (expected.mutationImprovementCount > 0u) ==
                    (actual.mutationImprovementCount > 0u) &&
            expected.bestState.raceCompleted == actual.bestState.raceCompleted &&
            expected.bestState.finishTimeMs == actual.bestState.finishTimeMs &&
            expected.bestState.finishTime == actual.bestState.finishTime &&
            (!compareGpuCounters ||
             (expected.evaluatorCalls == actual.evaluatorCalls &&
              expected.mutationImprovementCount ==
                      actual.mutationImprovementCount &&
              expected.totalMutationCount == actual.totalMutationCount)) &&
            sameInputs;
    if (!same) {
        std::cerr << label << " mismatch: winner "
                  << (expected.winningIterationIndex
                              ? std::to_string(*expected.winningIterationIndex)
                              : "baseline")
                  << "/"
                  << (actual.winningIterationIndex
                              ? std::to_string(*actual.winningIterationIndex)
                              : "baseline")
                  << " score " << expected.bestScore << "/" << actual.bestScore
                  << " evaluation " << expected.bestEvaluationTimeMs << "/"
                  << actual.bestEvaluationTimeMs << " improvements "
                  << expected.mutationImprovementCount << "/"
                  << actual.mutationImprovementCount << " exact finish "
                  << (expected.bestState.finishTime == actual.bestState.finishTime)
                  << " mutation count " << expected.totalMutationCount << "/"
                  << actual.totalMutationCount
                  << " inputs " << sameInputs
                  << '\n';
    }
    return same;
}

bool SameVector(const forevervalidator::Vector3 &a,
                const forevervalidator::Vector3 &b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool SameState(const forevervalidator::experimental::PhysicsSandboxStateView &a,
               const forevervalidator::experimental::PhysicsSandboxStateView &b) {
    const auto &x = a.car;
    const auto &y = b.car;
    return a.tick == b.tick && a.timeMs == b.timeMs &&
            a.durationMs == b.durationMs &&
            a.mapEnvironment == b.mapEnvironment &&
            a.vehicleModel == b.vehicleModel &&
            a.playMode == b.playMode &&
            x.rotationX == y.rotationX && x.rotationY == y.rotationY &&
            x.rotationZ == y.rotationZ && x.rotationW == y.rotationW &&
            SameVector(x.position, y.position) &&
            SameVector(x.linearSpeed, y.linearSpeed) &&
            SameVector(x.angularSpeed, y.angularSpeed) &&
            SameVector(x.force, y.force) && SameVector(x.torque, y.torque) &&
            x.signedSpeed == y.signedSpeed && x.turbo == y.turbo &&
            x.cameraFlightTransition == y.cameraFlightTransition &&
            x.burning == y.burning && x.gearChanged == y.gearChanged &&
            x.wheelContact == y.wheelContact &&
            x.wheelHasSurface == y.wheelHasSurface &&
            SameVector(x.cameraSupportUp, y.cameraSupportUp) &&
            SameVector(x.localSpeed, y.localSpeed) &&
            x.freeWheeling == y.freeWheeling &&
            x.lateralContact == y.lateralContact &&
            x.sliding == y.sliding && x.gear == y.gear && x.rpm == y.rpm &&
            x.turningRate == y.turningRate &&
            x.turboType == y.turboType &&
            x.turboBoostFactor == y.turboBoostFactor &&
            x.wheelSliding == y.wheelSliding &&
            x.wheelSurface == y.wheelSurface &&
            a.accelerate == b.accelerate && a.brake == b.brake &&
            a.steering == b.steering &&
            a.checkpointsCollected == b.checkpointsCollected &&
            a.checkpointsTotal == b.checkpointsTotal &&
            a.completedLaps == b.completedLaps &&
            a.totalLaps == b.totalLaps &&
            a.raceCompleted == b.raceCompleted &&
            a.finishTimeMs == b.finishTimeMs &&
            a.finishTime == b.finishTime &&
            a.respawnCount == b.respawnCount &&
            a.stuntsScore == b.stuntsScore;
}

std::vector<forevervalidator::experimental::PhysicsSandboxStateView>
ReplayTimeline(const char *packs,
               const char *replay,
               PhysicsBackend backend,
               const std::vector<forevertas::SandboxInputEvent> &inputs) {
    using namespace forevervalidator;
    using namespace forevervalidator::experimental;
    PhysicsSandboxOptions options;
    options.backend = forevertas::ToForeverValidatorBackend(backend);
    options.tickDurationMs = forevertas::kSearchTickDurationMs;
    options.timelineMode = PhysicsSandboxTimelineMode::Canonical;
    options.simulationHorizonMs = 6000u;
    auto source = OpenInstalledPackDirectory(packs);
    if (!source) throw std::runtime_error("could not open Packs directory");
    auto created = CreatePhysicsSandbox(std::move(source).Value(), options);
    if (!created) throw std::runtime_error("could not create replay sandbox");
    PhysicsSandbox sandbox = std::move(created).Value();
    const ReplayIdentity identity{replay};
    auto bytes = forevertas::ReadReplayFileUtf8(replay, identity);
    if (!bytes) throw std::runtime_error("could not read replay");
    AssetBytes scenario = std::move(bytes).Value();
    if (!sandbox.LoadScenario(
                {scenario.data(), scenario.size()}, identity) ||
        !sandbox.ReplaceInputs(inputs)) {
        throw std::runtime_error("could not load replay inputs");
    }
    auto initial = sandbox.ReadState();
    if (!initial) throw std::runtime_error("could not read initial state");
    std::vector<PhysicsSandboxStateView> frames;
    frames.reserve(601u);
    frames.push_back(initial.Value());
    std::optional<PhysicsSandboxState> checkpoint;
    for (std::uint32_t tick = 1u; tick <= 600u; ++tick) {
        auto advanced = sandbox.AdvanceTicks(1u);
        if (!advanced) throw std::runtime_error("replay tick failed");
        frames.push_back(advanced.Value());
        if (tick == 300u) {
            auto captured = sandbox.CaptureState();
            if (!captured) throw std::runtime_error("replay capture failed");
            checkpoint.emplace(std::move(captured).Value());
        }
    }
    if (!checkpoint || !sandbox.RestoreState(*checkpoint)) {
        throw std::runtime_error("replay restore failed");
    }
    for (std::uint32_t tick = 301u; tick <= 350u; ++tick) {
        auto replayed = sandbox.AdvanceTicks(1u);
        if (!replayed || !SameState(frames[tick], replayed.Value())) {
            throw std::runtime_error("replay restoration changed a tick");
        }
    }
    return frames;
}

bool CheckReplayTimeline(const char *packs, const char *replay) {
    const SearchResult baseline = Run(
            packs, replay, PhysicsBackend::Reference, 0u, 1u,
            Modifier(forevertas::kRandomSteeringModifierId),
            Evaluator(forevertas::kVelocityEvaluationId), 6000u);
    const auto reference = ReplayTimeline(
            packs, replay, PhysicsBackend::Reference, baseline.bestInputs);
    const auto hip = ReplayTimeline(
            packs, replay, PhysicsBackend::Hip, baseline.bestInputs);
    for (std::size_t tick = 0u; tick < reference.size(); ++tick) {
        if (!SameState(reference[tick], hip[tick])) {
            std::cerr << "HIP replay physics diverged at tick " << tick
                      << " (" << reference[tick].timeMs << " ms)\n";
            return false;
        }
    }
#if FOREVERVALIDATOR_HAS_CUDA
    const auto cuda = ReplayTimeline(
            packs, replay, PhysicsBackend::Cuda, baseline.bestInputs);
    for (std::size_t tick = 0u; tick < cuda.size(); ++tick) {
        if (!SameState(cuda[tick], hip[tick])) {
            std::cerr << "CUDA/HIP replay physics diverged at tick " << tick
                      << '\n';
            return false;
        }
    }
#endif
    std::cout << "HIP replay exact ticks=" << reference.size()
              << " restored=50\n";
    return true;
}

bool CheckSearch(const char *packs, const char *replay) {
    OptionConfiguration deletion = Modifier(
            forevertas::kInputDeletionModifierId);
    deletion.settings["minTimeMs"] = "10";
    deletion.settings["maxTimeMs"] = "5990";
    deletion.settings["steerEnabled"] = "true";
    deletion.settings["steerMaxCount"] = "4";
    deletion.settings["accelerateEnabled"] = "true";
    deletion.settings["accelerateMaxCount"] = "2";
    deletion.settings["brakeEnabled"] = "true";
    deletion.settings["brakeMaxCount"] = "2";
    OptionConfiguration stunt = Evaluator(
            forevertas::kStuntPointsEvaluationId);
    stunt.settings["targetTimeMs"] = "6000";
    const SearchResult reference = Run(
            packs, replay, PhysicsBackend::Reference, 128u, 1u,
            deletion, stunt, 6000u);
    const SearchResult hip = Run(
            packs, replay, PhysicsBackend::Hip, 128u, 64u,
            deletion, stunt, 6000u);
    bool okay = SameResult(reference, hip, "HIP search winner");
    if (reference.winnerSource != forevertas::SearchWinnerSource::Mutation) {
        std::cerr << "search fixture did not produce a mutation winner\n";
        okay = false;
    }
#if FOREVERVALIDATOR_HAS_CUDA
    const SearchResult cuda = Run(
            packs, replay, PhysicsBackend::Cuda, 128u, 64u,
            deletion, stunt, 6000u);
    okay &= SameResult(cuda, hip, "CUDA/HIP search winner", true);
#endif
    OptionConfiguration steering = Modifier(
            forevertas::kRandomSteeringModifierId);
    steering.settings["minTimeMs"] = "1000";
    steering.settings["maxTimeMs"] = "1000";
    OptionConfiguration velocity = Evaluator(
            forevertas::kVelocityEvaluationId);
    velocity.settings["minTimeMs"] = "1010";
    velocity.settings["maxTimeMs"] = "1010";
    const SearchResult conditionedReference = Run(
            packs, replay, PhysicsBackend::Reference, 16u, 1u,
            steering, velocity, 6000u, false, false, nullptr, nullptr,
            "car.cps = 0");
    const SearchResult conditionedHip = Run(
            packs, replay, PhysicsBackend::Hip, 16u, 16u,
            steering, velocity, 6000u, false, false, nullptr, nullptr,
            "car.cps = 0");
    okay &= SameResult(conditionedReference, conditionedHip,
                       "HIP conditioned search");
#if FOREVERVALIDATOR_HAS_CUDA
    const SearchResult conditionedCuda = Run(
            packs, replay, PhysicsBackend::Cuda, 16u, 16u,
            steering, velocity, 6000u, false, false, nullptr, nullptr,
            "car.cps = 0");
    okay &= SameResult(conditionedCuda, conditionedHip,
                       "CUDA/HIP conditioned search", true);
#endif
    return okay;
}

bool CheckPreciseFinish(const char *packs, const char *replay) {
    const OptionConfiguration modifier = Modifier(
            forevertas::kRandomSteeringModifierId);
    const OptionConfiguration evaluator = Evaluator(
            forevertas::kPreciseFinishTimeEvaluationId);
    const SearchResult reference = Run(
            packs, replay, PhysicsBackend::Reference, 32u, 1u,
            modifier, evaluator, 30000u, true);
    const SearchResult hip = Run(
            packs, replay, PhysicsBackend::Hip, 32u, 32u,
            modifier, evaluator, 30000u, true);
    bool okay = SameResult(reference, hip, "HIP precise finish");
    const auto &finish = hip.bestState.finishTime;
    if (!hip.bestState.raceCompleted || !finish || !finish->IsValid() ||
        hip.bestScore != static_cast<double>(finish->upperBoundNs) ||
        hip.bestEvaluationTimeMs !=
                static_cast<double>(finish->upperBoundNs) / 1000000.0) {
        std::cerr << "HIP precise finish lost its exact nanosecond score\n";
        okay = false;
    }
#if FOREVERVALIDATOR_HAS_CUDA
    const SearchResult cuda = Run(
            packs, replay, PhysicsBackend::Cuda, 32u, 32u,
            modifier, evaluator, 30000u, true);
    okay &= SameResult(cuda, hip, "CUDA/HIP precise finish", true);
#endif
    return okay;
}

bool CheckCalibration(const char *packs, const char *replay) {
    OptionConfiguration insertion = Modifier(
            forevertas::kInputInsertionModifierId);
    insertion.settings["minTimeMs"] = "1000";
    insertion.settings["maxTimeMs"] = "1000";
    insertion.settings["steerMinCount"] = "1";
    insertion.settings["steerMaxCount"] = "1";
    insertion.settings["steerMaxHoldMs"] = "0";
    insertion.settings["steerOffsetMin"] = "0.1";
    insertion.settings["steerOffsetMax"] = "0.1";
    OptionConfiguration velocity = Evaluator(
            forevertas::kVelocityEvaluationId);
    velocity.settings["minTimeMs"] = "1010";
    velocity.settings["maxTimeMs"] = "1010";
    std::vector<std::uint32_t> updates;
    bool completed = false;
    const SearchResult hip = Run(
            packs, replay, PhysicsBackend::Hip, 100000000u, 64u,
            insertion, velocity, 6000u, false, true, &updates, &completed);
    const bool grew = std::any_of(updates.begin(), updates.end(),
            [](std::uint32_t value) { return value > 1u; });
    if (!completed || updates.size() < 3u || updates.front() != 1u ||
        !grew || hip.iterations == 0u || hip.evaluatorCalls == 0u) {
        std::cerr << "HIP calibration failed: complete=" << completed
                  << " iterations=" << hip.iterations << " updates=";
        for (const auto update : updates) std::cerr << update << ',';
        std::cerr << '\n';
        return false;
    }
    std::cout << "HIP calibration updates=";
    for (const auto update : updates) std::cout << update << ',';
    std::cout << '\n';
    return true;
}

bool CheckCancellation(const char *packs, const char *replay) {
    forevertas::SearchRequest request{packs, replay};
    const auto parsed = forevertas::ParseInputScript(
            forevertas::ExtractReplayInputScript(packs, replay));
    if (!parsed) throw std::runtime_error(*parsed.error);
    request.baseInputCommands = parsed.commands;
    request.backend = PhysicsBackend::Hip;
    request.parallelSampleCount = 4096u;
    forevertas::SearchRunControl control;
    std::chrono::steady_clock::time_point mutationStarted{};
    control.progressChanged = [&](const forevertas::SearchProgress &progress) {
        if (progress.stage == forevertas::SearchProgressStage::Mutations)
            mutationStarted = std::chrono::steady_clock::now();
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
    std::cerr << "HIP search ignored cancellation\n";
    return false;
}

}  // namespace

int main(int argc, char **argv) {
    const bool focused = argc == 4;
    if ((!focused && argc != 3) ||
        (focused && std::string_view(argv[1]) != "--precise-finish-only" &&
                    std::string_view(argv[1]) != "--calibration-only")) {
        std::cerr << "expected [mode] Packs directory and replay path\n";
        return 2;
    }
    const char *packs = argv[focused ? 2 : 1];
    const char *replay = argv[focused ? 3 : 2];
    try {
        if (focused && std::string_view(argv[1]) == "--precise-finish-only")
            return CheckPreciseFinish(packs, replay) ? 0 : 1;
        if (focused) return CheckCalibration(packs, replay) ? 0 : 1;
        return CheckReplayTimeline(packs, replay) &&
                       CheckSearch(packs, replay) &&
                       CheckCancellation(packs, replay)
                ? 0 : 1;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
