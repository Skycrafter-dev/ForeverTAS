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
inline constexpr std::uint32_t kMaximumCpuWorkerCount = 256u;
inline constexpr std::uint32_t kDefaultSimulationHorizonMs = 6000u;
inline constexpr std::uint32_t kMaximumSimulationHorizonMs = 2147481040u;
// A search thread's stack: searches run deep with large frames (a GPU
// session's set-up alone takes more than the 1 MB Windows gives a thread).
inline constexpr std::size_t kSearchThreadStackBytes = 64u * 1024u * 1024u;

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
    // Batch calibration, on CUDA and HIP alike.
    bool calibrateCudaParallelSampleCount = false;
    OptionConfiguration searchAlgorithm =
            DefaultSearchAlgorithmConfiguration();
    std::vector<OptionConfiguration> modifiers =
            DefaultModifierConfigurations();
    OptionConfiguration evaluationTarget =
            DefaultEvaluationTargetConfiguration();
    std::vector<ParsedInputCommand> baseInputCommands = {};
    // CUDA only: candidates on the twin's GPU engine, each new best rebuilt on
    // exact physics (optimized CPU) before it is reported ("Approximate").
    bool approximatePhysics = false;
    std::uint32_t simulationHorizonMs = kDefaultSimulationHorizonMs;
    std::optional<ConditionProgram> condition;
    // A device of a search on several devices that runs on another computer
    // (its remote worker's id); empty: this computer.
    std::string remoteDevice;
};

SearchResult RunSearch(
        const SearchRequest &request,
        const SearchRunControl *control = nullptr);

// One GPU of a search on several: its backend and batch settings.
struct SearchDevice {
    PhysicsBackend backend = PhysicsBackend::Cuda;
    std::uint32_t parallelSampleCount = 1u;
    bool calibrate = false;
    // A remote worker's id (docs/REMOTE.md): the device runs there.
    std::string remote;
};

// Runs a search request on one GPU (its backend's module).
using DeviceSearchRunner =
        std::function<SearchResult(const SearchRequest &, const SearchRunControl *)>;

// One search on several GPUs at once, sharing its best like CPU workers: each
// GPU runs the request through runOnDevice, with its own backend and batch
// settings and its own candidates.
SearchResult RunSearchOnDevices(
        const SearchRequest &request,
        const std::vector<SearchDevice> &devices,
        const DeviceSearchRunner &runOnDevice,
        const SearchRunControl *control = nullptr);

// Uses the search evaluator and condition, without mutation, promotion, or sampling.
// Auxiliary evaluation is always Optimized CPU, independently of the search device.
// Whether a best beats another, as a search on `request` ranks them (from its
// target's settings alone: no map needed).
std::function<bool(const SearchLiveUpdate &, const SearchLiveUpdate &)> LiveImproves(const SearchRequest &request);

std::optional<SearchResult> EvaluateBaseline(
        const SearchRequest &request, const SearchRunControl *control = nullptr);

// Where a search starts judging its target: the earliest input any pass may
// change (what came before is the base run's, the same in every iteration).
// Nullopt when a pass's settings are invalid.
std::optional<std::int64_t> EvaluationStartMs(
        const std::vector<OptionConfiguration> &modifiers,
        std::uint32_t simulationHorizonMs);

}  // namespace forevertas

#endif
