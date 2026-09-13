#include "blocks/block_program_io.h"

#include "blocks/block_catalog.h"
#include "blocks/block_expression.h"
#include "blocks/block_value.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <memory>
#include <sstream>

namespace forevertas::blocks {
namespace {

const BlockDefinition *RequireBlock(const std::string &id) {
    return FindBlock(id);
}

std::string JsonString(const std::string &text) {
    std::string quoted = "\"";
    for (const char character : text) {
        if (character == '"' || character == '\\') quoted += '\\';
        quoted += character;
    }
    return quoted + "\"";
}

bool IsValidBareword(const std::string &text) {
    if (text.empty()) return false;
    if (std::isdigit(static_cast<unsigned char>(text.front()))) return false;
    for (const char character : text) {
        const unsigned char raw = static_cast<unsigned char>(character);
        if (!std::isalnum(raw) && character != '_' && character != '.' &&
            character != '-') {
            return false;
        }
    }
    // Words the text parser treats as structure would be misparsed as
    // slot values; they must print quoted.
    if (text == "evaluate" || text == "mutate" || text == "op" ||
        text == "min" || text == "max") {
        return false;
    }
    return true;
}

std::string LocalId(const BlockDefinition &definition) {
    const auto separator = definition.id.find('/');
    return separator == std::string::npos
            ? definition.id
            : definition.id.substr(separator + 1);
}

// ---------------------------------------------------------------------------
// Text interchange: tokenizer
// ---------------------------------------------------------------------------

struct TextToken {
    enum class Kind { Word, Number, String, Symbol, End };

    Kind kind = Kind::End;
    std::string text;
    std::size_t line = 1;
    std::size_t column = 1;
};

class TextLexer final {
public:
    explicit TextLexer(const std::string &source) : source_(source) {}

    TextToken next() {
        skipSpaceAndComments();
        TextToken token;
        token.line = line_;
        token.column = column_;
        if (position_ >= source_.size()) return token;
        const char character = source_[position_];
        if (character == '"' || character == '\'') return lexString(character);
        if (std::isdigit(static_cast<unsigned char>(character)) ||
            (character == '.' && position_ + 1 < source_.size() &&
             std::isdigit(static_cast<unsigned char>(
                     source_[position_ + 1])))) {
            return lexNumber();
        }
        if (std::isalpha(static_cast<unsigned char>(character)) ||
            character == '_') {
            return lexWord();
        }
        advance();
        token.kind = TextToken::Kind::Symbol;
        token.text = std::string(1, character);
        return token;
    }

private:
    void advance() {
        if (source_[position_] == '\n') {
            ++line_;
            column_ = 1;
        } else {
            ++column_;
        }
        ++position_;
    }

    void skipSpaceAndComments() {
        while (position_ < source_.size()) {
            const char character = source_[position_];
            if (std::isspace(static_cast<unsigned char>(character))) {
                advance();
            } else if (character == '#') {
                while (position_ < source_.size() &&
                       source_[position_] != '\n') {
                    advance();
                }
            } else {
                break;
            }
        }
    }

    TextToken lexString(char quote) {
        TextToken token;
        token.line = line_;
        token.column = column_;
        token.kind = TextToken::Kind::String;
        advance();
        while (position_ < source_.size() && source_[position_] != quote) {
            if (source_[position_] == '\\' && position_ + 1 < source_.size()) {
                advance();
                token.text += source_[position_] == 'n'
                        ? '\n'
                        : source_[position_];
            } else {
                token.text += source_[position_];
            }
            advance();
        }
        if (position_ >= source_.size()) {
            token.kind = TextToken::Kind::End;
            return token;
        }
        advance();
        return token;
    }

    TextToken lexNumber() {
        TextToken token;
        token.line = line_;
        token.column = column_;
        token.kind = TextToken::Kind::Number;
        while (position_ < source_.size()) {
            const char character = source_[position_];
            if (std::isdigit(static_cast<unsigned char>(character)) ||
                character == '.' || character == 'e' || character == 'E' ||
                ((character == '+' || character == '-') &&
                 !token.text.empty() &&
                 (token.text.back() == 'e' || token.text.back() == 'E'))) {
                token.text += character;
                advance();
            } else {
                break;
            }
        }
        return token;
    }

    TextToken lexWord() {
        TextToken token;
        token.line = line_;
        token.column = column_;
        token.kind = TextToken::Kind::Word;
        while (position_ < source_.size()) {
            const char character = source_[position_];
            if (std::isalnum(static_cast<unsigned char>(character)) ||
                character == '_' || character == '.' || character == '-') {
                token.text += character;
                advance();
            } else {
                break;
            }
        }
        return token;
    }

    const std::string &source_;
    std::size_t position_ = 0;
    std::size_t line_ = 1;
    std::size_t column_ = 1;
};

// ---------------------------------------------------------------------------
// Text interchange: expression terms and parser
// ---------------------------------------------------------------------------

struct ExpressionTerm {
    enum class Op { Literal, Add, Subtract, Multiply, Divide, Min, Max };

    Op op = Op::Literal;
    std::string literal;
    std::shared_ptr<ExpressionTerm> left;
    std::shared_ptr<ExpressionTerm> right;
};

using ExpressionPointer = std::shared_ptr<ExpressionTerm>;

class TextParser final {
public:
    explicit TextParser(const std::string &source)
        : lexer_(source), program_(std::make_unique<BlockProgram>()) {
        advance();
    }

    BlockProgramText parse() {
        BlockProgramText result;
        BlockId script = 0;
        while (current_.kind != TextToken::Kind::End && error_.empty()) {
            if (!expectWord("search")) break;
            if (script != 0) {
                fail("only one search block is allowed");
                break;
            }
            script = parseSearch();
        }
        if (error_.empty() && script == 0) fail("expected a search block");
        if (error_.empty()) {
            program_->setScript(script);
            result.program = std::move(*program_);
        } else {
            result.error = error_;
        }
        return result;
    }

private:
    void advance() { current_ = lexer_.next(); }

    void fail(const std::string &message) {
        std::ostringstream stream;
        stream << "line " << current_.line << ", column " << current_.column
               << ": " << message;
        error_ = stream.str();
    }

    bool expectWord(const std::string &word) {
        if (error_.empty() && (current_.kind != TextToken::Kind::Word ||
                               current_.text != word)) {
            fail("expected '" + word + "'");
        }
        const bool ok = error_.empty();
        if (ok) advance();
        return ok;
    }

    bool expectSymbol(char symbol) {
        if (error_.empty() &&
            (current_.kind != TextToken::Kind::Symbol ||
             current_.text != std::string(1, symbol))) {
            fail(std::string("expected '") + symbol + "'");
        }
        const bool ok = error_.empty();
        if (ok) advance();
        return ok;
    }

    std::string expectOptionId(const std::string &description) {
        if (error_.empty() && current_.kind != TextToken::Kind::Word) {
            fail("expected " + description + " id");
        }
        if (!error_.empty()) return {};
        const std::string optionId = current_.text;
        advance();
        return optionId;
    }

    static bool isSectionWord(const std::string &word) {
        return word == "evaluate" || word == "mutate" || word == "op";
    }

    BlockId createBlockFromDefinition(const std::string &definitionId,
                                      const std::string &description) {
        if (RequireBlock(definitionId) == nullptr) {
            fail("unknown " + description);
            return 0;
        }
        return program_->createBlock(definitionId);
    }

    BlockId parseSearch() {
        const std::string optionId = expectOptionId("search block");
        if (!error_.empty()) return 0;
        const std::string definitionId = "search/" + optionId;
        const BlockDefinition *const definition = RequireBlock(definitionId);
        if (definition == nullptr || definition->shape != BlockShape::Hat) {
            fail("unknown search block '" + optionId + "'");
            return 0;
        }
        const BlockId hat = createBlockFromDefinition(
                definitionId, "search block '" + optionId + "'");
        if (hat == 0) return 0;
        if (!expectSymbol('{')) return 0;
        while (error_.empty() &&
               !(current_.kind == TextToken::Kind::Symbol &&
                 current_.text == "}")) {
            if (current_.kind != TextToken::Kind::Word) {
                fail("expected 'evaluate' or 'mutate'");
                break;
            }
            if (!isSectionWord(current_.text)) {
                parseSettings(hat, "search block");
                continue;
            }
            if (current_.text == "evaluate") {
                advance();
                if (!parseEvaluate(hat)) break;
            } else if (current_.text == "mutate") {
                advance();
                if (!parseMutate(hat)) break;
            } else {
                fail("expected 'evaluate' or 'mutate'");
            }
        }
        if (!expectSymbol('}')) return 0;
        return error_.empty() ? hat : 0;
    }

    bool parseEvaluate(BlockId hat) {
        const std::string localId = expectOptionId("evaluation block");
        if (!error_.empty()) return false;
        const std::string definitionId = "evaluate/" + localId;
        const BlockDefinition *const definition = RequireBlock(definitionId);
        if (definition == nullptr || definition->optionKind != "evaluation") {
            fail("unknown evaluation block '" + localId + "'");
            return false;
        }
        const BlockId evaluator = createBlockFromDefinition(
                definitionId, "evaluation block '" + localId + "'");
        if (evaluator == 0) return false;
        if (!expectSymbol('{')) return false;
        parseSettings(evaluator, "evaluation block");
        if (!error_.empty()) return false;
        if (!expectSymbol('}')) return false;
        program_->setEvaluator(hat, evaluator);
        return true;
    }

    bool parseMutate(BlockId hat) {
        const std::string localId = expectOptionId("mutation block");
        if (!error_.empty()) return false;
        if (localId != "window") {
            fail("unknown mutator block '" + localId + "'");
            return false;
        }
        const BlockId group = createBlockFromDefinition(
                "mutate/window", "mutation window");
        if (group == 0) return false;
        if (!expectSymbol('{')) return false;
        while (error_.empty() &&
               !(current_.kind == TextToken::Kind::Symbol &&
                 current_.text == "}")) {
            if (current_.kind == TextToken::Kind::Word &&
                current_.text == "op") {
                advance();
                if (!parseOp(group)) break;
                continue;
            }
            if (current_.kind == TextToken::Kind::Word) {
                parseSettings(group, "mutation window");
                continue;
            }
            fail("expected a setting or 'op'");
            break;
        }
        if (!expectSymbol('}')) return false;
        if (error_.empty()) program_->appendToSubstack(hat, group);
        return error_.empty();
    }

    bool parseOp(BlockId group) {
        const std::string localId = expectOptionId("mutation block");
        if (!error_.empty()) return false;
        const BlockDefinition *const definition =
                RequireBlock("mutate/" + localId);
        if (definition == nullptr || definition->shape != BlockShape::Stack ||
            definition->optionKind != "mutation") {
            fail("unknown mutation block '" + localId + "'");
            return false;
        }
        const BlockId atom = program_->createBlock(definition->id);
        if (!expectSymbol('{')) return false;
        parseSettings(atom, "mutation block");
        if (!error_.empty()) return false;
        if (!expectSymbol('}')) return false;
        program_->appendToSubstack(group, atom);
        return true;
    }

    void parseSettings(BlockId block, const std::string &kindWord) {
        const BlockNode *const node = program_->find(block);
        while (error_.empty() && current_.kind == TextToken::Kind::Word &&
               !isSectionWord(current_.text)) {
            const std::string key = current_.text;
            if (node->fields.find(key) == node->fields.end()) {
                fail("unknown setting '" + key + "' for " + kindWord);
                break;
            }
            advance();
            if (!expectSymbol('=')) break;
            if (error_.empty() &&
                (current_.kind == TextToken::Kind::Number ||
                 current_.kind == TextToken::Kind::Symbol ||
                 (current_.kind == TextToken::Kind::Word &&
                  (current_.text == "min" || current_.text == "max")))) {
                const ExpressionPointer expression = parseExpression();
                if (error_.empty()) {
                    applyExpression(block, key, expression);
                }
                continue;
            }
            if (error_.empty() &&
                (current_.kind == TextToken::Kind::String ||
                 current_.kind == TextToken::Kind::Word)) {
                program_->setFieldValue(block, key, current_.text);
                advance();
                continue;
            }
            if (error_.empty()) fail("expected a value");
        }
    }

    void applyExpression(BlockId block,
                         const std::string &key,
                         const ExpressionPointer &expression) {
        const BlockId reporter = buildReporter(expression);
        if (reporter != 0) {
            program_->graftReporter(block, key, reporter);
        } else {
            program_->setFieldValue(block, key, expression->literal);
        }
    }

    // Builds the reporter tree bottom-up. Returns 0 when the expression
    // collapsed to a single literal.
    BlockId buildReporter(const ExpressionPointer &expression) {
        if (expression->op == ExpressionTerm::Op::Literal) return 0;
        const char *definition = nullptr;
        switch (expression->op) {
        case ExpressionTerm::Op::Add: definition = "values/add"; break;
        case ExpressionTerm::Op::Subtract:
            definition = "values/subtract";
            break;
        case ExpressionTerm::Op::Multiply:
            definition = "values/multiply";
            break;
        case ExpressionTerm::Op::Divide: definition = "values/divide"; break;
        case ExpressionTerm::Op::Min: definition = "values/minimum"; break;
        case ExpressionTerm::Op::Max: definition = "values/maximum"; break;
        case ExpressionTerm::Op::Literal: return 0;
        }
        const BlockId left = buildReporter(expression->left);
        const BlockId right = buildReporter(expression->right);
        std::map<std::string, std::string> literals;
        if (left == 0) literals.emplace("left", expression->left->literal);
        if (right == 0) literals.emplace("right", expression->right->literal);
        const BlockId node = program_->createBlock(definition, literals);
        if (left != 0) program_->graftReporter(node, "left", left);
        if (right != 0) program_->graftReporter(node, "right", right);
        return node;
    }

    ExpressionPointer parseExpression() { return parseSum(); }

    ExpressionPointer parseSum() {
        ExpressionPointer left = parseProduct();
        while (error_.empty() && current_.kind == TextToken::Kind::Symbol &&
               (current_.text == "+" || current_.text == "-")) {
            const bool add = current_.text == "+";
            advance();
            ExpressionPointer right = parseProduct();
            if (error_.empty()) {
                left = fold(add ? ExpressionTerm::Op::Add
                               : ExpressionTerm::Op::Subtract,
                            left,
                            right);
            }
        }
        return left;
    }

    ExpressionPointer parseProduct() {
        ExpressionPointer left = parseUnary();
        while (error_.empty() && current_.kind == TextToken::Kind::Symbol &&
               (current_.text == "*" || current_.text == "/")) {
            const bool multiply = current_.text == "*";
            advance();
            ExpressionPointer right = parseUnary();
            if (error_.empty()) {
                left = fold(multiply ? ExpressionTerm::Op::Multiply
                                     : ExpressionTerm::Op::Divide,
                            left,
                            right);
            }
        }
        return left;
    }

    ExpressionPointer parseUnary() {
        if (error_.empty() && current_.kind == TextToken::Kind::Symbol &&
            current_.text == "-") {
            advance();
            ExpressionPointer operand = parseUnary();
            if (!error_.empty()) return operand;
            if (operand->op == ExpressionTerm::Op::Literal) {
                if (const auto value = ParseNumberValue(operand->literal)) {
                    return literal(FormatNumberValue(-*value));
                }
            }
            return fold(ExpressionTerm::Op::Subtract, literal("0"), operand);
        }
        return parsePrimary();
    }

    ExpressionPointer parsePrimary() {
        if (error_.empty() && current_.kind == TextToken::Kind::Word &&
            (current_.text == "min" || current_.text == "max")) {
            const bool isMin = current_.text == "min";
            advance();
            if (!expectSymbol('(')) return {};
            ExpressionPointer left = parseSum();
            if (!expectSymbol(',')) return {};
            ExpressionPointer right = parseSum();
            if (!expectSymbol(')')) return {};
            return fold(isMin ? ExpressionTerm::Op::Min
                              : ExpressionTerm::Op::Max,
                        left,
                        right);
        }
        if (error_.empty() && current_.kind == TextToken::Kind::Symbol &&
            current_.text == "(") {
            advance();
            ExpressionPointer expression = parseSum();
            if (!expectSymbol(')')) return {};
            return expression;
        }
        if (error_.empty() &&
            (current_.kind == TextToken::Kind::Number ||
             current_.kind == TextToken::Kind::String)) {
            const std::string text = current_.text;
            advance();
            return literal(text);
        }
        if (error_.empty()) fail("expected a value");
        return {};
    }

    ExpressionPointer literal(const std::string &text) {
        auto term = std::make_shared<ExpressionTerm>();
        term->op = ExpressionTerm::Op::Literal;
        term->literal = text;
        return term;
    }

    ExpressionPointer fold(ExpressionTerm::Op op,
                           const ExpressionPointer &left,
                           const ExpressionPointer &right) {
        auto term = std::make_shared<ExpressionTerm>();
        term->op = op;
        term->left = left;
        term->right = right;
        return term;
    }

    TextLexer lexer_;
    TextToken current_;
    std::unique_ptr<BlockProgram> program_;
    std::string error_;
};

// ---------------------------------------------------------------------------
// Text interchange: printer
// ---------------------------------------------------------------------------

std::string PrintSlotValue(const BlockProgram &program,
                           const BlockNode &node,
                           const std::string &key);

std::string PrintExpression(const BlockProgram &program, BlockId reporterId) {
    const BlockNode *const node = program.find(reporterId);
    if (node == nullptr) return "0";
    if (node->definitionId == "values/number") {
        return PrintSlotValue(program, *node, "value");
    }
    const std::string left = PrintSlotValue(program, *node, "left");
    const std::string right = PrintSlotValue(program, *node, "right");
    if (node->definitionId == "values/add")
        return "(" + left + " + " + right + ")";
    if (node->definitionId == "values/subtract")
        return "(" + left + " - " + right + ")";
    if (node->definitionId == "values/multiply")
        return "(" + left + " * " + right + ")";
    if (node->definitionId == "values/divide")
        return "(" + left + " / " + right + ")";
    if (node->definitionId == "values/minimum")
        return "min(" + left + ", " + right + ")";
    if (node->definitionId == "values/maximum")
        return "max(" + left + ", " + right + ")";
    return left;
}

std::string PrintSlotValue(const BlockProgram &program,
                           const BlockNode &node,
                           const std::string &key) {
    const auto found = node.reporters.find(key);
    if (found != node.reporters.end()) {
        return PrintExpression(program, found->second);
    }
    const auto literalEntry = node.fields.find(key);
    const std::string value =
            literalEntry == node.fields.end() ? std::string()
                                              : literalEntry->second;
    if (ParseNumberValue(value)) return value;
    if (IsValidBareword(value)) return value;
    return JsonString(value);
}

}  // namespace

BlockProgramText ParseBlockProgramText(const std::string &text) {
    TextParser parser(text);
    return parser.parse();
}

std::string PrintBlockProgramText(const BlockProgram &program) {
    if (!program.script()) return {};
    const BlockNode *const hat = program.find(*program.script());
    if (hat == nullptr) return {};
    const BlockDefinition *const hatDefinition = RequireBlock(hat->definitionId);
    if (hatDefinition == nullptr) return {};
    std::ostringstream stream;
    stream << "search " << LocalId(*hatDefinition) << " {\n";
    for (const OptionField &field : hatDefinition->fields) {
        stream << "  " << field.key << " = "
               << PrintSlotValue(program, *hat, field.key) << "\n";
    }
    if (hat->evaluator != 0) {
        const BlockNode *const evaluator = program.find(hat->evaluator);
        const BlockDefinition *const evaluatorDefinition =
                evaluator == nullptr
                        ? nullptr
                        : RequireBlock(evaluator->definitionId);
        if (evaluatorDefinition != nullptr) {
            stream << "  evaluate " << LocalId(*evaluatorDefinition)
                   << " {\n";
            for (const OptionField &field : evaluatorDefinition->fields) {
                stream << "    " << field.key << " = "
                       << PrintSlotValue(program, *evaluator, field.key)
                       << "\n";
            }
            stream << "  }\n";
        }
    }
    for (const BlockId windowId : hat->substack) {
        const BlockNode *const window = program.find(windowId);
        const BlockDefinition *const windowDefinition =
                window == nullptr ? nullptr
                                  : RequireBlock(window->definitionId);
        if (windowDefinition == nullptr ||
            windowDefinition->shape != BlockShape::Container) {
            continue;
        }
        stream << "  mutate window {\n";
        for (const OptionField &field : windowDefinition->fields) {
            stream << "    " << field.key << " = "
                   << PrintSlotValue(program, *window, field.key) << "\n";
        }
        for (const BlockId atomId : window->substack) {
            const BlockNode *const atom = program.find(atomId);
            const BlockDefinition *const atomDefinition =
                    atom == nullptr ? nullptr
                                    : RequireBlock(atom->definitionId);
            if (atomDefinition == nullptr) continue;
            if (atomDefinition->fields.empty()) {
                stream << "    op " << LocalId(*atomDefinition) << " {}\n";
                continue;
            }
            stream << "    op " << LocalId(*atomDefinition) << " {\n";
            for (const OptionField &field : atomDefinition->fields) {
                stream << "      " << field.key << " = "
                       << PrintSlotValue(program, *atom, field.key) << "\n";
            }
            stream << "    }\n";
        }
        stream << "  }\n";
    }
    stream << "}\n";
    return stream.str();
}

// ---------------------------------------------------------------------------
// JSON persistence
// ---------------------------------------------------------------------------

namespace {

struct JsonValue {
    enum class Kind { String, Number, Object, Array };

    Kind kind = Kind::String;
    std::string text;
    double number = 0.0;
    std::vector<std::pair<std::string, JsonValue>> members;
    std::vector<JsonValue> items;

    const JsonValue *member(const std::string &key) const {
        for (const auto &[name, value] : members) {
            if (name == key) return &value;
        }
        return nullptr;
    }
};

class JsonParser final {
public:
    explicit JsonParser(const std::string &source) : source_(source) {}

    bool parse(JsonValue &value) {
        skipSpace();
        return parseValue(value) && position_ >= source_.size();
    }

    const std::string &error() const { return error_; }

private:
    void skipSpace() {
        while (position_ < source_.size() &&
               std::isspace(static_cast<unsigned char>(source_[position_]))) {
            ++position_;
        }
    }

    bool consume(char character) {
        skipSpace();
        if (position_ < source_.size() && source_[position_] == character) {
            ++position_;
            return true;
        }
        return false;
    }

    bool peek(char character) {
        skipSpace();
        return position_ < source_.size() && source_[position_] == character;
    }

    bool fail(const std::string &message) {
        if (error_.empty()) error_ = message;
        return false;
    }

    bool parseValue(JsonValue &value) {
        if (!error_.empty()) return false;
        if (peek('{')) return parseObject(value);
        if (peek('[')) return parseArray(value);
        if (peek('"')) {
            value.kind = JsonValue::Kind::String;
            value.text = parseString();
            return !error().empty() ? false : true;
        }
        return parseNumber(value);
    }

    bool parseObject(JsonValue &value) {
        value.kind = JsonValue::Kind::Object;
        if (!consume('{')) return fail("expected '{'");
        if (consume('}')) return true;
        while (true) {
            skipSpace();
            if (!peek('"')) return fail("expected an object key");
            std::string key = parseString();
            if (!error().empty()) return false;
            if (!consume(':')) return fail("expected ':'");
            JsonValue member;
            if (!parseValue(member)) return false;
            value.members.emplace_back(std::move(key), std::move(member));
            if (consume(',')) continue;
            return consume('}') || fail("expected '}'");
        }
    }

    bool parseArray(JsonValue &value) {
        value.kind = JsonValue::Kind::Array;
        if (!consume('[')) return fail("expected '['");
        if (consume(']')) return true;
        while (true) {
            JsonValue item;
            if (!parseValue(item)) return false;
            value.items.push_back(std::move(item));
            if (consume(',')) continue;
            return consume(']') || fail("expected ']'");
        }
    }

    std::string parseString() {
        ++position_;
        std::string value;
        while (position_ < source_.size() && source_[position_] != '"') {
            if (source_[position_] == '\\' && position_ + 1 < source_.size()) {
                ++position_;
                switch (source_[position_]) {
                case 'n': value += '\n'; break;
                case 't': value += '\t'; break;
                case 'r': value += '\r'; break;
                default: value += source_[position_]; break;
                }
            } else {
                value += source_[position_];
            }
            ++position_;
        }
        if (position_ >= source_.size()) {
            error_ = "unterminated string";
            return {};
        }
        ++position_;
        return value;
    }

    bool parseNumber(JsonValue &value) {
        value.kind = JsonValue::Kind::Number;
        const std::size_t start = position_;
        while (position_ < source_.size()) {
            const char character = source_[position_];
            if (std::isdigit(static_cast<unsigned char>(character)) ||
                character == '-' || character == '+' || character == '.' ||
                character == 'e' || character == 'E') {
                ++position_;
            } else {
                break;
            }
        }
        value.text = source_.substr(start, position_ - start);
        if (!ParseNumberValue(value.text)) {
            return fail("invalid number");
        }
        value.number = *ParseNumberValue(value.text);
        return true;
    }

    const std::string &source_;
    std::size_t position_ = 0;
    std::string error_;
};

// Raw JSON node parsed before catalog validation.
struct RawNode {
    BlockId id = 0;
    std::string definitionId;
    double x = 0.0;
    double y = 0.0;
    std::map<std::string, std::string> fields;
    std::map<std::string, BlockId> reporters;
    std::vector<BlockId> substack;
    BlockId evaluator = 0;
};

bool ParseRawNodes(const JsonValue &document, std::vector<RawNode> &nodes,
                   std::string &error) {
    const JsonValue *const blocksValue = document.member("blocks");
    if (blocksValue == nullptr || blocksValue->kind != JsonValue::Kind::Array) {
        error = "program document has no blocks array";
        return false;
    }
    for (const JsonValue &blockValue : blocksValue->items) {
        if (blockValue.kind != JsonValue::Kind::Object) {
            error = "block entries must be objects";
            return false;
        }
        const JsonValue *const idValue = blockValue.member("id");
        const JsonValue *const defValue = blockValue.member("def");
        if (idValue == nullptr || defValue == nullptr ||
            idValue->kind != JsonValue::Kind::Number ||
            defValue->kind != JsonValue::Kind::String ||
            idValue->number <= 0.0) {
            error = "block entry is missing id or def";
            return false;
        }
        RawNode node;
        node.id = static_cast<BlockId>(idValue->number);
        node.definitionId = defValue->text;
        if (const JsonValue *const xValue = blockValue.member("x");
            xValue != nullptr && xValue->kind == JsonValue::Kind::Number) {
            node.x = xValue->number;
        }
        if (const JsonValue *const yValue = blockValue.member("y");
            yValue != nullptr && yValue->kind == JsonValue::Kind::Number) {
            node.y = yValue->number;
        }
        if (const JsonValue *const fieldsValue = blockValue.member("fields");
            fieldsValue != nullptr &&
            fieldsValue->kind == JsonValue::Kind::Object) {
            for (const auto &[key, value] : fieldsValue->members) {
                if (value.kind != JsonValue::Kind::String) continue;
                node.fields.erase(key);
                node.fields.emplace(key, value.text);
            }
        }
        if (const JsonValue *const reportersValue =
                    blockValue.member("reporters");
            reportersValue != nullptr &&
            reportersValue->kind == JsonValue::Kind::Object) {
            for (const auto &[key, value] : reportersValue->members) {
                if (value.kind != JsonValue::Kind::Number) continue;
                node.reporters.erase(key);
                node.reporters.emplace(
                        key, static_cast<BlockId>(value.number));
            }
        }
        if (const JsonValue *const substackValue =
                    blockValue.member("substack");
            substackValue != nullptr &&
            substackValue->kind == JsonValue::Kind::Array) {
            for (const JsonValue &item : substackValue->items) {
                if (item.kind != JsonValue::Kind::Number) continue;
                node.substack.push_back(static_cast<BlockId>(item.number));
            }
        }
        if (const JsonValue *const evaluatorValue =
                    blockValue.member("evaluator");
            evaluatorValue != nullptr &&
            evaluatorValue->kind == JsonValue::Kind::Number) {
            node.evaluator = static_cast<BlockId>(evaluatorValue->number);
        }
        nodes.push_back(std::move(node));
    }
    return true;
}


}  // namespace

BlockProgramJson ParseBlockProgramJson(const std::string &json) {
    BlockProgramJson result;
    JsonParser parser(json);
    JsonValue document;
    if (!parser.parse(document)) {
        result.error = parser.error().empty() ? "invalid program document"
                                              : parser.error();
        return result;
    }
    if (document.kind != JsonValue::Kind::Object) {
        result.error = "program document must be an object";
        return result;
    }
    const JsonValue *const versionValue = document.member("version");
    if (versionValue == nullptr || versionValue->kind != JsonValue::Kind::Number ||
        versionValue->number != 3.0) {
        result.error = "unsupported program version";
        return result;
    }

    std::vector<RawNode> raw;
    if (!ParseRawNodes(document, raw, result.error)) {
        return result;
    }

    BlockProgram program;
    for (const RawNode &node : raw) {
        const BlockDefinition *const definition =
                RequireBlock(node.definitionId);
        if (definition == nullptr) {
            result.error =
                    "unknown block definition '" + node.definitionId + "'";
            return result;
        }
        BlockNode adopted;
        adopted.id = node.id;
        adopted.definitionId = node.definitionId;
        adopted.x = node.x;
        adopted.y = node.y;
        for (const OptionField &field : definition->fields) {
            adopted.fields.emplace(field.key, field.defaultValue);
        }
        for (const auto &[key, value] : node.fields) {
            if (adopted.fields.find(key) == adopted.fields.end()) continue;
            adopted.fields.erase(key);
            adopted.fields.emplace(key, value);
        }
        for (const auto &[key, value] : node.reporters) {
            bool numberField = false;
            for (const OptionField &field : definition->fields) {
                if (field.key == key &&
                    field.kind == OptionField::Kind::Number) {
                    numberField = true;
                }
            }
            if (numberField) adopted.reporters.emplace(key, value);
        }
        if (definition->shape == BlockShape::Hat ||
            definition->shape == BlockShape::Container) {
            adopted.substack = node.substack;
        }
        if (definition->shape == BlockShape::Hat) {
            adopted.evaluator = node.evaluator;
        }
        program.adoptNode(std::move(adopted));
    }
    if (const JsonValue *const looseValue = document.member("loose");
        looseValue != nullptr && looseValue->kind == JsonValue::Kind::Array) {
        for (const JsonValue &item : looseValue->items) {
            if (item.kind != JsonValue::Kind::Number) continue;
            if (program.find(static_cast<BlockId>(item.number)) != nullptr) {
                program.makeTopLevel(static_cast<BlockId>(item.number));
            }
        }
    }
    if (const JsonValue *const scriptValue = document.member("script");
        scriptValue != nullptr && scriptValue->kind == JsonValue::Kind::Number &&
        scriptValue->number > 0.0 &&
        program.find(static_cast<BlockId>(scriptValue->number)) != nullptr) {
        program.setScript(static_cast<BlockId>(scriptValue->number));
    }
    if (const JsonValue *const rememberedValue =
                document.member("remembered");
        rememberedValue != nullptr &&
        rememberedValue->kind == JsonValue::Kind::Object) {
        for (const auto &[name, value] : rememberedValue->members) {
            if (value.kind != JsonValue::Kind::Object) continue;
            for (const auto &[key, entry] : value.members) {
                if (entry.kind != JsonValue::Kind::String) continue;
                result.remembered[name][key] = entry.text;
            }
        }
    }
    program.collectGarbage();
    result.program = std::move(program);
    return result;
}

std::string PrintBlockProgramJson(
        const BlockProgram &program,
        const std::map<std::string, std::map<std::string, std::string>>
                &remembered) {
    std::ostringstream stream;
    stream << "{\"version\":3";
    if (program.script()) {
        stream << ",\"script\":" << *program.script();
    }
    stream << ",\"blocks\":[";
    bool firstBlock = true;
    for (const auto &[id, node] : program.nodes()) {
        if (!firstBlock) stream << ",";
        firstBlock = false;
        stream << "{\"id\":" << id << ",\"def\":" << JsonString(node.definitionId);
        if (node.x != 0.0 || node.y != 0.0) {
            stream << ",\"x\":" << FormatNumberValue(node.x)
                   << ",\"y\":" << FormatNumberValue(node.y);
        }
        if (!node.fields.empty()) {
            stream << ",\"fields\":{";
            bool first = true;
            for (const auto &[key, value] : node.fields) {
                if (!first) stream << ",";
                first = false;
                stream << JsonString(key) << ":" << JsonString(value);
            }
            stream << "}";
        }
        if (!node.reporters.empty()) {
            stream << ",\"reporters\":{";
            bool first = true;
            for (const auto &[key, value] : node.reporters) {
                if (!first) stream << ",";
                first = false;
                stream << JsonString(key) << ":" << value;
            }
            stream << "}";
        }
        if (!node.substack.empty()) {
            stream << ",\"substack\":[";
            bool first = true;
            for (const BlockId child : node.substack) {
                if (!first) stream << ",";
                first = false;
                stream << child;
            }
            stream << "]";
        }
        if (node.evaluator != 0) {
            stream << ",\"evaluator\":" << node.evaluator;
        }
        stream << "}";
    }
    stream << "]";
    if (!program.topLevel().empty()) {
        stream << ",\"loose\":[";
        bool first = true;
        for (const BlockId id : program.topLevel()) {
            if (!first) stream << ",";
            first = false;
            stream << id;
        }
        stream << "]";
    }
    if (!remembered.empty()) {
        stream << ",\"remembered\":{";
        bool firstOption = true;
        for (const auto &[name, values] : remembered) {
            if (values.empty()) continue;
            if (!firstOption) stream << ",";
            firstOption = false;
            stream << JsonString(name) << ":{";
            bool first = true;
            for (const auto &[key, value] : values) {
                if (!first) stream << ",";
                first = false;
                stream << JsonString(key) << ":" << JsonString(value);
            }
            stream << "}";
        }
        stream << "}";
    }
    stream << "}";
    return stream.str();
}

}  // namespace forevertas::blocks
