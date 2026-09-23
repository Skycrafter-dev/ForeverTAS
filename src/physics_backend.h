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
#if FOREVERVALIDATOR_HAS_CUDA
    Cuda,
#endif
#if FOREVERVALIDATOR_HAS_VULKAN
    Vulkan,
#endif
};

constexpr std::string_view PhysicsBackendId(PhysicsBackend backend) noexcept {
    switch (backend) {
    case PhysicsBackend::Reference:
        return "reference";
    case PhysicsBackend::OptimizedCpu:
        return "optimized-cpu";
    case PhysicsBackend::MultiThreadedCpu:
        return "multi-threaded-cpu";
#if FOREVERVALIDATOR_HAS_CUDA
    case PhysicsBackend::Cuda:
        return "cuda";
#endif
#if FOREVERVALIDATOR_HAS_VULKAN
    case PhysicsBackend::Vulkan:
        return "vulkan";
#endif
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
#if FOREVERVALIDATOR_HAS_CUDA
    if (id == PhysicsBackendId(PhysicsBackend::Cuda)) {
        return PhysicsBackend::Cuda;
    }
#endif
#if FOREVERVALIDATOR_HAS_VULKAN
    if (id == PhysicsBackendId(PhysicsBackend::Vulkan)) {
        return PhysicsBackend::Vulkan;
    }
#endif
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
#if FOREVERVALIDATOR_HAS_CUDA
    case PhysicsBackend::Cuda:
        return forevervalidator::SimulationBackend::Cuda;
#endif
#if FOREVERVALIDATOR_HAS_VULKAN
    case PhysicsBackend::Vulkan:
        return forevervalidator::SimulationBackend::Vulkan;
#endif
    }
    return forevervalidator::SimulationBackend::Reference;
}

constexpr bool IsGpuSimulationBackend(
        forevervalidator::SimulationBackend backend) noexcept {
    return backend == forevervalidator::SimulationBackend::Cuda ||
           backend == forevervalidator::SimulationBackend::Vulkan;
}

constexpr bool IsGpuBackend(PhysicsBackend backend) noexcept {
    return IsGpuSimulationBackend(ToForeverValidatorBackend(backend));
}

}  // namespace forevertas

#endif
