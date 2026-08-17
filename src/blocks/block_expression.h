#ifndef FOREVERTAS_BLOCKS_BLOCK_EXPRESSION_H
#define FOREVERTAS_BLOCKS_BLOCK_EXPRESSION_H

#include "blocks/block_program.h"

#include <optional>
#include <string>

namespace forevertas::blocks {

// Evaluates one number slot: the grafted reporter tree when present, the
// literal field value otherwise. Returns nullopt when the expression is
// invalid (unknown block, unparseable number, cycle, division by zero).
std::optional<std::string> EvaluateSlotValue(const BlockProgram &program,
                                             const BlockNode &node,
                                             const std::string &key);

}  // namespace forevertas::blocks

#endif
