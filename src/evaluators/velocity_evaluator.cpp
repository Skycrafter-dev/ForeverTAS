#include "evaluators/velocity_evaluator.h"

#include "evaluators/evaluator_utils.h"
#include "searches/option_settings_utils.h"

#include <stdexcept>

namespace forevertas {
namespace {

struct VelocitySettings {
    std::int64_t minimumTimeMs = 0;
    std::int64_t maximumTimeMs = 0;
    bool projected = false;
    bool alignmentEnabled = false;
    EvaluationVector3 direction;
    double minimumAlignment = -1.0;
    // The direction is from the car towards this point, every tick.
    bool towardsPoint = false;
    EvaluationVector3 point;
};

class VelocitySession final : public IterationEvaluationSession {
public:
    std::unique_ptr<IterationEvaluationSession> Clone() const override {
        return std::make_unique<VelocitySession>(*this);
    }

    explicit VelocitySession(VelocitySettings settings)
        : settings_(settings) {}

    std::optional<EvaluationSample> Observe(
            const std::optional<
                    forevervalidator::experimental::PhysicsSandboxStateView>
                    &previous,
            const forevervalidator::experimental::PhysicsSandboxStateView
                    &current) override {
        static_cast<void>(previous);
        const EvaluationVector3 velocity = VelocityOf(current);
        const double speed = Length(velocity);
        const EvaluationVector3 direction = settings_.towardsPoint
                ? Normalize(Difference(settings_.point, PositionOf(current)))
                : settings_.direction;
        double alignment = 1.0;
        if (settings_.projected || settings_.alignmentEnabled) {
            if (speed <= 1e-12) alignment = 0.0;
            else alignment = Dot(Normalize(velocity), direction);
            if (alignment < settings_.minimumAlignment) return std::nullopt;
        }
        const double objective = settings_.projected
                ? Dot(velocity, direction)
                : speed;
        return EvaluationSample{
                objective,
                static_cast<double>(current.timeMs),
                MetricDescription(settings_.projected
                                          ? "Projected velocity"
                                          : "Velocity",
                                  objective,
                                  "m/s",
                                  static_cast<double>(current.timeMs))};
    }

private:
    VelocitySettings settings_;
};

class VelocityEvaluator final : public IterationEvaluator {
public:
    explicit VelocityEvaluator(VelocitySettings settings)
        : settings_(settings) {}

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
        return std::make_unique<VelocitySession>(settings_);
    }

    bool IsBetter(const EvaluationSample &iteration,
                  const EvaluationSample &incumbent) const override {
        return iteration.score > incumbent.score;
    }

private:
    VelocitySettings settings_;
};

std::optional<VelocitySettings> ParseSettings(
        const OptionSettings &settings,
        std::string *error) {
    const auto minimum = ReadTimeSetting(settings, "minTimeMs");
    const auto maximum = ReadTimeSetting(settings, "maxTimeMs");
    const auto direction = ReadVector3Settings(
            settings, "directionX", "directionY", "directionZ");
    const auto alignment = ReadDoubleSetting(settings, "minAlignmentPercent");
    const auto alignmentEnabled = ParseBoolean(settings.at("alignmentEnabled"));
    const std::string &mode = settings.at("mode");
    const std::string &directionMode = settings.at("directionMode");
    const auto point = ReadVector3Settings(settings, "x", "y", "z");
    if (!minimum || !maximum || !direction || !alignment ||
        !alignmentEnabled || !point ||
        (mode != "total" && mode != "projected") ||
        (directionMode != "fixed" && directionMode != "point")) {
        if (error) *error = kUnreadableSetting;
        return std::nullopt;
    }
    const bool projected = mode == "projected";
    const bool towardsPoint = directionMode == "point";
    const EvaluationVector3 normalized = Normalize(*direction);
    if (!towardsPoint && (projected || *alignmentEnabled) &&
        Length(normalized) <= 1e-12) {
        if (error) *error = "the direction can't be zero";
        return std::nullopt;
    }
    if (*alignment < -100.0 || *alignment > 100.0) {
        if (error) *error = "the minimum alignment must be between -100 and 100";
        return std::nullopt;
    }
    return VelocitySettings{*minimum,
                            *maximum,
                            projected,
                            *alignmentEnabled,
                            normalized,
                            *alignment / 100.0,
                            towardsPoint,
                            *point};
}

}  // namespace

OptionSettings DefaultVelocityOptionSettings() {
    return {{"minTimeMs", "1000"},
            {"maxTimeMs", "6000"},
            {"mode", "total"},
            {"alignmentEnabled", "false"},
            {"directionX", "1"},
            {"directionY", "0"},
            {"directionZ", "0"},
            {"minAlignmentPercent", "-100"},
            {"directionMode", "fixed"},
            {"x", "0"},
            {"y", "0"},
            {"z", "0"}};
}

std::optional<std::string> ValidateVelocityOptionSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) {
    if (const auto keyError = ValidateOptionSettingKeys(
                settings, DefaultVelocityOptionSettings())) {
        return keyError;
    }
    std::string parseError;
    const auto parsed = ParseSettings(settings, &parseError);
    if (!parsed) return parseError;
    return ValidateTimeWindow(parsed->minimumTimeMs,
                              parsed->maximumTimeMs,
                              tickDurationMs,
                              "the minimum time",
                              true);
}

std::unique_ptr<IterationEvaluator> CreateVelocityEvaluator(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) {
    if (const auto error =
                ValidateVelocityOptionSettings(settings, tickDurationMs)) {
        throw std::invalid_argument(*error);
    }
    return std::make_unique<VelocityEvaluator>(*ParseSettings(settings, nullptr));
}

}  // namespace forevertas
