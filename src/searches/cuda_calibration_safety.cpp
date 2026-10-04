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
constexpr double kKernelPredictionMargin = 1.25;

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
    existing->reservationBytesPerCandidate = std::max(
            existing->reservationBytesPerCandidate, profile.reservationBytesPerCandidate);
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
    if (limits.totalMemoryBytes == 0u || limits.freeMemoryBytes == 0u) {
        return Unsafe("GPU memory limits are incomplete");
    }
    if (limits.requireCudaExecutionLimits) {
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
            PredictKernelMilliseconds(candidateBatchSize);
    if (limits.kernelExecutionTimeoutEnabled &&
        (!std::isfinite(limits.kernelBudgetMilliseconds) ||
         limits.kernelBudgetMilliseconds <= 0.0 ||
         result.predictedKernelMilliseconds <= 0.0 ||
         result.predictedKernelMilliseconds >
                 limits.kernelBudgetMilliseconds)) {
        CudaCalibrationSafetyDecision unsafe = Unsafe(
                "CUDA batch is too close to the kernel watchdog limit");
        unsafe.watchdogLimited = true;
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
    std::uint64_t fixedAllowance = 0;
    for (const auto &profile : profiles_)
    {
        maximumBytesPerCandidate = std::max(maximumBytesPerCandidate,
                static_cast<double>(profile.reservationBytesPerCandidate));
        fixedAllowance = std::max(fixedAllowance, profile.residentDeviceBytes);
    }
    if (maximumBytesPerCandidate > 0.0)
        return SaturatingAdd(SaturatingAdd(fixedAllowance, fixedAllowance),
                SaturatingCeil(maximumBytesPerCandidate * candidateBatchSize * kAllocationEstimateMargin));
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
            maximumBytesPerCandidate = std::max(
                    maximumBytesPerCandidate,
                    static_cast<double>(
                            right.residentDeviceBytes -
                            left.residentDeviceBytes) /
                            (right.batchCapacity - left.batchCapacity));
        }
    }
    return SaturatingCeil(
            maximumBytesPerCandidate *
            static_cast<double>(candidateBatchSize) *
            kAllocationEstimateMargin);
}

std::uint64_t CudaCalibrationSafetyPlanner::EstimateResidentBytes(
        std::uint32_t candidateBatchSize) const {
    if (profiles_.empty()) {
        return 0u;
    }
    std::uint64_t reservationBound = 0;
    std::uint64_t retainedBytes = 0;
    for (const auto &profile : profiles_) {
        reservationBound = std::max(reservationBound, profile.reservationBytesPerCandidate);
        retainedBytes = std::max(retainedBytes, profile.residentDeviceBytes);
    }
    if (reservationBound != 0)
        return SaturatingAdd(retainedBytes, SaturatingCeil(
                static_cast<double>(reservationBound) * candidateBatchSize * kAllocationEstimateMargin));
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
    if (!limits.requireCudaExecutionLimits) {
        return 0u;
    }

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
        std::uint32_t candidateBatchSize) const {
    if (profiles_.empty()) {
        return 0.0;
    }
    if (profiles_.size() == 1u) {
        const CudaCalibrationBatchProfile &profile = profiles_.front();
        return profile.batchSize == 0u
                       ? 0.0
                       : profile.kernelMilliseconds *
                                 static_cast<double>(
                                         candidateBatchSize) /
                                 profile.batchSize *
                                 kKernelPredictionMargin;
    }

    std::vector<const CudaCalibrationBatchProfile *> ordered;
    ordered.reserve(profiles_.size());
    for (const CudaCalibrationBatchProfile &profile : profiles_) {
        const auto duplicate = std::find_if(
                ordered.begin(),
                ordered.end(),
                [&profile](
                        const CudaCalibrationBatchProfile *candidate) {
                    return candidate->batchSize == profile.batchSize;
                });
        if (duplicate == ordered.end()) {
            ordered.push_back(&profile);
        } else if (
                profile.kernelMilliseconds >
                (*duplicate)->kernelMilliseconds) {
            *duplicate = &profile;
        }
    }
    std::sort(
            ordered.begin(),
            ordered.end(),
            [](const CudaCalibrationBatchProfile *left,
               const CudaCalibrationBatchProfile *right) {
                return left->batchSize < right->batchSize;
            });
    if (ordered.size() == 1u) {
        const CudaCalibrationBatchProfile &profile =
                *ordered.front();
        return profile.kernelMilliseconds *
                static_cast<double>(candidateBatchSize) /
                profile.batchSize *
                kKernelPredictionMargin;
    }
    const auto upper = std::lower_bound(
            ordered.begin(),
            ordered.end(),
            candidateBatchSize,
            [](const CudaCalibrationBatchProfile *profile,
               std::uint32_t size) {
                return profile->batchSize < size;
            });
    if (upper != ordered.end()) {
        if (upper == ordered.begin() ||
            (*upper)->batchSize == candidateBatchSize) {
            return (*upper)->kernelMilliseconds *
                   kKernelPredictionMargin;
        }
        const CudaCalibrationBatchProfile &high = **upper;
        const CudaCalibrationBatchProfile &low = **(upper - 1);
        return std::max(
                       low.kernelMilliseconds,
                       high.kernelMilliseconds) *
                kKernelPredictionMargin;
    }
    const std::size_t firstTrendIndex =
            ordered.size() > 3u ? ordered.size() - 3u : 0u;
    double maximumMeasuredMilliseconds = 0.0;
    for (const CudaCalibrationBatchProfile *profile : ordered) {
        maximumMeasuredMilliseconds = std::max(
                maximumMeasuredMilliseconds,
                profile->kernelMilliseconds);
    }
    const std::uint32_t largestMeasuredBatch =
            ordered.back()->batchSize;
    double maximumSlope = 0.0;
    const CudaCalibrationBatchProfile &previous =
            *ordered[ordered.size() - 2u];
    const CudaCalibrationBatchProfile &latest = *ordered.back();
    if (latest.kernelMilliseconds > previous.kernelMilliseconds) {
        maximumSlope = (latest.kernelMilliseconds -
                        previous.kernelMilliseconds) /
                       (latest.batchSize - previous.batchSize);
    }
    const CudaCalibrationBatchProfile &trendStart =
            *ordered[firstTrendIndex];
    if (latest.kernelMilliseconds > trendStart.kernelMilliseconds) {
        maximumSlope = std::max(
                maximumSlope,
                (latest.kernelMilliseconds -
                 trendStart.kernelMilliseconds) /
                        (latest.batchSize - trendStart.batchSize));
    }
    const std::uint32_t additionalCandidates =
            candidateBatchSize > largestMeasuredBatch
                    ? candidateBatchSize - largestMeasuredBatch
                    : 0u;
    return (maximumMeasuredMilliseconds +
            maximumSlope * additionalCandidates) *
           kKernelPredictionMargin;
}

}  // namespace forevertas
