#include "evaluators/pose_target_evaluator.h"

#include "evaluators/evaluator_utils.h"
#include "searches/option_settings_utils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <sstream>
#include <stdexcept>

namespace forevertas {
namespace {

struct Quaternion {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double w = 1.0;
};

Quaternion FromEulerDegrees(double yaw, double pitch, double roll) {
    constexpr double degreesToRadians = 3.14159265358979323846 / 180.0;
    const double cp = std::cos(pitch * degreesToRadians * 0.5);
    const double sp = std::sin(pitch * degreesToRadians * 0.5);
    const double cy = std::cos(yaw * degreesToRadians * 0.5);
    const double sy = std::sin(yaw * degreesToRadians * 0.5);
    const double cr = std::cos(roll * degreesToRadians * 0.5);
    const double sr = std::sin(roll * degreesToRadians * 0.5);
    return {sp * cy * cr + cp * sy * sr,
            cp * sy * cr - sp * cy * sr,
            cp * cy * sr - sp * sy * cr,
            cp * cy * cr + sp * sy * sr};
}

// The rotation from the target orientation to the car's, as a rotation
// vector in the target's own axes: x is pitch, y is yaw, z is roll (the
// axes FromEulerDegrees turns about). Its length is the angle between them.
EvaluationVector3 RotationErrorVector(
        const Quaternion &target,
        const forevervalidator::experimental::PhysicsSandboxStateView
                &state) {
    const double cx = state.car.rotationX;
    const double cy = state.car.rotationY;
    const double cz = state.car.rotationZ;
    const double cw = state.car.rotationW;
    // conjugate(target) * car
    const double tx = -target.x;
    const double ty = -target.y;
    const double tz = -target.z;
    const double tw = target.w;
    double x = tw * cx + tx * cw + ty * cz - tz * cy;
    double y = tw * cy - tx * cz + ty * cw + tz * cx;
    double z = tw * cz + tx * cy - ty * cx + tz * cw;
    double w = tw * cw - tx * cx - ty * cy - tz * cz;
    if (w < 0.0) {
        x = -x;
        y = -y;
        z = -z;
        w = -w;
    }
    const double sine = std::sqrt(x * x + y * y + z * z);
    if (sine <= 1e-12) return {};
    const double angle = 2.0 * std::atan2(sine, w);
    return {x / sine * angle, y / sine * angle, z / sine * angle};
}

struct PoseWeights {
    // World X, Y, Z; then yaw, pitch, roll. 1 counts an axis fully.
    EvaluationVector3 position{1.0, 1.0, 1.0};
    double yaw = 1.0;
    double pitch = 1.0;
    double roll = 1.0;
};

struct PoseSettings {
    std::int64_t minimumTimeMs = 0;
    std::int64_t maximumTimeMs = 0;
    EvaluationVector3 position;
    Quaternion orientation;
    double rotationWeight = 0.5;
    PoseWeights weights;
};

// Distance and angle with each axis weighted; equal weights of 1 give the
// plain distance and angle.
double WeightedPositionError(const PoseSettings &settings,
                             const EvaluationVector3 &position) {
    const EvaluationVector3 d = Difference(position, settings.position);
    const EvaluationVector3 &w = settings.weights.position;
    return std::sqrt(w.x * d.x * d.x + w.y * d.y * d.y + w.z * d.z * d.z);
}

double WeightedRotationError(const PoseSettings &settings,
                             const EvaluationVector3 &rotation) {
    const PoseWeights &w = settings.weights;
    return std::sqrt(w.pitch * rotation.x * rotation.x +
                     w.yaw * rotation.y * rotation.y +
                     w.roll * rotation.z * rotation.z);
}

class PoseSession final : public IterationEvaluationSession {
public:
    std::unique_ptr<IterationEvaluationSession> Clone() const override {
        return std::make_unique<PoseSession>(*this);
    }

    explicit PoseSession(PoseSettings settings) : settings_(settings) {}

    std::optional<EvaluationSample> Observe(
            const std::optional<
                    forevervalidator::experimental::PhysicsSandboxStateView>
                    &previous,
            const forevervalidator::experimental::PhysicsSandboxStateView
                    &current) override {
        static_cast<void>(previous);
        const double positionError =
                WeightedPositionError(settings_, PositionOf(current));
        const double rotationError = WeightedRotationError(
                settings_,
                RotationErrorVector(settings_.orientation, current));
        const double score = (1.0 - settings_.rotationWeight) * positionError +
                settings_.rotationWeight * rotationError;
        std::ostringstream description;
        description.precision(8);
        description << "Pose error: " << score
                    << " (position " << positionError << " m, rotation "
                    << rotationError * 180.0 / 3.14159265358979323846
                    << " deg) at "
                    << FormatRaceTimeMilliseconds(
                               static_cast<double>(current.timeMs));
        return EvaluationSample{score,
                                static_cast<double>(current.timeMs),
                                description.str()};
    }

private:
    PoseSettings settings_;
};

class PoseEvaluator final : public IterationEvaluator {
public:
    explicit PoseEvaluator(PoseSettings settings) : settings_(settings) {}
    EvaluationPlan Plan(std::int64_t simulationHorizonMs,
                        std::int64_t earliestMutationTimeMs,
                        std::uint32_t tickDurationMs) const override {
        static_cast<void>(tickDurationMs);
        static_cast<void>(simulationHorizonMs);
        return {std::max(settings_.minimumTimeMs, earliestMutationTimeMs),
                settings_.maximumTimeMs};
    }
    std::unique_ptr<IterationEvaluationSession> CreateSession()
            const override {
        return std::make_unique<PoseSession>(settings_);
    }
    bool IsBetter(const EvaluationSample &iteration,
                  const EvaluationSample &incumbent) const override {
        return iteration.score < incumbent.score;
    }

private:
    PoseSettings settings_;
};

std::optional<PoseSettings> ParseSettings(const OptionSettings &settings) {
    const auto minimum = ReadTimeSetting(settings, "minTimeMs");
    const auto maximum = ReadTimeSetting(settings, "maxTimeMs");
    const auto position = ReadVector3Settings(settings, "x", "y", "z");
    const auto yaw = ReadDoubleSetting(settings, "yawDegrees");
    const auto pitch = ReadDoubleSetting(settings, "pitchDegrees");
    const auto roll = ReadDoubleSetting(settings, "rollDegrees");
    const auto weight = ReadDoubleSetting(settings, "rotationWeightPercent");
    if (!minimum || !maximum || !position || !yaw || !pitch || !roll ||
        !weight || *weight < 0.0 || *weight > 100.0) {
        return std::nullopt;
    }
    std::array<double, 6> axes{};
    constexpr const char *axisKeys[] = {"weightX", "weightY", "weightZ",
                                        "weightYaw", "weightPitch",
                                        "weightRoll"};
    for (std::size_t index = 0; index < axes.size(); ++index) {
        const auto value = ReadDoubleSetting(settings, axisKeys[index]);
        if (!value || *value < 0.0 || *value > 100.0) return std::nullopt;
        axes[index] = *value / 100.0;
    }
    return PoseSettings{*minimum,
                        *maximum,
                        *position,
                        FromEulerDegrees(*yaw, *pitch, *roll),
                        *weight / 100.0,
                        {{axes[0], axes[1], axes[2]},
                         axes[3],
                         axes[4],
                         axes[5]}};
}

}  // namespace

OptionSettings DefaultPoseTargetOptionSettings() {
    return {{"minTimeMs", "1000"},
            {"maxTimeMs", "6000"},
            {"x", "0"},
            {"y", "0"},
            {"z", "0"},
            {"yawDegrees", "0"},
            {"pitchDegrees", "0"},
            {"rollDegrees", "0"},
            {"rotationWeightPercent", "50"},
            {"weightX", "100"},
            {"weightY", "100"},
            {"weightZ", "100"},
            {"weightYaw", "100"},
            {"weightPitch", "100"},
            {"weightRoll", "100"}};
}

std::optional<std::string> ValidatePoseTargetOptionSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) {
    if (const auto keyError = ValidateOptionSettingKeys(
                settings, DefaultPoseTargetOptionSettings())) {
        return keyError;
    }
    const auto parsed = ParseSettings(settings);
    if (!parsed) {
        return "positions and angles must be numbers, and weights between 0 and 100";
    }
    return ValidateTimeWindow(parsed->minimumTimeMs,
                              parsed->maximumTimeMs,
                              tickDurationMs,
                              "the minimum time",
                              true);
}

std::unique_ptr<IterationEvaluator> CreatePoseTargetEvaluator(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) {
    if (const auto error =
                ValidatePoseTargetOptionSettings(settings, tickDurationMs)) {
        throw std::invalid_argument(*error);
    }
    return std::make_unique<PoseEvaluator>(*ParseSettings(settings));
}

}  // namespace forevertas
