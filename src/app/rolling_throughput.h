#ifndef FOREVERTAS_APP_ROLLING_THROUGHPUT_H
#define FOREVERTAS_APP_ROLLING_THROUGHPUT_H

#include <chrono>
#include <cstdint>
#include <deque>

namespace forevertas::app {

class RollingThroughput final {
public:
    using Duration = std::chrono::steady_clock::duration;

    RollingThroughput() {
        Reset();
    }

    void Reset() {
        Reset(0u, Duration::zero());
    }

    void Reset(std::uint64_t totalIterations, Duration elapsed) {
        samples_.clear();
        samples_.push_back(
                {elapsed, static_cast<double>(totalIterations)});
    }

    double Observe(std::uint64_t totalIterations, Duration elapsed) {
        if (elapsed < samples_.back().elapsed ||
            static_cast<double>(totalIterations) < samples_.back().iterations) {
            Reset();
        }

        const double iterations = static_cast<double>(totalIterations);
        if (elapsed == samples_.back().elapsed) {
            samples_.back().iterations = iterations;
        } else {
            samples_.push_back({elapsed, iterations});
        }

        constexpr auto window = std::chrono::seconds(10);
        const Duration cutoff =
                elapsed > window ? elapsed - window : Duration::zero();
        while (samples_.size() > 1u &&
               samples_[1].elapsed <= cutoff) {
            samples_.pop_front();
        }

        Duration startElapsed = samples_.front().elapsed;
        double startIterations = samples_.front().iterations;
        if (startElapsed < cutoff && samples_.size() > 1u) {
            const Sample &next = samples_[1];
            const double span =
                    std::chrono::duration<double>(
                            next.elapsed - startElapsed)
                            .count();
            const double offset =
                    std::chrono::duration<double>(
                            cutoff - startElapsed)
                            .count();
            const double fraction = span <= 0.0 ? 0.0 : offset / span;
            startIterations +=
                    (next.iterations - startIterations) * fraction;
            startElapsed = cutoff;
        }

        const double seconds =
                std::chrono::duration<double>(elapsed - startElapsed).count();
        return seconds <= 0.0
                ? 0.0
                : (iterations - startIterations) / seconds;
    }

private:
    struct Sample {
        Duration elapsed;
        double iterations;
    };

    std::deque<Sample> samples_;
};

}  // namespace forevertas::app

#endif
