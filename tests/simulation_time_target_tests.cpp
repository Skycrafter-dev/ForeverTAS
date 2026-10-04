#include "conditions/condition_program.h"
#include "evaluators/scripted_target_evaluator.h"
#include "mutations/random_steering_mutator.h"
#include "mutations/replay_input_script.h"
#include "searches/search_runner.h"

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
        {"min time.ms", "time.ms >= 30\ntime_since(last_restart.time) >= 0", "10", "100", {30}, false}
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
            std::cout << PhysicsBackendId(backend) << ": six simulation-time target cases passed\n";
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
