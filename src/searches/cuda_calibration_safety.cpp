#include "searches/cuda_calibration_safety.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace forevertas {
namespace {

constexpr std::uint64_t kMinimumMemoryHeadroomBytes =
        512ull * 1024ull * 1024ull;
constexpr double kMemoryHeadroomFraction = 0.15;
constexpr double kAllocationEstimateMargin = 1.15;
constexpr double kGridLimitFraction = 0.90;
constexpr double kWatchdogKernelBudgetMilliseconds = 250.0;
constexpr double kKernelPredictionMargin = 1.25;
constexpr std::uint32_t kMaximumUnrepresentativeGrowthFactor = 8u;

std::uint64_t SaturatingCeil(double value) {
    if (!std::isfinite(value) || value <= 0.0) {
        return 0u;
    }
    if (value >= static_cast<double>(
                         std::numeric_limits<std::uint64_t>::max())) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(std::ceil(value));
}

CudaCalibrationSafetyDecision Unsafe(std::string reason) {
    CudaCalibrationSafetyDecision result;
    result.reason = std::move(reason);
    return result;
}

std::uint64_t SaturatingAdd(std::uint64_t left, std::uint64_t right) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left + right;
}

std::uint64_t SaturatingMultiply(
        std::uint64_t left,
        std::uint64_t right) {
    if (left != 0u &&
        right > std::numeric_limits<std::uint64_t>::max() / left) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left * right;
}

std::uint64_t SimulationWaveCapacity(
        const CudaCalibrationBatchProfile &profile,
        const CudaCalibrationDeviceLimits &limits) {
    return SaturatingMultiply(
            SaturatingMultiply(
                    profile.simulationThreadsPerBlock,
                    profile.simulationActiveBlocksPerMultiprocessor),
            limits.multiprocessorCount);
}

std::uint64_t RepresentativeBatchSize(
        const CudaCalibrationBatchProfile &profile,
        const CudaCalibrationDeviceLimits &limits) {
    return SimulationWaveCapacity(profile, limits);
}

bool HasRepresentativeProfile(
        const std::vector<CudaCalibrationBatchProfile> &profiles,
        const CudaCalibrationDeviceLimits &limits) {
    return std::any_of(
            profiles.begin(),
            profiles.end(),
            [&limits](const CudaCalibrationBatchProfile &profile) {
                const std::uint64_t representativeBatchSize =
                        RepresentativeBatchSize(profile, limits);
                return representativeBatchSize != 0u &&
                        profile.batchSize >= representativeBatchSize;
            });
}

std::uint32_t LargestMeasuredBatchSize(
        const std::vector<CudaCalibrationBatchProfile> &profiles) {
    std::uint32_t result = 0u;
    for (const CudaCalibrationBatchProfile &profile : profiles) {
        result = std::max(result, profile.batchSize);
    }
    return result;
}

std::uint64_t WaveCount(std::uint32_t batchSize,
                        std::uint64_t waveCapacity) {
    if (batchSize == 0u || waveCapacity == 0u) {
        return 0u;
    }
    return (static_cast<std::uint64_t>(batchSize) - 1u) /
                    waveCapacity +
            1u;
}

}  // namespace

void CudaCalibrationSafetyPlanner::Observe(
        const CudaCalibrationBatchProfile &profile) {
    if (profile.batchSize == 0u || profile.batchCapacity == 0u ||
        profile.residentDeviceBytes == 0u) {
        return;
    }
    const auto existing = std::find_if(
            profiles_.begin(),
            profiles_.end(),
            [&profile](const CudaCalibrationBatchProfile &candidate) {
                return candidate.batchCapacity ==
                               profile.batchCapacity &&
                       candidate.batchSize == profile.batchSize;
            });
    if (existing == profiles_.end()) {
        profiles_.push_back(profile);
        return;
    }
    existing->residentDeviceBytes = std::max(
            existing->residentDeviceBytes,
            profile.residentDeviceBytes);
    existing->kernelMilliseconds = std::max(
            existing->kernelMilliseconds,
            profile.kernelMilliseconds);
    existing->simulationThreadsPerBlock =
            profile.simulationThreadsPerBlock;
    existing->simulationRegistersPerThread =
            profile.simulationRegistersPerThread;
    existing->simulationLocalBytesPerThread =
            profile.simulationLocalBytesPerThread;
    existing->simulationActiveBlocksPerMultiprocessor =
            profile.simulationActiveBlocksPerMultiprocessor;
    existing->simulationTheoreticalOccupancy =
            profile.simulationTheoreticalOccupancy;
}

std::optional<std::uint32_t>
CudaCalibrationSafetyPlanner::NextStagedProbe(
        std::uint32_t requestedBatchSize,
        const CudaCalibrationDeviceLimits &limits) const {
    if (requestedBatchSize <= 1u || profiles_.empty() ||
        HasRepresentativeProfile(profiles_, limits)) {
        return std::nullopt;
    }
    const std::uint32_t largestMeasured =
            LargestMeasuredBatchSize(profiles_);
    const bool hasCapacitySlope = std::any_of(
            profiles_.begin(),
            profiles_.end(),
            [](const CudaCalibrationBatchProfile &profile) {
                return profile.batchCapacity > 1u;
            });
    if (!hasCapacitySlope) {
        return std::min(requestedBatchSize, 2u);
    }
    const std::uint64_t stagedLimit =
            static_cast<std::uint64_t>(largestMeasured) *
            kMaximumUnrepresentativeGrowthFactor;
    if (requestedBatchSize <= stagedLimit) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
            requestedBatchSize, stagedLimit));
}

CudaCalibrationSafetyDecision CudaCalibrationSafetyPlanner::Evaluate(
        std::uint32_t candidateBatchSize,
        std::uint32_t currentBatchCapacity,
        const CudaCalibrationDeviceLimits &limits) const {
    if (candidateBatchSize == 0u) {
        return Unsafe("CUDA batch size is zero");
    }
    if (profiles_.empty()) {
        return Unsafe(
                "CUDA calibration has no measured device profile");
    }
    if (limits.totalMemoryBytes == 0u || limits.freeMemoryBytes == 0u ||
        limits.maximumThreadsPerBlock == 0u ||
        limits.maximumGridDimensionX == 0u ||
        limits.registersPerBlock == 0u ||
        limits.registersPerMultiprocessor == 0u ||
        limits.maximumThreadsPerMultiprocessor == 0u ||
        limits.maximumBlocksPerMultiprocessor == 0u ||
        limits.multiprocessorCount == 0u) {
        return Unsafe("CUDA device limits are incomplete");
    }

    const CudaCalibrationBatchProfile &profile =
            ProfileForBatchSize(candidateBatchSize);
    const std::uint32_t threads = profile.simulationThreadsPerBlock;
    const std::uint32_t registers =
            profile.simulationRegistersPerThread;
    const std::uint32_t activeBlocks =
            profile.simulationActiveBlocksPerMultiprocessor;
    const double occupancy = profile.simulationTheoreticalOccupancy;
    if (threads == 0u || threads > limits.maximumThreadsPerBlock) {
        return Unsafe(
                "CUDA simulation block size exceeds the device limit");
    }
    if (registers == 0u ||
        static_cast<std::uint64_t>(registers) * threads >
                limits.registersPerBlock) {
        return Unsafe(
                "CUDA simulation register use exceeds the block limit");
    }
    if (activeBlocks == 0u ||
        activeBlocks > limits.maximumBlocksPerMultiprocessor ||
        static_cast<std::uint64_t>(activeBlocks) * threads >
                limits.maximumThreadsPerMultiprocessor ||
        static_cast<std::uint64_t>(activeBlocks) * threads * registers >
                limits.registersPerMultiprocessor) {
        return Unsafe(
                "CUDA simulation occupancy exceeds multiprocessor "
                "limits");
    }
    if (!std::isfinite(occupancy) || occupancy <= 0.0 ||
        occupancy > 1.0 + 1e-9) {
        return Unsafe("CUDA simulation occupancy is invalid");
    }

    const std::uint32_t largestMeasured =
            LargestMeasuredBatchSize(profiles_);
    const std::uint64_t maximumUnrepresentativeProbe =
            static_cast<std::uint64_t>(largestMeasured) *
            kMaximumUnrepresentativeGrowthFactor;
    if (!HasRepresentativeProfile(profiles_, limits) &&
        candidateBatchSize > maximumUnrepresentativeProbe) {
        return Unsafe(
                "CUDA calibration requires a staged occupancy probe");
    }

    const std::uint64_t blocks =
            (static_cast<std::uint64_t>(candidateBatchSize) + threads -
             1u) /
            threads;
    const std::uint64_t safeGridLimit = static_cast<std::uint64_t>(
            static_cast<double>(limits.maximumGridDimensionX) *
            kGridLimitFraction);
    if (blocks == 0u || blocks > safeGridLimit) {
        return Unsafe(
                "CUDA batch launch is too close to the grid dimension "
                "limit");
    }

    CudaCalibrationSafetyDecision result;
    result.reservedMemoryHeadroomBytes = std::max(
            kMinimumMemoryHeadroomBytes,
            SaturatingCeil(
                    static_cast<double>(limits.totalMemoryBytes) *
                    kMemoryHeadroomFraction));
    if (limits.freeMemoryBytes <= result.reservedMemoryHeadroomBytes) {
        return Unsafe(
                "CUDA free memory is already inside the required "
                "headroom");
    }
    const std::uint64_t reservationBytes =
            candidateBatchSize > currentBatchCapacity
                    ? (currentBatchCapacity == 0u
                               ? EstimateResidentBytes(
                                         candidateBatchSize)
                               : EstimateTransientReservationBytes(
                                         candidateBatchSize))
                    : 0u;
    const std::uint64_t localWorkingSetBytes =
            EstimateLocalWorkingSetBytes(candidateBatchSize, limits);
    result.requiredTransientBytes =
            SaturatingAdd(reservationBytes, localWorkingSetBytes);
    const std::uint64_t usableMemory =
            limits.freeMemoryBytes - result.reservedMemoryHeadroomBytes;
    if ((candidateBatchSize > currentBatchCapacity &&
         reservationBytes == 0u) ||
        result.requiredTransientBytes > usableMemory) {
        CudaCalibrationSafetyDecision unsafe = Unsafe(
                "CUDA batch execution or reservation would consume "
                "memory headroom");
        unsafe.requiredTransientBytes = result.requiredTransientBytes;
        unsafe.reservedMemoryHeadroomBytes =
                result.reservedMemoryHeadroomBytes;
        return unsafe;
    }

    result.predictedKernelMilliseconds =
            PredictKernelMilliseconds(candidateBatchSize, limits);
    if (limits.kernelExecutionTimeoutEnabled &&
        (result.predictedKernelMilliseconds <= 0.0 ||
         result.predictedKernelMilliseconds >
                 kWatchdogKernelBudgetMilliseconds)) {
        CudaCalibrationSafetyDecision unsafe = Unsafe(
                "CUDA batch is too close to the kernel watchdog limit");
        unsafe.requiredTransientBytes = result.requiredTransientBytes;
        unsafe.reservedMemoryHeadroomBytes =
                result.reservedMemoryHeadroomBytes;
        unsafe.predictedKernelMilliseconds =
                result.predictedKernelMilliseconds;
        return unsafe;
    }

    result.safe = true;
    return result;
}

const CudaCalibrationBatchProfile &
CudaCalibrationSafetyPlanner::ProfileForBatchSize(
        std::uint32_t candidateBatchSize) const {
    const CudaCalibrationBatchProfile *closest = &profiles_.front();
    std::uint64_t closestDistance =
            candidateBatchSize > closest->batchSize
                    ? static_cast<std::uint64_t>(
                              candidateBatchSize - closest->batchSize)
                    : static_cast<std::uint64_t>(
                              closest->batchSize - candidateBatchSize);
    for (const CudaCalibrationBatchProfile &profile : profiles_) {
        const std::uint64_t distance =
                candidateBatchSize > profile.batchSize
                        ? static_cast<std::uint64_t>(
                                  candidateBatchSize -
                                  profile.batchSize)
                        : static_cast<std::uint64_t>(
                                  profile.batchSize -
                                  candidateBatchSize);
        if (distance < closestDistance ||
            (distance == closestDistance &&
             profile.batchSize > closest->batchSize)) {
            closest = &profile;
            closestDistance = distance;
        }
    }
    return *closest;
}

std::uint64_t
CudaCalibrationSafetyPlanner::EstimateTransientReservationBytes(
        std::uint32_t candidateBatchSize) const {
    double maximumBytesPerCandidate = 0.0;
    bool measuredCapacitySlope = false;
    if (profiles_.size() == 1u) {
        maximumBytesPerCandidate =
                static_cast<double>(
                        profiles_.front().residentDeviceBytes) /
                profiles_.front().batchCapacity;
    }
    for (const CudaCalibrationBatchProfile &left : profiles_) {
        for (const CudaCalibrationBatchProfile &right : profiles_) {
            if (left.batchCapacity >= right.batchCapacity ||
                left.residentDeviceBytes > right.residentDeviceBytes) {
                continue;
            }
            measuredCapacitySlope = true;
            maximumBytesPerCandidate = std::max(
                    maximumBytesPerCandidate,
                    static_cast<double>(
                            right.residentDeviceBytes -
                            left.residentDeviceBytes) /
                            (right.batchCapacity - left.batchCapacity));
        }
    }
    const std::uint64_t estimate = SaturatingCeil(
            maximumBytesPerCandidate *
            static_cast<double>(candidateBatchSize) *
            kAllocationEstimateMargin);
    return measuredCapacitySlope
            ? std::max<std::uint64_t>(estimate, 1u)
            : estimate;
}

std::uint64_t CudaCalibrationSafetyPlanner::EstimateResidentBytes(
        std::uint32_t candidateBatchSize) const {
    if (profiles_.empty()) {
        return 0u;
    }
    if (profiles_.size() == 1u) {
        const CudaCalibrationBatchProfile &profile = profiles_.front();
        return SaturatingCeil(
                static_cast<double>(profile.residentDeviceBytes) /
                profile.batchCapacity * candidateBatchSize *
                kAllocationEstimateMargin);
    }

    double maximumBytesPerCandidate = 0.0;
    for (const CudaCalibrationBatchProfile &left : profiles_) {
        for (const CudaCalibrationBatchProfile &right : profiles_) {
            if (left.batchCapacity >= right.batchCapacity ||
                left.residentDeviceBytes > right.residentDeviceBytes) {
                continue;
            }
            maximumBytesPerCandidate = std::max(
                    maximumBytesPerCandidate,
                    static_cast<double>(
                            right.residentDeviceBytes -
                            left.residentDeviceBytes) /
                            (right.batchCapacity - left.batchCapacity));
        }
    }
    double maximumFixedBytes = 0.0;
    for (const CudaCalibrationBatchProfile &profile : profiles_) {
        maximumFixedBytes = std::max(
                maximumFixedBytes,
                std::max(
                        0.0,
                        static_cast<double>(
                                profile.residentDeviceBytes) -
                                maximumBytesPerCandidate *
                                        profile.batchCapacity));
    }
    return SaturatingCeil(
            (maximumFixedBytes +
             maximumBytesPerCandidate * candidateBatchSize) *
            kAllocationEstimateMargin);
}

std::uint64_t
CudaCalibrationSafetyPlanner::EstimateLocalWorkingSetBytes(
        std::uint32_t candidateBatchSize,
        const CudaCalibrationDeviceLimits &limits) const {
    if (profiles_.empty()) {
        return 0u;
    }
    const CudaCalibrationBatchProfile &profile =
            ProfileForBatchSize(candidateBatchSize);
    if (profile.simulationLocalBytesPerThread == 0u) {
        return 0u;
    }
    const long double maximumResidentThreads =
            static_cast<long double>(
                    profile.simulationActiveBlocksPerMultiprocessor) *
            profile.simulationThreadsPerBlock *
            limits.multiprocessorCount;
    const long double residentThreads = std::min(
            static_cast<long double>(candidateBatchSize),
            maximumResidentThreads);
    return SaturatingCeil(
            static_cast<double>(
                    residentThreads *
                    profile.simulationLocalBytesPerThread *
                    kAllocationEstimateMargin));
}

double CudaCalibrationSafetyPlanner::PredictKernelMilliseconds(
        std::uint32_t candidateBatchSize,
        const CudaCalibrationDeviceLimits &limits) const {
    if (profiles_.empty()) {
        return 0.0;
    }
    if (HasRepresentativeProfile(profiles_, limits)) {
        double maximumMillisecondsPerWave = 0.0;
        const CudaCalibrationBatchProfile *representative = nullptr;
        for (const CudaCalibrationBatchProfile &profile : profiles_) {
            if (profile.batchSize <
                RepresentativeBatchSize(profile, limits)) {
                continue;
            }
            const std::uint64_t waves = WaveCount(
                    profile.batchSize,
                    SimulationWaveCapacity(profile, limits));
            if (waves == 0u) {
                continue;
            }
            const double millisecondsPerWave =
                    profile.kernelMilliseconds /
                    static_cast<double>(waves);
            if (representative == nullptr ||
                millisecondsPerWave > maximumMillisecondsPerWave) {
                representative = &profile;
                maximumMillisecondsPerWave = millisecondsPerWave;
            }
        }
        if (representative != nullptr) {
            const std::uint64_t candidateWaves = WaveCount(
                    candidateBatchSize,
                    SimulationWaveCapacity(*representative, limits));
            return maximumMillisecondsPerWave *
                    static_cast<double>(candidateWaves) *
                    kKernelPredictionMargin;
        }
    }

    const std::uint32_t largestMeasured =
            LargestMeasuredBatchSize(profiles_);
    double maximumMeasuredMilliseconds = 0.0;
    double largestBatchMilliseconds = 0.0;
    for (const CudaCalibrationBatchProfile &profile : profiles_) {
        maximumMeasuredMilliseconds = std::max(
                maximumMeasuredMilliseconds,
                profile.kernelMilliseconds);
        if (profile.batchSize == largestMeasured) {
            largestBatchMilliseconds = std::max(
                    largestBatchMilliseconds,
                    profile.kernelMilliseconds);
        }
    }
    const double growth = candidateBatchSize > largestMeasured
            ? static_cast<double>(candidateBatchSize) / largestMeasured
            : 1.0;
    return std::max(
                   maximumMeasuredMilliseconds,
                   largestBatchMilliseconds * growth) *
            kKernelPredictionMargin;
}

}  // namespace forevertas
