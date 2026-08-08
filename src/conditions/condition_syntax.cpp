#include "conditions/condition_syntax.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <utility>

namespace forevertas {
namespace {

enum class TokenKind {
    End,
    Identifier,
    Number,
    String,
    Dot,
    LeftParen,
    RightParen,
    Comma,
    Plus,
    Minus,
    Star,
    Slash,
    Greater,
    Less,
    GreaterOrEqual,
    LessOrEqual,
    Equal,
    Invalid
};

struct Token final {
    TokenKind kind = TokenKind::End;
    ConditionSourceRange range;
    std::string text;
    double number = 0.0;
};

bool IsIdentifierStart(unsigned char value) {
    return std::isalpha(value) != 0 || value == '_';
}

bool IsIdentifierContinue(unsigned char value) {
    return std::isalnum(value) != 0 || value == '_';
}

std::vector<Token> Lex(std::string_view source, std::size_t baseOffset) {
    std::vector<Token> tokens;
    std::size_t position = 0u;
    const auto push = [&](TokenKind kind,
                          std::size_t begin,
                          std::size_t end,
                          std::string text = {},
                          double number = 0.0) {
        tokens.push_back({kind,
                          {baseOffset + begin, baseOffset + end},
                          std::move(text),
                          number});
    };

    while (position < source.size()) {
        const unsigned char value =
                static_cast<unsigned char>(source[position]);
        if (std::isspace(value) != 0) {
            ++position;
            continue;
        }
        const std::size_t begin = position;
        if (IsIdentifierStart(value)) {
            ++position;
            while (position < source.size() &&
                   IsIdentifierContinue(
                           static_cast<unsigned char>(source[position]))) {
                ++position;
            }
            push(TokenKind::Identifier,
                 begin,
                 position,
                 std::string(source.substr(begin, position - begin)));
            continue;
        }
        if (std::isdigit(value) != 0 ||
            (value == '.' && position + 1u < source.size() &&
             std::isdigit(static_cast<unsigned char>(source[position + 1u])) !=
                     0)) {
            const std::string tail(source.substr(position));
            char *end = nullptr;
            const double number = std::strtod(tail.c_str(), &end);
            if (end != tail.c_str() && std::isfinite(number)) {
                position += static_cast<std::size_t>(end - tail.c_str());
                push(TokenKind::Number,
                     begin,
                     position,
                     std::string(source.substr(begin, position - begin)),
                     number);
                continue;
            }
        }
        if (value == '"') {
            ++position;
            const std::size_t contentBegin = position;
            while (position < source.size() && source[position] != '"') {
                ++position;
            }
            const std::size_t contentEnd = position;
            if (position < source.size()) ++position;
            push(TokenKind::String,
                 begin,
                 position,
                 std::string(source.substr(contentBegin,
                                           contentEnd - contentBegin)));
            continue;
        }

        const auto two = position + 1u < source.size()
                ? source.substr(position, 2u)
                : std::string_view{};
        if (two == ">=") {
            position += 2u;
            push(TokenKind::GreaterOrEqual, begin, position);
        } else if (two == "<=") {
            position += 2u;
            push(TokenKind::LessOrEqual, begin, position);
        } else {
            ++position;
            switch (value) {
            case '.': push(TokenKind::Dot, begin, position); break;
            case '(': push(TokenKind::LeftParen, begin, position); break;
            case ')': push(TokenKind::RightParen, begin, position); break;
            case ',': push(TokenKind::Comma, begin, position); break;
            case '+': push(TokenKind::Plus, begin, position); break;
            case '-': push(TokenKind::Minus, begin, position); break;
            case '*': push(TokenKind::Star, begin, position); break;
            case '/': push(TokenKind::Slash, begin, position); break;
            case '>': push(TokenKind::Greater, begin, position); break;
            case '<': push(TokenKind::Less, begin, position); break;
            case '=': push(TokenKind::Equal, begin, position); break;
            default:
                push(TokenKind::Invalid,
                     begin,
                     position,
                     std::string(1, static_cast<char>(value)));
                break;
            }
        }
    }
    tokens.push_back({TokenKind::End,
                      {baseOffset + source.size(),
                       baseOffset + source.size()},
                      {},
                      0.0});
    return tokens;
}

std::shared_ptr<ConditionSyntaxNode> Node(
        ConditionSyntaxKind kind,
        ConditionSourceRange range) {
    auto node = std::make_shared<ConditionSyntaxNode>();
    node->kind = kind;
    node->range = range;
    node->segmentRange = range;
    return node;
}

class SyntaxParser final {
public:
    SyntaxParser(std::string_view source, std::size_t baseOffset)
        : source_(source),
          baseOffset_(baseOffset),
          tokens_(Lex(source, baseOffset)) {}

    ConditionParsedLine Parse() {
        ConditionParsedLine result;
        auto left = ParseExpression();
        ConditionComparisonOperator comparison =
                ConditionComparisonOperator::None;
        const Token comparator = Current();
        switch (comparator.kind) {
        case TokenKind::Greater:
            comparison = ConditionComparisonOperator::Greater;
            break;
        case TokenKind::Less:
            comparison = ConditionComparisonOperator::Less;
            break;
        case TokenKind::GreaterOrEqual:
            comparison = ConditionComparisonOperator::GreaterOrEqual;
            break;
        case TokenKind::LessOrEqual:
            comparison = ConditionComparisonOperator::LessOrEqual;
            break;
        case TokenKind::Equal:
            comparison = ConditionComparisonOperator::Equal;
            break;
        default: break;
        }

        std::shared_ptr<ConditionSyntaxNode> right;
        if (comparison == ConditionComparisonOperator::None) {
            SetError(Current().range, "expected comparison operator");
            right = Node(ConditionSyntaxKind::Missing, Current().range);
        } else {
            Advance();
            right = ParseExpression();
        }

        auto root = Node(ConditionSyntaxKind::Comparison,
                         {left->range.begin, right->range.end});
        root->comparison = comparison;
        root->children = {std::move(left), std::move(right)};
        if (Current().kind != TokenKind::End) {
            SetError(Current().range, "unexpected text after comparison");
        }
        result.root = std::move(root);
        result.error = std::move(error_);
        return result;
    }

private:
    const Token &Current() const { return tokens_[position_]; }
    const Token &Previous() const { return tokens_[position_ - 1u]; }
    bool Match(TokenKind kind) {
        if (Current().kind != kind) return false;
        Advance();
        return true;
    }
    void Advance() {
        if (position_ + 1u < tokens_.size()) ++position_;
    }
    void SetError(ConditionSourceRange range, std::string message) {
        if (error_) return;
        error_ = ConditionSyntaxError{range, std::move(message)};
    }

    std::shared_ptr<ConditionSyntaxNode> ParseExpression() {
        auto left = ParseTerm();
        while (Current().kind == TokenKind::Plus ||
               Current().kind == TokenKind::Minus) {
            const TokenKind operation = Current().kind;
            Advance();
            auto right = ParseTerm();
            auto binary = Node(operation == TokenKind::Plus
                                       ? ConditionSyntaxKind::Add
                                       : ConditionSyntaxKind::Subtract,
                               {left->range.begin, right->range.end});
            binary->children = {std::move(left), std::move(right)};
            left = std::move(binary);
        }
        return left;
    }

    std::shared_ptr<ConditionSyntaxNode> ParseTerm() {
        auto left = ParsePrimary();
        while (Current().kind == TokenKind::Star ||
               Current().kind == TokenKind::Slash) {
            const TokenKind operation = Current().kind;
            Advance();
            auto right = ParsePrimary();
            auto binary = Node(operation == TokenKind::Star
                                       ? ConditionSyntaxKind::Multiply
                                       : ConditionSyntaxKind::Divide,
                               {left->range.begin, right->range.end});
            binary->children = {std::move(left), std::move(right)};
            left = std::move(binary);
        }
        return left;
    }

    std::shared_ptr<ConditionSyntaxNode> ParsePrimary() {
        if (Current().kind == TokenKind::Plus ||
            Current().kind == TokenKind::Minus) {
            const bool negative = Current().kind == TokenKind::Minus;
            const ConditionSourceRange sign = Current().range;
            Advance();
            if (Current().kind != TokenKind::Number) {
                SetError(Current().range, "expected number after sign");
                return Node(ConditionSyntaxKind::Missing,
                            {sign.begin, Current().range.end});
            }
            auto number = ParsePrimary();
            if (negative) number->number = -number->number;
            number->range.begin = sign.begin;
            number->segmentRange = number->range;
            return number;
        }
        if (Match(TokenKind::Number)) {
            auto number = Node(ConditionSyntaxKind::Number, Previous().range);
            number->number = Previous().number;
            return number;
        }
        if (Match(TokenKind::String)) {
            auto string = Node(ConditionSyntaxKind::String, Previous().range);
            string->text = Previous().text;
            return string;
        }
        if (Match(TokenKind::Identifier)) {
            const Token first = Previous();
            auto expression = Node(ConditionSyntaxKind::Name, first.range);
            expression->text = first.text;

            if (Current().kind == TokenKind::LeftParen) {
                return ParseCall(std::move(expression));
            }
            while (Match(TokenKind::Dot)) {
                const Token dot = Previous();
                auto member = Node(ConditionSyntaxKind::Member,
                                   {expression->range.begin, dot.range.end});
                member->children.push_back(std::move(expression));
                member->segmentRange = {dot.range.end, dot.range.end};
                if (Match(TokenKind::Identifier)) {
                    member->text = Previous().text;
                    member->segmentRange = Previous().range;
                    member->range.end = Previous().range.end;
                } else {
                    SetError(Current().range, "expected member name after '.'");
                }
                expression = std::move(member);
            }
            return expression;
        }
        if (Match(TokenKind::LeftParen)) {
            const Token open = Previous();
            auto first = ParseExpression();
            if (Match(TokenKind::Comma)) {
                auto second = ParseExpression();
                if (!Match(TokenKind::Comma)) {
                    SetError(Current().range,
                             "vector literal must contain three numbers");
                }
                auto third = ParseExpression();
                const ConditionSourceRange end = Current().range;
                if (!Match(TokenKind::RightParen)) {
                    SetError(Current().range,
                             "expected ')' after vector");
                }
                auto vector = Node(ConditionSyntaxKind::Vector,
                                   {open.range.begin,
                                    Previous().kind == TokenKind::RightParen
                                            ? Previous().range.end
                                            : end.end});
                vector->children = {std::move(first),
                                    std::move(second),
                                    std::move(third)};
                return vector;
            }
            const ConditionSourceRange end = Current().range;
            if (!Match(TokenKind::RightParen)) {
                SetError(Current().range, "expected ')'");
            }
            auto group = Node(ConditionSyntaxKind::Group,
                              {open.range.begin,
                               Previous().kind == TokenKind::RightParen
                                       ? Previous().range.end
                                       : end.end});
            group->children.push_back(std::move(first));
            return group;
        }

        const ConditionSourceRange missing = Current().range;
        if (Current().kind == TokenKind::Invalid) {
            SetError(missing, "unexpected character");
            Advance();
        } else {
            SetError(missing, "expected number, variable, or function");
        }
        return Node(ConditionSyntaxKind::Missing, missing);
    }

    std::shared_ptr<ConditionSyntaxNode> ParseCall(
            std::shared_ptr<ConditionSyntaxNode> callee) {
        const Token open = Current();
        Advance();
        auto call = Node(ConditionSyntaxKind::Call,
                         {callee->range.begin, open.range.end});
        call->text = callee->text;
        call->segmentRange = callee->range;

        const ConditionLanguageFunction *const function =
                FindConditionFunction(call->text);
        if (function != nullptr &&
            function->kind ==
                    ConditionLanguageFunctionKind::VariableLookup) {
            if (Current().kind == TokenKind::String) {
                call->children.push_back(ParsePrimary());
            } else {
                std::size_t rawBegin = Current().range.begin;
                while (Current().kind != TokenKind::RightParen &&
                       Current().kind != TokenKind::End) {
                    Advance();
                }
                std::size_t rawEnd = Current().range.begin;
                while (rawBegin < rawEnd &&
                       std::isspace(static_cast<unsigned char>(
                               source_[rawBegin - baseOffset_])) != 0) {
                    ++rawBegin;
                }
                auto argument = Node(
                        rawBegin == rawEnd ? ConditionSyntaxKind::Missing
                                           : ConditionSyntaxKind::Name,
                        {rawBegin, rawEnd});
                if (rawBegin != rawEnd) {
                    argument->text = std::string(source_.substr(
                            rawBegin - baseOffset_, rawEnd - rawBegin));
                }
                call->children.push_back(std::move(argument));
            }
            const ConditionSourceRange end = Current().range;
            if (Match(TokenKind::RightParen)) {
                call->range.end = Previous().range.end;
            } else {
                SetError(end, "expected ')' after variable name");
                call->range.end = end.end;
            }
            return call;
        }

        bool expectArgument = true;
        while (Current().kind != TokenKind::RightParen &&
               Current().kind != TokenKind::End) {
            call->children.push_back(ParseExpression());
            expectArgument = false;
            if (!Match(TokenKind::Comma)) break;
            expectArgument = true;
        }
        if (expectArgument) {
            const std::size_t insertionPoint = Current().range.begin;
            call->children.push_back(
                    Node(ConditionSyntaxKind::Missing,
                         {insertionPoint, insertionPoint}));
        }
        const ConditionSourceRange end = Current().range;
        if (Match(TokenKind::RightParen)) {
            call->range.end = Previous().range.end;
        } else {
            SetError(end, "expected ')' after function arguments");
            call->range.end = end.end;
        }
        return call;
    }

    std::string_view source_;
    std::size_t baseOffset_ = 0u;
    std::vector<Token> tokens_;
    std::size_t position_ = 0u;
    std::optional<ConditionSyntaxError> error_;
};

bool Contains(ConditionSourceRange range, std::size_t position) {
    return position >= range.begin && position <= range.end;
}

ConditionLanguageValueType ParameterType(
        const ConditionLanguageFunction *function,
        std::size_t index) {
    if (function == nullptr || index >= function->parameterTypes.size()) {
        return ConditionLanguageValueType::Unknown;
    }
    return function->parameterTypes[index];
}

bool FindCursorContext(const ConditionSyntaxNode &node,
                       std::string_view source,
                       std::size_t cursor,
                       ConditionLanguageValueType expected,
                       const ConditionLanguageFunction *function,
                       std::size_t argumentIndex,
                       ConditionCursorContext *context) {
    if (!Contains(node.range, cursor)) return false;

    if (node.kind == ConditionSyntaxKind::Call) {
        const ConditionLanguageFunction *const called =
                FindConditionFunction(node.text);
        for (std::size_t index = 0u; index < node.children.size(); ++index) {
            const auto &argument = node.children[index];
            if (FindCursorContext(*argument,
                                  source,
                                  cursor,
                                  ParameterType(called, index),
                                  called,
                                  index,
                                  context)) {
                return true;
            }
        }
    }

    if (node.kind == ConditionSyntaxKind::Member &&
        Contains(node.segmentRange, cursor)) {
        context->site = expected == ConditionLanguageValueType::ExternalName
                ? ConditionCursorSite::ExternalName
                : ConditionCursorSite::Member;
        context->replacement = node.segmentRange;
        context->receiver = ConditionSyntaxName(*node.children.front());
        const std::size_t fragmentEnd =
                std::min(cursor, node.segmentRange.end);
        if (fragmentEnd >= node.segmentRange.begin) {
            context->fragment = std::string(source.substr(
                    node.segmentRange.begin,
                    fragmentEnd - node.segmentRange.begin));
        }
        context->expected = expected;
        context->functionName = function == nullptr
                ? std::string{}
                : std::string(function->canonicalName);
        context->argumentIndex = argumentIndex;
        context->automaticTrigger =
                node.segmentRange.begin == node.segmentRange.end;
        return true;
    }

    if ((node.kind == ConditionSyntaxKind::Name ||
         node.kind == ConditionSyntaxKind::String) &&
        Contains(node.segmentRange, cursor)) {
        context->site = expected == ConditionLanguageValueType::ExternalName
                ? ConditionCursorSite::ExternalName
                : ConditionCursorSite::Expression;
        context->replacement = node.segmentRange;
        const std::size_t fragmentEnd =
                std::min(cursor, node.segmentRange.end);
        if (fragmentEnd >= node.segmentRange.begin) {
            context->fragment = std::string(source.substr(
                    node.segmentRange.begin,
                    fragmentEnd - node.segmentRange.begin));
        }
        context->expected = expected;
        context->functionName = function == nullptr
                ? std::string{}
                : std::string(function->canonicalName);
        context->argumentIndex = argumentIndex;
        return true;
    }

    for (const auto &child : node.children) {
        ConditionLanguageValueType childExpected = expected;
        if (node.kind == ConditionSyntaxKind::Comparison ||
            node.kind == ConditionSyntaxKind::Add ||
            node.kind == ConditionSyntaxKind::Subtract ||
            node.kind == ConditionSyntaxKind::Multiply ||
            node.kind == ConditionSyntaxKind::Divide) {
            childExpected = ConditionLanguageValueType::Scalar;
        } else if (node.kind == ConditionSyntaxKind::Vector) {
            childExpected = ConditionLanguageValueType::Scalar;
        }
        if (FindCursorContext(*child,
                              source,
                              cursor,
                              childExpected,
                              function,
                              argumentIndex,
                              context)) {
            return true;
        }
    }

    if (node.kind == ConditionSyntaxKind::Missing) {
        context->site = expected == ConditionLanguageValueType::ExternalName
                ? ConditionCursorSite::ExternalName
                : function == nullptr ? ConditionCursorSite::Expression
                                      : ConditionCursorSite::FunctionArgument;
        context->replacement = node.range;
        context->expected = expected;
        context->functionName = function == nullptr
                ? std::string{}
                : std::string(function->canonicalName);
        context->argumentIndex = argumentIndex;
        context->automaticTrigger = function != nullptr;
        return true;
    }
    return false;
}

}  // namespace

ConditionParsedLine ParseConditionLine(std::string_view source,
                                       std::size_t baseOffset) {
    return SyntaxParser(source, baseOffset).Parse();
}

std::string ConditionSyntaxName(const ConditionSyntaxNode &node) {
    if (node.kind == ConditionSyntaxKind::Name ||
        node.kind == ConditionSyntaxKind::String) {
        return node.text;
    }
    if (node.kind == ConditionSyntaxKind::Member && !node.children.empty()) {
        const std::string receiver = ConditionSyntaxName(*node.children.front());
        return receiver.empty() ? node.text : receiver + "." + node.text;
    }
    return {};
}

std::optional<ConditionCursorContext> AnalyzeConditionCursor(
        std::string_view source,
        std::size_t cursorByteOffset) {
    const std::size_t cursor = std::min(cursorByteOffset, source.size());
    std::size_t lineBegin = cursor;
    while (lineBegin > 0u && source[lineBegin - 1u] != '\n') --lineBegin;
    std::size_t lineEnd = cursor;
    while (lineEnd < source.size() && source[lineEnd] != '\n' &&
           source[lineEnd] != '\r') {
        ++lineEnd;
    }
    const std::string_view line =
            source.substr(lineBegin, lineEnd - lineBegin);
    const auto firstContent = std::find_if(
            line.begin(), line.end(), [](unsigned char value) {
                return std::isspace(value) == 0;
            });
    if (firstContent != line.end() &&
        std::distance(firstContent, line.end()) >= 2 &&
        firstContent[0] == '/' && firstContent[1] == '/') {
        return std::nullopt;
    }

    const ConditionParsedLine parsed = ParseConditionLine(line, lineBegin);
    if (!parsed.root) return std::nullopt;
    ConditionCursorContext context;
    if (!FindCursorContext(*parsed.root,
                           source,
                           cursor,
                           ConditionLanguageValueType::Scalar,
                           nullptr,
                           0u,
                           &context)) {
        return std::nullopt;
    }
    return context;
}

}  // namespace forevertas
