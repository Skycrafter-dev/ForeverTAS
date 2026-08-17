#include "evaluators/point_target_evaluator.h"

#include "evaluators/evaluator_utils.h"
#include "searches/option_settings_utils.h"

#include <stdexcept>

namespace forevertas {
namespace {

struct PointSettings {
    std::int64_t minimumTimeMs = 0;
    std::int64_t maximumTimeMs = 0;
    EvaluationVector3 target;
};

class PointSession final : public IterationEvaluationSession {
public:
    explicit PointSession(EvaluationVector3 target) : target_(target) {}

    std::optional<EvaluationSample> Observe(
            const std::optional<
                    forevervalidator::experimental::PhysicsSandboxStateView>
                    &previous,
            const forevervalidator::experimental::PhysicsSandboxStateView
                    &current) override {
        static_cast<void>(previous);
        const double distance = Distance(PositionOf(current), target_);
        return EvaluationSample{
                distance,
                static_cast<double>(current.timeMs),
                MetricDescription("Point distance",
                                  distance,
                                  "m",
                                  static_cast<double>(current.timeMs))};
    }

private:
    EvaluationVector3 target_;
};

class PointEvaluator final : public IterationEvaluator {
public:
    explicit PointEvaluator(PointSettings settings) : settings_(settings) {}

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
        return std::make_unique<PointSession>(settings_.target);
    }
    bool IsBetter(const EvaluationSample &iteration,
                  const EvaluationSample &incumbent) const override {
        return iteration.score < incumbent.score;
    }

private:
    PointSettings settings_;
};

std::optional<PointSettings> ParseSettings(const OptionSettings &settings) {
    const auto minimum = ReadTimeSetting(settings, "minTimeMs");
    const auto maximum = ReadTimeSetting(settings, "maxTimeMs");
    const auto target = ReadVector3Settings(settings, "x", "y", "z");
    if (!minimum || !maximum || !target) return std::nullopt;
    return PointSettings{*minimum, *maximum, *target};
}

}  // namespace

OptionFieldList PointTargetOptionFields() {
    OptionFieldList fields;
    AppendWindowFields(fields, "1000", "6000");
    OptionField x = NumberField("x", "X", "0");
    x.group = "Target point";
    fields.push_back(x);
    OptionField y = NumberField("y", "Y", "0");
    y.group = "Target point";
    fields.push_back(y);
    OptionField z = NumberField("z", "Z", "0");
    z.group = "Target point";
    fields.push_back(z);
    return fields;
}

OptionSettings DefaultPointTargetOptionSettings() {
    return {{"minTimeMs", "1000"},
            {"maxTimeMs", "6000"},
            {"x", "0"},
            {"y", "0"},
            {"z", "0"}};
}

std::optional<std::string> ValidatePointTargetOptionSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) {
    if (const auto keyError = ValidateOptionSettingKeys(
                settings, DefaultPointTargetOptionSettings())) {
        return keyError;
    }
    const auto parsed = ParseSettings(settings);
    if (!parsed) return "point target settings contain invalid values";
    return ValidateTimeWindow(parsed->minimumTimeMs,
                              parsed->maximumTimeMs,
                              tickDurationMs,
                              "evaluation",
                              true);
}

std::unique_ptr<IterationEvaluator> CreatePointTargetEvaluator(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) {
    if (const auto error =
                ValidatePointTargetOptionSettings(settings, tickDurationMs)) {
        throw std::invalid_argument(*error);
    }
    return std::make_unique<PointEvaluator>(*ParseSettings(settings));
}

}  // namespace forevertas
