#include "conditions/condition_ast_compiler.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string_view>

namespace forevertas {
namespace {

using namespace forevervalidator::experimental;

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

class AstCompiler final {
public:
    AstCompiler(
            const ConditionVariables &variables,
            std::vector<PhysicsSandboxCudaConditionInstruction> *output,
            ConditionAstCompileError *error)
        : variables_(variables), output_(output), error_(error) {}

    bool Compile(const ConditionSyntaxNode &root) {
        if (root.kind != ConditionSyntaxKind::Comparison ||
            root.children.size() != 2u) {
            return Fail(root.range, "expected comparison");
        }
        if (!ValidateConstants(*root.children[0], *root.children[1])) {
            return false;
        }
        if (!EmitExpression(*root.children[0],
                            ConditionLanguageValueType::Scalar) ||
            !EmitExpression(*root.children[1],
                            ConditionLanguageValueType::Scalar)) {
            return false;
        }
        switch (root.comparison) {
        case ConditionComparisonOperator::Greater:
            Emit(PhysicsSandboxCudaConditionOpcode::Greater);
            return true;
        case ConditionComparisonOperator::Less:
            Emit(PhysicsSandboxCudaConditionOpcode::Less);
            return true;
        case ConditionComparisonOperator::GreaterOrEqual:
            Emit(PhysicsSandboxCudaConditionOpcode::GreaterOrEqual);
            return true;
        case ConditionComparisonOperator::LessOrEqual:
            Emit(PhysicsSandboxCudaConditionOpcode::LessOrEqual);
            return true;
        case ConditionComparisonOperator::Equal:
            Emit(PhysicsSandboxCudaConditionOpcode::Equal);
            return true;
        case ConditionComparisonOperator::None:
            return Fail(root.range, "expected comparison operator");
        }
        return false;
    }

private:
    const ConditionSyntaxNode *FirstConstant(
            const ConditionSyntaxNode &node) const {
        if ((node.kind == ConditionSyntaxKind::Name ||
             node.kind == ConditionSyntaxKind::Member) &&
            FindConditionConstant(ConditionSyntaxName(node)) != nullptr) {
            return &node;
        }
        for (const auto &child : node.children) {
            if (const ConditionSyntaxNode *const constant =
                        FirstConstant(*child)) {
                return constant;
            }
        }
        return nullptr;
    }

    const ConditionSyntaxNode &Ungroup(const ConditionSyntaxNode &node) const {
        if (node.kind == ConditionSyntaxKind::Group &&
            node.children.size() == 1u) {
            return Ungroup(*node.children.front());
        }
        return node;
    }

    bool ValidateRightConstants(const ConditionSyntaxNode &node,
                                std::string_view expectedEnum) {
        if (node.kind == ConditionSyntaxKind::Name ||
            node.kind == ConditionSyntaxKind::Member) {
            const std::string spelling = ConditionSyntaxName(node);
            if (const ConditionLanguageConstant *const constant =
                        FindConditionConstant(spelling)) {
                if (expectedEnum.empty()) {
                    return Fail(node.range,
                                "constant '" + spelling +
                                        "' requires a matching typed field "
                                        "on the left");
                }
                if (constant->enumNamespace != expectedEnum) {
                    return Fail(node.range,
                                "expected " + std::string(expectedEnum) +
                                        " value, got " +
                                        std::string(constant->enumNamespace));
                }
            }
        }
        for (const auto &child : node.children) {
            if (!ValidateRightConstants(*child, expectedEnum)) return false;
        }
        return true;
    }

    bool ValidateConstants(const ConditionSyntaxNode &left,
                           const ConditionSyntaxNode &right) {
        if (const ConditionSyntaxNode *const constant = FirstConstant(left)) {
            return Fail(constant->range,
                        "constants can only be used on the right side of a "
                        "comparison");
        }

        std::string_view expectedEnum;
        const ConditionSyntaxNode &field = Ungroup(left);
        if (field.kind == ConditionSyntaxKind::Name ||
            field.kind == ConditionSyntaxKind::Member) {
            if (const ConditionLanguageSymbol *const symbol =
                        FindConditionSymbol(ConditionSyntaxName(field))) {
                expectedEnum = symbol->enumNamespace;
            }
        }
        return ValidateRightConstants(right, expectedEnum);
    }

    bool EmitExpression(const ConditionSyntaxNode &node,
                        ConditionLanguageValueType expected) {
        switch (node.kind) {
        case ConditionSyntaxKind::Number:
            if (expected == ConditionLanguageValueType::Vector ||
                expected == ConditionLanguageValueType::ExternalName) {
                return Fail(node.range, "expected vector expression");
            }
            output_->push_back(
                    {PhysicsSandboxCudaConditionOpcode::Constant,
                     {},
                     node.number});
            return true;
        case ConditionSyntaxKind::Name:
        case ConditionSyntaxKind::Member:
            return EmitSymbol(node, expected);
        case ConditionSyntaxKind::String:
            return Fail(node.range, "string is only valid as a variable name");
        case ConditionSyntaxKind::Call:
            return EmitCall(node, expected);
        case ConditionSyntaxKind::Vector:
            return EmitVector(node, expected);
        case ConditionSyntaxKind::Group:
            return node.children.size() == 1u &&
                    EmitExpression(*node.children.front(), expected);
        case ConditionSyntaxKind::Add:
        case ConditionSyntaxKind::Subtract:
        case ConditionSyntaxKind::Multiply:
        case ConditionSyntaxKind::Divide:
            return EmitBinary(node, expected);
        case ConditionSyntaxKind::Missing:
            return Fail(node.range, "expected number, variable, or function");
        case ConditionSyntaxKind::Comparison:
            return Fail(node.range, "comparison cannot be nested");
        }
        return false;
    }

    bool EmitSymbol(const ConditionSyntaxNode &node,
                    ConditionLanguageValueType expected) {
        const std::string spelling = ConditionSyntaxName(node);
        if (const ConditionLanguageConstant *const constant =
                    FindConditionConstant(spelling)) {
            if (expected == ConditionLanguageValueType::Vector ||
                expected == ConditionLanguageValueType::ExternalName) {
                return Fail(node.range, "expected vector expression");
            }
            output_->push_back(
                    {PhysicsSandboxCudaConditionOpcode::Constant,
                     {},
                     constant->value});
            return true;
        }
        const auto resolution = ResolveConditionSymbol(spelling);
        if (!resolution) {
            return Fail(node.range,
                        "unknown condition variable '" + spelling + "'");
        }
        const bool vector = resolution->symbol->kind ==
                ConditionLanguageSymbolKind::Vector;
        if (expected == ConditionLanguageValueType::Vector && !vector) {
            return Fail(node.range, "expected vector expression");
        }
        if (expected == ConditionLanguageValueType::Scalar && vector) {
            return Fail(node.range, "expected scalar expression");
        }
        output_->push_back(
                {vector ? PhysicsSandboxCudaConditionOpcode::Vector
                        : PhysicsSandboxCudaConditionOpcode::Scalar,
                 resolution->sourceValue,
                 static_cast<double>(resolution->sourceComponent)});
        return true;
    }

    bool EmitBinary(const ConditionSyntaxNode &node,
                    ConditionLanguageValueType expected) {
        if (expected == ConditionLanguageValueType::Vector ||
            node.children.size() != 2u) {
            return Fail(node.range, "expected scalar expression");
        }
        if (!EmitExpression(*node.children[0],
                            ConditionLanguageValueType::Scalar) ||
            !EmitExpression(*node.children[1],
                            ConditionLanguageValueType::Scalar)) {
            return false;
        }
        switch (node.kind) {
        case ConditionSyntaxKind::Add:
            Emit(PhysicsSandboxCudaConditionOpcode::Add);
            break;
        case ConditionSyntaxKind::Subtract:
            Emit(PhysicsSandboxCudaConditionOpcode::Subtract);
            break;
        case ConditionSyntaxKind::Multiply:
            Emit(PhysicsSandboxCudaConditionOpcode::Multiply);
            break;
        case ConditionSyntaxKind::Divide:
            Emit(PhysicsSandboxCudaConditionOpcode::Divide);
            break;
        default: return false;
        }
        return true;
    }

    bool EmitVector(const ConditionSyntaxNode &node,
                    ConditionLanguageValueType expected) {
        if (expected == ConditionLanguageValueType::Scalar ||
            node.children.size() != 3u) {
            return Fail(node.range, "expected scalar expression");
        }
        double values[3]{};
        for (std::size_t index = 0u; index < 3u; ++index) {
            if (node.children[index]->kind != ConditionSyntaxKind::Number) {
                return Fail(node.children[index]->range,
                            "vector literal must contain three numbers");
            }
            values[index] = node.children[index]->number;
        }
        output_->push_back(
                {PhysicsSandboxCudaConditionOpcode::ConstantVector,
                 {},
                 values[0],
                 values[1],
                 values[2]});
        return true;
    }

    bool EmitCall(const ConditionSyntaxNode &node,
                  ConditionLanguageValueType expected) {
        const ConditionLanguageFunction *const function =
                FindConditionFunction(node.text);
        if (function == nullptr) {
            return Fail(node.segmentRange,
                        "unknown condition function '" + node.text + "'");
        }
        if (node.children.size() != function->parameterTypes.size()) {
            return Fail(node.range,
                        "wrong number of arguments for '" + node.text + "'");
        }
        if (function->kind ==
            ConditionLanguageFunctionKind::VariableLookup) {
            return EmitExternalVariable(node, expected);
        }
        if (expected == ConditionLanguageValueType::Vector &&
            function->resultType != ConditionLanguageValueType::Vector) {
            return Fail(node.range, "expected vector expression");
        }
        if (function->kind == ConditionLanguageFunctionKind::TimeSince) {
            output_->push_back(
                    {PhysicsSandboxCudaConditionOpcode::Scalar,
                     PhysicsSandboxCudaConditionValue::CurrentTime});
        }
        for (std::size_t index = 0u; index < node.children.size(); ++index) {
            if (!EmitExpression(*node.children[index],
                                function->parameterTypes[index])) {
                return false;
            }
        }
        switch (function->kind) {
        case ConditionLanguageFunctionKind::KmH:
            Emit(PhysicsSandboxCudaConditionOpcode::KilometersPerHour);
            break;
        case ConditionLanguageFunctionKind::Deg:
            Emit(PhysicsSandboxCudaConditionOpcode::Degrees);
            break;
        case ConditionLanguageFunctionKind::TimeSince:
            Emit(PhysicsSandboxCudaConditionOpcode::Subtract);
            break;
        case ConditionLanguageFunctionKind::Distance:
            Emit(PhysicsSandboxCudaConditionOpcode::Distance);
            break;
        case ConditionLanguageFunctionKind::VariableLookup:
            return false;
        }
        return true;
    }

    bool EmitExternalVariable(const ConditionSyntaxNode &call,
                              ConditionLanguageValueType expected) {
        const ConditionSyntaxNode &argument = *call.children.front();
        const std::string name = argument.kind == ConditionSyntaxKind::String
                ? argument.text
                : ConditionSyntaxName(argument);
        if (name.empty()) {
            return Fail(argument.range, "expected external variable name");
        }
        const auto found = variables_.find(Lower(name));
        if (found == variables_.end()) {
            return Fail(argument.range,
                        "unknown external variable '" + name + "'");
        }
        if (expected == ConditionLanguageValueType::Vector &&
            !found->second.vector) {
            return Fail(argument.range, "expected vector expression");
        }
        if (expected == ConditionLanguageValueType::Scalar &&
            found->second.vector) {
            return Fail(argument.range, "expected scalar expression");
        }
        if (found->second.vector) {
            output_->push_back(
                    {PhysicsSandboxCudaConditionOpcode::ConstantVector,
                     {},
                     found->second.x,
                     found->second.y,
                     found->second.z});
        } else {
            output_->push_back(
                    {PhysicsSandboxCudaConditionOpcode::Constant,
                     {},
                     found->second.x});
        }
        return true;
    }

    void Emit(PhysicsSandboxCudaConditionOpcode opcode) {
        output_->push_back({opcode});
    }

    bool Fail(ConditionSourceRange range, std::string message) {
        error_->range = range;
        error_->message = std::move(message);
        return false;
    }

    const ConditionVariables &variables_;
    std::vector<PhysicsSandboxCudaConditionInstruction> *output_;
    ConditionAstCompileError *error_;
};

}  // namespace

bool CompileConditionAst(
        const ConditionSyntaxNode &root,
        const ConditionVariables &variables,
        std::vector<PhysicsSandboxCudaConditionInstruction> *output,
        ConditionAstCompileError *error) {
    return AstCompiler(variables, output, error).Compile(root);
}

}  // namespace forevertas
