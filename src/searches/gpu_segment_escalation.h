#ifndef FOREVERTAS_SEARCHES_GPU_SEGMENT_ESCALATION_H
#define FOREVERTAS_SEARCHES_GPU_SEGMENT_ESCALATION_H

#include <algorithm>
#include <cstdint>

namespace forevertas {

class GpuSegmentEscalation final {
public:
    GpuSegmentEscalation(std::uint32_t segmentCount,
                        std::uint32_t changedSegmentCount,
                        std::uint32_t stalledBatchLimit)
        : segmentCount_(std::max(segmentCount, 1u)),
          changedSegmentCount_(stalledBatchLimit != 0u ? 1u :
                  std::min(segmentCount_, changedSegmentCount == 0u
                          ? segmentCount_ : changedSegmentCount)),
          stalledBatchLimit_(stalledBatchLimit) {}

    std::uint32_t ChangedSegmentCount() const {
        return changedSegmentCount_;
    }

    void RecordBatch(bool improved) {
        if (stalledBatchLimit_ == 0u) return;
        if (improved) {
            changedSegmentCount_ = 1u;
            stalledBatches_ = 0u;
        } else if (++stalledBatches_ >= stalledBatchLimit_) {
            changedSegmentCount_ = changedSegmentCount_ > segmentCount_ / 2u
                    ? segmentCount_ : changedSegmentCount_ * 2u;
            stalledBatches_ = 0u;
        }
    }

private:
    std::uint32_t segmentCount_;
    std::uint32_t changedSegmentCount_;
    std::uint32_t stalledBatchLimit_;
    std::uint32_t stalledBatches_ = 0u;
};

}  // namespace forevertas

#endif
