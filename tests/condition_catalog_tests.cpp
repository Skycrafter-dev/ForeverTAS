#include "conditions/condition_catalog.h"
#include "conditions/condition_program.h"
#include "evaluators/scripted_target_evaluator.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <unordered_set>

namespace {
std::string Reference() {
    std::ostringstream out;
    out << "# Condition and Custom-Target Reference\n\n"
           "Generated from the parser's symbol catalog. Regenerate with "
           "`forevertas-condition-catalog-tests --write-doc docs/CONDITIONS.md`.\n\n"
           "## Setup and Preflight\n\n"
           "ForeverTAS runs standalone against a readable installed TMUF Packs directory "
           "and a replay or Challenge.Gbx. No in-game plugin is required for local search. "
           "The scenario supplies the map, not the control script. An empty base script "
           "is valid and means no scripted controls; Extract inputs to script imports "
           "replay controls. Exported scripts are text; applying them in a game is a "
           "separate workflow and may require an external tool.\n\n"
           "Start remains disabled while the validation message identifies invalid paths, "
           "script syntax, horizon, target/pass windows, conditions, or an unavailable "
           "backend. Correct that message and recheck. Device/runtime errors discovered "
           "during loading are reported separately. Choose CPU Reference or CPU Optimized "
           "when a GPU route is unavailable; this is not an automatic fallback.\n\n"
           "## Syntax\n\n"
           "Conditions use one boolean expression per line; all non-comment lines must hold. "
           "Comparisons are `> < >= <= = == !=` (`=` and `==` are equivalent). "
           "Combine comparisons with `AND`/`&&` and `OR`/`||`; AND binds more tightly "
           "than OR, and parentheses override precedence. Arithmetic is `+ - * /` "
           "with parentheses and binds more tightly than comparisons. Names are "
           "case-insensitive. Vectors are accepted by `distance`; vector literals contain "
           "three numbers, e.g. `(0, 0, 0)`. Boolean fields are numeric 0 or 1. "
           "Invalid expressions produce a line/column diagnostic.\n\n"
           "`#` and `//` start comments outside double-quoted external variable names, "
           "including inline comments. Quoted names may escape quotes and backslashes "
           "with a backslash. Comment-only conditions impose no constraint. Each condition "
           "script or individual custom-target expression is limited to 16,384 source "
           "bytes, 64 nesting levels, 256 instructions, and 32 stacked values. "
           "Arithmetic and comparisons require scalars; use `distance` for vectors.\n\n"
           "Custom targets use `min EXPRESSION`, `max EXPRESSION`, or "
           "`target VALUE EXPRESSION`, one per line. Search-clock values are not "
           "car-state objectives and are rejected there. Previous values refer to the "
           "previous simulation observation, not another candidate. Conditions filter "
           "eligible observations; they do not stop the simulation.\n\n"
           "To reach a state as early (or as late) as possible, use the Time target: "
           "it scores the first tick in its evaluation window on which every "
           "condition holds, at 10 ms tick precision. A run whose conditions never "
           "hold has no score, not a zero-time success.\n\n"
           "For multilap constraints use `car.completed_laps >= 1` (alias `car.laps`) "
           "to require the first finish passage, or the Time target with that "
           "condition to reach it as early as possible. `car.cps` counts accepted "
           "ordinary checkpoints cumulatively across laps and deliberately excludes "
           "finish passages.\n\n"
           "The Properties and functions panel is always shown below both editors. "
           "It browses names as a tree: `car` opens to `car.prev`, `car.wheels` and the "
           "other car properties, so each level stays short. While you type in the "
           "editor, its search follows the word at the caret: the text before the last "
           "dot opens that branch and the rest filters it. Ctrl+Space moves focus to the "
           "results for keyboard selection. Selecting an entry replaces the word at the "
           "caret with the canonical spelling. Aliases remain valid.\n\n"
           "## Symbols\n\n"
           "| Name | Aliases | Type | Units | Context | Meaning |\n"
           "| --- | --- | --- | --- | --- | --- |\n";
    for (const auto &entry : forevertas::ConditionSymbols()) {
        out << "| `" << entry.name << "` | ";
        for (std::size_t i = 0; i < entry.aliases.size(); ++i)
            out << (i ? ", " : "") << '`' << entry.aliases[i] << '`';
        out << " | " << entry.type << " | " << entry.units << " | "
            << (entry.conditionsOnly ? "conditions only" : "conditions and custom targets")
            << " | " << entry.description << " |\n";
    }
    out << "\n## Functions\n\n| Function | Aliases | Result | Units | Context | Meaning | Example |\n"
           "| --- | --- | --- | --- | --- | --- | --- |\n";
    for (const auto &entry : forevertas::ConditionFunctions()) {
        out << "| `" << entry.name << "` | ";
        for (const auto &alias : entry.aliases) out << '`' << alias << '`';
        out << " | " << entry.type << " | " << entry.units << " | "
            << (entry.needsPointTarget ? "point-target conditions" : entry.conditionsOnly ? "conditions only" : "conditions and custom targets")
            << " | " << entry.description << " | `" << entry.example << "` |\n";
    }
    out << "\n`variable`/`var` resolve constants supplied by the caller. The application "
           "currently supplies only the vector `bf_target_point`, and only to conditions "
           "when Point target is selected. Other names are rejected, not silently zero.\n";
    return out.str();
}
}

int main(int argc, char **argv) {
    using namespace forevertas;
    bool okay = true;
    std::unordered_set<std::string> names;
    const auto check = [&](bool valid, const std::string &name) {
        if (!valid) std::cerr << "Catalog mismatch: " << name << '\n';
        okay &= valid;
    };
    for (const auto &entry : ConditionSymbols()) {
        auto spellings = entry.aliases;
        spellings.push_back(entry.name);
        for (const auto &name : spellings) {
            check(names.insert(name).second, name + " is duplicated");
            const std::string expression = entry.type == "vector"
                    ? "distance(" + name + ", (0,0,0))" : name;
            const auto scalar = CompileScalarExpression(expression);
            check(scalar.program && scalar.program->instructions.front().value == entry.value &&
                          scalar.program->instructions.front().x == entry.component, name);
            check(CompileConditionScript(expression + " >= 0").program.has_value(), name);
            auto settings = DefaultScriptedTargetOptionSettings();
            settings["script"] = "max " + expression;
            check(ValidateScriptedTargetOptionSettings(settings, 10).has_value() == entry.conditionsOnly,
                  name + " context");
        }
    }
    const ConditionVariables variables{{"bf_target_point", {1, 2, 3, true}}};
    for (const auto &entry : ConditionFunctions()) {
        auto spellings = entry.aliases;
        spellings.push_back(entry.name);
        for (const auto &name : spellings) {
            std::string expression = name + entry.example.substr(entry.name.size());
            if (entry.type == "vector") expression = "distance(" + expression + ", (0,0,0))";
            check(CompileScalarExpression(expression, variables).program.has_value(), expression);
            check(CompileConditionScript(expression + " >= 0", variables).program.has_value(), expression);
            if (!entry.needsPointTarget) {
                auto settings = DefaultScriptedTargetOptionSettings();
                settings["script"] = "max " + expression;
                check(ValidateScriptedTargetOptionSettings(settings, 10).has_value() == entry.conditionsOnly,
                      expression + " context");
            }
        }
    }
    check(!CompileScalarExpression("distance(var(\"bf_target_point\"),(0,0,0))").program,
          "point target must not exist outside its context");
    if (!okay) return 1;
    const std::string document = Reference();
    if (argc == 3 && std::string(argv[1]) == "--write-doc") {
        std::ofstream out(argv[2], std::ios::binary);
        out << document;
        return out.good() ? 0 : 1;
    }
    std::ifstream input(FOREVERTAS_SOURCE_DIR "/docs/CONDITIONS.md", std::ios::binary);
    const std::string actual{std::istreambuf_iterator<char>(input), {}};
    check(actual == document, "generated documentation is stale");
    return okay ? 0 : 1;
}
