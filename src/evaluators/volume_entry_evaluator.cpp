#include "evaluators/volume_entry_evaluator.h"

#include "evaluators/evaluator_utils.h"
#include "searches/option_settings_utils.h"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace forevertas {
namespace {

struct Box {
    EvaluationVector3 minimum;
    EvaluationVector3 maximum;
};

bool Contains(const Box &box, const EvaluationVector3 &point) {
    return point.x >= box.minimum.x && point.x <= box.maximum.x &&
           point.y >= box.minimum.y && point.y <= box.maximum.y &&
           point.z >= box.minimum.z && point.z <= box.maximum.z;
}

std::optional<double> SegmentEntryFraction(const Box &box,
                                           const EvaluationVector3 &from,
                                           const EvaluationVector3 &to) {
    const std::array<double, 3> a{from.x, from.y, from.z};
    const std::array<double, 3> b{to.x, to.y, to.z};
    const std::array<double, 3> minimum{
            box.minimum.x, box.minimum.y, box.minimum.z};
    const std::array<double, 3> maximum{
            box.maximum.x, box.maximum.y, box.maximum.z};
    for (std::size_t axis = 0u; axis < 3u; ++axis) {
        if (std::max(a[axis], b[axis]) < minimum[axis] ||
            std::min(a[axis], b[axis]) > maximum[axis]) {
            return std::nullopt;
        }
    }

    double enter = 0.0;
    double leave = 1.0;
    for (std::size_t axis = 0u; axis < 3u; ++axis) {
        const double delta = b[axis] - a[axis];
        if (std::abs(delta) <= 1e-12) {
            if (a[axis] < minimum[axis] || a[axis] > maximum[axis]) {
                return std::nullopt;
            }
            continue;
        }
        double near = (minimum[axis] - a[axis]) / delta;
        double far = (maximum[axis] - a[axis]) / delta;
        if (near > far) std::swap(near, far);
        enter = std::max(enter, near);
        leave = std::min(leave, far);
        if (enter > leave) return std::nullopt;
    }
    return enter >= 0.0 && enter <= 1.0
            ? std::optional<double>(enter)
            : std::nullopt;
}

class VolumeEntrySession final : public IterationEvaluationSession {
public:
    explicit VolumeEntrySession(Box box) : box_(box) {}

    std::optional<EvaluationSample> Observe(
            const std::optional<
                    forevervalidator::experimental::PhysicsSandboxStateView>
                    &previous,
            const forevervalidator::experimental::PhysicsSandboxStateView
                    &current) override {
        if (reported_) return std::nullopt;
        const EvaluationVector3 currentPosition = PositionOf(current);
        if (!previous) {
            if (!Contains(box_, currentPosition)) return std::nullopt;
            reported_ = true;
            const double time = static_cast<double>(current.timeMs);
            return EvaluationSample{
                    time,
                    time,
                    TimeMetricDescription("Volume entry time", time)};
        }

        const EvaluationVector3 previousPosition = PositionOf(*previous);
        if (Contains(box_, previousPosition)) return std::nullopt;
        const auto fraction = SegmentEntryFraction(
                box_, previousPosition, currentPosition);
        if (!fraction) return std::nullopt;
        reported_ = true;
        const double time = static_cast<double>(previous->timeMs) +
                *fraction * static_cast<double>(
                        current.timeMs - previous->timeMs);
        return EvaluationSample{
                time,
                time,
                TimeMetricDescription("Volume entry time", time)};
    }

private:
    Box box_;
    bool reported_ = false;
};

class VolumeEntryEvaluator final : public IterationEvaluator {
public:
    explicit VolumeEntryEvaluator(Box box) : box_(box) {}

    EvaluationPlan Plan(std::int64_t simulationHorizonMs,
                        std::int64_t earliestMutationTimeMs,
                        std::uint32_t tickDurationMs) const override {
        static_cast<void>(tickDurationMs);
        return {earliestMutationTimeMs, simulationHorizonMs};
    }

    std::unique_ptr<IterationEvaluationSession> CreateSession()
            const override {
        return std::make_unique<VolumeEntrySession>(box_);
    }

    bool IsBetter(const EvaluationSample &iteration,
                  const EvaluationSample &incumbent) const override {
        return iteration.score < incumbent.score;
    }

private:
    Box box_;
};

std::optional<Box> ParseBox(const OptionSettings &settings) {
    const auto center = ReadVector3Settings(
            settings, "centerX", "centerY", "centerZ");
    const auto size = ReadVector3Settings(
            settings, "sizeX", "sizeY", "sizeZ");
    if (!center || !size || size->x <= 0.0 || size->y <= 0.0 ||
        size->z <= 0.0) {
        return std::nullopt;
    }
    const EvaluationVector3 half{size->x * 0.5,
                                 size->y * 0.5,
                                 size->z * 0.5};
    return Box{{center->x - half.x,
                center->y - half.y,
                center->z - half.z},
               {center->x + half.x,
                center->y + half.y,
                center->z + half.z}};
}

}  // namespace

OptionFieldList VolumeEntryOptionFields() {
    return {
            MirroredField("centerX", "cuboid", "0"),
            MirroredField("centerY", "cuboid", "0"),
            MirroredField("centerZ", "cuboid", "0"),
            MirroredField("sizeX", "cuboid", "10"),
            MirroredField("sizeY", "cuboid", "10"),
            MirroredField("sizeZ", "cuboid", "10")};
}

OptionSettings DefaultVolumeEntryOptionSettings() {
    return {{"centerX", "0"},
            {"centerY", "0"},
            {"centerZ", "0"},
            {"sizeX", "10"},
            {"sizeY", "10"},
            {"sizeZ", "10"}};
}

std::optional<std::string> ValidateVolumeEntryOptionSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) {
    static_cast<void>(tickDurationMs);
    if (const auto error = ValidateOptionSettingKeys(
                settings, DefaultVolumeEntryOptionSettings())) {
        return error;
    }
    if (!ParseBox(settings)) {
        return "volume center must be finite and all sizes must be positive";
    }
    return std::nullopt;
}

std::unique_ptr<IterationEvaluator> CreateVolumeEntryEvaluator(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) {
    if (const auto error =
                ValidateVolumeEntryOptionSettings(settings, tickDurationMs)) {
        throw std::invalid_argument(*error);
    }
    return std::make_unique<VolumeEntryEvaluator>(*ParseBox(settings));
}

}  // namespace forevertas
