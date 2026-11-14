#ifndef FOREVERTAS_PHYSICS_BACKEND_H
#define FOREVERTAS_PHYSICS_BACKEND_H

#include <forevervalidator/validation.h>

#include <cstdint>
#include <optional>
#include <string_view>

namespace forevertas {

enum class PhysicsBackend : std::uint8_t {
    Reference,
    OptimizedCpu,
    MultiThreadedCpu,
    Cuda,
    Hip,
};

// Single-run reconstruction and viewer simulation are intentionally fixed to
// optimized CPU. The selected backend is only for bulk search attempts.
inline constexpr PhysicsBackend kAuxiliarySimulationBackend =
        PhysicsBackend::OptimizedCpu;

constexpr std::string_view PhysicsBackendId(PhysicsBackend backend) noexcept {
    switch (backend) {
    case PhysicsBackend::Reference:
        return "reference";
    case PhysicsBackend::OptimizedCpu:
        return "optimized-cpu";
    case PhysicsBackend::MultiThreadedCpu:
        return "multi-threaded-cpu";
    case PhysicsBackend::Cuda:
        return "cuda";
    case PhysicsBackend::Hip:
        return "hip";
    }
    return "reference";
}

inline std::optional<PhysicsBackend> ParsePhysicsBackend(
        std::string_view id) noexcept {
    if (id == PhysicsBackendId(PhysicsBackend::Reference)) {
        return PhysicsBackend::Reference;
    }
    if (id == PhysicsBackendId(PhysicsBackend::OptimizedCpu)) {
        return PhysicsBackend::OptimizedCpu;
    }
    if (id == PhysicsBackendId(PhysicsBackend::MultiThreadedCpu)) {
        return PhysicsBackend::MultiThreadedCpu;
    }
    if (id == PhysicsBackendId(PhysicsBackend::Cuda)) {
        return PhysicsBackend::Cuda;
    }
    if (id == PhysicsBackendId(PhysicsBackend::Hip)) {
        return PhysicsBackend::Hip;
    }
    return std::nullopt;
}

constexpr forevervalidator::SimulationBackend ToForeverValidatorBackend(
        PhysicsBackend backend) noexcept {
    switch (backend) {
    case PhysicsBackend::Reference:
        return forevervalidator::SimulationBackend::Reference;
    case PhysicsBackend::OptimizedCpu:
    case PhysicsBackend::MultiThreadedCpu:
        return forevervalidator::SimulationBackend::OptimizedCpu;
    case PhysicsBackend::Cuda:
        return forevervalidator::SimulationBackend::Cuda;
    case PhysicsBackend::Hip:
        return forevervalidator::SimulationBackend::Hip;
    }
    return forevervalidator::SimulationBackend::Reference;
}

constexpr bool IsGpuSimulationBackend(
        forevervalidator::SimulationBackend backend) noexcept {
    return backend == forevervalidator::SimulationBackend::Cuda ||
           backend == forevervalidator::SimulationBackend::Hip;
}

constexpr bool IsGpuBackend(PhysicsBackend backend) noexcept {
    return IsGpuSimulationBackend(ToForeverValidatorBackend(backend));
}

}  // namespace forevertas

#endif
