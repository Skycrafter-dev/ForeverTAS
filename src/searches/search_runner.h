#ifndef FOREVERTAS_SEARCHES_SEARCH_RUNNER_H
#define FOREVERTAS_SEARCHES_SEARCH_RUNNER_H

#include "input_timeline_time.h"
#include "conditions/condition_program.h"
#include "mutations/input_event_formatter.h"
#include "physics_backend.h"
#include "searches/algorithm_registry.h"
#include "searches/search_algorithm.h"

#include <algorithm>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace forevertas {

inline constexpr std::uint32_t kSearchTickDurationMs =
        kInputTimelineTickDurationMs;
inline constexpr std::uint32_t kDefaultCudaParallelSampleCount = 256u;
inline constexpr std::uint32_t
        kDefaultCudaCalibrationStartSampleCount = 1u;
inline constexpr std::uint32_t kMaximumCpuWorkerCount = 256u;
inline constexpr std::uint32_t kDefaultSimulationHorizonMs = 6000u;
inline constexpr std::uint32_t kMaximumSimulationHorizonMs = 2147481040u;

inline std::uint32_t DefaultCpuWorkerCount() noexcept {
    const std::uint32_t detected = std::thread::hardware_concurrency();
    return detected == 0u
            ? 1u
            : std::min(detected, kMaximumCpuWorkerCount);
}

struct SearchRequest {
    SearchRequest(std::string packDirectoryValue,
                  std::string replayPathValue)
        : packDirectory(std::move(packDirectoryValue)),
          replayPath(std::move(replayPathValue)) {}

    std::string packDirectory;
    std::string replayPath;
    PhysicsBackend backend = PhysicsBackend::Reference;
    std::uint32_t parallelSampleCount = 1u;
    bool calibrateCudaParallelSampleCount = false;
    std::uint32_t cudaCalibrationStartSampleCount =
            kDefaultCudaCalibrationStartSampleCount;
    OptionConfiguration searchAlgorithm =
            DefaultSearchAlgorithmConfiguration();
    std::vector<OptionConfiguration> modifiers =
            DefaultModifierConfigurations();
    OptionConfiguration evaluationTarget =
            DefaultEvaluationTargetConfiguration();
    std::vector<ParsedInputCommand> baseInputCommands = {};
    bool useCudaSessionSpecialization = true;
    std::uint32_t simulationHorizonMs = kDefaultSimulationHorizonMs;
    std::optional<ConditionProgram> condition;
};

SearchResult RunSearch(
        const SearchRequest &request,
        const SearchRunControl *control = nullptr);

}  // namespace forevertas

#endif
