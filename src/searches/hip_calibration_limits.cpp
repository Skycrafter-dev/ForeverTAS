#include "searches/hip_calibration_limits.h"

#include <stdexcept>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmacro-redefined"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wunused-parameter"
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include <hip/hip_runtime_api.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace forevertas {

CudaCalibrationDeviceLimits QueryHipCalibrationDeviceLimits() {
    int device = 0;
    hipDeviceProp_t properties{};
    std::size_t freeMemory = 0u;
    std::size_t totalMemory = 0u;
    int kernelExecutionTimeoutEnabled = 0;
    hipError_t error = hipGetDevice(&device);
    if (error == hipSuccess) {
        error = hipGetDeviceProperties(&properties, device);
    }
    if (error == hipSuccess) {
        error = hipMemGetInfo(&freeMemory, &totalMemory);
    }
    if (error == hipSuccess) {
        error = hipDeviceGetAttribute(
                &kernelExecutionTimeoutEnabled,
                hipDeviceAttributeKernelExecTimeout, device);
    }
    if (error != hipSuccess) {
        throw std::runtime_error(
                std::string("querying HIP calibration safety limits failed: ") +
                hipGetErrorString(error));
    }
    CudaCalibrationDeviceLimits limits;
    limits.totalMemoryBytes = totalMemory;
    limits.freeMemoryBytes = freeMemory;
    limits.maximumThreadsPerBlock = properties.maxThreadsPerBlock;
    limits.maximumGridDimensionX = properties.maxGridSize[0];
    limits.registersPerBlock = properties.regsPerBlock;
    limits.registersPerMultiprocessor = properties.regsPerMultiprocessor;
    limits.maximumThreadsPerMultiprocessor =
            properties.maxThreadsPerMultiProcessor;
    limits.maximumBlocksPerMultiprocessor =
            properties.maxBlocksPerMultiProcessor;
    limits.multiprocessorCount = properties.multiProcessorCount;
    limits.kernelExecutionTimeoutEnabled =
            kernelExecutionTimeoutEnabled != 0;
    return limits;
}

}  // namespace forevertas
