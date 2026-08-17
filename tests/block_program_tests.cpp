#include "blocks/block_catalog.h"
#include "blocks/block_compiler.h"
#include "blocks/block_lowering.h"
#include "blocks/block_program.h"
#include "blocks/block_program_io.h"
#include "blocks/block_value.h"
#include "searches/algorithm_registry.h"
#include "searches/search_runner.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <string>

using namespace forevertas;
using namespace forevertas::blocks;

namespace {

int failures = 0;

void Check(bool condition, const char *message) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

BlockProgram DefaultProgram() {
    return BuildProgramFromComponents(SearchComponentConfiguration{
            DefaultSearchAlgorithmConfiguration(),
            DefaultModifierConfigurations(),
            DefaultEvaluationTargetConfiguration()});
}

// Expands compiled components to an atom program, recompiles, and expects
// the exact same components back: the migration/lowering equivalence that
// preserves search behavior byte for byte.
bool RoundTripsComponents(const SearchComponentConfiguration &components,
                          std::string *detail) {
    const BlockProgram program = BuildProgramFromComponents(components);
    const CompileResult result = CompileProgram(program);
    if (!result.ok) {
        *detail = "compile failed";
        if (!result.errors.empty()) *detail += ": " + result.errors.front();
        return false;
    }
    if (!(result.configuration == components)) {
        *detail = "components differ after lowering round trip";
        return false;
    }
    return true;
}

OptionSettings SettingsWithOverrides(const char *optionId,
                                     std::map<std::string, std::string> values) {
    const ModifierRegistration *const registration =
            FindModifier(optionId);
    OptionSettings settings = registration->defaultSettings;
    for (auto &[key, value] : values) {
        settings.erase(key);
        settings.emplace(key, value);
    }
    return settings;
}

void TestCatalogAtoms() {
    const auto checkAtom = [](const std::string &id,
                              BlockShape shape,
                              const std::string &optionKind,
                              const std::string &optionId) {
        const BlockDefinition *const definition = FindBlock(id);
        Check(definition != nullptr, "atom exists");
        if (definition == nullptr) return;
        Check(definition->shape == shape, "atom shape");
        Check(definition->optionKind == optionKind, "atom option kind");
        Check(definition->optionId == optionId, "atom option binding");
    };

    checkAtom("search/basic-brute-force", BlockShape::Hat, "search",
              kBasicBruteForceSearchId);
    checkAtom("mutate/window", BlockShape::Container, "mutation", "");
    checkAtom("mutate/reroll-steering", BlockShape::Stack, "mutation",
              kRandomSteeringModifierId);
    checkAtom("mutate/shift-events", BlockShape::Stack, "mutation",
              kExistingEventPerturbationModifierId);
    checkAtom("mutate/nudge-steering", BlockShape::Stack, "mutation",
              kExistingEventPerturbationModifierId);
    checkAtom("mutate/set-steering", BlockShape::Stack, "mutation",
              kExistingEventPerturbationModifierId);
    checkAtom("mutate/flip-accelerate", BlockShape::Stack, "mutation",
              kExistingEventPerturbationModifierId);
    checkAtom("mutate/flip-brake", BlockShape::Stack, "mutation",
              kExistingEventPerturbationModifierId);
    checkAtom("mutate/insert-steering-at", BlockShape::Stack, "mutation",
              kInputInsertionModifierId);
    checkAtom("mutate/adjust-steering-by", BlockShape::Stack, "mutation",
              kInputInsertionModifierId);
    checkAtom("mutate/press-accelerate", BlockShape::Stack, "mutation",
              kInputInsertionModifierId);
    checkAtom("mutate/press-brake", BlockShape::Stack, "mutation",
              kInputInsertionModifierId);
    checkAtom("mutate/delete-steering", BlockShape::Stack, "mutation",
              kInputDeletionModifierId);
    checkAtom("mutate/delete-accelerate", BlockShape::Stack, "mutation",
              kInputDeletionModifierId);
    checkAtom("mutate/delete-brake", BlockShape::Stack, "mutation",
              kInputDeletionModifierId);
    checkAtom("mutate/smooth-steering", BlockShape::Stack, "mutation",
              kSmoothSteeringModifierId);
    checkAtom("evaluate/finish-time", BlockShape::Reporter, "evaluation",
              kPreciseFinishTimeEvaluationId);
    checkAtom("evaluate/stunt-points", BlockShape::Reporter, "evaluation",
              kStuntPointsEvaluationId);
    checkAtom("evaluate/speed", BlockShape::Reporter, "evaluation",
              kVelocityEvaluationId);
    checkAtom("evaluate/speed-toward", BlockShape::Reporter, "evaluation",
              kVelocityEvaluationId);
    checkAtom("evaluate/distance-to-point", BlockShape::Reporter,
              "evaluation", kPointTargetEvaluationId);
    checkAtom("evaluate/distance-to-pose", BlockShape::Reporter,
              "evaluation", kPoseTargetEvaluationId);
    checkAtom("evaluate/box-entry-time", BlockShape::Reporter,
              "evaluation", kVolumeEntryEvaluationId);
    checkAtom("evaluate/prism-entry-time", BlockShape::Reporter,
              "evaluation", kCustomVolumeEntryEvaluationId);

    Check(FindBlock("values/number") != nullptr, "number primitive exists");
    Check(FindBlock("values/add") != nullptr, "add primitive exists");
    Check(FindBlock("values/minimum") != nullptr, "min primitive exists");
    Check(FindBlock("values/nope") == nullptr, "unknown block is null");

    // The legacy multi-configuration option blocks are gone from the
    // palette vocabulary.
    Check(FindBlock("mutate/random-steering") == nullptr,
          "legacy random-steering block removed");
    Check(FindBlock("mutate/input-insertion") == nullptr,
          "legacy insertion block removed");
    Check(FindBlock("mutate/existing-event-perturbation") == nullptr,
          "legacy perturbation block removed");
    Check(FindBlock("mutate/input-deletion") == nullptr,
          "legacy deletion block removed");
    Check(FindBlock("evaluate/velocity") == nullptr,
          "legacy velocity block removed");
    Check(FindBlock("evaluate/pose-target") == nullptr,
          "legacy pose block removed");
}

void TestFieldSchemasMatchOptions() {
    // Every atom field key must be a legacy settings key with a
    // byte-identical default, so lowering stays byte-stable.
    for (const BlockDefinition &definition : BlockCatalog()) {
        if (definition.optionId.empty()) continue;
        const OptionSettings *defaults = nullptr;
        if (definition.optionKind == "mutation") {
            const ModifierRegistration *const registration =
                    FindModifier(definition.optionId);
            Check(registration != nullptr, "atom binds registered modifier");
            if (registration != nullptr) defaults =
                    &registration->defaultSettings;
        } else if (definition.optionKind == "evaluation") {
            const EvaluationTargetRegistration *const registration =
                    FindEvaluationTarget(definition.optionId);
            Check(registration != nullptr,
                  "atom binds registered evaluator");
            if (registration != nullptr) defaults =
                    &registration->defaultSettings;
        } else if (definition.optionKind == "search") {
            const SearchAlgorithmRegistration *const registration =
                    FindSearchAlgorithm(definition.optionId);
            Check(registration != nullptr, "atom binds registered search");
            if (registration != nullptr) defaults =
                    &registration->defaultSettings;
        }
        if (defaults == nullptr) continue;
        for (const OptionField &field : definition.fields) {
            const auto found = defaults->find(field.key);
            Check(found != defaults->end(), "atom field is option key");
            if (found != defaults->end()) {
                Check(found->second == field.defaultValue,
                      "atom field default matches option default");
            }
            if (field.kind == OptionField::Kind::Enum) {
                Check(!field.enumValues.empty(), "enum has values");
            }
        }
    }
}

void TestProgramOperations() {
    BlockProgram program = DefaultProgram();
    Check(program.script() != std::nullopt, "default program has script");
    const BlockId hat = *program.script();
    Check(program.find(hat)->substack.size() == 1, "one mutation window");
    const BlockId group = program.find(hat)->substack.front();
    Check(program.find(group)->substack.size() == 1, "one atom in window");

    const BlockId second = program.createBlock("mutate/delete-steering");
    program.appendToSubstack(group, second);
    Check(program.find(group)->substack.size() == 2, "two atoms");
    Check(program.moveWithinSubstack(second, 0), "move atom");
    Check(program.find(group)->substack.front() == second,
          "moved atom first");

    Check(program.removeBlock(second), "remove atom");
    Check(program.find(group)->substack.size() == 1,
          "one atom after remove");
    Check(program.find(second) == nullptr, "removed atom is gone");

    Check(program.detachReporter(group, "minTimeMs") == false,
          "detach without reporter fails");
    const BlockId reporter =
            program.attachReporter(group, "seed", "values/add",
                                   {{"left", "1179926867"}, {"right", "1"}});
    Check(reporter != 0, "attach reporter");
    Check(program.find(group)->reporters.at("seed") == reporter,
          "reporter linked");
    Check(program.detachReporter(group, "seed"), "detach reporter");
    Check(program.find(reporter) == nullptr, "detached reporter collected");
}

void TestCompileDefaultProgram() {
    BlockProgram program = DefaultProgram();
    const CompileResult result = CompileProgram(program);
    Check(result.ok, "default program compiles");
    if (!result.ok) return;
    Check(result.configuration.searchAlgorithm ==
                  DefaultSearchAlgorithmConfiguration(),
          "search components match defaults");
    Check(result.configuration.evaluationTarget ==
                  DefaultEvaluationTargetConfiguration(),
          "evaluation components match defaults");
    Check(result.configuration.modifiers.size() == 1, "one modifier");
    Check(result.configuration.modifiers.front() ==
                  DefaultModifierConfigurations().front(),
          "modifier components match defaults");
}

void TestLoweringEquivalence() {
    std::string detail;
    const auto check = [&detail](const SearchComponentConfiguration &components,
                                 const char *what) {
        if (RoundTripsComponents(components, &detail)) return;
        std::fprintf(stderr, "FAIL(%s): %s\n", what, detail.c_str());
        ++failures;
    };

    const OptionConfiguration search = DefaultSearchAlgorithmConfiguration();
    const OptionConfiguration evaluation =
            DefaultEvaluationTargetConfiguration();

    // Every modifier's default configuration round-trips.
    for (const ModifierRegistration &registration : ModifierRegistry()) {
        check(SearchComponentConfiguration{
                      search,
                      {OptionConfiguration{registration.id,
                                           registration.defaultSettings}},
                      evaluation},
              "modifier defaults");
    }
    // Every evaluator's default configuration round-trips.
    for (const EvaluationTargetRegistration &registration :
         EvaluationTargetRegistry()) {
        check(SearchComponentConfiguration{
                      search,
                      DefaultModifierConfigurations(),
                      OptionConfiguration{registration.id,
                                          registration.defaultSettings}},
              "evaluator defaults");
    }
    // The search policy option round-trips.
    {
        OptionSettings promoted = search.settings;
        promoted["autoPromoteBest"] = "true";
        check(SearchComponentConfiguration{
                      OptionConfiguration{search.id, promoted},
                      DefaultModifierConfigurations(),
                      evaluation},
              "search promote-best");
    }

    // A multi-channel insertion (offset steering, accelerate, brake) with
    // custom values round-trips exactly.
    {
        const OptionSettings settings = SettingsWithOverrides(
                kInputInsertionModifierId,
                {{"minTimeMs", "500"},
                 {"maxTimeMs", "3000"},
                 {"seed", "42"},
                 {"steerMode", "offset"},
                 {"steerOffsetMin", "-0.3"},
                 {"steerOffsetMax", "0.25"},
                 {"steerMinCount", "1"},
                 {"steerMaxCount", "4"},
                 {"steerMaxHoldMs", "300"},
                 {"accelerateEnabled", "true"},
                 {"accelerateMinCount", "1"},
                 {"accelerateMaxCount", "2"},
                 {"accelerateMaxHoldMs", "150"},
                 {"brakeEnabled", "true"},
                 {"brakeMinCount", "2"},
                 {"brakeMaxCount", "3"},
                 {"brakeMaxHoldMs", "120"}});
        check(SearchComponentConfiguration{
                      search,
                      {OptionConfiguration{kInputInsertionModifierId,
                                           settings}},
                      evaluation},
              "insertion multi-channel offset");
    }
    // Absolute steering insertion round-trips.
    {
        const OptionSettings settings = SettingsWithOverrides(
                kInputInsertionModifierId,
                {{"minTimeMs", "800"},
                 {"maxTimeMs", "2600"},
                 {"seed", "9"},
                 {"steerMode", "absolute"},
                 {"steerAbsoluteMin", "-0.5"},
                 {"steerAbsoluteMax", "0.6"},
                 {"steerMinCount", "0"},
                 {"steerMaxCount", "1"},
                 {"steerMaxHoldMs", "90"}});
        check(SearchComponentConfiguration{
                      search,
                      {OptionConfiguration{kInputInsertionModifierId,
                                           settings}},
                      evaluation},
              "insertion absolute");
    }
    // A multi-feature perturbation (shift, delta nudge, brake toggle, no
    // accelerate toggle) round-trips.
    {
        const OptionSettings settings = SettingsWithOverrides(
                kExistingEventPerturbationModifierId,
                {{"minTimeMs", "2000"},
                 {"maxTimeMs", "5000"},
                 {"seed", "7"},
                 {"minCount", "2"},
                 {"maxCount", "5"},
                 {"maxTimeShiftMs", "150"},
                 {"steerDeltaMin", "-0.2"},
                 {"steerDeltaMax", "0.2"},
                 {"toggleAccelerate", "false"},
                 {"toggleBrake", "true"}});
        check(SearchComponentConfiguration{
                      search,
                      {OptionConfiguration{
                              kExistingEventPerturbationModifierId,
                              settings}},
                      evaluation},
              "perturbation multi-feature");
    }
    // Absolute perturbation round-trips.
    {
        const OptionSettings settings = SettingsWithOverrides(
                kExistingEventPerturbationModifierId,
                {{"steerMode", "absolute"},
                 {"steerAbsoluteMin", "-0.4"},
                 {"steerAbsoluteMax", "0.6"},
                 {"maxTimeShiftMs", "0"}});
        check(SearchComponentConfiguration{
                      search,
                      {OptionConfiguration{
                              kExistingEventPerturbationModifierId,
                              settings}},
                      evaluation},
              "perturbation absolute");
    }
    // Multi-channel deletion round-trips.
    {
        const OptionSettings settings = SettingsWithOverrides(
                kInputDeletionModifierId,
                {{"minTimeMs", "1100"},
                 {"maxTimeMs", "2200"},
                 {"seed", "3"},
                 {"steerMaxCount", "5"},
                 {"accelerateEnabled", "true"},
                 {"accelerateMaxCount", "2"},
                 {"brakeEnabled", "true"},
                 {"brakeMaxCount", "1"}});
        check(SearchComponentConfiguration{
                      search,
                      {OptionConfiguration{kInputDeletionModifierId,
                                           settings}},
                      evaluation},
              "deletion multi-channel");
    }
    // Smooth steering and random steering custom values round-trip.
    {
        const OptionSettings settings = SettingsWithOverrides(
                kSmoothSteeringModifierId,
                {{"minTimeMs", "900"},
                 {"maxTimeMs", "3210"},
                 {"seed", "11"},
                 {"deformationCount", "3"},
                 {"radiusMs", "120"},
                 {"amplitudeMin", "-0.1"},
                 {"amplitudeMax", "0.35"}});
        check(SearchComponentConfiguration{
                      search,
                      {OptionConfiguration{kSmoothSteeringModifierId,
                                           settings}},
                      evaluation},
              "smooth custom");
    }
    {
        const OptionSettings settings = SettingsWithOverrides(
                kRandomSteeringModifierId,
                {{"minTimeMs", "400"}, {"maxTimeMs", "900"},
                 {"seed", "1234"}});
        check(SearchComponentConfiguration{
                      search,
                      {OptionConfiguration{kRandomSteeringModifierId,
                                           settings}},
                      evaluation},
              "random custom");
    }
    // Projected velocity without the gate round-trips through
    // speed-toward.
    {
        OptionSettings settings =
                FindEvaluationTarget(kVelocityEvaluationId)->defaultSettings;
        settings["mode"] = "projected";
        settings["directionX"] = "0.5";
        settings["directionY"] = "1";
        settings["directionZ"] = "-0.25";
        check(SearchComponentConfiguration{
                      search,
                      DefaultModifierConfigurations(),
                      OptionConfiguration{kVelocityEvaluationId, settings}},
              "velocity projected");
    }
    // Projected velocity with the gate round-trips.
    {
        OptionSettings settings =
                FindEvaluationTarget(kVelocityEvaluationId)->defaultSettings;
        settings["mode"] = "projected";
        settings["alignmentEnabled"] = "true";
        settings["minAlignmentPercent"] = "25";
        check(SearchComponentConfiguration{
                      search,
                      DefaultModifierConfigurations(),
                      OptionConfiguration{kVelocityEvaluationId, settings}},
              "velocity gated");
    }
    // Total velocity with the gate is the one documented approximation:
    // it migrates to speed-toward, preserving direction and gate but
    // switching the measure to projected.
    {
        OptionSettings settings =
                FindEvaluationTarget(kVelocityEvaluationId)->defaultSettings;
        settings["alignmentEnabled"] = "true";
        settings["minAlignmentPercent"] = "25";
        settings["directionX"] = "0";
        settings["directionY"] = "0";
        settings["directionZ"] = "1";
        const BlockProgram program = BuildProgramFromComponents(
                SearchComponentConfiguration{
                        search,
                        DefaultModifierConfigurations(),
                        OptionConfiguration{kVelocityEvaluationId,
                                            settings}});
        const CompileResult result = CompileProgram(program);
        Check(result.ok, "total gated compiles");
        if (result.ok) {
            const OptionSettings &compiled =
                    result.configuration.evaluationTarget.settings;
            Check(compiled.at("mode") == "projected",
                  "total gated migrates to projected");
            Check(compiled.at("alignmentEnabled") == "true",
                  "gate preserved");
            Check(compiled.at("minAlignmentPercent") == "25",
                  "gate threshold preserved");
            Check(compiled.at("directionZ") == "1",
                  "direction preserved");
        }
    }
    // Explicit evaluation fields (formerly mirrored) round-trip.
    {
        OptionSettings settings =
                FindEvaluationTarget(kVolumeEntryEvaluationId)->defaultSettings;
        settings["centerX"] = "12.5";
        settings["centerY"] = "-3";
        settings["centerZ"] = "40";
        settings["sizeX"] = "8";
        settings["sizeY"] = "8";
        settings["sizeZ"] = "12";
        check(SearchComponentConfiguration{
                      search,
                      DefaultModifierConfigurations(),
                      OptionConfiguration{kVolumeEntryEvaluationId,
                                          settings}},
              "box entry custom");
    }
    {
        OptionSettings settings =
                FindEvaluationTarget(kCustomVolumeEntryEvaluationId)
                        ->defaultSettings;
        settings["plane"] = "xy";
        settings["originX"] = "10";
        settings["originY"] = "-4";
        settings["originZ"] = "2";
        settings["depth"] = "7";
        settings["polygon"] = "0,0;10,0;10,10;0,10";
        check(SearchComponentConfiguration{
                      search,
                      DefaultModifierConfigurations(),
                      OptionConfiguration{kCustomVolumeEntryEvaluationId,
                                          settings}},
              "prism entry custom");
    }
}

void TestCompileStructureErrors() {
    {
        BlockProgram program;
        const CompileResult result = CompileProgram(program);
        Check(!result.ok, "empty program fails");
        Check(!result.errors.empty(), "empty program reports error");
    }
    {
        BlockProgram program = DefaultProgram();
        program.detachEvaluator(*program.script());
        const CompileResult result = CompileProgram(program);
        Check(!result.ok, "missing evaluator fails");
    }
    {
        BlockProgram program = DefaultProgram();
        program.removeBlock(program.find(*program.script())
                                    ->substack.front());
        const CompileResult result = CompileProgram(program);
        Check(!result.ok, "missing mutation window fails");
    }
    {
        // Atoms may not sit directly in the hat's substack.
        BlockProgram program = DefaultProgram();
        const BlockId atom = program.createBlock("mutate/reroll-steering");
        program.appendToSubstack(*program.script(), atom);
        const CompileResult result = CompileProgram(program);
        Check(!result.ok, "atom outside window fails");
    }
    {
        // An empty window is a structure error.
        BlockProgram program = DefaultProgram();
        const BlockId group = program.createBlock("mutate/window");
        program.appendToSubstack(*program.script(), group);
        const CompileResult result = CompileProgram(program);
        Check(!result.ok, "empty window fails");
    }
    {
        // Conflicting steering modes cannot share a window.
        BlockProgram program = DefaultProgram();
        const BlockId group = program.find(*program.script())->substack.front();
        const BlockId nudge = program.createBlock("mutate/nudge-steering");
        const BlockId set = program.createBlock("mutate/set-steering");
        program.appendToSubstack(group, nudge);
        program.appendToSubstack(group, set);
        const CompileResult result = CompileProgram(program);
        Check(!result.ok, "conflicting steer modes fail");
    }
    {
        // Two atoms disagreeing on a shared key cannot share a window.
        BlockProgram program = DefaultProgram();
        const BlockId group = program.find(*program.script())->substack.front();
        const BlockId flip = program.createBlock(
                "mutate/flip-brake", {{"minCount", "1"}, {"maxCount", "1"}});
        program.appendToSubstack(group, flip);
        const BlockId shift = program.createBlock(
                "mutate/shift-events",
                {{"minCount", "3"}, {"maxCount", "4"},
                 {"maxTimeShiftMs", "100"}});
        program.appendToSubstack(group, shift);
        const CompileResult result = CompileProgram(program);
        Check(!result.ok, "conflicting shared values fail");
    }
    {
        // Mixed options in one window lower to sequential passes sharing
        // the window and seed.
        BlockProgram program;
        const BlockId hat = program.createBlock("search/basic-brute-force");
        const BlockId evaluator = program.createBlock("evaluate/speed");
        const BlockId group = program.createBlock(
                "mutate/window",
                {{"minTimeMs", "1000"},
                 {"maxTimeMs", "5990"},
                 {"seed", "55"}});
        const BlockId nudge = program.createBlock("mutate/nudge-steering");
        const BlockId press = program.createBlock("mutate/press-brake");
        program.setEvaluator(hat, evaluator);
        program.appendToSubstack(group, nudge);
        program.appendToSubstack(group, press);
        program.appendToSubstack(hat, group);
        program.setScript(hat);
        const CompileResult result = CompileProgram(program);
        Check(result.ok, "mixed window compiles");
        if (result.ok) {
            Check(result.configuration.modifiers.size() == 2,
                  "mixed window lowers to two passes");
            const OptionSettings &first =
                    result.configuration.modifiers.front().settings;
            const OptionSettings &second =
                    result.configuration.modifiers.back().settings;
            Check(first.at("minTimeMs") == "1000" &&
                          first.at("seed") == "55",
                  "first pass shares window");
            Check(second.at("minTimeMs") == "1000" &&
                          second.at("seed") == "55",
                  "second pass shares window");
            Check(result.configuration.modifiers.front().id ==
                          kExistingEventPerturbationModifierId,
                  "first pass is perturbation");
            Check(result.configuration.modifiers.back().id ==
                          kInputInsertionModifierId,
                  "second pass is insertion");
            Check(!ValidateSearchComponents(
                          result.configuration, kSearchTickDurationMs, 6000u)
                          .has_value(),
                  "mixed window validates");
        }
    }
}

void TestCompileReporterExpressions() {
    BlockProgram program = DefaultProgram();
    const BlockId group = program.find(*program.script())->substack.front();
    // Swap the reroll atom for a smooth-steering atom so there are atom
    // fields to plug expressions into.
    program.removeBlock(program.find(group)->substack.front());
    const BlockId smooth = program.createBlock("mutate/smooth-steering");
    program.appendToSubstack(group, smooth);
    const BlockId sum = program.attachReporter(
            group, "maxTimeMs", "values/add",
            {{"left", "3000"}, {"right", "2990"}});
    Check(sum != 0, "expression attached");
    const BlockId deformation = program.attachReporter(
            smooth, "deformationCount", "values/maximum",
            {{"left", "2"}, {"right", "5"}});
    Check(deformation != 0, "atom expression attached");
    const CompileResult result = CompileProgram(program);
    Check(result.ok, "expression program compiles");
    if (result.ok) {
        const auto found = result.configuration.modifiers.front().settings.find(
                "maxTimeMs");
        Check(found != result.configuration.modifiers.front().settings.end() &&
                      found->second == "5990",
              "window expression evaluates");
        Check(result.configuration.modifiers.front().settings.at(
                      "deformationCount") == "5",
              "atom expression evaluates");
    }
    // Division by zero must surface as a compile error.
    const BlockId quotient = program.attachReporter(
            group, "minTimeMs", "values/divide",
            {{"left", "1000"}, {"right", "0"}});
    Check(quotient != 0, "division attached");
    const CompileResult failing = CompileProgram(program);
    Check(!failing.ok, "division by zero fails compile");
}

void TestTextRoundTrip() {
    BlockProgram program = DefaultProgram();
    program.setFieldValue(*program.script(), "autoPromoteBest", "true");
    const BlockId group = program.find(*program.script())->substack.front();
    program.setFieldValue(group, "maxTimeMs", "4500");
    program.setFieldValue(group, "seed", "31337");

    const std::string printed = PrintBlockProgramText(program);
    const BlockProgramText parsed = ParseBlockProgramText(printed);
    if (!parsed.program.has_value())
        std::fprintf(stderr, "round-trip error: %s\n",
                     parsed.error.c_str());
    Check(parsed.program.has_value(), "printed text parses");
    if (!parsed.program.has_value()) return;
    const std::string reparsed =
            PrintBlockProgramText(*parsed.program);
    Check(reparsed == printed, "text round trip is stable");

    const CompileResult direct = CompileProgram(program);
    const CompileResult indirect = CompileProgram(*parsed.program);
    Check(direct.ok && indirect.ok, "both compile");
    if (direct.ok && indirect.ok) {
        Check(direct.configuration == indirect.configuration,
              "components survive text");
    }
}

void TestTextExpressions() {
    const BlockProgramText parsed = ParseBlockProgramText(
            "search basic-brute-force {\n"
            "  autoPromoteBest = false\n"
            "  evaluate speed {\n"
            "    minTimeMs = 0\n"
            "    maxTimeMs = 6000\n"
            "  }\n"
            "  mutate window {\n"
            "    minTimeMs = (1000 + 500)\n"
            "    maxTimeMs = min(5990, 7000)\n"
            "    seed = 1179926867\n"
            "    op smooth-steering {\n"
            "      deformationCount = max(1, 2)\n"
            "      radiusMs = 200\n"
            "      amplitudeMin = -0.2\n"
            "      amplitudeMax = 0.2\n"
            "    }\n"
            "  }\n"
            "}\n");
    Check(parsed.program.has_value(), "expression text parses");
    if (!parsed.program.has_value()) {
        std::fprintf(stderr, "expression error: %s\n",
                     parsed.error.c_str());
        Check(!parsed.error.empty(), "parse error reported");
        return;
    }
    const CompileResult result = CompileProgram(*parsed.program);
    Check(result.ok, "expression text compiles");
    if (result.ok) {
        const OptionSettings &settings =
                result.configuration.modifiers.front().settings;
        Check(settings.at("minTimeMs") == "1500", "sum expression");
        Check(settings.at("maxTimeMs") == "5990", "min expression");
        Check(settings.at("deformationCount") == "2", "max expression");
    }
}

void TestLegacyTextParsing() {
    // Option-id text from the previous interchange remains loadable and
    // expands to the equivalent atom program.
    const BlockProgramText parsed = ParseBlockProgramText(
            "search basic-brute-force {\n"
            "  evaluate velocity {\n"
            "    minTimeMs = 1000\n"
            "    maxTimeMs = 6000\n"
            "    mode = projected\n"
            "  }\n"
            "  mutate random-steering {\n"
            "    minTimeMs = 1200\n"
            "    maxTimeMs = 4400\n"
            "    seed = 5\n"
            "  }\n"
            "}\n");
    Check(parsed.program.has_value(), "legacy text parses");
    if (!parsed.program.has_value()) {
        std::fprintf(stderr, "legacy text error: %s\n",
                     parsed.error.c_str());
        return;
    }
    const CompileResult result = CompileProgram(*parsed.program);
    Check(result.ok, "legacy text compiles");
    if (result.ok) {
        Check(result.configuration.evaluationTarget.id ==
                      kVelocityEvaluationId,
              "legacy evaluation id");
        Check(result.configuration.evaluationTarget.settings.at("mode") ==
                      "projected",
              "legacy evaluation mode");
        Check(result.configuration.modifiers.front().id ==
                      kRandomSteeringModifierId,
              "legacy modifier id");
        Check(result.configuration.modifiers.front().settings.at(
                      "maxTimeMs") == "4400",
              "legacy modifier window");
    }

    Check(!ParseBlockProgramText(
                  "search basic-brute-force {\n"
                  "  mutate no-such-modifier { seed = 1 }\n"
                  "}\n")
                  .program.has_value(),
          "unknown legacy modifier rejected");
}

void TestTextParseErrors() {
    Check(!ParseBlockProgramText("").program.has_value(),
          "empty text rejected");
    Check(!ParseBlockProgramText("search no-such {}\n").program.has_value(),
          "unknown search rejected");
    Check(!ParseBlockProgramText(
                  "search basic-brute-force {\n"
                  "  mutate window { nonsense = 1 }\n"
                  "}\n")
                  .program.has_value(),
          "unknown setting rejected");
    Check(!ParseBlockProgramText(
                  "search basic-brute-force {\n"
                  "  mutate window { op no-such {} }\n"
                  "}\n")
                  .program.has_value(),
          "unknown mutation block rejected");
    Check(!ParseBlockProgramText(
                  "search basic-brute-force {}\n"
                  "search basic-brute-force {}\n")
                  .program.has_value(),
          "second search rejected");
    Check(!ParseBlockProgramText(
                  "search basic-brute-force { bad = }\n")
                  .program.has_value(),
          "missing value rejected");
}

void TestJsonRoundTrip() {
    BlockProgram program = DefaultProgram();
    program.setFieldValue(*program.script(), "autoPromoteBest", "true");
    const BlockId group = program.find(*program.script())->substack.front();
    program.attachReporter(group, "seed", "values/add",
                           {{"left", "1"}, {"right", "2"}});
    const BlockId loose = program.createBlock("values/number");
    program.find(loose)->x = 12.0;
    program.find(loose)->y = 34.0;

    std::map<std::string, std::map<std::string, std::string>> remembered;
    remembered["evaluate/speed"] = {{"minTimeMs", "1200"}};

    const std::string json =
            PrintBlockProgramJson(program, remembered);
    const BlockProgramJson parsed = ParseBlockProgramJson(json);
    Check(parsed.program.has_value(), "json parses");
    if (!parsed.program.has_value()) return;
    const std::string reprinted =
            PrintBlockProgramJson(*parsed.program, parsed.remembered);
    Check(reprinted == json, "json round trip is stable");
    Check(parsed.remembered == remembered, "remembered values survive");

    const CompileResult direct = CompileProgram(program);
    const CompileResult indirect = CompileProgram(*parsed.program);
    Check(direct.ok && indirect.ok &&
                  direct.configuration == indirect.configuration,
          "json round trip compiles identically");
    Check(parsed.program->find(loose) != nullptr, "loose block survives");
    Check(parsed.program->find(loose)->x == 12.0, "position survives");
}

void TestJsonVersionOneMigration() {
    const std::string velocityFields =
            "\"minTimeMs\":\"1000\",\"maxTimeMs\":\"6000\","
            "\"mode\":\"total\",\"alignmentEnabled\":\"false\","
            "\"directionX\":\"1\",\"directionY\":\"0\","
            "\"directionZ\":\"0\",\"minAlignmentPercent\":\"-100\"";
    const std::string document =
            "{\"version\":1,\"script\":1,\"blocks\":["
            "{\"id\":1,\"def\":\"search/basic-brute-force\","
            "\"fields\":{\"autoPromoteBest\":\"true\"},"
            "\"evaluator\":2,\"substack\":[3,4]},"
            "{\"id\":2,\"def\":\"evaluate/velocity\",\"fields\":{" +
            velocityFields +
            "}},"
            "{\"id\":3,\"def\":\"mutate/random-steering\","
            "\"fields\":{\"minTimeMs\":\"1000\",\"maxTimeMs\":\"4500\","
            "\"seed\":\"42\"}},"
            "{\"id\":4,\"def\":\"mutate/existing-event-perturbation\","
            "\"fields\":{\"minTimeMs\":\"600\",\"maxTimeMs\":\"2500\","
            "\"seed\":\"9\",\"minCount\":\"2\",\"maxCount\":\"2\","
            "\"maxTimeShiftMs\":\"120\",\"steerMode\":\"absolute\","
            "\"steerDeltaMin\":\"-0.15\",\"steerDeltaMax\":\"0.15\","
            "\"steerAbsoluteMin\":\"-0.5\",\"steerAbsoluteMax\":\"0.5\","
            "\"toggleAccelerate\":\"true\",\"toggleBrake\":\"false\"}},"
            "{\"id\":5,\"def\":\"values/number\","
            "\"fields\":{\"value\":\"9\"},\"x\":5,\"y\":6}"
            "],\"loose\":[5]}";
    const BlockProgramJson parsed = ParseBlockProgramJson(document);
    Check(parsed.program.has_value(), "version 1 migrates");
    if (!parsed.program.has_value()) {
        std::fprintf(stderr, "migration error: %s\n", parsed.error.c_str());
        return;
    }
    const CompileResult result = CompileProgram(*parsed.program);
    Check(result.ok, "migrated program compiles");
    if (!result.ok) return;
    Check(result.configuration.searchAlgorithm.settings.at(
                  "autoPromoteBest") == "true",
          "search setting migrates");
    Check(result.configuration.evaluationTarget.id ==
                  kVelocityEvaluationId,
          "evaluation migrates");
    Check(result.configuration.evaluationTarget.settings ==
                  FindEvaluationTarget(kVelocityEvaluationId)
                          ->defaultSettings,
          "evaluation defaults migrate");
    Check(result.configuration.modifiers.size() == 2,
          "two modifiers migrate");
    Check(result.configuration.modifiers.front().id ==
                  kRandomSteeringModifierId,
          "first modifier migrates");
    Check(result.configuration.modifiers.front().settings.at(
                  "maxTimeMs") == "4500",
          "first modifier window migrates");
    const OptionSettings &perturbation =
            result.configuration.modifiers.back().settings;
    Check(perturbation.at("steerMode") == "absolute",
          "perturbation mode migrates");
    Check(perturbation.at("steerAbsoluteMin") == "-0.5",
          "perturbation absolute range migrates");
    Check(perturbation.at("maxTimeShiftMs") == "120",
          "perturbation shift migrates");
    Check(perturbation.at("toggleAccelerate") == "true",
          "perturbation toggle migrates");
    Check(perturbation.at("toggleBrake") == "false",
          "perturbation absent toggle migrates");
    // The reprinted document is version 2 and stable.
    const std::string printed =
            PrintBlockProgramJson(*parsed.program, parsed.remembered);
    const BlockProgramJson reparsed = ParseBlockProgramJson(printed);
    Check(reparsed.program.has_value(), "reprinted v2 parses");
    if (reparsed.program.has_value()) {
        const CompileResult recompiled = CompileProgram(*reparsed.program);
        Check(recompiled.ok && recompiled.configuration ==
                      result.configuration,
              "migrated program survives v2 round trip");
    }
    // The loose value block survives as a number block at its position.
    bool foundLoose = false;
    for (const auto &[id, node] : parsed.program->nodes()) {
        if (node.definitionId != "values/number") continue;
        if (node.fields.at("value") == "9" && node.x == 5.0 &&
            node.y == 6.0) {
            foundLoose = true;
        }
    }
    Check(foundLoose, "loose value block survives migration");

    // Unknown versions are rejected.
    Check(!ParseBlockProgramJson("{\"version\":3,\"blocks\":[]}")
                   .program.has_value(),
          "unsupported version rejected");
}

void TestNumberFormatting() {
    Check(FormatNumberValue(1000.0) == "1000", "integer formatting");
    Check(FormatNumberValue(-0.5) == "-0.5", "decimal formatting");
    Check(FormatNumberValue(0.1 + 0.2) == "0.30000000000000004",
          "shortest round trip");
    Check(ParseNumberValue("1e3").has_value(), "exponent accepted");
    Check(!ParseNumberValue("total").has_value(), "words rejected");
    Check(!ParseNumberValue("1 2").has_value(), "spaces rejected");
    Check(!ParseNumberValue("").has_value(), "empty rejected");
}

void TestValidationParity() {
    // Modifier windows beyond the horizon are silently clamped (legacy
    // behavior); evaluation windows beyond it are rejected.
    BlockProgram program = DefaultProgram();
    const BlockId group = program.find(*program.script())->substack.front();
    program.setFieldValue(group, "maxTimeMs", "9000");
    CompileResult compiled = CompileProgram(program);
    Check(compiled.ok, "compiles before validation");
    Check(!ValidateSearchComponents(compiled.configuration,
                                    kSearchTickDurationMs, 6000u)
                  .has_value(),
          "modifier window beyond horizon is clamped silently");
    const BlockId evaluator = program.find(*program.script())->evaluator;
    program.setFieldValue(evaluator, "maxTimeMs", "7000");
    compiled = CompileProgram(program);
    Check(compiled.ok, "compiles with late evaluation window");
    const auto error = ValidateSearchComponents(
            compiled.configuration, kSearchTickDurationMs, 6000u);
    Check(error.has_value(), "evaluation window beyond horizon rejected");
}

}  // namespace

int main() {
    TestCatalogAtoms();
    TestFieldSchemasMatchOptions();
    TestProgramOperations();
    TestCompileDefaultProgram();
    TestLoweringEquivalence();
    TestCompileStructureErrors();
    TestCompileReporterExpressions();
    TestTextRoundTrip();
    TestTextExpressions();
    TestLegacyTextParsing();
    TestTextParseErrors();
    TestJsonRoundTrip();
    TestJsonVersionOneMigration();
    TestNumberFormatting();
    TestValidationParity();
    if (failures != 0) {
        std::fprintf(stderr, "block program tests: %d failure(s)\n",
                     failures);
        return 1;
    }
    std::printf("block program tests passed\n");
    return 0;
}
