#ifndef FOREVERTAS_BLOCKS_BLOCK_CATALOG_H
#define FOREVERTAS_BLOCKS_BLOCK_CATALOG_H

#include "searches/option_fields.h"

#include <string>
#include <vector>

namespace forevertas::blocks {

// Visual and semantic shape of a block, following the Scratch vocabulary.
enum class BlockShape {
    Hat,       // a program root: owns one evaluation slot and a mutation
               // substack of window blocks; binds to a registered search
               // algorithm
    Container, // a mutation window: owns the shared from/to times and seed
               // plus a substack of mutation atoms
    Stack,     // a mutation atom: one input operation, snapped inside a
               // mutation window
    Reporter   // plugs into a typed slot or the hat's evaluation slot;
               // either an evaluation goal or a value expression
};

struct BlockDefinition {
    std::string id;
    std::string categoryId;  // search | evaluate | mutate | values
    std::string label;
    BlockShape shape = BlockShape::Reporter;
    // Output port type: "number" for value reporters, "evaluation" for
    // evaluation goals, empty for hat/container/stack blocks.
    std::string outputType;
    OptionFieldList fields;
    // Binding to the registered option the block lowers to; optionKind is
    // "search", "mutation", or "evaluation" and optionId is the registry
    // id. Empty for value primitives.
    std::string optionKind;
    std::string optionId;
    // Optional rich QML detail component (for example a target picker).
    std::string settingsComponent;
};

// The single registry of block kinds. Every block does one thing with one
// small configuration; the compiler lowers them to registered options
// (see block_lowering.h). Registered options are never blocks themselves.
const std::vector<BlockDefinition> &BlockCatalog();

const BlockDefinition *FindBlock(const std::string &id);

// Category presentation shared by the palette and the workspace.
struct BlockCategory {
    std::string id;
    std::string label;
    std::string color;
};

const std::vector<BlockCategory> &BlockCategories();

const BlockCategory *FindBlockCategory(const std::string &id);

}  // namespace forevertas::blocks

#endif
