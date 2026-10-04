#include "conditions/condition_program.h"
#include "evaluators/checkpoint_time_evaluator.h"
#include "mutations/random_steering_mutator.h"
#include "mutations/replay_input_script.h"
#include "replay_file_io.h"
#include "searches/cuda_search_configuration.h"
#include "searches/search_runner.h"
#include "time_format.h"

#include <forevervalidator/native.h>

#include <algorithm>
#include <optional>
#include <iostream>
#include <stdexcept>

namespace {
using namespace forevertas;
using namespace forevervalidator;
using namespace forevervalidator::experimental;

void Check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

template<typename Result>
auto Require(Result result) {
    if (!result) throw std::runtime_error(result.Error().diagnostic);
    return std::move(result).Value();
}

bool SameEvent(const PhysicsSandboxAcceptedCheckpointEvent &left,
               const PhysicsSandboxAcceptedCheckpointEvent &right) {
    return left.checkpointSlot == right.checkpointSlot && left.checkpointIndex == right.checkpointIndex &&
            left.lap == right.lap && left.eventIndex == right.eventIndex && left.tick == right.tick &&
            left.timeMs == right.timeMs && left.finish == right.finish;
}

bool SameEvents(const std::vector<PhysicsSandboxAcceptedCheckpointEvent> &left,
                const std::vector<PhysicsSandboxAcceptedCheckpointEvent> &right) {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(), SameEvent);
}

void TestEvaluator() {
    auto settings = DefaultCheckpointTimeOptionSettings();
    auto evaluator = CreateCheckpointTimeEvaluator(settings, 10);
    const auto plan = evaluator->Plan(60000, 5000, 10);
    Check(plan.startTimeMs == 10 && plan.endTimeMs == 60000, "event before mutation boundary was excluded");
    PhysicsSandboxStateView state;
    state.tick = 10;
    state.timeMs = 100;
    state.checkpointsCollected = 1;
    Check(!evaluator->CreateSession()->Observe({}, state), "checkpoint count was mistaken for acceptance");
    state.acceptedCheckpointEvents = {{7, 1, 2, 4, 10, 100, false},
                                      {3, 0, 2, 5, 10, 100, false},
                                      {8, 2, 2, 6, 10, 100, true}};
    Check(!evaluator->CreateSession()->Observe({}, state), "wrong lap matched");
    settings["lap"] = "2";
    settings["checkpointSlot"] = "3";
    settings["eventIndex"] = "5";
    evaluator = CreateCheckpointTimeEvaluator(settings, 10);
    auto session = evaluator->CreateSession();
    const auto sample = session->Observe({}, state);
    Check(sample && sample->score == 100 && sample->timeMs == 100 && session->IsComplete(),
          "simultaneous event selection or canonical timestamp failed");
    Check(sample->description == "Checkpoint 1 of lap 2 (map slot 3, event index 5): " +
                  FormatRaceTimeMilliseconds(100),
          "evaluation description did not name the checkpoint number, map slot and event index");
    Check(!session->Observe({}, state), "accepted event was reported twice");
    Check(evaluator->IsBetter({90, 90, {}}, *sample) && !evaluator->IsBetter({110, 110, {}}, *sample),
          "checkpoint time was not minimized");
    state.timeMs = 110;
    state.tick = 11;
    Check(!evaluator->CreateSession()->Observe({}, state), "stale journal entry leaked into a later tick");
    state.timeMs = 100;
    state.tick = 10;
    settings["eventType"] = "finish";
    settings["checkpointSlot"] = "8";
    settings["eventIndex"] = "6";
    const auto finish = CreateCheckpointTimeEvaluator(settings, 10)->CreateSession()->Observe({}, state);
    Check(finish && finish->description == "Finish of lap 2 (map slot 8, event index 6): " +
                  FormatRaceTimeMilliseconds(100),
          "accepted finish selector failed");
    settings["checkpointSlot"] = "-1";
    settings["eventIndex"] = "18446744073709551615";
    state.acceptedCheckpointEvents.back().eventIndex = UINT64_MAX;
    Check(CreateCheckpointTimeEvaluator(settings, 10)->CreateSession()->Observe({}, state).has_value(),
          "64-bit accepted event identity was rounded");
    for (const auto &[key, value] : std::vector<std::pair<std::string, std::string>>{
            {"checkpointIndex", "0"}, {"lap", "0"}, {"checkpointSlot", "-2"},
            {"eventIndex", "18446744073709551616"}, {"eventType", "position"}}) {
        auto invalid = DefaultCheckpointTimeOptionSettings();
        invalid[key] = value;
        Check(ValidateCheckpointTimeOptionSettings(invalid, 10).has_value(), "invalid checkpoint selector accepted");
    }
    auto gpuSettings = DefaultCheckpointTimeOptionSettings();
    gpuSettings["checkpointIndex"] = "3";
    gpuSettings["lap"] = "2";
    gpuSettings["checkpointSlot"] = "7";
    gpuSettings["eventIndex"] = "18446744073709551615";
    const auto gpu = BuildCudaEvaluator({kCheckpointTimeEvaluationId, gpuSettings}, 10);
    const auto *checkpoint = gpu ? std::get_if<PhysicsSandboxCudaCheckpointEvaluator>(&*gpu) : nullptr;
    Check(checkpoint && !checkpoint->finish && checkpoint->checkpointIndex == 2 && checkpoint->lap == 2 &&
                  checkpoint->checkpointSlot == 7u && checkpoint->eventIndex == UINT64_MAX,
          "checkpoint selectors did not reach the CUDA evaluator exactly");
}

std::vector<PhysicsSandboxAcceptedCheckpointEvent> ReadAcceptedEvents(
        const char *packs, const char *path, SimulationBackend backend,
        const std::vector<ParsedInputCommand> &commands) {
    PhysicsSandboxOptions options;
    options.backend = backend;
    options.timelineMode = PhysicsSandboxTimelineMode::Canonical;
    options.simulationHorizonMs = 60000;
    auto sandbox = Require(CreatePhysicsSandbox(Require(OpenInstalledPackDirectory(packs)), options));
    ReplayIdentity identity{path};
    const auto bytes = Require(ReadReplayFileUtf8(path, identity));
    Require(sandbox.LoadReplay({bytes.data(), bytes.size()}, identity));
    const auto baseline = BuildInputScriptBaseline(Require(sandbox.ReadInputs()), commands, 10);
    Check(!baseline.error, "checkpoint fixture input conversion failed");
    Require(sandbox.ReplaceInputs(baseline.events));
    std::vector<PhysicsSandboxAcceptedCheckpointEvent> events;
    bool testedSnapshot = false;
    for (int tick = 0; tick < 6000; ++tick) {
        auto view = Require(sandbox.AdvanceTicks(1));
        for (const auto &event : view.acceptedCheckpointEvents) {
            Check(event.tick == view.tick && event.timeMs == view.timeMs,
                  "accepted event did not use the authoritative simulation tick");
            events.push_back(event);
        }
        if (!testedSnapshot && !view.acceptedCheckpointEvents.empty()) {
            const auto snapshot = Require(sandbox.CaptureState());
            const auto next = Require(sandbox.AdvanceTicks(1));
            Check(next.acceptedCheckpointEvents.empty(), "accepted event persisted into another tick");
            const auto restored = Require(sandbox.RestoreState(snapshot));
            Check(SameEvents(restored.acceptedCheckpointEvents, view.acceptedCheckpointEvents),
                  "sandbox snapshot lost accepted event identity or time");
            testedSnapshot = true;
        }
        if (view.raceCompleted) break;
    }
    Check(testedSnapshot, "replay fixture did not produce accepted events");
    return events;
}

#if FOREVERVALIDATOR_HAS_CUDA
// The CUDA kernel names each accepted event from the race counters. Every
// event of the run, selected several ways, must be reported at the CPU
// journal's tick (or not at all for a selector that names no event). The CPU
// runs up to shortly before each event so each GPU launch stays short enough
// for a display GPU's watchdog.
void TestCudaEvents(const char *packs, const char *path,
                    const std::vector<ParsedInputCommand> &commands,
                    const std::vector<PhysicsSandboxAcceptedCheckpointEvent> &reference) {
    if (!QueryCudaBackendDiagnostics().IsReady()) {
        std::cout << "SKIP unavailable CUDA device\n";
        return;
    }
    PhysicsSandboxOptions options;
    options.backend = SimulationBackend::Cuda;
    options.timelineMode = PhysicsSandboxTimelineMode::Canonical;
    // A run just past the last event keeps the GPU allocation small.
    options.simulationHorizonMs = static_cast<std::uint32_t>(reference.back().timeMs / 10 * 10 + 1000);
    auto sandbox = Require(CreatePhysicsSandbox(Require(OpenInstalledPackDirectory(packs)), options));
    ReplayIdentity identity{path};
    const auto bytes = Require(ReadReplayFileUtf8(path, identity));
    Require(sandbox.LoadReplay({bytes.data(), bytes.size()}, identity));
    const auto baseline = BuildInputScriptBaseline(Require(sandbox.ReadInputs()), commands, 10);
    Check(!baseline.error, "CUDA checkpoint fixture input conversion failed");
    Require(sandbox.ReplaceInputs(baseline.events));
    const auto start = Require(sandbox.CaptureState());
    std::size_t compared = 0;
    for (const auto &event : reference) {
        // The GPU branch starts one tick after the sandbox's current time.
        const std::int64_t branch = (static_cast<std::int64_t>(event.timeMs) - 200) / 10 * 10;
        Require(sandbox.RestoreState(start));
        Require(sandbox.AdvanceTicks(static_cast<std::uint32_t>((branch - 10) / 10)));
        for (int selector = 0; selector < 5; ++selector) {
            PhysicsSandboxCudaCheckpointEvaluator evaluator;
            evaluator.finish = event.finish;
            evaluator.checkpointIndex = event.checkpointIndex;
            evaluator.lap = static_cast<std::uint32_t>(event.lap);
            if (selector == 1 || selector == 3) evaluator.checkpointSlot = event.checkpointSlot;
            if (selector == 2 || selector == 3) evaluator.eventIndex = event.eventIndex;
            if (selector == 4) evaluator.checkpointSlot = event.checkpointSlot + 1u;
            PhysicsSandboxCudaSearchConfiguration configuration;
            configuration.maximumBatchSize = 4;
            configuration.earliestMutationTimeMs = branch;
            configuration.evaluationStartTimeMs = branch;
            configuration.evaluationEndTimeMs = static_cast<std::int64_t>(event.timeMs) + 100;
            // Mutations come after the event, so every candidate reaches it
            // at the same tick as the base inputs.
            PhysicsSandboxCudaRandomSteeringModifier steering;
            steering.window = {static_cast<std::int64_t>(event.timeMs) + 50,
                               static_cast<std::int64_t>(event.timeMs) + 50, 7u};
            configuration.modifiers = {steering};
            configuration.evaluator = evaluator;
            auto session = Require(CreatePhysicsSandboxCudaSearchSession(sandbox, configuration));
            const auto base = Require(session.EvaluateBaseline());
            const auto batch = Require(session.RunBatch(0, 4));
            const bool expected = selector != 4;
            Check(base.bestValid == expected && (!expected || base.bestScore == event.timeMs),
                  "CUDA baseline named a different accepted checkpoint event than the CPU journal");
            Check(!batch.bestChanged || !batch.bestIsMutation,
                  "a CUDA candidate reached the same accepted event earlier than possible");
            ++compared;
        }
    }
    std::cout << "cuda: " << compared << " checkpoint selections match the CPU journal\n";
}
#endif

void TestReplayAndSearch(const char *packs, const char *path) {
    auto parsed = ParseInputScript(ExtractReplayInputScript(packs, path));
    Check(!parsed.error, "checkpoint replay script failed to parse");
    const auto reference = ReadAcceptedEvents(packs, path, SimulationBackend::Reference, parsed.commands);
    const auto optimized = ReadAcceptedEvents(packs, path, SimulationBackend::OptimizedCpu, parsed.commands);
    Check(SameEvents(reference, optimized), "CPU backends disagree on accepted checkpoint identity/time");
    const auto finish = std::find_if(reference.begin(), reference.end(), [](const auto &event) { return event.finish; });
    Check(finish != reference.end(), "fixture has no accepted finish event");
    SearchRequest request(packs, path);
    const auto lateTime = finish->timeMs + 100;
    request.simulationHorizonMs = static_cast<std::uint32_t>(lateTime + 100);
    request.baseInputCommands = parsed.commands;
    ParsedInputCommand later;
    later.userTimeMs = static_cast<std::int64_t>(lateTime);
    later.action = SandboxInputAction::Steer;
    later.value.kind = PhysicsSandboxInputValueKind::Analog;
    later.value.analog = 0;
    request.baseInputCommands.push_back(later);
    auto modifier = DefaultRandomSteeringOptionSettings();
    modifier["minTimeMs"] = modifier["maxTimeMs"] = std::to_string(lateTime);
    request.modifiers = {{kRandomSteeringModifierId, modifier}};
    auto target = DefaultCheckpointTimeOptionSettings();
    target["eventType"] = "finish";
    target["lap"] = std::to_string(finish->lap);
    target["checkpointSlot"] = std::to_string(finish->checkpointSlot);
    target["eventIndex"] = std::to_string(finish->eventIndex);
    request.evaluationTarget = {kCheckpointTimeEvaluationId, target};
    // Completed laps rise on the finish tick itself, so this holds exactly
    // from the selected event onward and its negation rejects that tick.
    request.condition = CompileConditionScript("car.completed_laps >= " + std::to_string(finish->lap)).program;
    SearchRunControl control;
    control.sampleBestTimeline = false;
    control.iterationLimit = 4;
    for (const auto backend : {PhysicsBackend::Reference, PhysicsBackend::OptimizedCpu,
                               PhysicsBackend::MultiThreadedCpu}) {
        request.backend = backend;
        request.parallelSampleCount = backend == PhysicsBackend::MultiThreadedCpu ? 2 : 1;
        const auto result = RunSearch(request, &control);
        Check(result.bestScore == finish->timeMs && result.bestState.timeMs == finish->timeMs &&
              result.iterations == 4, "checkpoint search missed an eligible event before its mutation window");
        std::cout << PhysicsBackendId(backend) << ": accepted event " << finish->eventIndex
                  << " at " << finish->timeMs << " ms, 4 mutation attempts\n";
    }
    request.backend = PhysicsBackend::OptimizedCpu;
    control.iterationLimit = 0;
    const auto expectNoEvent = [&]() {
        bool missing = false;
        try { RunSearch(request, &control); }
        catch (const std::runtime_error &error) {
            if (std::string(error.what()) != "no iteration satisfied the selected evaluation target") throw;
            missing = true;
        }
        Check(missing, "missing or condition-rejected checkpoint was reported later");
    };
    request.condition = CompileConditionScript("car.completed_laps < " + std::to_string(finish->lap)).program;
    expectNoEvent();
    request.condition = CompileConditionScript("").program;
    request.evaluationTarget.settings["eventIndex"] = std::to_string(finish->eventIndex + 1);
    expectNoEvent();
#if FOREVERVALIDATOR_HAS_CUDA
    TestCudaEvents(packs, path, parsed.commands, reference);
#endif
}
}  // namespace

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    try {
        TestEvaluator();
        TestReplayAndSearch(argv[1], argv[2]);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
