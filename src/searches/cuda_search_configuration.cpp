#include "searches/cuda_search_configuration.h"

#include "input_timeline_time.h"
#include "evaluators/visual_expression_evaluator.h"
#include "searches/algorithm_registry.h"
#include "searches/option_settings_utils.h"

#include <cmath>
#include <stdexcept>
#include <string>

namespace forevertas {
namespace {

using namespace forevervalidator::experimental;

const OptionSettings &SimulationInputSettings(
        const OptionConfiguration &configuration,
        std::uint32_t tickDurationMs,
        OptionSettings *storage) {
    const auto translated = SimulationInputSettingsFromUserTimeline(
            configuration.settings, tickDurationMs);
    if (!translated) {
        throw std::invalid_argument(
                "CUDA configuration timeline translation failed");
    }
    *storage = *translated;
    return *storage;
}

PhysicsSandboxCudaModifierWindow Window(const OptionSettings &settings) {
    return {*ParseSignedDecimal(settings.at("minTimeMs")),
            *ParseSignedDecimal(settings.at("maxTimeMs")),
            *ParseUnsignedDecimal32(settings.at("seed"))};
}

PhysicsSandboxCudaInsertionChannel InsertionChannel(
        const OptionSettings &settings,
        const char *enabled,
        const char *minimum,
        const char *maximum,
        const char *hold) {
    return {*ParseBoolean(settings.at(enabled)),
            *ParseUnsignedDecimal32(settings.at(minimum)),
            *ParseUnsignedDecimal32(settings.at(maximum)),
            *ParseSignedDecimal(settings.at(hold))};
}

PhysicsSandboxCudaDeletionChannel DeletionChannel(
        const OptionSettings &settings,
        const char *enabled,
        const char *maximum) {
    return {*ParseBoolean(settings.at(enabled)),
            *ParseUnsignedDecimal32(settings.at(maximum))};
}

double Number(const OptionSettings &settings, const char *key) {
    return *ParseFiniteDouble(settings.at(key));
}

PhysicsSandboxCudaVector3 Vector(const OptionSettings &settings,
                                 const char *x,
                                 const char *y,
                                 const char *z) {
    return {Number(settings, x),
            Number(settings, y),
            Number(settings, z)};
}

}  // namespace

std::vector<PhysicsSandboxCudaModifier> BuildCudaModifiers(
        const std::vector<OptionConfiguration> &modifiers,
        std::uint32_t tickDurationMs) {
    std::vector<PhysicsSandboxCudaModifier> result;
    result.reserve(modifiers.size());
    for (const OptionConfiguration &configuration : modifiers) {
        OptionSettings storage;
        const OptionSettings &settings = SimulationInputSettings(
                configuration, tickDurationMs, &storage);
        if (configuration.id == kRandomSteeringModifierId) {
            result.emplace_back(
                    PhysicsSandboxCudaRandomSteeringModifier{
                            Window(settings)});
        } else if (configuration.id ==
                   kExistingEventPerturbationModifierId) {
            result.emplace_back(
                    PhysicsSandboxCudaExistingEventModifier{
                            Window(settings),
                            *ParseUnsignedDecimal32(
                                    settings.at("minCount")),
                            *ParseUnsignedDecimal32(
                                    settings.at("maxCount")),
                            *ParseSignedDecimal(
                                    settings.at("maxTimeShiftMs")),
                            settings.at("steerMode") == "absolute",
                            *ParseNormalizedAnalogInput(
                                    settings.at("steerDeltaMin")),
                            *ParseNormalizedAnalogInput(
                                    settings.at("steerDeltaMax")),
                            *ParseNormalizedAnalogInput(
                                    settings.at("steerAbsoluteMin")),
                            *ParseNormalizedAnalogInput(
                                    settings.at("steerAbsoluteMax")),
                            *ParseBoolean(
                                    settings.at("toggleAccelerate")),
                            *ParseBoolean(settings.at("toggleBrake"))});
        } else if (configuration.id == kSmoothSteeringModifierId) {
            result.emplace_back(
                    PhysicsSandboxCudaSmoothSteeringModifier{
                            Window(settings),
                            *ParseUnsignedDecimal32(
                                    settings.at("deformationCount")),
                            *ParseSignedDecimal(settings.at("radiusMs")),
                            *ParseNormalizedAnalogInput(
                                    settings.at("amplitudeMin")),
                            *ParseNormalizedAnalogInput(
                                    settings.at("amplitudeMax"))});
        } else if (configuration.id == kInputInsertionModifierId) {
            result.emplace_back(
                    PhysicsSandboxCudaInputInsertionModifier{
                            Window(settings),
                            InsertionChannel(
                                    settings,
                                    "steerEnabled",
                                    "steerMinCount",
                                    "steerMaxCount",
                                    "steerMaxHoldMs"),
                            InsertionChannel(
                                    settings,
                                    "accelerateEnabled",
                                    "accelerateMinCount",
                                    "accelerateMaxCount",
                                    "accelerateMaxHoldMs"),
                            InsertionChannel(
                                    settings,
                                    "brakeEnabled",
                                    "brakeMinCount",
                                    "brakeMaxCount",
                                    "brakeMaxHoldMs"),
                            settings.at("steerMode") == "offset",
                            *ParseNormalizedAnalogInput(
                                    settings.at("steerAbsoluteMin")),
                            *ParseNormalizedAnalogInput(
                                    settings.at("steerAbsoluteMax")),
                            *ParseNormalizedAnalogInput(
                                    settings.at("steerOffsetMin")),
                            *ParseNormalizedAnalogInput(
                                    settings.at("steerOffsetMax"))});
        } else if (configuration.id == kInputDeletionModifierId) {
            result.emplace_back(
                    PhysicsSandboxCudaInputDeletionModifier{
                            Window(settings),
                            DeletionChannel(
                                    settings,
                                    "steerEnabled",
                                    "steerMaxCount"),
                            DeletionChannel(
                                    settings,
                                    "accelerateEnabled",
                                    "accelerateMaxCount"),
                            DeletionChannel(
                                    settings,
                                    "brakeEnabled",
                                    "brakeMaxCount")});
        } else {
            throw std::invalid_argument(
                    "CUDA does not support modifier: " +
                    configuration.id);
        }
    }
    return result;
}

std::optional<PhysicsSandboxCudaEvaluator> BuildCudaEvaluator(
        const OptionConfiguration &configuration,
        std::uint32_t tickDurationMs) {
    if (tickDurationMs == 0u) {
        throw std::invalid_argument(
                "CUDA evaluator tick duration must be greater than zero");
    }
    const OptionSettings &settings = configuration.settings;
    if (configuration.id == kVelocityEvaluationId) {
        const double x = Number(settings, "directionX");
        const double y = Number(settings, "directionY");
        const double z = Number(settings, "directionZ");
        const double length = std::sqrt((x * x + y * y) + z * z);
        return PhysicsSandboxCudaVelocityEvaluator{
                settings.at("mode") == "projected",
                *ParseBoolean(settings.at("alignmentEnabled")),
                {length <= 1e-12 ? 0.0 : x / length,
                 length <= 1e-12 ? 0.0 : y / length,
                 length <= 1e-12 ? 0.0 : z / length},
                Number(settings, "minAlignmentPercent") / 100.0};
    }
    if (configuration.id == kPointTargetEvaluationId) {
        return PhysicsSandboxCudaPointEvaluator{
                Vector(settings, "x", "y", "z")};
    }
    if (configuration.id == kPoseTargetEvaluationId) {
        constexpr double degreesToRadians =
                3.14159265358979323846 / 180.0;
        const double yaw =
                Number(settings, "yawDegrees") *
                degreesToRadians * 0.5;
        const double pitch =
                Number(settings, "pitchDegrees") *
                degreesToRadians * 0.5;
        const double roll =
                Number(settings, "rollDegrees") *
                degreesToRadians * 0.5;
        const double cy = std::cos(yaw);
        const double sy = std::sin(yaw);
        const double cp = std::cos(pitch);
        const double sp = std::sin(pitch);
        const double cr = std::cos(roll);
        const double sr = std::sin(roll);
        return PhysicsSandboxCudaPoseEvaluator{
                Vector(settings, "x", "y", "z"),
                sr * cp * cy - cr * sp * sy,
                cr * sp * cy + sr * cp * sy,
                cr * cp * sy - sr * sp * cy,
                cr * cp * cy + sr * sp * sy,
                Number(settings, "rotationWeightPercent") / 100.0};
    }
    if (configuration.id == kVolumeEntryEvaluationId) {
        const double centerX = Number(settings, "centerX");
        const double centerY = Number(settings, "centerY");
        const double centerZ = Number(settings, "centerZ");
        const double halfX = Number(settings, "sizeX") * 0.5;
        const double halfY = Number(settings, "sizeY") * 0.5;
        const double halfZ = Number(settings, "sizeZ") * 0.5;
        return PhysicsSandboxCudaVolumeEntryEvaluator{
                {centerX - halfX, centerY - halfY, centerZ - halfZ},
                {centerX + halfX, centerY + halfY, centerZ + halfZ}};
    }
    if (configuration.id == kPreciseFinishTimeEvaluationId ||
        configuration.id == "finish-time") {
        return PhysicsSandboxCudaFinishTimeEvaluator{};
    }
    if (configuration.id == kStuntPointsEvaluationId) {
        return PhysicsSandboxCudaStuntPointsEvaluator{};
    }
    if (configuration.id == kVisualExpressionEvaluationId) {
        std::string error;
        const auto evaluator = BuildCudaVisualExpressionEvaluator(
                settings, tickDurationMs, &error);
        if (!evaluator) {
            throw std::invalid_argument(
                    error.empty()
                            ? "CUDA could not compile the visual expression"
                            : error);
        }
        return *evaluator;
    }
    throw std::invalid_argument(
            "CUDA does not support evaluator: " + configuration.id);
}

}  // namespace forevertas
