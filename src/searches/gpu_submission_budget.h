#ifndef FOREVERTAS_GPU_SUBMISSION_BUDGET_H
#define FOREVERTAS_GPU_SUBMISSION_BUDGET_H
#include <chrono>
#include <optional>

namespace forevertas {
class GpuSubmissionBudget {
public:
    using Clock = std::chrono::steady_clock;
    explicit GpuSubmissionBudget(bool enabled,
            std::chrono::milliseconds limit = std::chrono::milliseconds(250))
        : limit_(limit), enabled_(enabled) {}
    void Reset() { started_.reset(); expired_ = false; }
    bool Expired(Clock::time_point now = Clock::now()) {
        if (!started_) started_ = now;
        expired_ = enabled_ && now - *started_ >= limit_;
        return expired_;
    }
    bool WasExceeded() const { return expired_; }
private:
    std::chrono::milliseconds limit_;
    bool enabled_;
    bool expired_ = false;
    std::optional<Clock::time_point> started_;
};
}  // namespace forevertas
#endif
