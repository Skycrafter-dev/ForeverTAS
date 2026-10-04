#ifndef FOREVERTAS_CONDITIONS_CONDITION_PROGRAM_H
#define FOREVERTAS_CONDITIONS_CONDITION_PROGRAM_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <forevervalidator/experimental/physics_sandbox.h>

namespace forevertas {

struct ConditionVariable {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    bool vector = false;
};

using ConditionVariables = std::unordered_map<std::string, ConditionVariable>;

// Returns a view of one line, retaining comment markers inside quoted names.
std::string_view StripScriptComment(std::string_view line);

struct ConditionExecutionContext {
    std::uint64_t iterations = 0u;
    double lastImprovementTimeSeconds = 0.0;
    double lastRestartTimeSeconds = 0.0;
    double currentTimeSeconds = 0.0;
};

class ConditionProgram {
public:
    bool Evaluate(
            const forevervalidator::experimental::PhysicsSandboxStateView
                    &previous,
            const forevervalidator::experimental::PhysicsSandboxStateView
                    &current,
            const ConditionExecutionContext &context) const;

    forevervalidator::experimental::PhysicsSandboxCudaConditionProgram
            cuda;
};

struct ConditionCompileResult {
    std::optional<ConditionProgram> program;
    std::optional<std::string> error;
};

class ScalarExpressionProgram {
public:
    std::optional<double> Evaluate(
            const forevervalidator::experimental::PhysicsSandboxStateView
                    &previous,
            const forevervalidator::experimental::PhysicsSandboxStateView
                    &current,
            const ConditionExecutionContext &context) const;

    std::vector<forevervalidator::experimental::
                        PhysicsSandboxCudaConditionInstruction>
            instructions;
};

struct ScalarExpressionCompileResult {
    std::optional<ScalarExpressionProgram> program;
    std::optional<std::string> error;
};

ScalarExpressionCompileResult CompileScalarExpression(
        const std::string &source,
        const ConditionVariables &variables = {});

ConditionCompileResult CompileConditionScript(
        const std::string &source,
        const ConditionVariables &variables = {});

}  // namespace forevertas

#endif
