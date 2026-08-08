#include "conditions/condition_language_catalog.h"
#include "conditions/condition_program.h"
#include "conditions/condition_syntax.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>

namespace {

bool Check(bool condition, const char *message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

std::string CompileLineForSymbol(
        const forevertas::ConditionLanguageSymbol &symbol,
        std::string_view spelling) {
    if (symbol.external) {
        return "distance(car.pos, variable(" + std::string(spelling) +
                ")) >= 0";
    }
    if (symbol.kind == forevertas::ConditionLanguageSymbolKind::Vector) {
        return "distance(" + std::string(spelling) + ", (0,0,0)) >= 0";
    }
    return std::string(spelling) + " = 0";
}

std::string CompileLineForFunction(
        const forevertas::ConditionLanguageFunction &function,
        std::string_view spelling) {
    using forevertas::ConditionLanguageFunctionKind;
    switch (function.kind) {
    case ConditionLanguageFunctionKind::KmH:
    case ConditionLanguageFunctionKind::Deg:
        return std::string(spelling) + "(car.speed) >= 0";
    case ConditionLanguageFunctionKind::TimeSince:
        return std::string(spelling) +
                "(last_improvement.time) >= 0";
    case ConditionLanguageFunctionKind::Distance:
        return std::string(spelling) +
                "(car.pos, variable(bf_target_point)) >= 0";
    case ConditionLanguageFunctionKind::VariableLookup:
        return "distance(car.pos, " + std::string(spelling) +
                "(bf_target_point)) >= 0";
    }
    return {};
}

bool TestCatalogueCompiles() {
    const forevertas::ConditionVariables variables{{
            "bf_target_point", {1.0, 2.0, 3.0, true}}};
    bool okay = true;
    for (const forevertas::ConditionLanguageSymbol &symbol :
         forevertas::GetConditionSymbols()) {
        okay &= Check(!symbol.canonicalName.empty() &&
                              !symbol.documentation.empty() &&
                              !symbol.example.empty(),
                      "catalogue symbol is missing user-facing metadata");
        for (const std::string_view alias : symbol.aliases) {
            const auto result = forevertas::CompileConditionScript(
                    CompileLineForSymbol(symbol, alias), variables);
            if (result.error) std::cerr << *result.error << '\n';
            okay &= Check(result.program.has_value() && !result.error,
                          "catalogue symbol spelling did not compile");
        }
        const auto example = forevertas::CompileConditionScript(
                std::string(symbol.example), variables);
        if (example.error) std::cerr << *example.error << '\n';
        okay &= Check(example.program.has_value() && !example.error,
                      "catalogue symbol example did not compile");
    }

    const std::vector<forevertas::ConditionLanguageConstant> &constants =
            forevertas::GetConditionConstants();
    okay &= Check(constants.size() == 38u,
                  "typed constant catalogue lost an enum value");
    for (const forevertas::ConditionLanguageConstant &constant : constants) {
        okay &= Check(!constant.canonicalName.empty() &&
                              !constant.enumNamespace.empty() &&
                              !constant.documentation.empty() &&
                              !constant.example.empty(),
                      "catalogue constant is missing typed metadata");
        for (const std::string_view alias : constant.aliases) {
            const auto result = forevertas::CompileConditionScript(
                    std::string(alias) + " = " +
                    std::to_string(constant.value), variables);
            if (result.error) std::cerr << *result.error << '\n';
            okay &= Check(result.program.has_value() && !result.error,
                          "catalogue constant alias did not compile");
        }
        const auto example = forevertas::CompileConditionScript(
                std::string(constant.example), variables);
        if (example.error) std::cerr << *example.error << '\n';
        okay &= Check(example.program.has_value() && !example.error,
                      "catalogue constant example did not compile");
    }
    const auto *const ice =
            forevertas::FindConditionConstant("SURFACE.ICE");
    const auto *const roulette =
            forevertas::FindConditionConstant("turbo.roulette");
    const auto *const reverse =
            forevertas::FindConditionConstant("gear.reverse");
    okay &= Check(ice != nullptr && ice->value == 3.0 &&
                          roulette != nullptr && roulette->value == 2.0 &&
                          reverse != nullptr && reverse->value == -1.0,
                  "typed constants do not match runtime enum values");

    for (const forevertas::ConditionLanguageFunction &function :
         forevertas::GetConditionFunctions()) {
        okay &= Check(!function.signature.empty() &&
                              !function.documentation.empty() &&
                              !function.example.empty(),
                      "catalogue function is missing user-facing metadata");
        for (const std::string_view alias : function.aliases) {
            const auto result = forevertas::CompileConditionScript(
                    CompileLineForFunction(function, alias), variables);
            if (result.error) std::cerr << *result.error << '\n';
            okay &= Check(result.program.has_value() && !result.error,
                          "catalogue function spelling did not compile");
        }
        const auto example = forevertas::CompileConditionScript(
                std::string(function.example), variables);
        if (example.error) std::cerr << *example.error << '\n';
        okay &= Check(example.program.has_value() && !example.error,
                      "catalogue function example did not compile");
    }

    std::string uppercase = "CAR.WHEELS.FRONTLEFT.SURFACE = SURFACE.ICE";
    const auto caseInsensitive =
            forevertas::CompileConditionScript(uppercase, variables);
    okay &= Check(caseInsensitive.program.has_value() &&
                          !caseInsensitive.error,
                  "condition names stopped being case insensitive");
    return okay;
}

bool TestStructuredDiagnostics() {
    const auto invalid = forevertas::CompileConditionScript(
            "nope = 1\r\niterations >\r\ncar.speed = 0 trailing");
    bool okay = Check(!invalid.program && invalid.error.has_value(),
                      "invalid script unexpectedly produced a program");
    okay &= Check(invalid.diagnostics.size() == 3u,
                  "compiler did not retain one diagnostic per broken line");
    if (invalid.diagnostics.size() == 3u) {
        okay &= Check(invalid.diagnostics[0].line == 1u &&
                              invalid.diagnostics[0].column == 1u &&
                              invalid.diagnostics[0].length == 4u,
                      "unknown-symbol range is not precise");
        okay &= Check(invalid.diagnostics[1].line == 2u &&
                              invalid.diagnostics[1].column == 13u,
                      "incomplete comparison range is not precise");
        okay &= Check(invalid.diagnostics[2].line == 3u &&
                              invalid.diagnostics[2].column == 15u,
                      "trailing-text range is not precise");
    }
    okay &= Check(invalid.error &&
                          invalid.error->find("Condition line 1") !=
                                  std::string::npos &&
                          invalid.error->find("column 1") !=
                                  std::string::npos,
                  "legacy compile error lost its line/column summary");

    const forevertas::ConditionVariables unicodeVariable{{
            "\xC3\xA9", {2.0, 0.0, 0.0, false}}};
    const auto unicode = forevertas::CompileConditionScript(
            "variable(\"\xC3\xA9\") = nope", unicodeVariable);
    okay &= Check(unicode.diagnostics.size() == 1u &&
                          unicode.diagnostics[0].column == 18u,
                  "compiler diagnostics are not expressed in UTF-8 byte columns");

    const forevertas::ConditionVariables punctuationVariable{{
            "foo-bar", {1.0, 0.0, 0.0, false}}};
    const auto unquotedExternal = forevertas::CompileConditionScript(
            "variable(foo-bar) = 1", punctuationVariable);
    okay &= Check(unquotedExternal.program.has_value() &&
                          !unquotedExternal.error.has_value(),
                  "legacy unquoted external-variable names stopped parsing");

    const forevertas::ConditionVariables spacedVariable{{
            "foo ", {1.0, 0.0, 0.0, false}}};
    const auto trailingSpaceExternal = forevertas::CompileConditionScript(
            "variable(foo ) = 1", spacedVariable);
    okay &= Check(trailingSpaceExternal.program.has_value() &&
                          !trailingSpaceExternal.error.has_value(),
                  "legacy external-variable trailing space was discarded");

    const auto empty = forevertas::CompileConditionScript(" \r\n\t\r\n");
    okay &= Check(!empty.program && !empty.error &&
                          empty.diagnostics.empty(),
                  "blank script should make every tick eligible");
    return okay;
}

bool TestInstructionLimit() {
    std::string source;
    for (int line = 0; line < 65; ++line) {
        if (!source.empty()) source.push_back('\n');
        source += "iterations = 0";
    }
    const auto result = forevertas::CompileConditionScript(source);
    return Check(!result.program && result.error &&
                         result.diagnostics.size() == 1u &&
                         result.diagnostics[0].line == 65u &&
                         result.diagnostics[0].message.find("256-instruction") !=
                                 std::string::npos,
                 "instruction-limit diagnostic is missing or misplaced");
}

bool TestGateModesAndDisabledLines() {
    using forevervalidator::experimental::PhysicsSandboxStateView;
    const PhysicsSandboxStateView state{};
    forevertas::ConditionExecutionContext context;
    context.iterations = 1u;

    const std::string source =
            "iterations = 1\niterations = 2";
    const auto all = forevertas::CompileConditionScript(
            source, {}, forevertas::ConditionGateMode::All);
    const auto any = forevertas::CompileConditionScript(
            source, {}, forevertas::ConditionGateMode::Any);
    bool okay = Check(all.program && any.program,
                      "AND/OR gate programs did not compile");
    if (all.program && any.program) {
        okay &= Check(!all.program->Evaluate(state, state, context),
                      "AND accepted a tick with one false gate");
        okay &= Check(any.program->Evaluate(state, state, context),
                      "OR rejected a tick with one true gate");
    }

    const auto disabled = forevertas::CompileConditionScript(
            "// iterations = 2\niterations = 1");
    okay &= Check(disabled.program &&
                          disabled.program->Evaluate(state, state, context),
                  "disabled gate still affected the program");
    const auto allDisabled = forevertas::CompileConditionScript(
            " // iterations = 1\n\t// iterations = 2");
    okay &= Check(!allDisabled.program && !allDisabled.error,
                  "a script with only disabled gates was not empty");
    const auto enumConstants = forevertas::CompileConditionScript(
            "car.wheels.frontleft.surface = surface.concrete\n"
            "car.turbo_type = turbo.inactive\n"
            "car.freewheel = false\n"
            "car.gear = gear.neutral");
    okay &= Check(
            enumConstants.program &&
                    enumConstants.program->Evaluate(state, state, context),
            "typed constants did not evaluate to their runtime values");
    return okay;
}

bool TestParserOwnedCursorContexts() {
    const auto member = forevertas::AnalyzeConditionCursor("car.", 4u);
    bool okay = Check(
            member &&
                    member->site ==
                            forevertas::ConditionCursorSite::Member &&
                    member->receiver == "car" && member->fragment.empty() &&
                    member->replacement.begin == 4u &&
                    member->replacement.end == 4u &&
                    member->automaticTrigger,
            "parser did not expose the empty car member site");

    const auto midToken =
            forevertas::AnalyzeConditionCursor("car.speed", 6u);
    okay &= Check(midToken && midToken->receiver == "car" &&
                          midToken->fragment == "sp" &&
                          midToken->replacement.begin == 4u &&
                          midToken->replacement.end == 9u,
                  "parser completion range did not include the token suffix");

    const auto existingMemberStart =
            forevertas::AnalyzeConditionCursor("car.speed", 4u);
    okay &= Check(existingMemberStart &&
                          existingMemberStart->fragment.empty() &&
                          !existingMemberStart->automaticTrigger,
                  "moving to an existing member start triggered completion");

    const std::string unicode =
            "variable(\"\xC3\xA9\") + car.speed >= 0";
    const std::size_t speed = unicode.find("speed");
    const auto unicodeContext =
            forevertas::AnalyzeConditionCursor(unicode, speed + 2u);
    okay &= Check(unicodeContext &&
                          unicodeContext->replacement.begin == speed &&
                          unicodeContext->replacement.end == speed + 5u,
                  "UTF-8 prefix corrupted the parser replacement range");

    const std::string distance = "distance(car.pos, )";
    const auto vectorArgument = forevertas::AnalyzeConditionCursor(
            distance, distance.find(')'));
    okay &= Check(vectorArgument &&
                          vectorArgument->expected ==
                                  forevertas::ConditionLanguageValueType::Vector &&
                          vectorArgument->functionName == "distance" &&
                          vectorArgument->argumentIndex == 1u,
                  "AST did not retain the active typed function argument");

    const std::string surfaceComparison =
            "car.wheels.frontleft.surface = ";
    const auto surfaceValue = forevertas::AnalyzeConditionCursor(
            surfaceComparison, surfaceComparison.size());
    okay &= Check(surfaceValue &&
                          surfaceValue->enumNamespace == "surface" &&
                          surfaceValue->expected ==
                                  forevertas::ConditionLanguageValueType::Scalar,
                  "AST did not infer the surface enum on the comparison RHS");

    const std::string emptyFunction = "kmh()";
    const auto emptyFunctionArgument = forevertas::AnalyzeConditionCursor(
            emptyFunction, emptyFunction.find(')'));
    okay &= Check(
            emptyFunctionArgument &&
                    emptyFunctionArgument->site ==
                            forevertas::ConditionCursorSite::FunctionArgument &&
                    emptyFunctionArgument->replacement.begin == 4u &&
                    emptyFunctionArgument->replacement.end == 4u,
            "empty function argument completion consumed the closing parenthesis");

    const std::string external = "variable(foo-bar) = 1";
    const auto externalContext = forevertas::AnalyzeConditionCursor(
            external, external.find("bar") + 2u);
    okay &= Check(
            externalContext &&
                    externalContext->site ==
                            forevertas::ConditionCursorSite::ExternalName &&
                    externalContext->replacement.begin == 9u &&
                    externalContext->replacement.end == 16u,
            "external-name parser did not retain the raw replacement span");

    okay &= Check(!forevertas::AnalyzeConditionCursor("// car.", 7u),
                  "disabled condition line exposed completion context");
    return okay;
}

}  // namespace

int main() {
    return TestCatalogueCompiles() && TestStructuredDiagnostics() &&
                           TestInstructionLimit() &&
                           TestGateModesAndDisabledLines() &&
                           TestParserOwnedCursorContexts()
            ? 0
            : 1;
}
