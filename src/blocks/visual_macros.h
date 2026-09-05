#ifndef FOREVERTAS_BLOCKS_VISUAL_MACROS_H
#define FOREVERTAS_BLOCKS_VISUAL_MACROS_H

#include "blocks/visual_program.h"

namespace forevertas::blocks {

// Source templates, not executable block types. The editor inserts the start
// hat's body as an ordinary editable stack, with fresh variable names and IDs.
struct VisualMacro {
  std::string id, category, label, description;
  VisualProgram program;
};

const std::vector<VisualMacro> &VisualMacroCatalog();

} // namespace forevertas::blocks
#endif
