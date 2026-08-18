#ifndef FOREVERTAS_BLOCKS_VISUAL_COMPILER_H
#define FOREVERTAS_BLOCKS_VISUAL_COMPILER_H

#include "blocks/block_compiler.h"
#include "blocks/visual_program.h"

namespace forevertas::blocks {

// Compiles the compositional v3 visual language to the existing category-
// neutral engine configuration. The runtime continues to receive the same
// optimized evaluator/mutator registrations; only the editor language changes.
CompileResult CompileVisualProgram(
        const VisualProgram &program,
        const SearchComponentConfiguration &baseConfiguration);

}  // namespace forevertas::blocks

#endif
