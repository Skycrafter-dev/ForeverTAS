#ifndef FOREVERTAS_BLOCKS_BLOCK_LOWERING_H
#define FOREVERTAS_BLOCKS_BLOCK_LOWERING_H

#include "blocks/block_compiler.h"
#include "blocks/block_program.h"
#include "searches/option_configuration.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace forevertas::blocks {

// Field values compiled from one atom block (number reporters already
// evaluated), keyed by the atom's field key.
using AtomFieldValues = OptionSettings;

// ---------------------------------------------------------------------------
// Atoms -> registered options (compilation)
// ---------------------------------------------------------------------------

struct MutationGroupLowering {
    // One configuration per registered option touched by the group's atoms,
    // in first-appearance order. Atoms bound to the same option merge into
    // one configuration, which is what makes a migrated window behave
    // exactly like the legacy multi-feature block it came from.
    std::vector<OptionConfiguration> configurations;
    std::vector<std::string> errors;
};

// Lowers the atoms of one mutation window. `atoms` pairs each atom's
// definition id with its compiled own-field values in substack order;
// `window` carries the compiled minTimeMs/maxTimeMs/seed shared by every
// atom in the group.
MutationGroupLowering LowerMutationGroup(
        const std::vector<std::pair<std::string, AtomFieldValues>> &atoms,
        const AtomFieldValues &window);

// Lowers one search or evaluation atom to its registered option.
struct OptionAtomLowering {
    std::string optionId;
    OptionSettings settings;
    std::string error;
};
OptionAtomLowering LowerSearchAtom(const std::string &definitionId,
                                   const AtomFieldValues &values);
OptionAtomLowering LowerEvaluationAtom(const std::string &definitionId,
                                       const AtomFieldValues &values);

// ---------------------------------------------------------------------------
// Registered options -> atoms (migration)
// ---------------------------------------------------------------------------

struct AtomExpansion {
    std::string definitionId;
    OptionSettings fields;
};

// One mutation window rebuilt from a legacy modifier configuration.
struct ModifierExpansion {
    OptionSettings window;  // minTimeMs / maxTimeMs / seed
    std::vector<AtomExpansion> atoms;
};

// Expands a legacy modifier configuration into the atom composition that
// recompiles to it byte for byte. Returns an empty expansion when the
// option id is unknown.
ModifierExpansion ExpandModifierAtoms(const OptionConfiguration &modifier);

// Expands a legacy evaluation configuration into one evaluation atom.
// Returns a null expansion (empty definitionId) when unknown.
AtomExpansion ExpandEvaluationAtom(const OptionConfiguration &evaluation);

// Rebuilds an editable atom program from compiled components: the inverse
// of CompileProgram for migrated programs. Unknown option ids fall back to
// the registered defaults.
BlockProgram BuildProgramFromComponents(
        const SearchComponentConfiguration &components);

// Resolves registered option ids (including legacy aliases) to the
// canonical atom definition ids used by the workspace.
std::string SearchAtomDefinitionForOption(const std::string &optionId);
std::string EvaluationAtomDefinitionForOption(const std::string &optionId);

}  // namespace forevertas::blocks

#endif
