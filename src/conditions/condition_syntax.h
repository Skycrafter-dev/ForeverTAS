#ifndef FOREVERTAS_CONDITIONS_CONDITION_SYNTAX_H
#define FOREVERTAS_CONDITIONS_CONDITION_SYNTAX_H

#include "conditions/condition_language_catalog.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace forevertas {

struct ConditionSourceRange final {
    std::size_t begin = 0u;
    std::size_t end = 0u;
};

enum class ConditionSyntaxKind {
    Missing,
    Number,
    Name,
    Member,
    String,
    Call,
    Vector,
    Group,
    Add,
    Subtract,
    Multiply,
    Divide,
    Comparison
};

enum class ConditionComparisonOperator {
    None,
    Greater,
    Less,
    GreaterOrEqual,
    LessOrEqual,
    Equal
};

struct ConditionSyntaxNode final {
    ConditionSyntaxKind kind = ConditionSyntaxKind::Missing;
    ConditionSourceRange range;
    ConditionSourceRange segmentRange;
    std::string text;
    double number = 0.0;
    ConditionComparisonOperator comparison =
            ConditionComparisonOperator::None;
    std::vector<std::shared_ptr<ConditionSyntaxNode>> children;
};

struct ConditionSyntaxError final {
    ConditionSourceRange range;
    std::string message;
};

struct ConditionParsedLine final {
    std::shared_ptr<ConditionSyntaxNode> root;
    std::optional<ConditionSyntaxError> error;
};

enum class ConditionCursorSite {
    None,
    Expression,
    Member,
    FunctionArgument,
    ExternalName
};

struct ConditionCursorContext final {
    ConditionCursorSite site = ConditionCursorSite::None;
    ConditionSourceRange replacement;
    std::string receiver;
    std::string fragment;
    ConditionLanguageValueType expected =
            ConditionLanguageValueType::Unknown;
    std::string enumNamespace;
    std::string functionName;
    std::size_t argumentIndex = 0u;
    bool automaticTrigger = false;
};

ConditionParsedLine ParseConditionLine(std::string_view source,
                                       std::size_t baseOffset = 0u);

std::optional<ConditionCursorContext> AnalyzeConditionCursor(
        std::string_view source,
        std::size_t cursorByteOffset);

std::string ConditionSyntaxName(const ConditionSyntaxNode &node);

}  // namespace forevertas

#endif
