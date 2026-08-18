#include "blocks/visual_program.h"

#include "blocks/block_value.h"
#include "blocks/visual_catalog.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace forevertas::blocks {

const VisualNode *VisualProgram::find(VisualNodeId id) const {
  const auto found = nodes.find(id);
  return found == nodes.end() ? nullptr : &found->second;
}

VisualNode *VisualProgram::find(VisualNodeId id) {
  const auto found = nodes.find(id);
  return found == nodes.end() ? nullptr : &found->second;
}

std::string VisualValueTypeName(VisualValueType type) {
  switch (type) {
  case VisualValueType::None:
    return "none";
  case VisualValueType::Scalar:
    return "scalar";
  case VisualValueType::Number:
    return "number";
  case VisualValueType::Integer:
    return "integer";
  case VisualValueType::NumberRange:
    return "number range";
  case VisualValueType::IntegerRange:
    return "integer range";
  case VisualValueType::Milliseconds:
    return "milliseconds";
  case VisualValueType::Meters:
    return "meters";
  case VisualValueType::MetersPerSecond:
    return "meters/second";
  case VisualValueType::Degrees:
    return "degrees";
  case VisualValueType::Percent:
    return "percent";
  case VisualValueType::Boolean:
    return "boolean";
  case VisualValueType::Vector3:
    return "vector3";
  case VisualValueType::Position3:
    return "position3";
  case VisualValueType::Direction3:
    return "direction3";
  case VisualValueType::Rotation3:
    return "rotation3";
  case VisualValueType::Volume:
    return "volume";
  case VisualValueType::Polygon2:
    return "polygon2";
  case VisualValueType::TimeRange:
    return "time range";
  case VisualValueType::Score:
    return "score";
  }
  return "unknown";
}

bool VisualTypeCompatible(VisualValueType output, VisualValueType input) {
  if (output == input)
    return true;
  if (input == VisualValueType::Scalar) {
    return output == VisualValueType::Number ||
           output == VisualValueType::Integer ||
           output == VisualValueType::Milliseconds ||
           output == VisualValueType::Meters ||
           output == VisualValueType::MetersPerSecond ||
           output == VisualValueType::Degrees;
  }
  if (input == VisualValueType::Number) {
    return output == VisualValueType::Integer;
  }
  if (input == VisualValueType::Vector3) {
    return output == VisualValueType::Position3 ||
           output == VisualValueType::Direction3;
  }
  return false;
}

VisualProgramValidation ValidateVisualProgram(const VisualProgram &program,
                                              std::size_t maxNodes,
                                              std::size_t maxDepth) {
  VisualProgramValidation result;
  if (program.nodes.size() > maxNodes) {
    result.errors.push_back("Block program exceeds the node limit.");
    return result;
  }

  std::set<VisualNodeId> topLevel(program.topLevel.begin(),
                                  program.topLevel.end());
  if (topLevel.size() != program.topLevel.size()) {
    result.errors.push_back(
        "Block program contains duplicate top-level nodes.");
  }

  std::map<VisualNodeId, VisualNodeId> parent;
  for (const auto &[id, node] : program.nodes) {
    if (id == 0 || node.id != id) {
      result.errors.push_back("Block program contains an invalid node id.");
      continue;
    }
    const VisualBlockDefinition *const definition =
        FindVisualBlock(node.definitionId);
    if (definition == nullptr) {
      result.errors.push_back("Unknown block type '" + node.definitionId +
                              "'.");
      continue;
    }
    if (!std::isfinite(node.x) || !std::isfinite(node.y)) {
      result.errors.push_back("Block '" + definition->label +
                              "' has an invalid canvas position.");
    }

    for (const auto &entry : node.fields) {
      const std::string &key = entry.first;
      const std::string &value = entry.second;
      const auto field =
          std::find_if(definition->fields.begin(), definition->fields.end(),
                       [&key](const VisualFieldDefinition &candidate) {
                         return candidate.key == key;
                       });
      if (field == definition->fields.end()) {
        result.errors.push_back("Block '" + definition->label +
                                "' has unknown field '" + key + "'.");
        continue;
      }
      switch (field->kind) {
      case VisualFieldDefinition::Kind::Number:
        if (!ParseNumberValue(value)) {
          result.errors.push_back("Field '" + key + "' of '" +
                                  definition->label +
                                  "' must be a finite number.");
        }
        break;
      case VisualFieldDefinition::Kind::Integer: {
        const auto parsed = ParseNumberValue(value);
        if (!parsed || std::floor(*parsed) != *parsed) {
          result.errors.push_back("Field '" + key + "' of '" +
                                  definition->label + "' must be an integer.");
        }
        break;
      }
      case VisualFieldDefinition::Kind::Boolean:
        if (value != "true" && value != "false") {
          result.errors.push_back("Field '" + key + "' of '" +
                                  definition->label +
                                  "' must be true or false.");
        }
        break;
      case VisualFieldDefinition::Kind::Enum: {
        const bool known = std::any_of(
            field->enumValues.begin(), field->enumValues.end(),
            [&value](const auto &choice) { return choice.first == value; });
        if (!known) {
          result.errors.push_back("Field '" + key + "' of '" +
                                  definition->label +
                                  "' has an unknown option.");
        }
        break;
      }
      case VisualFieldDefinition::Kind::Text:
        if (value.size() > 64 * 1024) {
          result.errors.push_back("Field '" + key + "' of '" +
                                  definition->label + "' is too large.");
        }
        break;
      }
    }
    for (const auto &entry : node.inputs) {
      const std::string &key = entry.first;
      const VisualNodeId childId = entry.second;
      const VisualNode *const child = program.find(childId);
      if (child == nullptr) {
        result.errors.push_back("Block '" + definition->label +
                                "' references a missing input node.");
        continue;
      }
      const auto input =
          std::find_if(definition->inputs.begin(), definition->inputs.end(),
                       [&key](const VisualInputDefinition &candidate) {
                         return candidate.key == key;
                       });
      if (input == definition->inputs.end()) {
        result.errors.push_back("Block '" + definition->label +
                                "' has unknown input '" + key + "'.");
        continue;
      }
      const VisualBlockDefinition *const childDefinition =
          FindVisualBlock(child->definitionId);
      if (childDefinition != nullptr &&
          !VisualTypeCompatible(childDefinition->outputType, input->type)) {
        result.errors.push_back(
            "Input '" + key + "' of '" + definition->label + "' expects " +
            VisualValueTypeName(input->type) + ", not " +
            VisualValueTypeName(childDefinition->outputType) + ".");
      }
      const auto existing = parent.emplace(childId, id);
      if (!existing.second) {
        result.errors.push_back("A block cannot be connected to two parents.");
      }
    }
    for (const auto &entry : node.statements) {
      const std::string &key = entry.first;
      const std::vector<VisualNodeId> &children = entry.second;
      const auto statement = std::find_if(
          definition->statements.begin(), definition->statements.end(),
          [&key](const VisualStatementDefinition &candidate) {
            return candidate.key == key;
          });
      if (statement == definition->statements.end()) {
        result.errors.push_back("Block '" + definition->label +
                                "' has unknown statement input '" + key + "'.");
      }
      for (const VisualNodeId childId : children) {
        const VisualNode *const child = program.find(childId);
        if (child == nullptr) {
          result.errors.push_back("Block '" + definition->label +
                                  "' references a missing statement node.");
          continue;
        }
        const VisualBlockDefinition *const childDefinition =
            FindVisualBlock(child->definitionId);
        if (childDefinition != nullptr &&
            childDefinition->shape != VisualBlockShape::Command &&
            childDefinition->shape != VisualBlockShape::Control) {
          result.errors.push_back(
              "Only command blocks can be placed in a statement stack.");
        } else if (childDefinition != nullptr &&
                   statement != definition->statements.end() &&
                   !statement->family.empty() &&
                   childDefinition->statementFamily != statement->family) {
          result.errors.push_back(
              "Block '" + childDefinition->label + "' cannot be placed in '" +
              definition->label + "'.");
        }
        const auto existing = parent.emplace(childId, id);
        if (!existing.second) {
          result.errors.push_back(
              "A block cannot be connected to two parents.");
        }
      }
    }
  }

  for (const VisualNodeId id : program.topLevel) {
    if (program.find(id) == nullptr) {
      result.errors.push_back("Top-level block references a missing node.");
    } else if (parent.count(id) != 0) {
      result.errors.push_back("A connected block cannot also be top-level.");
    }
  }

  std::set<VisualNodeId> visiting;
  std::set<VisualNodeId> visited;
  const auto visit = [&](const auto &self, VisualNodeId id,
                         std::size_t depth) -> void {
    if (depth > maxDepth) {
      result.errors.push_back("Block program exceeds the nesting limit.");
      return;
    }
    if (visited.count(id) != 0)
      return;
    if (!visiting.insert(id).second) {
      result.errors.push_back("Block program contains a connection cycle.");
      return;
    }
    const VisualNode *const node = program.find(id);
    if (node != nullptr) {
      for (const auto &[key, child] : node->inputs) {
        (void)key;
        self(self, child, depth + 1);
      }
      for (const auto &[key, children] : node->statements) {
        (void)key;
        for (const VisualNodeId child : children) {
          self(self, child, depth + 1);
        }
      }
    }
    visiting.erase(id);
    visited.insert(id);
  };
  for (const VisualNodeId id : program.topLevel)
    visit(visit, id, 1);
  if (visited.size() != program.nodes.size()) {
    result.errors.push_back("Block program contains unreachable nodes.");
  }

  result.ok = result.errors.empty();
  return result;
}

} // namespace forevertas::blocks
