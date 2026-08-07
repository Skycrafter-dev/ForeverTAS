#ifndef FOREVERTAS_SEARCHES_CUDA_BATCH_CALIBRATOR_H
#define FOREVERTAS_SEARCHES_CUDA_BATCH_CALIBRATOR_H

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace forevertas {

class CudaBatchCalibrator final {
public:
    explicit CudaBatchCalibrator(
            std::uint32_t initialBatchSize = 1u) noexcept;

    std::uint32_t CurrentBatchSize() const noexcept;
    std::uint32_t BestBatchSize() const noexcept;
    double BestThroughput() const noexcept;
    std::size_t ReliableMeasurementCount() const noexcept;
    bool Complete() const noexcept;
    bool HasReliableMeasurement() const noexcept;
    void Observe(std::uint32_t candidateCount,
                 std::chrono::steady_clock::duration elapsed);
    void CapacityUnavailable();
    void RejectUnsafeCurrent();

private:
    enum class Phase : std::uint8_t {
        Seed,
        Growth,
        Refinement,
        Complete,
    };

    struct Measurement {
        std::uint32_t batchSize = 1u;
        double throughput = 0.0;
    };

    void FinishMeasurement(double sustainedThroughput);
    void RejectCurrent(bool upperBound);
    void BeginGrowth();
    void BeginRefinement();
    void BeginRefinementRound();
    void AdvanceRefinement();
    void SetCurrent(std::uint32_t batchSize);
    bool Measured(std::uint32_t batchSize) const;

    std::uint32_t currentBatchSize_ = 1u;
    std::uint32_t bestBatchSize_ = 1u;
    double bestThroughput_ = 0.0;
    std::uint32_t growthSteps_ = 0u;
    std::uint32_t declineSteps_ = 0u;
    Phase phase_ = Phase::Seed;
    std::vector<double> samples_;
    std::size_t warmupSamplesRemaining_ = 2u;
    std::vector<Measurement> measurements_;
    std::vector<std::uint32_t> refinementQueue_;
    std::size_t refinementIndex_ = 0u;
    std::uint32_t refinementRound_ = 0u;
    std::optional<std::uint32_t> unavailableBatchSize_;
};

}  // namespace forevertas

#endif
