#ifndef FOREVERTAS_BLOCKS_VISUAL_CATALOG_H
#define FOREVERTAS_BLOCKS_VISUAL_CATALOG_H

#include "blocks/visual_program.h"

#include <string>
#include <vector>

namespace forevertas::blocks {

struct VisualInputDefinition {
    std::string key;
    std::string label;
    VisualValueType type = VisualValueType::None;
    std::string defaultBlockId;
    std::string defaultValue;
};

struct VisualFieldDefinition {
    enum class Kind { Number, Integer, Boolean, Enum, Text };

    std::string key;
    std::string label;
    Kind kind = Kind::Text;
    std::string defaultValue;
    std::vector<std::pair<std::string, std::string>> enumValues;
};

struct VisualStatementDefinition {
    std::string key;
    std::string label;
    std::string family;
};

struct VisualBlockDefinition {
    std::string id;
    std::string blocklyType;
    std::string categoryId;
    std::string label;
    VisualBlockShape shape = VisualBlockShape::Reporter;
    VisualValueType outputType = VisualValueType::None;
    // Whether to offer this primitive in the editor toolbox.
    bool toolboxVisible = true;
    // Multi-parameter process/objective blocks can deliberately use separate
    // compact rows instead of forcing nested reporters into one very wide row.
    bool inputsInline = true;
    // Blockly statement-connection family for command/control blocks. Empty
    // means the block is not connectable as a statement.
    std::string statementFamily;
    // Optional bridge-owned viewer integration. Blockly only uses this as an
    // affordance hint; native C++ still validates the resulting workspace.
    std::string viewerPicker;
    std::vector<VisualInputDefinition> inputs;
    std::vector<VisualFieldDefinition> fields;
    std::vector<VisualStatementDefinition> statements;
};

struct VisualCategory {
    std::string id;
    std::string label;
    std::string color;
};

const std::vector<VisualCategory> &VisualCategories();
const std::vector<VisualBlockDefinition> &VisualBlockCatalog();
const VisualBlockDefinition *FindVisualBlock(const std::string &id);
const VisualBlockDefinition *FindVisualBlockByBlocklyType(const std::string &type);

std::string VisualBlocklyType(const std::string &definitionId);

// Calls have one socket per named parameter, rather than a fixed argument
// count. The signature is stored with the call so disconnected calls remain
// editable; executable validation checks it against the definition.
std::vector<std::string> VisualProcedureParameters(const VisualNode &node);
std::vector<VisualInputDefinition> VisualInputsForNode(const VisualNode &node);

}  // namespace forevertas::blocks

#endif
