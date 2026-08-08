#ifndef FOREVERTAS_CONDITIONS_CONDITION_AST_COMPILER_H
#define FOREVERTAS_CONDITIONS_CONDITION_AST_COMPILER_H

#include "conditions/condition_program.h"
#include "conditions/condition_syntax.h"

#include <string>
#include <vector>

namespace forevertas {

struct ConditionAstCompileError final {
    ConditionSourceRange range;
    std::string message;
};

bool CompileConditionAst(
        const ConditionSyntaxNode &root,
        const ConditionVariables &variables,
        std::vector<forevervalidator::experimental::
                            PhysicsSandboxCudaConditionInstruction> *output,
        ConditionAstCompileError *error);

}  // namespace forevertas

#endif
