#ifndef FOREVERTAS_SEARCHES_CUDA_BATCH_CALIBRATOR_H
#define FOREVERTAS_SEARCHES_CUDA_BATCH_CALIBRATOR_H

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace forevertas {

// Picks the GPU search's batch size from the search's own batches within a
// time budget. A batch takes about latency + size / rate: the latency of one
// candidate's whole timeline (the baseline batch measures it) and the rate of
// a full device. Sizes double from the device's resident threads while a
// doubling still pays and the next batch fits the budget; when the budget
// ends first, the model extends the last measurement and the next search
// batch confirms the larger size (or the best measured one stays).
class CudaBatchCalibrator final {
public:
    struct Settings {
        std::uint32_t startBatchSize = 4096u;
        double latencySeconds = 0.0;   // one candidate's batch (0: unknown)
        double budgetSeconds = 10.0;   // the calibration's batches, all told
        double maximumBatchSeconds = 12.0;
    };

    CudaBatchCalibrator() = default;
    explicit CudaBatchCalibrator(const Settings &settings);

    std::uint32_t CurrentBatchSize() const noexcept;
    std::uint32_t BestBatchSize() const noexcept;
    double BestThroughput() const noexcept;
    std::size_t ReliableMeasurementCount() const noexcept;
    double SpentSeconds() const noexcept;
    bool Complete() const noexcept;
    // The size is chosen; its first search batch confirms it (a slower one
    // brings back the best measured size).
    bool Confirming() const noexcept;
    bool HasReliableMeasurement() const noexcept;
    // What a batch's time may include besides the batch.
    enum class Disturbance : std::uint8_t {
        None,
        Possible,   // the first batch since the session was made or grown
        Certain,    // work of its own (around walls: the batch ran again)
    };
    // One search batch of the current size.
    void Observe(std::uint32_t candidateCount,
                 std::chrono::steady_clock::duration elapsed,
                 Disturbance disturbance = Disturbance::None);
    void CapacityUnavailable();
    void RejectUnsafeCurrent();
    bool CompleteWithBestMeasurement();

private:
    enum class Phase : std::uint8_t {
        Growth,
        Probe,     // a smaller size for the model's second point
        Confirm,
        Complete,
    };

    struct Measurement {
        std::uint32_t batchSize = 1u;
        double throughput = 0.0;
        double seconds = 0.0;   // one batch
    };

    void Record(double throughput, double seconds);
    void Grow();
    void Stop();
    void Finish();
    void SetCurrent(std::uint32_t batchSize);
    std::uint32_t Extrapolated() const;

    Settings settings_{};
    std::uint32_t currentBatchSize_ = 4096u;
    std::uint32_t bestBatchSize_ = 4096u;
    double bestThroughput_ = 0.0;
    Phase phase_ = Phase::Growth;
    std::vector<double> samples_;
    std::vector<double> sampleSeconds_;
    std::uint32_t retries_ = 0u;
    double spentSeconds_ = 0.0;
    std::vector<Measurement> measurements_;
    std::optional<std::uint32_t> unavailableBatchSize_;
};

}  // namespace forevertas

#endif
