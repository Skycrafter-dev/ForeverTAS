#ifndef FOREVERTAS_BLOCKS_BLOCK_COMPILER_H
#define FOREVERTAS_BLOCKS_BLOCK_COMPILER_H

#include "blocks/block_program.h"
#include "searches/option_configuration.h"

#include <optional>
#include <string>
#include <vector>

namespace forevertas::blocks {

// The compiled, category-neutral search description consumed by the
// application and RunSearch. Atomic blocks lower to registered options on
// the way out (see block_lowering.h); this is the only shape the engine
// ever sees.
struct SearchComponentConfiguration {
    OptionConfiguration searchAlgorithm;
    std::vector<OptionConfiguration> modifiers;
    OptionConfiguration evaluationTarget;
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
