#include "blocks/block_lowering.h"

#include "blocks/block_catalog.h"
#include "blocks/block_value.h"
#include "searches/algorithm_registry.h"

#include <algorithm>
#include <set>

namespace forevertas::blocks {
namespace {

using AtomEntry = std::pair<std::string, AtomFieldValues>;

std::string SettingValue(const OptionSettings &settings,
                         const std::string &key,
                         const std::string &fallback) {
    const auto found = settings.find(key);
    return found == settings.end() ? fallback : found->second;
}

// Applies one value per key, recording an actionable error when two atoms
// of the same window disagree on a shared key. The first value wins so the
// assembled settings stay deterministic while all conflicts accumulate.
class SettingsOverlay final {
public:
    SettingsOverlay(OptionSettings settings,
                    std::vector<std::string> &errors,
                    std::string context)
        : settings_(std::move(settings)),
          errors_(errors),
          context_(std::move(context)) {}

    void apply(const std::string &key, const std::string &value) {
        const auto existing = applied_.find(key);
        if (existing != applied_.end()) {
            if (existing->second != value) {
                errors_.push_back(
                        "Two blocks in the same mutation window set '" +
                        key + "' differently; use another mutation window." +
                        context_);
            }
            return;
        }
        applied_.emplace(key, value);
        settings_[key] = value;
    }

    void apply(const AtomFieldValues &values) {
        for (const auto &[key, value] : values) {
            apply(key, value);
        }
    }

    const OptionSettings &settings() const { return settings_; }

private:
    OptionSettings settings_;
    std::vector<std::string> &errors_;
    std::string context_;
    std::map<std::string, std::string> applied_;
};

void OverlayWindow(OptionSettings &settings, const AtomFieldValues &window) {
    for (const char *key : {"minTimeMs", "maxTimeMs", "seed"}) {
        const auto found = window.find(key);
        if (found != window.end()) settings[key] = found->second;
    }
}

std::string AtomLabel(const std::string &definitionId) {
    const BlockDefinition *const definition = FindBlock(definitionId);
    return definition == nullptr ? definitionId : definition->label;
}

// --- mutation assemblers ---------------------------------------------------
//
// Each assembler derives channel flags and neutralizes the multi-feature
// defaults for absent atoms first (plain assignments, overridable by atom
// values), then applies the atom field values in order through the
// conflict-tracking overlay: two atoms of the same window disagreeing on
// a shared key is an error.

void AssemblePerturbation(const std::vector<const AtomEntry *> &atoms,
                          OptionSettings &settings,
                          std::vector<std::string> &errors) {
    bool shift = false;
    bool nudge = false;
    bool set = false;
    bool flipAccelerate = false;
    bool flipBrake = false;
    for (const AtomEntry *const atom : atoms) {
        if (atom->first == "mutate/shift-events") shift = true;
        else if (atom->first == "mutate/nudge-steering") nudge = true;
        else if (atom->first == "mutate/set-steering") set = true;
        else if (atom->first == "mutate/flip-accelerate") flipAccelerate = true;
        else if (atom->first == "mutate/flip-brake") flipBrake = true;
    }
    if (nudge && set) {
        errors.push_back("Blocks '" + AtomLabel("mutate/nudge-steering") +
                         "' and '" + AtomLabel("mutate/set-steering") +
                         "' cannot share a mutation window.");
    }
    if (!shift) settings["maxTimeShiftMs"] = "0";
    if (!nudge && !set) {
        settings["steerDeltaMin"] = "0";
        settings["steerDeltaMax"] = "0";
    }
    if (!flipAccelerate) settings["toggleAccelerate"] = "false";
    if (!flipBrake) settings["toggleBrake"] = "false";
    if (set) settings["steerMode"] = "absolute";
    SettingsOverlay overlay(std::move(settings), errors, std::string());
    for (const AtomEntry *const atom : atoms) {
        overlay.apply(atom->second);
    }
    settings = overlay.settings();
}

void AssembleInsertion(const std::vector<const AtomEntry *> &atoms,
                       OptionSettings &settings,
                       std::vector<std::string> &errors) {
    bool absolute = false;
    bool offset = false;
    bool accelerate = false;
    bool brake = false;
    for (const AtomEntry *const atom : atoms) {
        if (atom->first == "mutate/insert-steering-at") absolute = true;
        else if (atom->first == "mutate/adjust-steering-by") offset = true;
        else if (atom->first == "mutate/press-accelerate") accelerate = true;
        else if (atom->first == "mutate/press-brake") brake = true;
    }
    if (absolute && offset) {
        errors.push_back(
                "Blocks '" + AtomLabel("mutate/adjust-steering-by") +
                "' and '" + AtomLabel("mutate/insert-steering-at") +
                "' cannot share a mutation window.");
    }
    // Channel enablement is derived from which atoms are present, so the
    // legacy per-channel flags never appear as block fields.
    settings["steerEnabled"] = absolute || offset ? "true" : "false";
    settings["accelerateEnabled"] = accelerate ? "true" : "false";
    settings["brakeEnabled"] = brake ? "true" : "false";
    if (absolute) settings["steerMode"] = "absolute";
    SettingsOverlay overlay(std::move(settings), errors, std::string());
    for (const AtomEntry *const atom : atoms) {
        overlay.apply(atom->second);
    }
    settings = overlay.settings();
}

void AssembleDeletion(const std::vector<const AtomEntry *> &atoms,
                      OptionSettings &settings,
                      std::vector<std::string> &errors) {
    bool steer = false;
    bool accelerate = false;
    bool brake = false;
    for (const AtomEntry *const atom : atoms) {
        if (atom->first == "mutate/delete-steering") steer = true;
        else if (atom->first == "mutate/delete-accelerate") accelerate = true;
        else if (atom->first == "mutate/delete-brake") brake = true;
    }
    settings["steerEnabled"] = steer ? "true" : "false";
    settings["accelerateEnabled"] = accelerate ? "true" : "false";
    settings["brakeEnabled"] = brake ? "true" : "false";
    SettingsOverlay overlay(std::move(settings), errors, std::string());
    for (const AtomEntry *const atom : atoms) {
        overlay.apply(atom->second);
    }
    settings = overlay.settings();
}

void AssembleDirect(const std::vector<const AtomEntry *> &atoms,
                    OptionSettings &settings,
                    std::vector<std::string> &errors) {
    SettingsOverlay overlay(std::move(settings), errors, std::string());
    for (const AtomEntry *const atom : atoms) {
        overlay.apply(atom->second);
    }
    settings = overlay.settings();
}

// --- migration helpers -----------------------------------------------------

AtomFieldValues PickFields(const OptionSettings &settings,
                           const std::string &definitionId) {
    AtomFieldValues fields;
    const BlockDefinition *const definition = FindBlock(definitionId);
    if (definition == nullptr) return fields;
    for (const OptionField &field : definition->fields) {
        const auto found = settings.find(field.key);
        if (found != settings.end()) {
            fields.emplace(field.key, found->second);
        }
    }
    return fields;
}

OptionSettings PickWindow(const OptionSettings &settings) {
    OptionSettings window;
    for (const char *key : {"minTimeMs", "maxTimeMs", "seed"}) {
        const auto found = settings.find(key);
        if (found != settings.end()) window.emplace(key, found->second);
    }
    return window;
}

}  // namespace

MutationGroupLowering LowerMutationGroup(
        const std::vector<AtomEntry> &atoms, const AtomFieldValues &window) {
    MutationGroupLowering result;
    if (atoms.empty()) {
        result.errors.push_back(
                "Add at least one mutation block inside the mutation window.");
        return result;
    }

    std::vector<std::string> optionOrder;
    std::map<std::string, std::vector<const AtomEntry *>> partitions;
    for (const AtomEntry &atom : atoms) {
        const BlockDefinition *const definition = FindBlock(atom.first);
        if (definition == nullptr || definition->shape != BlockShape::Stack ||
            definition->optionKind != "mutation" ||
            definition->optionId.empty()) {
            result.errors.push_back(
                    "Only mutation blocks may run inside a mutation window.");
            continue;
        }
        if (partitions.find(definition->optionId) == partitions.end()) {
            optionOrder.push_back(definition->optionId);
        }
        partitions[definition->optionId].push_back(&atom);
    }

    for (const std::string &optionId : optionOrder) {
        const ModifierRegistration *const registration =
                FindModifier(optionId);
        if (registration == nullptr) {
            result.errors.push_back(
                    "The mutation window contains an unregistered block.");
            continue;
        }
        OptionSettings settings = registration->defaultSettings;
        OverlayWindow(settings, window);
        const std::vector<const AtomEntry *> &partitionAtoms =
                partitions[optionId];
        if (optionId == kExistingEventPerturbationModifierId) {
            AssemblePerturbation(partitionAtoms, settings, result.errors);
        } else if (optionId == kInputInsertionModifierId) {
            AssembleInsertion(partitionAtoms, settings, result.errors);
        } else if (optionId == kInputDeletionModifierId) {
            AssembleDeletion(partitionAtoms, settings, result.errors);
        } else {
            AssembleDirect(partitionAtoms, settings, result.errors);
        }
        result.configurations.push_back(
                OptionConfiguration{optionId, std::move(settings)});
    }
    return result;
}

OptionAtomLowering LowerSearchAtom(const std::string &definitionId,
                                   const AtomFieldValues &values) {
    OptionAtomLowering result;
    const BlockDefinition *const definition = FindBlock(definitionId);
    if (definition == nullptr || definition->optionKind != "search") {
        result.error = "The script must start with a search block.";
        return result;
    }
    const SearchAlgorithmRegistration *const registration =
            FindSearchAlgorithm(definition->optionId);
    if (registration == nullptr) {
        result.error = "The script must start with a search block.";
        return result;
    }
    result.settings = registration->defaultSettings;
    for (const auto &[key, value] : values) {
        result.settings[key] = value;
    }
    result.optionId = definition->optionId;
    return result;
}

OptionAtomLowering LowerEvaluationAtom(const std::string &definitionId,
                                       const AtomFieldValues &values) {
    OptionAtomLowering result;
    const BlockDefinition *const definition = FindBlock(definitionId);
    if (definition == nullptr || definition->optionKind != "evaluation") {
        result.error = "Plug an evaluation block into the search block.";
        return result;
    }
    const EvaluationTargetRegistration *const registration =
            FindEvaluationTarget(definition->optionId);
    if (registration == nullptr) {
        result.error = "Plug an evaluation block into the search block.";
        return result;
    }
    result.settings = registration->defaultSettings;
    for (const auto &[key, value] : values) {
        result.settings[key] = value;
    }
    if (definitionId == "evaluate/speed-toward") {
        result.settings["mode"] = "projected";
        // The alignment gate is a property of the direction goal: a
        // threshold of -100 accepts every direction, matching a disabled
        // gate.
        bool gated = false;
        const auto threshold = values.find("minAlignmentPercent");
        if (threshold != values.end()) {
            if (const auto parsed = ParseNumberValue(threshold->second)) {
                gated = *parsed > -100.0;
            }
        }
        result.settings["alignmentEnabled"] = gated ? "true" : "false";
    }
    result.optionId = definition->optionId;
    return result;
}

ModifierExpansion ExpandModifierAtoms(const OptionConfiguration &modifier) {
    ModifierExpansion expansion;
    const ModifierRegistration *const registration =
            FindModifier(modifier.id);
    if (registration == nullptr) return expansion;
    expansion.window = PickWindow(modifier.settings);
    const OptionSettings &settings = modifier.settings;
    if (modifier.id == kRandomSteeringModifierId) {
        expansion.atoms.push_back({"mutate/reroll-steering", {}});
    } else if (modifier.id == kSmoothSteeringModifierId) {
        expansion.atoms.push_back({"mutate/smooth-steering",
                                   PickFields(settings, "mutate/smooth-steering")});
    } else if (modifier.id == kExistingEventPerturbationModifierId) {
        expansion.atoms.push_back(
                {"mutate/shift-events",
                 PickFields(settings, "mutate/shift-events")});
        if (SettingValue(settings, "steerMode", "delta") == "absolute") {
            expansion.atoms.push_back(
                    {"mutate/set-steering",
                     PickFields(settings, "mutate/set-steering")});
        } else {
            expansion.atoms.push_back(
                    {"mutate/nudge-steering",
                     PickFields(settings, "mutate/nudge-steering")});
        }
        if (SettingValue(settings, "toggleAccelerate", "false") == "true") {
            expansion.atoms.push_back(
                    {"mutate/flip-accelerate",
                     PickFields(settings, "mutate/flip-accelerate")});
        }
        if (SettingValue(settings, "toggleBrake", "false") == "true") {
            expansion.atoms.push_back(
                    {"mutate/flip-brake",
                     PickFields(settings, "mutate/flip-brake")});
        }
    } else if (modifier.id == kInputInsertionModifierId) {
        if (SettingValue(settings, "steerEnabled", "false") == "true") {
            const std::string definitionId =
                    SettingValue(settings, "steerMode", "offset") == "absolute"
                            ? "mutate/insert-steering-at"
                            : "mutate/adjust-steering-by";
            expansion.atoms.push_back(
                    {definitionId, PickFields(settings, definitionId)});
        }
        if (SettingValue(settings, "accelerateEnabled", "false") == "true") {
            expansion.atoms.push_back(
                    {"mutate/press-accelerate",
                     PickFields(settings, "mutate/press-accelerate")});
        }
        if (SettingValue(settings, "brakeEnabled", "false") == "true") {
            expansion.atoms.push_back(
                    {"mutate/press-brake",
                     PickFields(settings, "mutate/press-brake")});
        }
    } else if (modifier.id == kInputDeletionModifierId) {
        if (SettingValue(settings, "steerEnabled", "false") == "true") {
            expansion.atoms.push_back(
                    {"mutate/delete-steering",
                     PickFields(settings, "mutate/delete-steering")});
        }
        if (SettingValue(settings, "accelerateEnabled", "false") == "true") {
            expansion.atoms.push_back(
                    {"mutate/delete-accelerate",
                     PickFields(settings, "mutate/delete-accelerate")});
        }
        if (SettingValue(settings, "brakeEnabled", "false") == "true") {
            expansion.atoms.push_back(
                    {"mutate/delete-brake",
                     PickFields(settings, "mutate/delete-brake")});
        }
    }
    return expansion;
}

AtomExpansion ExpandEvaluationAtom(const OptionConfiguration &evaluation) {
    const EvaluationTargetRegistration *const registration =
            FindEvaluationTarget(evaluation.id);
    if (registration == nullptr) return {};
    const OptionSettings &settings = evaluation.settings;
    if (evaluation.id == kPreciseFinishTimeEvaluationId) {
        return {"evaluate/finish-time", {}};
    }
    if (evaluation.id == kStuntPointsEvaluationId) {
        return {"evaluate/stunt-points",
                PickFields(settings, "evaluate/stunt-points")};
    }
    if (evaluation.id == kVelocityEvaluationId) {
        const bool projected =
                SettingValue(settings, "mode", "total") == "projected";
        const bool gated =
                SettingValue(settings, "alignmentEnabled", "false") == "true";
        if (projected || gated) {
            AtomFieldValues fields =
                    PickFields(settings, "evaluate/speed-toward");
            if (!gated) fields["minAlignmentPercent"] = "-100";
            return {"evaluate/speed-toward", std::move(fields)};
        }
        return {"evaluate/speed", PickFields(settings, "evaluate/speed")};
    }
    if (evaluation.id == kPointTargetEvaluationId) {
        return {"evaluate/distance-to-point",
                PickFields(settings, "evaluate/distance-to-point")};
    }
    if (evaluation.id == kPoseTargetEvaluationId) {
        return {"evaluate/distance-to-pose",
                PickFields(settings, "evaluate/distance-to-pose")};
    }
    if (evaluation.id == kVolumeEntryEvaluationId) {
        return {"evaluate/box-entry-time",
                PickFields(settings, "evaluate/box-entry-time")};
    }
    if (evaluation.id == kCustomVolumeEntryEvaluationId) {
        return {"evaluate/prism-entry-time",
                PickFields(settings, "evaluate/prism-entry-time")};
    }
    return {};
}

BlockProgram BuildProgramFromComponents(
        const SearchComponentConfiguration &components) {
    BlockProgram program;
    std::string hatDefinition =
            SearchAtomDefinitionForOption(components.searchAlgorithm.id);
    if (hatDefinition.empty()) {
        hatDefinition = SearchAtomDefinitionForOption(
                DefaultSearchAlgorithmConfiguration().id);
    }
    const BlockId hat = program.createBlock(hatDefinition);
    for (const auto &[key, value] : components.searchAlgorithm.settings) {
        program.setFieldValue(hat, key, value);
    }
    AtomExpansion evaluator = ExpandEvaluationAtom(
            components.evaluationTarget);
    if (evaluator.definitionId.empty()) {
        evaluator = ExpandEvaluationAtom(
                DefaultEvaluationTargetConfiguration());
    }
    const BlockId evaluatorId =
            program.createBlock(evaluator.definitionId, evaluator.fields);
    program.setEvaluator(hat, evaluatorId);
    for (const OptionConfiguration &modifier : components.modifiers) {
        const ModifierExpansion expansion = ExpandModifierAtoms(modifier);
        if (expansion.atoms.empty()) continue;
        const BlockId group =
                program.createBlock("mutate/window", expansion.window);
        for (const AtomExpansion &atom : expansion.atoms) {
            const BlockId block =
                    program.createBlock(atom.definitionId, atom.fields);
            program.appendToSubstack(group, block);
        }
        program.appendToSubstack(hat, group);
    }
    program.setScript(hat);
    return program;
}

std::string SearchAtomDefinitionForOption(const std::string &optionId) {
    const SearchAlgorithmRegistration *const registration =
            FindSearchAlgorithm(optionId);
    return registration == nullptr ? std::string()
                                   : "search/" + registration->id;
}

std::string EvaluationAtomDefinitionForOption(const std::string &optionId) {
    const EvaluationTargetRegistration *const registration =
            FindEvaluationTarget(optionId);
    if (registration == nullptr) return std::string();
    if (registration->id == kPreciseFinishTimeEvaluationId) {
        return "evaluate/finish-time";
    }
    if (registration->id == kStuntPointsEvaluationId) {
        return "evaluate/stunt-points";
    }
    if (registration->id == kVelocityEvaluationId) {
        return "evaluate/speed";
    }
    if (registration->id == kPointTargetEvaluationId) {
        return "evaluate/distance-to-point";
    }
    if (registration->id == kPoseTargetEvaluationId) {
        return "evaluate/distance-to-pose";
    }
    if (registration->id == kVolumeEntryEvaluationId) {
        return "evaluate/box-entry-time";
    }
    if (registration->id == kCustomVolumeEntryEvaluationId) {
        return "evaluate/prism-entry-time";
    }
    return std::string();
}

}  // namespace forevertas::blocks
