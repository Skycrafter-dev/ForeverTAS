#include "blocks/block_expression.h"

#include "blocks/block_value.h"

#include <cmath>
#include <vector>

namespace forevertas::blocks {
namespace {

std::optional<std::string> EvaluateReporter(const BlockProgram &program,
                                            BlockId reporterId,
                                            std::vector<BlockId> &ancestry);

std::optional<std::string> EvaluateSlot(const BlockProgram &program,
                                        const BlockNode &node,
                                        const std::string &key,
                                        std::vector<BlockId> &ancestry) {
    const auto reporter = node.reporters.find(key);
    if (reporter == node.reporters.end()) {
        const auto literal = node.fields.find(key);
        return literal == node.fields.end() ? std::string()
                                            : literal->second;
    }
    return EvaluateReporter(program, reporter->second, ancestry);
}

std::optional<std::string> EvaluateReporter(const BlockProgram &program,
                                            BlockId reporterId,
                                            std::vector<BlockId> &ancestry) {
    for (const BlockId ancestor : ancestry) {
        if (ancestor == reporterId) return std::nullopt;
    }
    const BlockNode *const node = program.find(reporterId);
    if (node == nullptr) return std::nullopt;
    ancestry.push_back(reporterId);
    double result = 0.0;
    if (node->definitionId == "values/number") {
        const auto literal = EvaluateSlot(program, *node, "value", ancestry);
        ancestry.pop_back();
        if (!literal) return std::nullopt;
        if (const auto parsed = ParseNumberValue(*literal)) {
            return literal;
        }
        return std::nullopt;
    }
    const auto leftText = EvaluateSlot(program, *node, "left", ancestry);
    const auto rightText = EvaluateSlot(program, *node, "right", ancestry);
    ancestry.pop_back();
    if (!leftText || !rightText) return std::nullopt;
    const auto left = ParseNumberValue(*leftText);
    const auto right = ParseNumberValue(*rightText);
    if (!left || !right) return std::nullopt;
    if (node->definitionId == "values/add") result = *left + *right;
    else if (node->definitionId == "values/subtract") result = *left - *right;
    else if (node->definitionId == "values/multiply") result = *left * *right;
    else if (node->definitionId == "values/divide") {
        if (*right == 0.0) return std::nullopt;
        result = *left / *right;
    } else if (node->definitionId == "values/minimum")
        result = std::min(*left, *right);
    else if (node->definitionId == "values/maximum")
        result = std::max(*left, *right);
    else {
        return std::nullopt;
    }
    // Overflow to infinity must surface as an invalid expression, not
    // format as a silent "0" that the search then runs against.
    if (!std::isfinite(result)) return std::nullopt;
    return FormatNumberValue(result);
}

}  // namespace

std::optional<std::string> EvaluateSlotValue(const BlockProgram &program,
                                             const BlockNode &node,
                                             const std::string &key) {
    std::vector<BlockId> ancestry{node.id};
    return EvaluateSlot(program, node, key, ancestry);
}

}  // namespace forevertas::blocks
