#include "compute/compute_local.h"

#include "searches/search_algorithm.h"

#include <forevervalidator/validation.h>

#include <exception>

namespace forevertas::compute {
namespace {

#if FOREVERVALIDATOR_HAS_TWIN
[[maybe_unused]] constexpr bool kTwin = true;
#else
[[maybe_unused]] constexpr bool kTwin = false;
#endif
#if FOREVERVALIDATOR_HAS_CUDA
[[maybe_unused]] constexpr bool kCuda = true;
#else
[[maybe_unused]] constexpr bool kCuda = false;
#endif
#if FOREVERVALIDATOR_HAS_HIP
[[maybe_unused]] constexpr bool kHip = true;
#else
[[maybe_unused]] constexpr bool kHip = false;
#endif

}  // namespace

bool LocalHas(PhysicsBackend backend) noexcept {
    switch (backend) {
    case PhysicsBackend::Cuda:
        return kCuda;
    case PhysicsBackend::Hip:
        return kHip;
    default:
        return true;
    }
}

void LocalStatus(PhysicsBackend backend, GpuStatus *out) {
    *out = {};
#if FOREVERVALIDATOR_HAS_CUDA
    if (backend == PhysicsBackend::Cuda) {
        const forevervalidator::CudaBackendDiagnostics cuda =
                forevervalidator::QueryCudaBackendDiagnostics();
        out->ready = cuda.IsReady();
        out->deviceName = cuda.deviceName;
        out->computeMajor = cuda.computeCapabilityMajor;
        out->computeMinor = cuda.computeCapabilityMinor;
        out->diagnostic = cuda.diagnostic;
        // the twin's GPU engine is built for CUDA when there is CUDA
        out->approximate = kTwin && out->ready;
        return;
    }
#endif
#if FOREVERVALIDATOR_HAS_HIP
    if (backend == PhysicsBackend::Hip) {
        const forevervalidator::HipBackendDiagnostics hip =
                forevervalidator::QueryHipBackendDiagnostics();
        out->ready = hip.IsReady();
        out->deviceName = hip.deviceName;
        out->diagnostic = hip.diagnostic;
        // ... and for HIP in builds without CUDA
        out->approximate = kTwin && !kCuda && out->ready;
        return;
    }
#endif
    static_cast<void>(backend);
    out->diagnostic = "not built in";
}

void LocalRunSearch(const SearchRequest &request, const SearchRunControl *control,
                    SearchOutcome *out) {
    *out = {};
    try {
        out->result.emplace(RunSearch(request, control));
        out->kind = SearchOutcome::Kind::Done;
    } catch (const NoEligibleEvaluation &error) {
        out->kind = SearchOutcome::Kind::NoEligibleEvaluation;
        out->message = error.what();
    } catch (const SearchCancelled &) {
        out->kind = SearchOutcome::Kind::Cancelled;
    } catch (const std::exception &error) {
        out->kind = SearchOutcome::Kind::Failed;
        out->message = error.what();
    } catch (...) {
        out->kind = SearchOutcome::Kind::Failed;
        out->message = "unknown failure in the GPU module";
    }
}

}  // namespace forevertas::compute
