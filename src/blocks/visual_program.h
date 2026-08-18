#ifndef FOREVERTAS_BLOCKS_VISUAL_PROGRAM_H
#define FOREVERTAS_BLOCKS_VISUAL_PROGRAM_H

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace forevertas::blocks {

using VisualNodeId = std::uint64_t;

enum class VisualValueType {
  None,
  Scalar,
  Number,
  Integer,
  NumberRange,
  IntegerRange,
  Milliseconds,
  Meters,
  MetersPerSecond,
  Degrees,
  Percent,
  Boolean,
  Vector3,
  Position3,
  Direction3,
  Rotation3,
  Volume,
  Polygon2,
  TimeRange,
  Score,
};

enum class VisualBlockShape {
  Hat,
  Command,
  Control,
  Reporter,
  Predicate,
};

struct VisualNode {
  VisualNodeId id = 0;
  std::string definitionId;
  std::map<std::string, std::string> fields;
  std::map<std::string, VisualNodeId> inputs;
  std::map<std::string, std::vector<VisualNodeId>> statements;
  double x = 0.0;
  double y = 0.0;
};

struct VisualProgram {
  std::map<VisualNodeId, VisualNode> nodes;
  std::vector<VisualNodeId> topLevel;

  const VisualNode *find(VisualNodeId id) const;
  VisualNode *find(VisualNodeId id);
};

// Structural validation is deliberately separate from Blockly. The browser is
// an editor, not a trust boundary: all workspace snapshots pass these checks
// again in C++ before they may affect a search configuration.
struct VisualProgramValidation {
  bool ok = false;
  std::vector<std::string> errors;
};

VisualProgramValidation ValidateVisualProgram(const VisualProgram &program,
                                              std::size_t maxNodes = 4096,
                                              std::size_t maxDepth = 128);

std::string VisualValueTypeName(VisualValueType type);
bool VisualTypeCompatible(VisualValueType output, VisualValueType input);

} // namespace forevertas::blocks

#endif
