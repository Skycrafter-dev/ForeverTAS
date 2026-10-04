#include "conditions/condition_program.h"
#include "evaluators/scripted_target_evaluator.h"
#include "evaluators/time_evaluator.h"
#include "mutations/random_steering_mutator.h"
#include "mutations/input_insertion_mutator.h"
#include "mutations/replay_input_script.h"
#include "searches/search_runner.h"
#include "replay_file_io.h"
#include "mutations/modifier_utils.h"
#include <forevervalidator/native.h>

#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <vector>

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    using namespace forevertas;
    std::vector<PhysicsBackend> backends{PhysicsBackend::Reference, PhysicsBackend::OptimizedCpu};
#if FOREVERVALIDATOR_HAS_VULKAN
    if (forevervalidator::QueryVulkanBackendDiagnostics().IsSearchReady()) backends.push_back(PhysicsBackend::Vulkan);
    else std::cout << "SKIP unavailable Vulkan device\n";
#endif
#if FOREVERVALIDATOR_HAS_CUDA
    if (forevervalidator::QueryCudaBackendDiagnostics().IsReady()) backends.push_back(PhysicsBackend::Cuda);
    else std::cout << "SKIP unavailable CUDA device\n";
#endif
#if FOREVERVALIDATOR_HAS_HIP
    if (forevervalidator::QueryHipBackendDiagnostics().IsReady()) backends.push_back(PhysicsBackend::Hip);
    else std::cout << "SKIP unavailable HIP device\n";
#endif
    // Time target cases with fixed answers. With an always-true condition
    // the first match is the window start (at least the first 10 ms tick).
    struct Case {
        const char *goal;
        const char *condition;
        const char *minimum;
        const char *maximum;
        std::optional<double> expected;
        bool mutation;
    };
    const std::vector<Case> cases{
        {"earliest", "car.speed >= 0", "0", "100", 10.0, false},
        {"earliest", "car.speed >= 0", "50", "90", 50.0, false},
        {"latest", "car.speed >= 0", "20", "100", 20.0, false},
        {"earliest", "car.speed < 0", "10", "100", std::nullopt, false},
        {"earliest", "car.speed >= 0\niterations > 0", "10", "100", 10.0, true},
        {"earliest", "car.speed >= 0\ntime_since(last_restart.time) >= 0", "10", "100", 10.0, false},
        {"earliest", "car.completed_laps >= 1", "10", "100", std::nullopt, false},
        {"earliest", "# disabled\n(car.speed >= 1000 || car.speed >= 0) AND car.cps != 1 // entry", "10", "100", 10.0, false},
        {"earliest", "car.speed >= 0 OR car.speed >= 1000 AND car.cps != 0", "10", "100", 10.0, false},
        {"earliest", "(car.speed >= 0 OR car.speed >= 1000) AND car.cps != 0", "10", "100", std::nullopt, false},
        {"latest", "car.speed != -1 && (car.cps <= 0 OR car.cps >= 5)", "30", "100", 30.0, false}
    };
    const auto timeTarget = [](const char *goal, const char *minimum, const char *maximum) {
        auto target = DefaultTimeOptionSettings();
        target["goal"] = goal;
        target["minTimeMs"] = minimum;
        target["maxTimeMs"] = maximum;
        return OptionConfiguration{kTimeEvaluationId, target};
    };
    const auto runOrNothing = [](const SearchRequest &request, SearchRunControl &control)
            -> std::optional<SearchResult> {
        try {
            return RunSearch(request, &control);
        } catch (const std::runtime_error &error) {
            if (std::string(error.what()) == "no iteration satisfied the selected evaluation target")
                return std::nullopt;
            throw;
        }
    };
    // Data-dependent first matches: Reference decides, every backend must agree.
    std::map<std::string, std::optional<SearchResult>> referenceResults;
    try {
        for (const auto backend : backends) {
            for (const auto &test : cases) {
                SearchRequest request(argv[1], argv[2]);
                request.backend = backend;
                request.parallelSampleCount = IsGpuBackend(backend) ? 4u : 1u;
                request.simulationHorizonMs = 120;
                request.baseInputCommands = ParseInputScript("0.00 steer 0\n0.00 press up").commands;
                auto modifier = DefaultRandomSteeringOptionSettings();
                modifier["minTimeMs"] = "0";
                modifier["maxTimeMs"] = "100";
                request.modifiers = {{kRandomSteeringModifierId, modifier}};
                request.evaluationTarget = timeTarget(test.goal, test.minimum, test.maximum);
                request.condition = CompileConditionScript(test.condition).program;
                SearchRunControl control;
                control.iterationLimit = 4;
                control.sampleBestTimeline = false;
                control.reuseLoadedSandbox = true;
                const auto result = runOrNothing(request, control);
                if (result.has_value() != test.expected.has_value() ||
                    (result && (result->bestScore != *test.expected ||
                                (result->winnerSource == SearchWinnerSource::Mutation) != test.mutation))) {
                    std::cerr << PhysicsBackendId(backend) << ' ' << test.goal << " / " << test.condition
                              << ": unexpected result " << (result ? result->bestScore : -1.0)
                              << " winner=" << (result ? static_cast<int>(result->winnerSource) : -1) << '\n';
                    return 1;
                }
            }
            for (const char *goal : {"earliest", "latest"}) {
                SearchRequest request(argv[1], argv[2]);
                request.backend = backend;
                request.parallelSampleCount = IsGpuBackend(backend) ? 4u : 1u;
                request.simulationHorizonMs = 3000;
                request.baseInputCommands = ParseInputScript("0.00 steer 0\n0.00 press up").commands;
                auto modifier = DefaultRandomSteeringOptionSettings();
                modifier["minTimeMs"] = "0";
                modifier["maxTimeMs"] = "2000";
                modifier["seed"] = "1234";
                request.modifiers = {{kRandomSteeringModifierId, modifier}};
                request.evaluationTarget = timeTarget(goal, "0", "3000");
                request.condition = CompileConditionScript("car.speed >= 8").program;
                SearchRunControl control;
                control.iterationLimit = 16;
                control.sampleBestTimeline = false;
                const auto result = runOrNothing(request, control);
                auto &reference = referenceResults[goal];
                if (backend == backends.front()) {
                    if (!result) throw std::runtime_error("reference never reached 8 m/s");
                    reference = result;
                    std::cout << goal << " first reached 8 m/s at " << result->bestScore
                              << " ms (" << (result->winnerSource == SearchWinnerSource::Mutation
                                                     ? "mutation" : "baseline") << ")\n";
                } else if (!result || !reference || result->bestScore != reference->bestScore ||
                           result->bestEvaluationTimeMs != reference->bestEvaluationTimeMs ||
                           result->winnerSource != reference->winnerSource ||
                           result->winningIterationIndex != reference->winningIterationIndex) {
                    std::cerr << PhysicsBackendId(backend) << ' ' << goal
                              << ": first match differs from Reference ("
                              << (result ? result->bestScore : -1.0) << " vs "
                              << (reference ? reference->bestScore : -1.0) << ")\n";
                    return 1;
                }
            }
            std::cout << PhysicsBackendId(backend) << ": " << cases.size() << " Time target cases and Reference parity passed\n";
            {
                SearchRequest insertion(argv[1], argv[2]);
                insertion.backend = backend;
                insertion.parallelSampleCount = IsGpuBackend(backend) ? 4u : 1u;
                insertion.simulationHorizonMs = 200;
                insertion.baseInputCommands = ParseInputScript("0.00 steer 0\n0.00 press up").commands;
                auto modifier = DefaultInputInsertionSettings();
                modifier["minTimeMs"] = modifier["maxTimeMs"] = "100";
                modifier["steerMode"] = "absolute";
                modifier["steerAbsoluteMin"] = modifier["steerAbsoluteMax"] = "0";
                modifier["steerMinCount"] = modifier["steerMaxCount"] = "1";
                modifier["steerMaxHoldMs"] = "0";
                insertion.modifiers = {{kInputInsertionModifierId, modifier}};
                insertion.evaluationTarget = timeTarget("latest", "0", "200");
                insertion.condition = CompileConditionScript("car.speed >= 0").program;
                SearchRunControl control;
                control.iterationLimit = 4;
                control.sampleBestTimeline = false;
                const auto result = RunSearch(insertion, &control);
                if (result.totalMutationCount != 0 || result.iterations != 4)
                    throw std::runtime_error(std::string(PhysicsBackendId(backend)) + ": redundant insertions were counted as mutations");
            }
            if (IsGpuBackend(backend)) {
                using namespace forevervalidator;
                using namespace forevervalidator::experimental;
                const auto require = [](auto result) {
                    if (!result) throw std::runtime_error("GPU insertion fixture setup failed: " + result.Error().diagnostic);
                    return std::move(result).Value();
                };
                ReplayIdentity identity{argv[2]};
                const auto bytes = require(ReadReplayFileUtf8(argv[2], identity));
                PhysicsSandboxOptions options;
                options.backend = ToForeverValidatorBackend(backend);
                options.timelineMode = PhysicsSandboxTimelineMode::Canonical;
                options.simulationHorizonMs = 200;
                auto sandbox = require(CreatePhysicsSandbox(require(OpenInstalledPackDirectory(argv[1])), options));
                require(sandbox.LoadReplay({bytes.data(), bytes.size()}, identity));
                const auto fixed = require(sandbox.ReadInputs());
                PhysicsSandboxCudaInputInsertionModifier insertion;
                insertion.window.minimumTimeMs = insertion.window.maximumTimeMs = 100;
                insertion.steering = {true, 1, 1, 0};
                insertion.steeringAbsoluteMinimum = insertion.steeringAbsoluteMaximum = 0;
                PhysicsSandboxCudaSearchConfiguration configuration;
                configuration.maximumBatchSize = 4;
                configuration.earliestMutationTimeMs = 10;
                configuration.evaluationStartTimeMs = 10;
                configuration.evaluationEndTimeMs = 200;
                configuration.modifiers = {insertion};
                configuration.evaluator = PhysicsSandboxCudaVelocityEvaluator{};
                for (const bool legacy : {false, true}) {
                    configuration.useLegacyMutationPipelineForTesting = legacy;
                    for (int variant = 0; variant < 4; ++variant) {
                        auto inputs = fixed;
                        inputs.push_back(AnalogEvent(0, SandboxInputAction::Steer, variant == 1 ? 65536 : 0));
                        if (variant >= 2) inputs.push_back(SwitchEvent(variant == 2 ? 0 : 300,
                                SandboxInputAction::SteerRight, false));
                        require(sandbox.ReplaceInputs(inputs));
                        auto session = require(CreatePhysicsSandboxCudaSearchSession(sandbox, configuration));
                        require(session.EvaluateBaseline());
                        const auto batch = require(session.RunBatch(0, 4));
                        if ((batch.totalMutationCount == 0) != (variant == 0))
                            throw std::runtime_error("sparse/materialized insertion pruning lost a release or ignored digital steering");
                    }
                }
                SearchRequest safety(argv[1], argv[2]);
                safety.backend = backend;
                safety.parallelSampleCount = UINT32_MAX;
                safety.simulationHorizonMs = 100;
                safety.baseInputCommands = ParseInputScript("0.00 steer 0\n0.00 press up").commands;
                auto modifier = DefaultRandomSteeringOptionSettings();
                modifier["minTimeMs"] = "0";
                modifier["maxTimeMs"] = "90";
                safety.modifiers = {{kRandomSteeringModifierId, modifier}};
                safety.evaluationTarget = timeTarget("latest", "0", "100");
                safety.condition = CompileConditionScript("car.speed >= 0").program;
                SearchRunControl bounded;
                bounded.sampleBestTimeline = false;
                bounded.iterationLimit = 1;
                if (RunSearch(safety, &bounded).iterations != 1)
                    throw std::runtime_error("manual capacity was not clamped to remaining attempts before allocation");
                bounded.iterationLimit = UINT32_MAX;
                bool rejected = false;
                try { RunSearch(safety, &bounded); }
                catch (const std::runtime_error &error) {
                    rejected = std::string(error.what()).find("Manual GPU batch rejected before allocation/dispatch") != std::string::npos;
                }
                if (!rejected) throw std::runtime_error("huge manual GPU batch was not rejected actionably");
            }
            SearchRequest finish(argv[1], argv[2]);
            finish.backend = backend;
            finish.parallelSampleCount = 1u;
            finish.simulationHorizonMs = 60000;
            finish.baseInputCommands = ParseInputScript(ExtractReplayInputScript(argv[1], argv[2])).commands;
            auto target = DefaultScriptedTargetOptionSettings();
            target["script"] = "max car.completed_laps";
            target["maxTimeMs"] = "60000";
            finish.evaluationTarget = {kScriptedTargetEvaluationId, target};
            finish.condition = CompileConditionScript("car.laps >= 1").program;
            SearchRunControl finishControl;
            finishControl.iterationLimit = 0;
            finishControl.sampleBestTimeline = false;
            const auto completed = RunSearch(finish, &finishControl);
            if (completed.metricValues != std::vector<double>{1}) {
                std::cerr << PhysicsBackendId(backend) << ": final finish not counted as a completed lap\n";
                return 1;
            }
            finish.evaluationTarget = timeTarget("earliest", "0", "60000");
            const auto lap = RunSearch(finish, &finishControl);
            auto &finishReference = referenceResults["finish"];
            if (backend == backends.front()) finishReference = lap;
            else if (!finishReference || lap.bestScore != finishReference->bestScore) {
                std::cerr << PhysicsBackendId(backend) << ": first completed lap differs from Reference ("
                          << lap.bestScore << ")\n";
                return 1;
            }
            std::cout << PhysicsBackendId(backend) << ": first completed lap at " << lap.bestScore << " ms\n";
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
