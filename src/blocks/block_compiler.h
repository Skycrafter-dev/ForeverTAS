#ifndef FOREVERTAS_BLOCKS_BLOCK_COMPILER_H
#define FOREVERTAS_BLOCKS_BLOCK_COMPILER_H

#include "blocks/block_program.h"
#include "conditions/condition_program.h"
#include "searches/option_configuration.h"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace forevertas::blocks {

// The compiled, category-neutral search description consumed by the
// application and RunSearch. Atomic blocks lower to registered options on
// the way out (see block_lowering.h); this is the only shape the engine
// ever sees.
struct SearchComponentConfiguration {
    SearchComponentConfiguration() = default;
    SearchComponentConfiguration(
            OptionConfiguration search,
            std::vector<OptionConfiguration> modifierList,
            OptionConfiguration evaluation,
            std::vector<std::size_t> windowGroups = {})
        : searchAlgorithm(std::move(search)),
          modifiers(std::move(modifierList)),
          evaluationTarget(std::move(evaluation)),
          modifierWindowGroups(std::move(windowGroups)) {}

    OptionConfiguration searchAlgorithm;
    std::vector<OptionConfiguration> modifiers;
    OptionConfiguration evaluationTarget;
    // Optional compiler metadata used by app-level operations that act once
    // per visual mutation window (not once per lowered native modifier). It
    // does not affect search semantics and is intentionally excluded from
    // equality so legacy/native round-trips remain category-neutral.
    std::vector<std::size_t> modifierWindowGroups;
};

inline bool operator==(const SearchComponentConfiguration &lhs,
                       const SearchComponentConfiguration &rhs) {
    return lhs.searchAlgorithm == rhs.searchAlgorithm &&
            lhs.modifiers == rhs.modifiers &&
            lhs.evaluationTarget == rhs.evaluationTarget;
}

struct CompileResult {
    bool ok = false;
    SearchComponentConfiguration configuration;
    // Visual-language execution metadata that is not a registered search
    // component. The v3 editor uses this so process steps such as simulation
    // are explicit in the program instead of remaining hidden app settings.
    std::optional<std::string> simulationHorizonMs;
    // Per-tick filter owned by the visual simulate step. It is compiled
    // directly to the shared CPU/CUDA condition bytecode instead of being
    // round-tripped through the legacy condition-script text format.
    std::optional<ConditionProgram> conditionProgram;
    std::vector<std::string> errors;
};

// Compiles the program's script into search components. Field values come
// from literals or evaluated number reporters; each mutation window lowers
// to the registered options its atoms bind to.
CompileResult CompileProgram(const BlockProgram &program);

// Validates compiled components against a tick duration and simulation
// horizon: registry validation, mutation windows, and the evaluation plan.
// Returns the first human-readable error, or nullopt.
std::optional<std::string> ValidateSearchComponents(
        const SearchComponentConfiguration &configuration,
        std::uint32_t tickDurationMs,
        std::uint32_t simulationHorizonMs);

}  // namespace forevertas::blocks

#endif
