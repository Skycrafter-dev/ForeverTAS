#include "evaluators/pose_target_evaluator.h"

#include "evaluators/evaluator_utils.h"
#include "searches/option_settings_utils.h"

#include <algorithm>
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
    const double cy = std::cos(yaw * degreesToRadians * 0.5);
    const double sy = std::sin(yaw * degreesToRadians * 0.5);
    const double cp = std::cos(pitch * degreesToRadians * 0.5);
    const double sp = std::sin(pitch * degreesToRadians * 0.5);
    const double cr = std::cos(roll * degreesToRadians * 0.5);
    const double sr = std::sin(roll * degreesToRadians * 0.5);
    return {sr * cp * cy - cr * sp * sy,
            cr * sp * cy + sr * cp * sy,
            cr * cp * sy - sr * sp * cy,
            cr * cp * cy + sr * sp * sy};
}

double RotationError(const Quaternion &target,
                     const forevervalidator::experimental::
                             PhysicsSandboxStateView &state) {
    const double dot = std::abs(target.x * state.car.rotationX +
                                target.y * state.car.rotationY +
                                target.z * state.car.rotationZ +
                                target.w * state.car.rotationW);
    return 2.0 * std::acos(std::clamp(dot, 0.0, 1.0));
}

struct PoseSettings {
    std::int64_t minimumTimeMs = 0;
    std::int64_t maximumTimeMs = 0;
    EvaluationVector3 position;
    Quaternion orientation;
    double rotationWeight = 0.5;
};

class PoseSession final : public IterationEvaluationSession {
public:
    explicit PoseSession(PoseSettings settings) : settings_(settings) {}

    std::optional<EvaluationSample> Observe(
            const std::optional<
                    forevervalidator::experimental::PhysicsSandboxStateView>
                    &previous,
            const forevervalidator::experimental::PhysicsSandboxStateView
                    &current) override {
        static_cast<void>(previous);
        const double positionError =
                Distance(PositionOf(current), settings_.position);
        const double rotationError = RotationError(settings_.orientation, current);
        const double score = (1.0 - settings_.rotationWeight) * positionError +
                settings_.rotationWeight * rotationError;
        std::ostringstream description;
        description.precision(8);
        description << "Pose error: " << score
                    << " (position " << positionError << " m, rotation "
                    << rotationError * 180.0 / 3.14159265358979323846
                    << " deg) at "
                    << FormatHumanDurationMilliseconds(
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
    return PoseSettings{*minimum,
                        *maximum,
                        *position,
                        FromEulerDegrees(*yaw, *pitch, *roll),
                        *weight / 100.0};
}

}  // namespace

OptionFieldList PoseTargetOptionFields() {
    OptionFieldList fields;
    AppendWindowFields(fields, "1000", "6000");
    fields.push_back(ClampedField("rotationWeightPercent",
                                  "Rotation weight", "50",
                                  0.0, 100.0, 0, 1));
    fields.push_back(MirroredField("x", "pose", "0"));
    fields.push_back(MirroredField("y", "pose", "0"));
    fields.push_back(MirroredField("z", "pose", "0"));
    fields.push_back(MirroredField("yawDegrees", "pose", "0"));
    fields.push_back(MirroredField("pitchDegrees", "pose", "0"));
    fields.push_back(MirroredField("rollDegrees", "pose", "0"));
    return fields;
}

OptionSettings DefaultPoseTargetOptionSettings() {
    return {{"minTimeMs", "1000"},
            {"maxTimeMs", "6000"},
            {"x", "0"},
            {"y", "0"},
            {"z", "0"},
            {"yawDegrees", "0"},
            {"pitchDegrees", "0"},
            {"rollDegrees", "0"},
            {"rotationWeightPercent", "50"}};
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
        return "pose settings must be finite and weight must be between 0 and 100";
    }
    return ValidateTimeWindow(parsed->minimumTimeMs,
                              parsed->maximumTimeMs,
                              tickDurationMs,
                              "evaluation",
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
