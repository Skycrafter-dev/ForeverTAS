#ifndef FOREVERTAS_CONDITIONS_CONDITION_PROGRAM_H
#define FOREVERTAS_CONDITIONS_CONDITION_PROGRAM_H

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <forevervalidator/experimental/physics_sandbox.h>

#include "conditions/condition_language_catalog.h"

namespace forevertas {

struct ConditionVariable {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    bool vector = false;
};

using ConditionVariables = std::unordered_map<std::string, ConditionVariable>;

struct ConditionExecutionContext {
    std::uint64_t iterations = 0u;
    double lastImprovementTimeSeconds = 0.0;
    double lastRestartTimeSeconds = 0.0;
    double currentTimeSeconds = 0.0;
};

struct ConditionDiagnostic {
    std::uint32_t line = 0u;
    std::uint32_t column = 1u;
    std::uint32_t length = 1u;
    std::string message;
};

enum class ConditionGateMode { All, Any };

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
    std::vector<ConditionDiagnostic> diagnostics;
};

ConditionCompileResult CompileConditionScript(
        const std::string &source,
        const ConditionVariables &variables = {},
        ConditionGateMode gateMode = ConditionGateMode::All);

}  // namespace forevertas

#endif
