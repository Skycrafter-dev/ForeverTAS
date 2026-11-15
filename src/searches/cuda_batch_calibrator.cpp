#include "searches/cuda_batch_calibrator.h"

#include <algorithm>
#include <limits>

namespace forevertas {
namespace {

// A doubling that gains less than this ends the growth.
constexpr double kMinimumDoublingGain = 1.03;
// A predicted doubling past the measurements is taken while it gains this.
constexpr double kMinimumPredictedGain = 1.08;
// Short batches are noisy: the median of three below this, the better of
// two below kTwoSampleSeconds, one batch above.
constexpr double kThreeSampleSeconds = 0.15;
constexpr double kTwoSampleSeconds = 0.5;
// A disturbed batch is measured again while it is this short.
constexpr double kRetryDisturbedSeconds = 1.0;
constexpr std::uint32_t kMaximumRetries = 2u;

}  // namespace

CudaBatchCalibrator::CudaBatchCalibrator(const Settings &settings)
    : settings_(settings),
      currentBatchSize_(std::max(settings.startBatchSize, 1u)),
      bestBatchSize_(std::max(settings.startBatchSize, 1u)) {}

std::uint32_t CudaBatchCalibrator::CurrentBatchSize() const noexcept {
    return currentBatchSize_;
}

std::uint32_t CudaBatchCalibrator::BestBatchSize() const noexcept {
    return bestBatchSize_;
}

double CudaBatchCalibrator::BestThroughput() const noexcept {
    return bestThroughput_;
}

std::size_t
CudaBatchCalibrator::ReliableMeasurementCount() const noexcept {
    return measurements_.size();
}

double CudaBatchCalibrator::SpentSeconds() const noexcept {
    return spentSeconds_;
}

bool CudaBatchCalibrator::Complete() const noexcept {
    return phase_ == Phase::Complete;
}

bool CudaBatchCalibrator::Confirming() const noexcept {
    return phase_ == Phase::Confirm;
}

bool CudaBatchCalibrator::HasReliableMeasurement() const noexcept {
    return !measurements_.empty();
}

void CudaBatchCalibrator::Observe(
        std::uint32_t candidateCount,
        std::chrono::steady_clock::duration elapsed,
        Disturbance disturbance) {
    if (Complete() || candidateCount != currentBatchSize_) {
        return;
    }
    const double seconds =
            std::chrono::duration<double>(elapsed).count();
    if (seconds <= 0.0) {
        return;
    }
    if (phase_ != Phase::Confirm) {   // the confirming batch is the search's
        spentSeconds_ += seconds;
    }
    // measured again: a possibly disturbed batch when it is short, a disturbed one while most of the budget is left
    const bool again = disturbance == Disturbance::Possible ? seconds < kRetryDisturbedSeconds
            : disturbance == Disturbance::Certain           ? spentSeconds_ <= settings_.budgetSeconds * 0.6
                                                            : false;
    if (again && retries_ < kMaximumRetries) {
        ++retries_;
        return;
    }
    samples_.push_back(static_cast<double>(candidateCount) / seconds);
    sampleSeconds_.push_back(seconds);
    const std::size_t wanted = seconds < kThreeSampleSeconds ? 3u
            : seconds < kTwoSampleSeconds                    ? 2u
                                                             : 1u;
    if (samples_.size() < wanted &&
        spentSeconds_ < settings_.budgetSeconds) {
        return;
    }
    std::vector<double> rates = samples_;
    std::vector<double> times = sampleSeconds_;
    std::sort(rates.begin(), rates.end());
    std::sort(times.begin(), times.end());
    // two samples: the faster (a slower one met interference)
    const std::size_t at = rates.size() == 2u ? 1u : rates.size() / 2u;
    Record(rates[at], times[times.size() - 1u - at]);
}

void CudaBatchCalibrator::Record(double throughput, double seconds) {
    measurements_.push_back({currentBatchSize_, throughput, seconds});
    const bool better = throughput > bestThroughput_;
    if (better) {
        bestThroughput_ = throughput;
        bestBatchSize_ = currentBatchSize_;
    }
    if (phase_ == Phase::Confirm) {
        Finish();
        return;
    }
    if (phase_ == Phase::Probe) {
        Stop();
        return;
    }
    Grow();
}

void CudaBatchCalibrator::Grow() {
    const Measurement &last = measurements_.back();
    if (measurements_.size() >= 2u &&
        last.throughput <
                measurements_[measurements_.size() - 2u].throughput *
                        kMinimumDoublingGain) {
        Finish();
        return;
    }
    if (currentBatchSize_ > std::numeric_limits<std::uint32_t>::max() / 2u ||
        (unavailableBatchSize_ &&
         currentBatchSize_ * 2u >= *unavailableBatchSize_)) {
        Finish();
        return;
    }
    // the next batch takes about twice this one; when it does not fit, a half
    // batch (cheaper) gives the model its second point
    if (spentSeconds_ + 2.0 * last.seconds > settings_.budgetSeconds) {
        const std::uint32_t half = currentBatchSize_ / 2u;
        if (measurements_.size() == 1u && half >= 256u &&
            spentSeconds_ + last.seconds <= settings_.budgetSeconds) {
            phase_ = Phase::Probe;
            SetCurrent(half);
            return;
        }
        Stop();
        return;
    }
    SetCurrent(currentBatchSize_ * 2u);
}

// The budget ends while doubling still pays: the model's larger size, which
// the next search batch confirms.
void CudaBatchCalibrator::Stop() {
    const std::uint32_t target = Extrapolated();
    if (target <= bestBatchSize_) {
        Finish();
        return;
    }
    phase_ = Phase::Confirm;
    SetCurrent(target);
}

void CudaBatchCalibrator::Finish() {
    phase_ = Phase::Complete;
    SetCurrent(bestBatchSize_);
}

// Batch time = latency + size / rate through the last measurement (the
// latency from the last two measurements, else the baseline's); doublings
// while the predicted gain holds and a batch stays short enough.
std::uint32_t CudaBatchCalibrator::Extrapolated() const {
    if (measurements_.empty()) {
        return bestBatchSize_;
    }
    std::vector<Measurement> sorted = measurements_;
    std::sort(sorted.begin(), sorted.end(),
              [](const Measurement &a, const Measurement &b) { return a.batchSize < b.batchSize; });
    const Measurement &last = sorted.back();
    double latency = -1.0;
    if (sorted.size() >= 2u) {
        const Measurement &previous = sorted[sorted.size() - 2u];
        if (last.batchSize > previous.batchSize &&
            last.seconds > previous.seconds) {
            const double slope = (last.seconds - previous.seconds) /
                    static_cast<double>(last.batchSize - previous.batchSize);
            latency = last.seconds - slope * last.batchSize;
        }
    }
    if (latency < 0.0 && settings_.latencySeconds > 0.0) {
        latency = std::min(settings_.latencySeconds, last.seconds * 0.95);
    }
    if (latency < 0.0) {
        return bestBatchSize_;
    }
    const double perCandidate =
            (last.seconds - latency) / static_cast<double>(last.batchSize);
    if (perCandidate <= 0.0) {
        return bestBatchSize_;
    }
    std::uint64_t size = last.batchSize;
    double rate = last.throughput;
    std::uint32_t chosen = bestBatchSize_;
    while (size <= std::numeric_limits<std::uint32_t>::max() / 2u) {
        const std::uint64_t next = size * 2u;
        if (unavailableBatchSize_ && next >= *unavailableBatchSize_) {
            break;
        }
        const double seconds = latency + perCandidate * static_cast<double>(next);
        const double nextRate = static_cast<double>(next) / seconds;
        if (seconds > settings_.maximumBatchSeconds ||
            nextRate < rate * kMinimumPredictedGain) {
            break;
        }
        size = next;
        rate = nextRate;
        chosen = static_cast<std::uint32_t>(size);
    }
    return chosen;
}

void CudaBatchCalibrator::CapacityUnavailable() {
    RejectUnsafeCurrent();
}

void CudaBatchCalibrator::RejectUnsafeCurrent() {
    if (Complete()) {
        return;
    }
    if (!unavailableBatchSize_ || currentBatchSize_ < *unavailableBatchSize_) {
        unavailableBatchSize_ = currentBatchSize_;
    }
    const std::uint32_t half = currentBatchSize_ / 2u;
    if (phase_ == Phase::Probe) {
        Stop();
        return;
    }
    if (phase_ == Phase::Confirm) {
        if (half > bestBatchSize_) {
            SetCurrent(half);
        } else {
            Finish();
        }
        return;
    }
    if (measurements_.empty()) {
        if (half == 0u) {
            phase_ = Phase::Complete;
            return;
        }
        SetCurrent(half);
        return;
    }
    Finish();
}

bool CudaBatchCalibrator::CompleteWithBestMeasurement() {
    if (measurements_.empty()) {
        return false;
    }
    Finish();
    return true;
}

void CudaBatchCalibrator::SetCurrent(std::uint32_t batchSize) {
    currentBatchSize_ = std::max(batchSize, 1u);
    samples_.clear();
    sampleSeconds_.clear();
    retries_ = 0u;
}

}  // namespace forevertas
