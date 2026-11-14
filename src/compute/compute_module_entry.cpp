// The one symbol a GPU module exports (only built into modules).
#include "compute/compute_local.h"
#include "diagnostics/diagnostics_log.h"
#include "forevertas_compute_abi.h"

#if defined(_WIN32)
#define FOREVERTAS_COMPUTE_EXPORT extern "C" __declspec(dllexport)
#else
#define FOREVERTAS_COMPUTE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

using namespace forevertas;

#if FOREVERVALIDATOR_HAS_CUDA
constexpr PhysicsBackend kBackend = PhysicsBackend::Cuda;
#else
constexpr PhysicsBackend kBackend = PhysicsBackend::Hip;
#endif

void UseLog(std::FILE *stream) {
    diagnostics::UseLogFile(stream);
}

void Status(compute::GpuStatus *out) {
    compute::LocalStatus(kBackend, out);
}

const compute::ModuleApi kApi{
        kComputeAbi,
        kBackend == PhysicsBackend::Cuda ? "cuda" : "hip",
        &UseLog,
        &Status,
        &compute::LocalRunSearch,
};

}  // namespace

FOREVERTAS_COMPUTE_EXPORT const forevertas::compute::ModuleApi *ForeverTasComputeModule() {
    return &kApi;
}
