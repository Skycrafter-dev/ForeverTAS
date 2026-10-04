#include "evaluators/scripted_target_evaluator.h"

#include "conditions/condition_program.h"
#include "searches/option_settings_utils.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace forevertas {
namespace {

enum class ObjectiveKind { Min, Max, Target };

struct Objective {
    ObjectiveKind kind;
    double target = 0.0;
    std::string expression;
    ScalarExpressionProgram program;
};

struct ScriptedSettings {
    std::int64_t minimumTimeMs = 0;
    std::int64_t maximumTimeMs = 0;
    std::vector<Objective> objectives;
};

std::string_view Trim(std::string_view text) {
    while (!text.empty() && std::isspace(
                   static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(
                   static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

std::optional<ScriptedSettings> ParseSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs,
        std::string *error) {
    const auto minimum = ParseSignedDecimal(settings.at("minTimeMs"));
    const auto maximum = ParseSignedDecimal(settings.at("maxTimeMs"));
    if (!minimum || !maximum || *minimum < 0 || *maximum < 0 ||
        (*maximum != 0 && *maximum < *minimum) ||
        tickDurationMs == 0u ||
        *minimum % tickDurationMs != 0 ||
        *maximum % tickDurationMs != 0) {
        *error = "custom target times must be non-negative, ordered, and aligned to whole ticks";
        return std::nullopt;
    }
    ScriptedSettings parsed{*minimum, *maximum, {}};
    std::istringstream lines(settings.at("script"));
    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(lines, line)) {
        ++lineNumber;
        std::string_view source = line;
        source = Trim(StripScriptComment(source));
        if (source.empty()) continue;
        const std::size_t commandEnd = source.find_first_of(" \t");
        const std::string command(source.substr(0, commandEnd));
        std::string lower = command;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        source = commandEnd == std::string_view::npos
                ? std::string_view{}
                : Trim(source.substr(commandEnd));
        Objective objective;
        if (lower == "min") objective.kind = ObjectiveKind::Min;
        else if (lower == "max") objective.kind = ObjectiveKind::Max;
        else if (lower == "target") {
            objective.kind = ObjectiveKind::Target;
            const std::size_t valueEnd = source.find_first_of(" \t");
            const std::string targetText(source.substr(0, valueEnd));
            const auto target = ParseFiniteDouble(targetText);
            if (!target || valueEnd == std::string_view::npos) {
                *error = "Custom target line " +
                        std::to_string(lineNumber) +
                        ": expected target VALUE EXPRESSION";
                return std::nullopt;
            }
            objective.target = *target;
            source = Trim(source.substr(valueEnd));
        } else {
            *error = "Custom target line " + std::to_string(lineNumber) +
                    ": expected min, max, or target";
            return std::nullopt;
        }
        if (source.empty()) {
            *error = "Custom target line " + std::to_string(lineNumber) +
                    ": missing expression";
            return std::nullopt;
        }
        objective.expression = std::string(source);
        auto compiled = CompileScalarExpression(objective.expression);
        if (!compiled.program) {
            *error = "Custom target line " + std::to_string(lineNumber) +
                    ": " + compiled.error.value_or("invalid expression");
            return std::nullopt;
        }
        using forevervalidator::experimental::PhysicsSandboxCudaConditionValue;
        for (const auto &instruction : compiled.program->instructions) {
            if (instruction.opcode ==
                        forevervalidator::experimental::
                                PhysicsSandboxCudaConditionOpcode::Scalar &&
                (instruction.value == PhysicsSandboxCudaConditionValue::Iterations ||
                 instruction.value == PhysicsSandboxCudaConditionValue::LastImprovementTime ||
                 instruction.value == PhysicsSandboxCudaConditionValue::LastRestartTime ||
                 instruction.value == PhysicsSandboxCudaConditionValue::CurrentTime)) {
                *error = "Custom target line " +
                        std::to_string(lineNumber) +
                        ": search-time variables are not car-state metrics";
                return std::nullopt;
            }
        }
        objective.program = std::move(*compiled.program);
        parsed.objectives.push_back(std::move(objective));
    }
    if (parsed.objectives.empty()) {
        *error = "Custom target needs at least one min, max, or target directive";
        return std::nullopt;
    }
    return parsed;
}

class ScriptedSession final : public IterationEvaluationSession {
public:
    explicit ScriptedSession(const ScriptedSettings &settings)
        : settings_(settings),
          scores_(settings.objectives.size(),
                  -std::numeric_limits<double>::infinity()),
          values_(settings.objectives.size(), 0.0) {}

    void SetExecutionContext(
            const ConditionExecutionContext &context) override {
        context_ = context;
    }

    std::optional<EvaluationSample> Observe(
            const std::optional<forevervalidator::experimental::
                                        PhysicsSandboxStateView> &previous,
            const forevervalidator::experimental::PhysicsSandboxStateView
                    &current) override {
        if (!previous) return std::nullopt;
        std::vector<double> nextScores = scores_;
        std::vector<double> nextValues = values_;
        for (std::size_t index = 0; index < settings_.objectives.size();
             ++index) {
            const Objective &objective = settings_.objectives[index];
            const auto value = objective.program.Evaluate(
                    *previous, current, context_);
            if (!value) return std::nullopt;
            const double score = objective.kind == ObjectiveKind::Min
                    ? -*value
                    : objective.kind == ObjectiveKind::Max
                      ? *value
                      : -std::abs(*value - objective.target);
            if (!std::isfinite(score)) return std::nullopt;
            if (score > nextScores[index]) {
                nextScores[index] = score;
                nextValues[index] = *value;
            }
        }
        scores_ = std::move(nextScores);
        values_ = std::move(nextValues);
        std::ostringstream description;
        description.precision(8);
        for (std::size_t index = 0; index < settings_.objectives.size();
             ++index) {
            if (index != 0) description << "; ";
            const Objective &objective = settings_.objectives[index];
            description << (objective.kind == ObjectiveKind::Min
                                    ? "min "
                                    : objective.kind == ObjectiveKind::Max
                                      ? "max " : "target ")
                        << objective.expression << "=" << values_[index];
        }
        return EvaluationSample{scores_.front(),
                                static_cast<double>(current.timeMs),
                                description.str(), scores_, values_};
    }

private:
    const ScriptedSettings &settings_;
    ConditionExecutionContext context_;
    std::vector<double> scores_;
    std::vector<double> values_;
};

class ScriptedEvaluator final : public IterationEvaluator {
public:
    explicit ScriptedEvaluator(ScriptedSettings settings)
        : settings_(std::move(settings)) {}

    EvaluationPlan Plan(std::int64_t horizon,
                        std::int64_t earliestMutationTimeMs,
                        std::uint32_t tickDurationMs) const override {
        static_cast<void>(earliestMutationTimeMs);
        // The shared pre-mutation prefix can dominate individual objectives,
        // so Pareto scores must include the entire requested window.
        return {std::max<std::int64_t>(settings_.minimumTimeMs,
                                       tickDurationMs),
                settings_.maximumTimeMs == 0
                        ? horizon : settings_.maximumTimeMs};
    }

    std::unique_ptr<IterationEvaluationSession> CreateSession()
            const override {
        return std::make_unique<ScriptedSession>(settings_);
    }

    bool IsBetter(const EvaluationSample &candidate,
                  const EvaluationSample &incumbent) const override {
        if (candidate.objectiveScores.size() !=
            incumbent.objectiveScores.size()) return false;
        bool anyBetter = false;
        for (std::size_t index = 0;
             index < candidate.objectiveScores.size(); ++index) {
            if (candidate.objectiveScores[index] <
                incumbent.objectiveScores[index]) return false;
            anyBetter |= candidate.objectiveScores[index] >
                    incumbent.objectiveScores[index];
        }
        return anyBetter;
    }

    bool IsEquivalent(const EvaluationSample &,
                      const EvaluationSample &) const override {
        return false;
    }

    bool CompareAtEndOnly() const override { return true; }

private:
    ScriptedSettings settings_;
};

}  // namespace

OptionSettings DefaultScriptedTargetOptionSettings() {
    return {{"minTimeMs", "0"}, {"maxTimeMs", "0"},
            {"script", "max car.speed"}};
}

std::optional<std::string> ValidateScriptedTargetOptionSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) {
    if (const auto keyError = ValidateOptionSettingKeys(
                settings, DefaultScriptedTargetOptionSettings())) {
        return keyError;
    }
    std::string error;
    if (!ParseSettings(settings, tickDurationMs, &error)) return error;
    return std::nullopt;
}

std::unique_ptr<IterationEvaluator> CreateScriptedTargetEvaluator(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) {
    std::string error;
    auto parsed = ParseSettings(settings, tickDurationMs, &error);
    if (!parsed) throw std::invalid_argument(error);
    return std::make_unique<ScriptedEvaluator>(std::move(*parsed));
}

forevervalidator::experimental::PhysicsSandboxCudaScriptedEvaluator
BuildCudaScriptedTargetEvaluator(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) {
    using namespace forevervalidator::experimental;
    std::string error;
    auto parsed = ParseSettings(settings, tickDurationMs, &error);
    if (!parsed) throw std::invalid_argument(error);
    PhysicsSandboxCudaScriptedEvaluator result;
    std::size_t instructionCount = 0u;
    for (const Objective &objective : parsed->objectives) {
        instructionCount += objective.program.instructions.size();
        if (result.objectives.size() >= 16u ||
            instructionCount > 256u) {
            throw std::invalid_argument(
                    "GPU custom targets support at most 16 objectives and "
                    "256 total expression instructions");
        }
        result.objectives.push_back({
                objective.kind == ObjectiveKind::Min
                        ? PhysicsSandboxCudaScriptedObjectiveKind::Min
                        : objective.kind == ObjectiveKind::Max
                          ? PhysicsSandboxCudaScriptedObjectiveKind::Max
                          : PhysicsSandboxCudaScriptedObjectiveKind::Target,
                objective.target,
                objective.program.instructions});
    }
    return result;
}

}  // namespace forevertas
