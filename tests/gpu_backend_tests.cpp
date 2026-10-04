#include "physics_backend.h"
#include "searches/cuda_calibration_safety.h"
#include "searches/gpu_submission_budget.h"

#include <iostream>

int main() {
    static_assert(forevertas::kAuxiliarySimulationBackend ==
                  forevertas::PhysicsBackend::OptimizedCpu,
                  "winner verification and previews must remain optimized CPU");
    using namespace forevertas;
    using forevervalidator::SimulationBackend;
    bool okay = true;
    const auto check = [&](bool condition, const char *message) {
        if (!condition) {
            std::cerr << message << '\n';
            okay = false;
        }
    };
    check(!IsGpuBackend(PhysicsBackend::Reference) &&
                  !IsGpuBackend(PhysicsBackend::OptimizedCpu) &&
                  !IsGpuBackend(PhysicsBackend::MultiThreadedCpu),
          "CPU backends must not use GPU search dispatch");
#if FOREVERVALIDATOR_HAS_CUDA
    check(ParsePhysicsBackend("cuda") == PhysicsBackend::Cuda &&
                  IsGpuBackend(PhysicsBackend::Cuda) &&
                  ToForeverValidatorBackend(PhysicsBackend::Cuda) ==
                          SimulationBackend::Cuda,
          "CUDA routing was changed by Vulkan integration");
#else
    check(!ParsePhysicsBackend("cuda"), "uncompiled CUDA backend is selectable");
#endif
#if FOREVERVALIDATOR_HAS_VULKAN
    check(ParsePhysicsBackend("vulkan") == PhysicsBackend::Vulkan &&
                  IsGpuBackend(PhysicsBackend::Vulkan) &&
                  ToForeverValidatorBackend(PhysicsBackend::Vulkan) ==
                          SimulationBackend::Vulkan,
          "Vulkan must resolve to Vulkan, not CUDA or CPU");
#else
    check(!ParsePhysicsBackend("vulkan"), "uncompiled Vulkan backend is selectable");
#endif
#if FOREVERVALIDATOR_HAS_HIP
    check(ParsePhysicsBackend("hip") == PhysicsBackend::Hip &&
                  IsGpuBackend(PhysicsBackend::Hip) &&
                  ToForeverValidatorBackend(PhysicsBackend::Hip) ==
                          SimulationBackend::Hip,
          "HIP must resolve to HIP, not CUDA or CPU");
#else
    check(!ParsePhysicsBackend("hip"), "uncompiled HIP backend is selectable");
#endif

    CudaCalibrationSafetyPlanner planner;
    CudaCalibrationBatchProfile profile;
    profile.batchSize = 1u;
    profile.batchCapacity = 1u;
    profile.residentDeviceBytes = 64ull * 1024 * 1024;
    profile.kernelMilliseconds = 1.0;
    planner.Observe(profile);
    CudaCalibrationDeviceLimits limits;
    limits.totalMemoryBytes = 8ull * 1024 * 1024 * 1024;
    limits.freeMemoryBytes = limits.totalMemoryBytes;
    check(!planner.Evaluate(2u, 1u, limits).safe,
          "CUDA must still reject missing execution/occupancy limits");
    limits.requireCudaExecutionLimits = false;
    check(planner.Evaluate(2u, 1u, limits).safe,
          "Vulkan memory calibration incorrectly required CUDA counters");
    limits.freeMemoryBytes = 0;
    check(!planner.Evaluate(2u, 1u, limits).safe,
          "Vulkan calibration bypassed memory safety");
    limits.freeMemoryBytes = limits.totalMemoryBytes;
    check(!planner.Evaluate(UINT32_MAX, 1, limits).safe, "huge manual capacity passed the shared planner");
    CudaCalibrationSafetyPlanner cachedWorkspace;
    auto cached = profile;
    cached.reservationBytesPerCandidate = 1024 * 1024;
    cachedWorkspace.Observe(cached);
    cached.batchCapacity = cached.batchSize = 2;
    cachedWorkspace.Observe(cached);
    check(cachedWorkspace.Evaluate(256, 1, limits).safe &&
          !cachedWorkspace.Evaluate(UINT32_MAX, 1, limits).safe,
          "cached Vulkan workspace yielded a zero slope or scaled fixed scene bytes");
    limits.kernelExecutionTimeoutEnabled = true;
    profile.kernelMilliseconds = 300;
    planner.Observe(profile);
    check(!planner.Evaluate(1, 1, limits).safe, "long horizon ignored the display watchdog");
    limits.kernelBudgetMilliseconds = 1000;
    check(planner.Evaluate(1, 1, limits).safe &&
          planner.Evaluate(8, 1, limits).watchdogLimited,
          "manual submission budget did not distinguish ordinary and oversized work");
    GpuSubmissionBudget budget(true);
    const auto start = GpuSubmissionBudget::Clock::time_point{};
    check(!budget.Expired(start) && !budget.Expired(start + std::chrono::milliseconds(249)) &&
          budget.Expired(start + std::chrono::milliseconds(250)) && budget.WasExceeded(),
          "unprofiled display-device submission deadline failed");
    budget.Reset();
    check(!budget.Expired(start), "submission deadline did not reset");
    GpuSubmissionBudget headless(false);
    check(!headless.Expired(start) && !headless.Expired(start + std::chrono::hours(1)),
          "headless device was assigned a display watchdog");
    GpuSubmissionBudget manual(true, std::chrono::milliseconds(1000));
    check(!manual.Expired(start) && !manual.Expired(start + std::chrono::milliseconds(999)) &&
          manual.Expired(start + std::chrono::milliseconds(1000)),
          "manual submission deadline did not enforce its budget");
    return okay ? 0 : 1;
}
