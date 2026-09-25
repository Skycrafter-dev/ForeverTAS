#include "physics_backend.h"
#include "searches/cuda_calibration_safety.h"

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
    return okay ? 0 : 1;
}
