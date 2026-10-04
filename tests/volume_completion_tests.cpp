#include "conditions/condition_program.h"
#include "evaluators/velocity_evaluator.h"
#include "evaluators/volume_entry_evaluator.h"
#include "mutations/existing_event_perturbation_mutator.h"
#include "mutations/replay_input_script.h"
#include "searches/search_runner.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    using namespace forevertas;
    try {
        SearchRequest request(argv[1], argv[2]);
        request.backend = PhysicsBackend::OptimizedCpu;
        request.simulationHorizonMs = 3000;
        // Each mutation changes a real event after both tested entries, so the
        // candidates are evaluated but tie the baseline instead of being no-ops.
        request.baseInputCommands = ParseInputScript("0.00 press up\n2.99 steer 0").commands;
        auto modifier = DefaultExistingEventPerturbationSettings();
        modifier["minTimeMs"] = "0";
        modifier["maxTimeMs"] = "2990";
        modifier["maxTimeShiftMs"] = "0";
        modifier["steerDeltaMin"] = "1";
        modifier["steerDeltaMax"] = "1";
        modifier["toggleAccelerate"] = "false";
        modifier["toggleBrake"] = "false";
        request.modifiers = {{kExistingEventPerturbationModifierId, modifier}};
        auto velocity = DefaultVelocityOptionSettings();
        velocity["minTimeMs"] = "10";
        velocity["maxTimeMs"] = "3000";
        request.evaluationTarget = {kVelocityEvaluationId, velocity};
        SearchRunControl control;
        control.iterationLimit = 0;
        control.sampleBestTimeline = true;
        const auto probe = RunSearch(request, &control);
        std::vector<PhysicsBackend> backends{PhysicsBackend::Reference, PhysicsBackend::OptimizedCpu};
#if FOREVERVALIDATOR_HAS_VULKAN
        if (forevervalidator::QueryVulkanBackendDiagnostics().IsSearchReady()) backends.push_back(PhysicsBackend::Vulkan);
#endif
#if FOREVERVALIDATOR_HAS_CUDA
        if (forevervalidator::QueryCudaBackendDiagnostics().IsReady()) backends.push_back(PhysicsBackend::Cuda);
#endif
#if FOREVERVALIDATOR_HAS_HIP
        if (forevervalidator::QueryHipBackendDiagnostics().IsReady()) backends.push_back(PhysicsBackend::Hip);
#endif
        for (const int entryTime : {1000, 2800}) {
            const auto frame = std::find_if(probe.bestTimeline.begin(), probe.bestTimeline.end(),
                    [=](const auto &value) { return value.timeMs == entryTime; });
            if (frame == probe.bestTimeline.end()) throw std::runtime_error("missing volume fixture frame");
            auto target = DefaultVolumeEntryOptionSettings();
            target["centerX"] = std::to_string(frame->positionX);
            target["centerY"] = std::to_string(frame->positionY);
            target["centerZ"] = std::to_string(frame->positionZ);
            target["sizeX"] = target["sizeY"] = target["sizeZ"] = "1";
            request.evaluationTarget = {kVolumeEntryEvaluationId, target};
            std::optional<SearchResult> expected;
            for (const auto backend : backends) {
                request.backend = backend;
                request.parallelSampleCount = IsGpuBackend(backend) ? 4u : 1u;
                control.iterationLimit = 4;
                const auto result = RunSearch(request, &control);
                const auto ticksPerCandidate = result.bestState.timeMs / 10u;
                if (!IsGpuBackend(backend) && result.evaluatorCalls != ticksPerCandidate * 5u)
                    throw std::runtime_error("equal-score candidates kept simulating after volume entry");
                if (result.iterations != 4u || result.winnerSource != SearchWinnerSource::Baseline ||
                    result.bestTimeline.empty() || result.bestTimeline.back().timeMs != 3000)
                    throw std::runtime_error("completion changed the winner or truncated its full preview");
                if (expected && (std::abs(result.bestScore - expected->bestScore) > 1e-5 ||
                                 result.bestState.timeMs != expected->bestState.timeMs))
                    throw std::runtime_error("volume entry CPU/GPU mismatch");
                expected = result;
                std::cout << PhysicsBackendId(backend) << " entry=" << result.bestScore
                          << " calls=" << result.evaluatorCalls << " full-window budget=1500\n";
            }
        }
        // A rejected crossing must not turn into an eligible zero-time result.
        request.condition = CompileConditionScript("time.ms < 0").program;
        control.sampleBestTimeline = false;
        for (const auto backend : backends) {
            request.backend = backend;
            request.parallelSampleCount = 1u;
            try {
                RunSearch(request, &control);
                throw std::runtime_error("condition-rejected volume entry was accepted");
            } catch (const std::runtime_error &error) {
                if (std::string(error.what()) != "no iteration satisfied the selected evaluation target") throw;
            }
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
