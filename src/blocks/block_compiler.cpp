#include "blocks/block_compiler.h"

#include "blocks/block_catalog.h"
#include "blocks/block_expression.h"
#include "blocks/block_lowering.h"
#include "input_timeline_time.h"
#include "searches/algorithm_registry.h"

#include <limits>
#include <memory>

namespace forevertas::blocks {
namespace {

// Compiles one atom's own fields, evaluating number reporters.
AtomFieldValues CompileSettings(const BlockProgram &program,
                               const BlockNode &node,
                               const BlockDefinition &definition,
                               std::vector<std::string> &errors) {
    AtomFieldValues settings;
    for (const OptionField &field : definition.fields) {
        const auto value = EvaluateSlotValue(program, node, field.key);
        if (!value) {
            errors.push_back("Field '" + field.label +
                             "' has an invalid number expression.");
            settings.emplace(field.key, field.defaultValue);
            continue;
        }
        settings.emplace(field.key, *value);
    }
    return settings;
}

}  // namespace

CompileResult CompileProgram(const BlockProgram &program) {
    CompileResult result;
    if (!program.script() || program.find(*program.script()) == nullptr) {
        result.errors.push_back("Add a search block to define the program.");
        return result;
    }
    const BlockNode &hat = *program.find(*program.script());
    const BlockDefinition *const hatDefinition =
            FindBlock(hat.definitionId);
    if (hatDefinition == nullptr || hatDefinition->shape != BlockShape::Hat) {
        result.errors.push_back("The script must start with a search block.");
        return result;
    }

    if (hat.evaluator == 0) {
        result.errors.push_back(
                "Plug an evaluation block into the search block.");
        return result;
    }
    const BlockNode *const evaluator = program.find(hat.evaluator);
    const BlockDefinition *const evaluatorDefinition =
            evaluator == nullptr ? nullptr
                                 : FindBlock(evaluator->definitionId);
    if (evaluatorDefinition == nullptr ||
        evaluatorDefinition->optionKind != "evaluation") {
        result.errors.push_back(
                "Plug an evaluation block into the search block.");
        return result;
    }

    if (hat.substack.empty()) {
        result.errors.push_back(
                "Add at least one mutation window block to the search.");
        return result;
    }

    const OptionAtomLowering searchLowering = LowerSearchAtom(
            hat.definitionId,
            CompileSettings(program, hat, *hatDefinition, result.errors));
    if (!searchLowering.error.empty()) {
        result.errors.push_back(searchLowering.error);
    } else {
        result.configuration.searchAlgorithm = OptionConfiguration{
                searchLowering.optionId, searchLowering.settings};
    }
    const OptionAtomLowering evaluationLowering = LowerEvaluationAtom(
            evaluator->definitionId,
            CompileSettings(
                    program, *evaluator, *evaluatorDefinition, result.errors));
    if (!evaluationLowering.error.empty()) {
        result.errors.push_back(evaluationLowering.error);
    } else {
        result.configuration.evaluationTarget = OptionConfiguration{
                evaluationLowering.optionId, evaluationLowering.settings};
    }

    for (const BlockId windowId : hat.substack) {
        const BlockNode *const window = program.find(windowId);
        const BlockDefinition *const windowDefinition =
                window == nullptr ? nullptr
                                  : FindBlock(window->definitionId);
        if (windowDefinition == nullptr ||
            windowDefinition->shape != BlockShape::Container) {
            result.errors.push_back(
                    "Only mutation window blocks may run inside the search.");
            continue;
        }
        const AtomFieldValues windowValues = CompileSettings(
                program, *window, *windowDefinition, result.errors);
        std::vector<std::pair<std::string, AtomFieldValues>> atoms;
        for (const BlockId atomId : window->substack) {
            const BlockNode *const atom = program.find(atomId);
            const BlockDefinition *const atomDefinition =
                    atom == nullptr ? nullptr
                                    : FindBlock(atom->definitionId);
            if (atomDefinition == nullptr ||
                atomDefinition->shape != BlockShape::Stack ||
                atomDefinition->optionKind != "mutation") {
                result.errors.push_back(
                        "Only mutation blocks may run inside a mutation "
                        "window.");
                continue;
            }
            atoms.emplace_back(
                    atom->definitionId,
                    CompileSettings(
                            program, *atom, *atomDefinition, result.errors));
        }
        const MutationGroupLowering lowered =
                LowerMutationGroup(atoms, windowValues);
        for (const std::string &error : lowered.errors) {
            result.errors.push_back(error);
        }
        for (const OptionConfiguration &configuration :
             lowered.configurations) {
            result.configuration.modifiers.push_back(configuration);
        }
    }
    if (!result.errors.empty()) return result;
    result.ok = true;
    return result;
}

std::optional<std::string> ValidateSearchComponents(
        const SearchComponentConfiguration &configuration,
        std::uint32_t tickDurationMs,
        std::uint32_t simulationHorizonMs) {
    const SearchAlgorithmRegistration *const searchRegistration =
            FindSearchAlgorithm(configuration.searchAlgorithm.id);
    if (searchRegistration == nullptr) {
        return "Select a valid search algorithm.";
    }
    const EvaluationTargetRegistration *const evaluationRegistration =
            FindEvaluationTarget(configuration.evaluationTarget.id);
    if (evaluationRegistration == nullptr) {
        return "Select a valid evaluation target.";
    }
    if (const auto error = searchRegistration->validateSettings(
                configuration.searchAlgorithm.settings, tickDurationMs)) {
        return error;
    }
    if (const auto error = evaluationRegistration->validateSettings(
                configuration.evaluationTarget.settings, tickDurationMs)) {
        return error;
    }
    if (configuration.modifiers.empty()) {
        return "Add at least one input modifier block.";
    }

    std::int64_t earliestMutationTimeMs =
            std::numeric_limits<std::int64_t>::max();
    for (std::size_t index = 0; index < configuration.modifiers.size();
         ++index) {
        const OptionConfiguration &modifier =
                configuration.modifiers[index];
        const ModifierRegistration *const registration =
                FindModifier(modifier.id);
        if (registration == nullptr) {
            return "Modifier block " + std::to_string(index + 1) +
                   " has an invalid type.";
        }
        if (const auto error = registration->validateSettings(
                    modifier.settings, tickDurationMs)) {
            return "Modifier block " + std::to_string(index + 1) + ": " +
                    *error;
        }
        const OptionSettings executionSettings = ClampInputWindowToSimulationHorizon(
                modifier.settings, tickDurationMs, simulationHorizonMs);
        const std::unique_ptr<InputMutator> mutator =
                registration->create(executionSettings, tickDurationMs);
        earliestMutationTimeMs = std::min(
                earliestMutationTimeMs, mutator->EarliestMutationTimeMs());
    }
    const std::unique_ptr<IterationEvaluator> evaluator =
            evaluationRegistration->create(
                    configuration.evaluationTarget.settings, tickDurationMs);
    const EvaluationPlan plan = evaluator->Plan(
            simulationHorizonMs, earliestMutationTimeMs, tickDurationMs);
    if (plan.startTimeMs < earliestMutationTimeMs) {
        return "Evaluation start time " + std::to_string(plan.startTimeMs) +
               " ms precedes the first modifier time at " +
               std::to_string(earliestMutationTimeMs) + " ms.";
    }
    if (plan.endTimeMs > simulationHorizonMs) {
        return "Evaluation maximum time " + std::to_string(plan.endTimeMs) +
               " ms exceeds the Simulation horizon of " +
               std::to_string(simulationHorizonMs) + " ms.";
    }
    return std::nullopt;
}

}  // namespace forevertas::blocks
