#include "conditions/condition_program.h"
#include "evaluators/scripted_target_evaluator.h"
#include "mutations/random_steering_mutator.h"
#include "mutations/input_insertion_mutator.h"
#include "mutations/replay_input_script.h"
#include "searches/search_runner.h"
#include "replay_file_io.h"
#include "mutations/modifier_utils.h"
#include <forevervalidator/native.h>

#include <iostream>
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
    struct Case {
        const char *script;
        const char *condition;
        const char *minimum;
        const char *maximum;
        std::vector<double> expected;
        bool mutation;
    };
    const std::vector<Case> cases{
        {"min time.ms", "time.ms >= 30", "10", "100", {30}, false},
        {"min time.ms", "time.ms >= 30", "50", "90", {50}, false},
        {"max time.ms", "time.ms <= 80", "20", "100", {80}, false},
        {"min time.ms", "time.ms < 0", "10", "100", {}, false},
        {"min time.ms", "time.ms >= 30\niterations > 0", "10", "100", {30}, true},
        {"min time.ms", "time.ms >= 30\ntime_since(last_restart.time) >= 0", "10", "100", {30}, false},
        {"max car.completed_laps", "car.laps = 0", "10", "100", {0}, false},
        {"min time.ms", "car.completed_laps >= 1", "10", "100", {}, false},
        {"min time.ms", "# disabled\n(time.ms == 30 || time.ms >= 80) AND car.cps != 1 // entry", "10", "100", {30}, false},
        {"min time.ms", "time.ms == 30 OR time.ms = 50 AND car.cps != 0", "10", "100", {30}, false},
        {"min time.ms", "(time.ms == 30 OR time.ms = 50) AND car.cps != 0", "10", "100", {}, false},
        {"max time.ms", "time.ms != 100 && (time.ms <= 50 OR time.ms >= 80)", "10", "100", {90}, false}
    };
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
                auto target = DefaultScriptedTargetOptionSettings();
                target["script"] = test.script;
                target["minTimeMs"] = test.minimum;
                target["maxTimeMs"] = test.maximum;
                request.evaluationTarget = {kScriptedTargetEvaluationId, target};
                request.condition = CompileConditionScript(test.condition).program;
                SearchRunControl control;
                control.iterationLimit = 4;
                control.sampleBestTimeline = false;
                control.reuseLoadedSandbox = true;
                const auto run = [&]() -> std::optional<SearchResult> {
                    try {
                        return RunSearch(request, &control);
                    } catch (const std::runtime_error &error) {
                        if (test.expected.empty() && std::string(error.what()) ==
                                "no iteration satisfied the selected evaluation target") return std::nullopt;
                        throw;
                    }
                };
                const auto actual = run();
                if (!actual) continue;
                const auto &result = *actual;
                if (result.metricValues != test.expected ||
                    (result.winnerSource == SearchWinnerSource::Mutation) != test.mutation) {
                    std::cerr << PhysicsBackendId(backend) << ' ' << test.script << " / " << test.condition
                              << ": unexpected metric or winner; metrics=";
                    for (const auto value : result.metricValues) std::cerr << value << ',';
                    std::cerr << " winner=" << static_cast<int>(result.winnerSource) << '\n';
                    return 1;
                }
            }
            std::cout << PhysicsBackendId(backend) << ": " << cases.size() << " simulation-time/lap target cases passed\n";
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
                auto objective = DefaultScriptedTargetOptionSettings();
                objective["script"] = "max time.ms";
                objective["maxTimeMs"] = "200";
                insertion.evaluationTarget = {kScriptedTargetEvaluationId, objective};
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
                auto objective = DefaultScriptedTargetOptionSettings();
                objective["script"] = "max time.ms";
                objective["maxTimeMs"] = "100";
                safety.evaluationTarget = {kScriptedTargetEvaluationId, objective};
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
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
