#ifndef FOREVERTAS_SEARCHES_CUDA_CALIBRATION_SAFETY_H
#define FOREVERTAS_SEARCHES_CUDA_CALIBRATION_SAFETY_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace forevertas {

struct CudaCalibrationDeviceLimits {
    std::uint64_t totalMemoryBytes = 0u;
    std::uint64_t freeMemoryBytes = 0u;
    std::uint32_t maximumThreadsPerBlock = 0u;
    std::uint32_t maximumGridDimensionX = 0u;
    std::uint32_t registersPerBlock = 0u;
    std::uint32_t registersPerMultiprocessor = 0u;
    std::uint32_t maximumThreadsPerMultiprocessor = 0u;
    std::uint32_t maximumBlocksPerMultiprocessor = 0u;
    std::uint32_t multiprocessorCount = 0u;
    bool kernelExecutionTimeoutEnabled = false;
};

struct CudaCalibrationBatchProfile {
    std::uint32_t batchSize = 0u;
    std::uint32_t batchCapacity = 0u;
    std::uint64_t residentDeviceBytes = 0u;
    double kernelMilliseconds = 0.0;
    std::uint32_t simulationThreadsPerBlock = 0u;
    std::uint32_t simulationRegistersPerThread = 0u;
    std::uint64_t simulationLocalBytesPerThread = 0u;
    std::uint32_t simulationActiveBlocksPerMultiprocessor = 0u;
    double simulationTheoreticalOccupancy = 0.0;
};

struct CudaCalibrationSafetyDecision {
    bool safe = false;
    std::string reason;
    std::uint64_t requiredTransientBytes = 0u;
    std::uint64_t reservedMemoryHeadroomBytes = 0u;
    double predictedKernelMilliseconds = 0.0;
};

class CudaCalibrationSafetyPlanner final {
  public:
    void Observe(const CudaCalibrationBatchProfile &profile);
    std::optional<std::uint32_t> NextStagedProbe(
            std::uint32_t requestedBatchSize,
            const CudaCalibrationDeviceLimits &limits) const;
    CudaCalibrationSafetyDecision Evaluate(
            std::uint32_t candidateBatchSize,
            std::uint32_t currentBatchCapacity,
            const CudaCalibrationDeviceLimits &limits) const;

  private:
    const CudaCalibrationBatchProfile &
    ProfileForBatchSize(std::uint32_t candidateBatchSize) const;
    std::uint64_t EstimateTransientReservationBytes(
            std::uint32_t candidateBatchSize) const;
    std::uint64_t
    EstimateResidentBytes(std::uint32_t candidateBatchSize) const;
    std::uint64_t EstimateLocalWorkingSetBytes(
            std::uint32_t candidateBatchSize,
            const CudaCalibrationDeviceLimits &limits) const;
    double PredictKernelMilliseconds(
            std::uint32_t candidateBatchSize,
            const CudaCalibrationDeviceLimits &limits) const;

    std::vector<CudaCalibrationBatchProfile> profiles_;
};

}  // namespace forevertas

#endif
