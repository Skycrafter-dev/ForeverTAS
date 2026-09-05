#ifndef FOREVERTAS_BLOCKS_VISUAL_COMPILER_H
#define FOREVERTAS_BLOCKS_VISUAL_COMPILER_H

#include "blocks/visual_program.h"
#include <memory>

namespace forevertas::blocks {

struct VisualCompileResult {
    bool ok = false;
    std::shared_ptr<const VisualProgram> executable;
    std::vector<std::string> errors;
};

// The visual language has one execution model: the graph itself.
VisualCompileResult CompileVisualProgram(const VisualProgram &program);

}  // namespace forevertas::blocks

#endif
