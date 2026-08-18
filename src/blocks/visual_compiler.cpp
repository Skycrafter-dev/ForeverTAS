#include "blocks/visual_compiler.h"

#include "blocks/block_lowering.h"
#include "blocks/block_value.h"
#include "blocks/visual_catalog.h"
#include "evaluators/custom_volume_entry_evaluator.h"
#include "evaluators/point_target_evaluator.h"
#include "evaluators/pose_target_evaluator.h"
#include "evaluators/precise_finish_time_evaluator.h"
#include "evaluators/stunt_points_evaluator.h"
#include "evaluators/velocity_evaluator.h"
#include "evaluators/visual_expression_evaluator.h"
#include "evaluators/volume_entry_evaluator.h"
#include "searches/algorithm_registry.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace forevertas::blocks {
namespace {

const VisualNode *InputNode(const VisualProgram &program,
                            const VisualNode &owner, const std::string &key,
                            std::vector<std::string> &errors) {
  const auto found = owner.inputs.find(key);
  if (found == owner.inputs.end()) {
    errors.push_back("Block '" + owner.definitionId + "' is missing input '" +
                     key + "'.");
    return nullptr;
  }
  const VisualNode *const child = program.find(found->second);
  if (child == nullptr) {
    errors.push_back("Block '" + owner.definitionId +
                     "' references a missing input node.");
  }
  return child;
}

std::optional<double> ConstantScalar(const VisualProgram &program,
                                     const VisualNode &node,
                                     std::vector<std::string> &errors,
                                     int depth = 0) {
  if (depth > 64) {
    errors.push_back("Numeric expression is nested too deeply.");
    return std::nullopt;
  }
  if (node.definitionId == "values/number" ||
      node.definitionId == "values/integer" ||
      node.definitionId == "values/meters" ||
      node.definitionId == "values/milliseconds" ||
      node.definitionId == "values/degrees" ||
      node.definitionId == "values/percent") {
    const auto found = node.fields.find("value");
    if (found == node.fields.end()) {
      errors.push_back("Numeric literal has no value.");
      return std::nullopt;
    }
    const auto parsed = ParseNumberValue(found->second);
    if (!parsed || !std::isfinite(*parsed)) {
      errors.push_back("Numeric literal '" + node.definitionId +
                       "' contains invalid value '" + found->second + "'.");
      return std::nullopt;
    }
    return *parsed;
  }

  const bool binary =
      node.definitionId == "math/add" || node.definitionId == "math/subtract" ||
      node.definitionId == "math/multiply" ||
      node.definitionId == "math/divide" || node.definitionId == "math/min" ||
      node.definitionId == "math/max";
  if (!binary) {
    errors.push_back("'" + node.definitionId +
                     "' is not a compile-time scalar expression.");
    return std::nullopt;
  }
  const VisualNode *const left = InputNode(program, node, "a", errors);
  const VisualNode *const right = InputNode(program, node, "b", errors);
  if (left == nullptr || right == nullptr)
    return std::nullopt;
  const auto a = ConstantScalar(program, *left, errors, depth + 1);
  const auto b = ConstantScalar(program, *right, errors, depth + 1);
  if (!a || !b)
    return std::nullopt;
  if (node.definitionId == "math/add")
    return *a + *b;
  if (node.definitionId == "math/subtract")
    return *a - *b;
  if (node.definitionId == "math/multiply")
    return *a * *b;
  if (node.definitionId == "math/divide") {
    if (*b == 0.0) {
      errors.push_back("Division by zero in a block expression.");
      return std::nullopt;
    }
    return *a / *b;
  }
  if (node.definitionId == "math/min")
    return std::min(*a, *b);
  return std::max(*a, *b);
}

std::string CompactNumber(double value) { return FormatNumberValue(value); }

std::optional<std::vector<std::pair<double, double>>> ParsePolygonVertices(
    const std::string &encoded, std::vector<std::string> &errors) {
  std::vector<std::pair<double, double>> vertices;
  std::size_t position = 0u;
  while (position < encoded.size()) {
    const std::size_t comma = encoded.find(',', position);
    const std::size_t semicolon = encoded.find(';', position);
    const std::size_t end =
        semicolon == std::string::npos ? encoded.size() : semicolon;
    if (comma == std::string::npos || comma >= end) {
      errors.push_back("Prism polygon must use x,y;x,y;... coordinates.");
      return std::nullopt;
    }
    const auto x = ParseNumberValue(encoded.substr(position, comma - position));
    const auto y = ParseNumberValue(encoded.substr(comma + 1u, end - comma - 1u));
    if (!x || !y || !std::isfinite(*x) || !std::isfinite(*y)) {
      errors.push_back("Prism polygon contains an invalid coordinate.");
      return std::nullopt;
    }
    vertices.emplace_back(*x, *y);
    if (vertices.size() > 256u) {
      errors.push_back("Prism polygon cannot contain more than 256 vertices.");
      return std::nullopt;
    }
    position = end + 1u;
  }
  if (vertices.size() < 3u) {
    errors.push_back("Prism polygon needs at least three vertices.");
    return std::nullopt;
  }
  return vertices;
}

std::optional<std::string> SerializePolygonLiteral(
    const std::string &encoded, std::vector<std::string> &errors) {
  const auto vertices = ParsePolygonVertices(encoded, errors);
  if (!vertices) return std::nullopt;
  std::string result = std::to_string(vertices->size());
  for (const auto &[x, y] : *vertices) {
    result += " " + CompactNumber(x) + " " + CompactNumber(y);
  }
  return result;
}

std::optional<std::string>
ConstantSettingValue(const VisualProgram &program, const VisualNode &node,
                     std::vector<std::string> &errors) {
  if (node.definitionId == "values/boolean") {
    const auto found = node.fields.find("value");
    if (found == node.fields.end() ||
        (found->second != "true" && found->second != "false")) {
      errors.push_back("Boolean literal contains an invalid value.");
      return std::nullopt;
    }
    return found->second;
  }
  const bool literal = node.definitionId == "values/number" ||
                       node.definitionId == "values/integer" ||
                       node.definitionId == "values/meters" ||
                       node.definitionId == "values/milliseconds" ||
                       node.definitionId == "values/degrees" ||
                       node.definitionId == "values/percent";
  const auto value = ConstantScalar(program, node, errors);
  if (!value)
    return std::nullopt;
  if (literal) {
    const auto found = node.fields.find("value");
    if (found != node.fields.end())
      return found->second;
  }
  return CompactNumber(*value);
}

enum class TimeRangeKind { Window, Point, All };

struct TimeRange {
  TimeRangeKind kind = TimeRangeKind::Window;
  std::string from;
  std::string to;
};

std::optional<TimeRange> CompileTimeRange(const VisualProgram &program,
                                          const VisualNode &node,
                                          std::vector<std::string> &errors) {
  if (node.definitionId == "time/all") {
    return TimeRange{TimeRangeKind::All, {}, {}};
  }
  if (node.definitionId == "time/at") {
    const VisualNode *const timeNode = InputNode(program, node, "time", errors);
    if (timeNode == nullptr)
      return std::nullopt;
    const auto time = ConstantScalar(program, *timeNode, errors);
    if (!time || *time < 0.0) {
      errors.push_back("Evaluation time must be non-negative.");
      return std::nullopt;
    }
    const std::string value = CompactNumber(*time);
    return TimeRange{TimeRangeKind::Point, value, value};
  }
  if (node.definitionId != "time/range") {
    errors.push_back(
        "Expected a time range, time point, or whole-simulation block.");
    return std::nullopt;
  }
  const VisualNode *const fromNode = InputNode(program, node, "from", errors);
  const VisualNode *const toNode = InputNode(program, node, "to", errors);
  if (fromNode == nullptr || toNode == nullptr)
    return std::nullopt;
  const auto from = ConstantScalar(program, *fromNode, errors);
  const auto to = ConstantScalar(program, *toNode, errors);
  if (!from || !to)
    return std::nullopt;
  if (*from < 0.0 || *to < 0.0 || *to < *from) {
    errors.push_back("Time range must satisfy 0 <= from <= to.");
    return std::nullopt;
  }
  return TimeRange{TimeRangeKind::Window, CompactNumber(*from),
                   CompactNumber(*to)};
}

struct PointValue {
  std::string x;
  std::string y;
  std::string z;
};

std::optional<PointValue> CompilePoint(const VisualProgram &program,
                                       const VisualNode &node,
                                       std::vector<std::string> &errors) {
  if (node.definitionId != "targets/point") {
    errors.push_back("Distance target must currently be a point block.");
    return std::nullopt;
  }
  const VisualNode *const xNode = InputNode(program, node, "x", errors);
  const VisualNode *const yNode = InputNode(program, node, "y", errors);
  const VisualNode *const zNode = InputNode(program, node, "z", errors);
  if (xNode == nullptr || yNode == nullptr || zNode == nullptr) {
    return std::nullopt;
  }
  const auto x = ConstantScalar(program, *xNode, errors);
  const auto y = ConstantScalar(program, *yNode, errors);
  const auto z = ConstantScalar(program, *zNode, errors);
  if (!x || !y || !z)
    return std::nullopt;
  return PointValue{CompactNumber(*x), CompactNumber(*y), CompactNumber(*z)};
}

struct DirectionValue {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

std::optional<DirectionValue>
CompileDirection(const VisualProgram &program, const VisualNode &node,
                 std::vector<std::string> &errors) {
  if (node.definitionId != "targets/direction") {
    errors.push_back("Expected a direction block.");
    return std::nullopt;
  }
  const VisualNode *const xNode = InputNode(program, node, "x", errors);
  const VisualNode *const yNode = InputNode(program, node, "y", errors);
  const VisualNode *const zNode = InputNode(program, node, "z", errors);
  if (xNode == nullptr || yNode == nullptr || zNode == nullptr)
    return std::nullopt;
  const auto x = ConstantScalar(program, *xNode, errors);
  const auto y = ConstantScalar(program, *yNode, errors);
  const auto z = ConstantScalar(program, *zNode, errors);
  if (!x || !y || !z)
    return std::nullopt;
  return DirectionValue{*x, *y, *z};
}

bool SameDirection(const DirectionValue &a, const DirectionValue &b) {
  const double al = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
  const double bl = std::sqrt(b.x * b.x + b.y * b.y + b.z * b.z);
  if (al <= 1e-12 || bl <= 1e-12)
    return false;
  constexpr double tolerance = 1e-10;
  return std::abs(a.x / al - b.x / bl) <= tolerance &&
         std::abs(a.y / al - b.y / bl) <= tolerance &&
         std::abs(a.z / al - b.z / bl) <= tolerance;
}

std::optional<DirectionValue>
ProjectedVelocityDirection(const VisualProgram &program, const VisualNode &node,
                           std::vector<std::string> &errors) {
  if (node.definitionId != "math/dot")
    return std::nullopt;
  const VisualNode *const a = InputNode(program, node, "a", errors);
  const VisualNode *const b = InputNode(program, node, "b", errors);
  if (a == nullptr || b == nullptr)
    return std::nullopt;
  const VisualNode *direction = nullptr;
  if (a->definitionId == "simulation/car-velocity")
    direction = b;
  if (b->definitionId == "simulation/car-velocity")
    direction = a;
  if (direction == nullptr)
    return std::nullopt;
  return CompileDirection(program, *direction, errors);
}

struct AlignmentGate {
  DirectionValue direction;
  double percent = -100.0;
};

std::optional<AlignmentGate>
CompileAlignmentGate(const VisualProgram &program, const VisualNode &node,
                     std::vector<std::string> &errors) {
  if (node.definitionId != "conditions/greater-equal") {
    errors.push_back(
        "The native objective gate currently expects a ≥ condition.");
    return std::nullopt;
  }
  const VisualNode *const left = InputNode(program, node, "a", errors);
  const VisualNode *const right = InputNode(program, node, "b", errors);
  if (left == nullptr || right == nullptr)
    return std::nullopt;
  if (left->definitionId != "math/dot" ||
      right->definitionId != "math/percent-ratio") {
    errors.push_back("Direction alignment must compare dot(normalize(car "
                     "velocity), direction) with a percent ratio.");
    return std::nullopt;
  }
  const VisualNode *const dotA = InputNode(program, *left, "a", errors);
  const VisualNode *const dotB = InputNode(program, *left, "b", errors);
  if (dotA == nullptr || dotB == nullptr)
    return std::nullopt;
  const VisualNode *normalize = nullptr;
  const VisualNode *direction = nullptr;
  if (dotA->definitionId == "math/normalize") {
    normalize = dotA;
    direction = dotB;
  } else if (dotB->definitionId == "math/normalize") {
    normalize = dotB;
    direction = dotA;
  }
  if (normalize == nullptr || direction == nullptr) {
    errors.push_back(
        "Alignment dot product needs normalized car velocity and a direction.");
    return std::nullopt;
  }
  const VisualNode *const vector =
      InputNode(program, *normalize, "value", errors);
  if (vector == nullptr || vector->definitionId != "simulation/car-velocity") {
    errors.push_back("Alignment normalization needs car velocity.");
    return std::nullopt;
  }
  const auto compiledDirection = CompileDirection(program, *direction, errors);
  const VisualNode *const percentNode =
      InputNode(program, *right, "value", errors);
  if (!compiledDirection || percentNode == nullptr)
    return std::nullopt;
  const auto percent = ConstantScalar(program, *percentNode, errors);
  if (!percent || *percent < -100.0 || *percent > 100.0) {
    errors.push_back("Alignment percent must be between -100 and 100.");
    return std::nullopt;
  }
  return AlignmentGate{*compiledDirection, *percent};
}

std::optional<PointValue>
PointDistanceTarget(const VisualProgram &program, const VisualNode &node,
                    std::vector<std::string> &errors) {
  if (node.definitionId != "math/distance")
    return std::nullopt;
  const VisualNode *const a = InputNode(program, node, "a", errors);
  const VisualNode *const b = InputNode(program, node, "b", errors);
  if (a == nullptr || b == nullptr)
    return std::nullopt;
  const VisualNode *target = nullptr;
  if (a->definitionId == "simulation/car-position")
    target = b;
  if (b->definitionId == "simulation/car-position")
    target = a;
  if (target == nullptr)
    return std::nullopt;
  return CompilePoint(program, *target, errors);
}

struct RotationValue {
  std::string yaw;
  std::string pitch;
  std::string roll;
};

std::optional<RotationValue> CompileRotation(const VisualProgram &program,
                                             const VisualNode &node,
                                             std::vector<std::string> &errors) {
  if (node.definitionId != "targets/rotation") {
    errors.push_back("Expected a rotation block.");
    return std::nullopt;
  }
  const VisualNode *const yawNode = InputNode(program, node, "yaw", errors);
  const VisualNode *const pitchNode = InputNode(program, node, "pitch", errors);
  const VisualNode *const rollNode = InputNode(program, node, "roll", errors);
  if (yawNode == nullptr || pitchNode == nullptr || rollNode == nullptr)
    return std::nullopt;
  const auto yaw = ConstantScalar(program, *yawNode, errors);
  const auto pitch = ConstantScalar(program, *pitchNode, errors);
  const auto roll = ConstantScalar(program, *rollNode, errors);
  if (!yaw || !pitch || !roll)
    return std::nullopt;
  return RotationValue{CompactNumber(*yaw), CompactNumber(*pitch),
                       CompactNumber(*roll)};
}

std::optional<RotationValue>
RotationDistanceTarget(const VisualProgram &program, const VisualNode &node,
                       std::vector<std::string> &errors) {
  if (node.definitionId != "math/rotation-distance")
    return std::nullopt;
  const VisualNode *const a = InputNode(program, node, "a", errors);
  const VisualNode *const b = InputNode(program, node, "b", errors);
  if (a == nullptr || b == nullptr)
    return std::nullopt;
  const VisualNode *target = nullptr;
  if (a->definitionId == "simulation/car-rotation")
    target = b;
  if (b->definitionId == "simulation/car-rotation")
    target = a;
  if (target == nullptr)
    return std::nullopt;
  return CompileRotation(program, *target, errors);
}

std::optional<std::string>
SerializeRuntimeExpression(const VisualProgram &program, const VisualNode &node,
                           std::vector<std::string> &errors,
                           std::size_t depth = 0u) {
  if (depth > 128u) {
    errors.push_back("Runtime expression is nested too deeply.");
    return std::nullopt;
  }
  const auto input = [&](const char *key) -> const VisualNode * {
    return InputNode(program, node, key, errors);
  };
  const auto child = [&](const char *key) -> std::optional<std::string> {
    const VisualNode *const value = input(key);
    return value == nullptr
               ? std::nullopt
               : SerializeRuntimeExpression(program, *value, errors, depth + 1u);
  };
  const auto unary = [&](const char *opcode,
                         const char *key) -> std::optional<std::string> {
    const auto value = child(key);
    return value ? std::optional<std::string>(std::string(opcode) + " " + *value)
                 : std::nullopt;
  };
  const auto binary = [&](const char *opcode) -> std::optional<std::string> {
    const auto a = child("a");
    const auto b = child("b");
    return a && b ? std::optional<std::string>(std::string(opcode) + " " + *a +
                                               " " + *b)
                  : std::nullopt;
  };
  const bool numericLiteral =
      node.definitionId == "values/number" ||
      node.definitionId == "values/integer" ||
      node.definitionId == "values/meters" ||
      node.definitionId == "values/milliseconds" ||
      node.definitionId == "values/degrees" ||
      node.definitionId == "values/percent";
  if (numericLiteral) {
    const auto found = node.fields.find("value");
    if (found == node.fields.end()) {
      errors.push_back("Numeric runtime expression has no value.");
      return std::nullopt;
    }
    const auto parsed = ParseNumberValue(found->second);
    if (!parsed || !std::isfinite(*parsed)) {
      errors.push_back("Numeric runtime expression contains an invalid value.");
      return std::nullopt;
    }
    return "num " + CompactNumber(*parsed);
  }
  if (node.definitionId == "values/boolean") {
    const auto found = node.fields.find("value");
    if (found == node.fields.end() ||
        (found->second != "true" && found->second != "false")) {
      errors.push_back("Boolean runtime expression contains an invalid value.");
      return std::nullopt;
    }
    return found->second == "true" ? "bool 1" : "bool 0";
  }
  if (node.definitionId == "simulation/car-position") return "carpos";
  if (node.definitionId == "simulation/car-velocity") return "carvel";
  if (node.definitionId == "simulation/car-local-velocity") return "carlocalvel";
  if (node.definitionId == "simulation/car-speed") return "carspeed";
  if (node.definitionId == "simulation/car-rotation") return "carrot";
  if (node.definitionId == "simulation/stunt-points") return "stunts";
  if (node.definitionId == "simulation/finish-time") return "finishms";
  if (node.definitionId == "simulation/time") return "simtime";
  if (node.definitionId == "simulation/checkpoint-count") return "checkpoints";
  if (node.definitionId == "simulation/race-completed") return "completed";
  if (node.definitionId == "simulation/sliding") return "sliding";
  if (node.definitionId == "simulation/freewheeling") return "freewheeling";

  if (node.definitionId == "targets/point" ||
      node.definitionId == "targets/direction" ||
      node.definitionId == "targets/size") {
    const auto x = child("x");
    const auto y = child("y");
    const auto z = child("z");
    if (!x || !y || !z) return std::nullopt;
    const char *const opcode = node.definitionId == "targets/direction"
                                   ? "dir"
                                   : "vec";
    return std::string(opcode) + " " + *x + " " + *y + " " + *z;
  }
  if (node.definitionId == "targets/rotation") {
    const auto yaw = child("yaw");
    const auto pitch = child("pitch");
    const auto roll = child("roll");
    if (!yaw || !pitch || !roll) return std::nullopt;
    return "rot " + *yaw + " " + *pitch + " " + *roll;
  }
  if (node.definitionId == "math/distance") return binary("distance");
  if (node.definitionId == "math/magnitude")
    return unary("magnitude", "value");
  if (node.definitionId == "math/normalize")
    return unary("normalize", "value");
  if (node.definitionId == "math/dot") return binary("dot");
  if (node.definitionId == "math/rotation-distance")
    return binary("rotdistance");
  if (node.definitionId == "math/percent-ratio")
    return unary("ratio", "value");
  if (node.definitionId == "math/kmh") return unary("kmh", "value");
  if (node.definitionId == "math/abs") return unary("abs", "value");
  if (node.definitionId == "math/clamp") {
    const auto value = child("value");
    const auto minimum = child("minimum");
    const auto maximum = child("maximum");
    if (!value || !minimum || !maximum) return std::nullopt;
    return "clamp " + *value + " " + *minimum + " " + *maximum;
  }
  if (node.definitionId == "math/add") return binary("add");
  if (node.definitionId == "math/subtract") return binary("sub");
  if (node.definitionId == "math/multiply") return binary("mul");
  if (node.definitionId == "math/divide") return binary("div");
  if (node.definitionId == "math/min") return binary("min");
  if (node.definitionId == "math/max") return binary("max");
  if (node.definitionId == "conditions/less") return binary("lt");
  if (node.definitionId == "conditions/less-equal") return binary("le");
  if (node.definitionId == "conditions/equal") return binary("eq");
  if (node.definitionId == "conditions/greater-equal") return binary("ge");
  if (node.definitionId == "conditions/greater") return binary("gt");
  if (node.definitionId == "conditions/and") return binary("and");
  if (node.definitionId == "conditions/or") return binary("or");
  if (node.definitionId == "conditions/not") return unary("not", "value");
  if (node.definitionId == "conditions/inside") {
    const auto position = child("position");
    const VisualNode *const volume = input("volume");
    if (!position || volume == nullptr) return std::nullopt;
    if (volume->definitionId == "targets/box") {
      const VisualNode *const centerNode =
          InputNode(program, *volume, "center", errors);
      const VisualNode *const sizeNode =
          InputNode(program, *volume, "size", errors);
      if (centerNode == nullptr || sizeNode == nullptr) return std::nullopt;
      const auto center =
          SerializeRuntimeExpression(program, *centerNode, errors, depth + 1u);
      const auto size =
          SerializeRuntimeExpression(program, *sizeNode, errors, depth + 1u);
      if (!center || !size) return std::nullopt;
      return "insidebox " + *position + " " + *center + " " + *size;
    }
    if (volume->definitionId == "targets/prism") {
      const VisualNode *const originNode =
          InputNode(program, *volume, "origin", errors);
      const VisualNode *const depthNode =
          InputNode(program, *volume, "depth", errors);
      const VisualNode *const polygonNode =
          InputNode(program, *volume, "polygon", errors);
      if (originNode == nullptr || depthNode == nullptr || polygonNode == nullptr)
        return std::nullopt;
      if (polygonNode->definitionId != "values/polygon") {
        errors.push_back("Prism polygon must come from a polygon value block.");
        return std::nullopt;
      }
      const auto plane = volume->fields.find("plane");
      const auto polygon = polygonNode->fields.find("value");
      if (plane == volume->fields.end() ||
          (plane->second != "xy" && plane->second != "xz" &&
           plane->second != "yz") ||
          polygon == polygonNode->fields.end()) {
        errors.push_back("Prism is missing a valid plane or polygon value.");
        return std::nullopt;
      }
      const auto origin =
          SerializeRuntimeExpression(program, *originNode, errors, depth + 1u);
      const auto prismDepth =
          SerializeRuntimeExpression(program, *depthNode, errors, depth + 1u);
      const auto polygonLiteral = SerializePolygonLiteral(polygon->second, errors);
      if (!origin || !prismDepth || !polygonLiteral) return std::nullopt;
      return "insideprism " + *position + " " + *origin + " " +
             *prismDepth + " " + plane->second + " " + *polygonLiteral;
    }
    errors.push_back("Inside expects a box or prism volume.");
    return std::nullopt;
  }
  if (node.definitionId == "math/weighted-blend") {
    const auto a = child("a");
    const auto b = child("b");
    const auto weight = child("weight");
    if (!a || !b || !weight) return std::nullopt;
    return "blend " + *a + " " + *b + " " + *weight;
  }

  errors.push_back("Runtime expression block '" + node.definitionId +
                   "' cannot be lowered by the generic CPU evaluator.");
  return std::nullopt;
}

bool EmitSimulationCondition(const VisualProgram &program, const VisualNode &node,
                             ConditionProgram *condition,
                             std::vector<std::string> &errors,
                             std::size_t depth = 0u) {
  using forevervalidator::experimental::PhysicsSandboxCudaExpressionPlane;
  using forevervalidator::experimental::PhysicsSandboxCudaExpressionPoint2;
  using forevervalidator::experimental::PhysicsSandboxCudaExpressionPrism;
  using forevervalidator::experimental::PhysicsSandboxCudaConditionInstruction;
  using forevervalidator::experimental::PhysicsSandboxCudaConditionOpcode;
  using forevervalidator::experimental::PhysicsSandboxCudaConditionValue;
  using Op = PhysicsSandboxCudaConditionOpcode;
  using Value = PhysicsSandboxCudaConditionValue;

  if (depth > 30u) {
    errors.push_back("Simulation predicate is nested too deeply.");
    return false;
  }
  auto &output = condition->cuda.instructions;
  const auto emit = [&output](Op opcode) {
    output.push_back(PhysicsSandboxCudaConditionInstruction{opcode});
  };
  const auto constant = [&output](double value) {
    output.push_back(PhysicsSandboxCudaConditionInstruction{
        Op::Constant, Value::Speed, value});
  };
  const auto source = [&output](Op opcode, Value value) {
    output.push_back(PhysicsSandboxCudaConditionInstruction{opcode, value});
  };
  const auto childNode = [&](const char *key) -> const VisualNode * {
    return InputNode(program, node, key, errors);
  };
  const auto child = [&](const char *key) {
    const VisualNode *const value = childNode(key);
    return value != nullptr &&
           EmitSimulationCondition(program, *value, condition, errors,
                                   depth + 1u);
  };
  const auto binary = [&](Op opcode) {
    if (!child("a") || !child("b")) return false;
    emit(opcode);
    return true;
  };
  const auto unary = [&](const char *key, Op opcode) {
    if (!child(key)) return false;
    emit(opcode);
    return true;
  };
  const auto ternary = [&](const char *a, const char *b, const char *c,
                           Op opcode) {
    if (!child(a) || !child(b) || !child(c)) return false;
    emit(opcode);
    return true;
  };

  const bool numericLiteral =
      node.definitionId == "values/number" ||
      node.definitionId == "values/integer" ||
      node.definitionId == "values/meters" ||
      node.definitionId == "values/milliseconds" ||
      node.definitionId == "values/degrees" ||
      node.definitionId == "values/percent";
  if (numericLiteral) {
    const auto found = node.fields.find("value");
    const auto value = found == node.fields.end()
                           ? std::nullopt
                           : ParseNumberValue(found->second);
    if (!value || !std::isfinite(*value)) {
      errors.push_back("Simulation predicate contains an invalid number.");
      return false;
    }
    constant(*value);
    return true;
  }
  if (node.definitionId == "values/boolean") {
    const auto found = node.fields.find("value");
    if (found == node.fields.end() ||
        (found->second != "true" && found->second != "false")) {
      errors.push_back("Simulation predicate contains an invalid Boolean value.");
      return false;
    }
    constant(found->second == "true" ? 1.0 : 0.0);
    return true;
  }
  if (node.definitionId == "conditions/legacy-script") {
    const auto sourceText = node.fields.find("source");
    if (sourceText == node.fields.end()) {
      errors.push_back("Legacy condition block is missing its source text.");
      return false;
    }
    ConditionCompileResult compiled = CompileConditionScript(sourceText->second);
    if (compiled.error) {
      errors.push_back(*compiled.error);
      return false;
    }
    if (!compiled.program || compiled.program->cuda.instructions.empty()) {
      errors.push_back("Nested legacy condition block cannot be empty.");
      return false;
    }
    output.insert(output.end(), compiled.program->cuda.instructions.begin(),
                  compiled.program->cuda.instructions.end());
    return true;
  }

  if (node.definitionId == "simulation/car-position") {
    source(Op::Vector, Value::Position);
    return true;
  }
  if (node.definitionId == "simulation/car-velocity") {
    source(Op::Vector, Value::Velocity);
    return true;
  }
  if (node.definitionId == "simulation/car-local-velocity") {
    source(Op::Vector, Value::LocalVelocity);
    return true;
  }
  if (node.definitionId == "simulation/car-speed") {
    source(Op::Scalar, Value::Speed);
    return true;
  }
  if (node.definitionId == "simulation/car-rotation") {
    source(Op::RotationSource, Value::CarRotation);
    return true;
  }
  if (node.definitionId == "simulation/stunt-points") {
    source(Op::Scalar, Value::StuntPoints);
    return true;
  }
  if (node.definitionId == "simulation/finish-time") {
    source(Op::Scalar, Value::FinishTime);
    return true;
  }
  if (node.definitionId == "simulation/time") {
    source(Op::Scalar, Value::SimulationTime);
    return true;
  }
  if (node.definitionId == "simulation/checkpoint-count") {
    source(Op::Scalar, Value::CheckpointCount);
    return true;
  }
  if (node.definitionId == "simulation/race-completed") {
    source(Op::Scalar, Value::RaceCompleted);
    return true;
  }
  if (node.definitionId == "simulation/sliding") {
    source(Op::Scalar, Value::Sliding);
    return true;
  }
  if (node.definitionId == "simulation/freewheeling") {
    source(Op::Scalar, Value::FreeWheeling);
    return true;
  }

  if (node.definitionId == "targets/point" ||
      node.definitionId == "targets/size")
    return ternary("x", "y", "z", Op::ComposeVector);
  if (node.definitionId == "targets/direction")
    return ternary("x", "y", "z", Op::Direction);
  if (node.definitionId == "targets/rotation")
    return ternary("yaw", "pitch", "roll", Op::Rotation);

  if (node.definitionId == "math/add") return binary(Op::Add);
  if (node.definitionId == "math/subtract") return binary(Op::Subtract);
  if (node.definitionId == "math/multiply") return binary(Op::Multiply);
  if (node.definitionId == "math/divide") return binary(Op::Divide);
  if (node.definitionId == "math/min") return binary(Op::Minimum);
  if (node.definitionId == "math/max") return binary(Op::Maximum);
  if (node.definitionId == "math/distance") return binary(Op::Distance);
  if (node.definitionId == "math/magnitude")
    return unary("value", Op::Magnitude);
  if (node.definitionId == "math/normalize")
    return unary("value", Op::Normalize);
  if (node.definitionId == "math/dot") return binary(Op::Dot);
  if (node.definitionId == "math/rotation-distance")
    return binary(Op::RotationDistance);
  if (node.definitionId == "math/weighted-blend")
    return ternary("a", "b", "weight", Op::WeightedBlend);
  if (node.definitionId == "math/percent-ratio")
    return unary("value", Op::PercentRatio);
  if (node.definitionId == "math/kmh")
    return unary("value", Op::KilometersPerHour);
  if (node.definitionId == "math/abs") return unary("value", Op::Absolute);
  if (node.definitionId == "math/clamp")
    return ternary("value", "minimum", "maximum", Op::Clamp);
  if (node.definitionId == "conditions/less") return binary(Op::Less);
  if (node.definitionId == "conditions/less-equal")
    return binary(Op::LessOrEqual);
  if (node.definitionId == "conditions/equal") return binary(Op::Equal);
  if (node.definitionId == "conditions/greater-equal")
    return binary(Op::GreaterOrEqual);
  if (node.definitionId == "conditions/greater") return binary(Op::Greater);
  if (node.definitionId == "conditions/and") return binary(Op::LogicalAnd);
  if (node.definitionId == "conditions/or") return binary(Op::LogicalOr);
  if (node.definitionId == "conditions/not")
    return unary("value", Op::LogicalNot);
  if (node.definitionId == "conditions/inside") {
    if (!child("position")) return false;
    const VisualNode *const volume = childNode("volume");
    if (volume == nullptr) return false;
    if (volume->definitionId == "targets/box") {
      const VisualNode *const center = InputNode(program, *volume, "center", errors);
      const VisualNode *const size = InputNode(program, *volume, "size", errors);
      if (center == nullptr || size == nullptr ||
          !EmitSimulationCondition(program, *center, condition, errors,
                                   depth + 1u) ||
          !EmitSimulationCondition(program, *size, condition, errors,
                                   depth + 1u))
        return false;
      emit(Op::InsideBox);
      return true;
    }
    if (volume->definitionId == "targets/prism") {
      const VisualNode *const origin = InputNode(program, *volume, "origin", errors);
      const VisualNode *const prismDepth =
          InputNode(program, *volume, "depth", errors);
      const VisualNode *const polygon =
          InputNode(program, *volume, "polygon", errors);
      if (origin == nullptr || prismDepth == nullptr || polygon == nullptr ||
          !EmitSimulationCondition(program, *origin, condition, errors,
                                   depth + 1u) ||
          !EmitSimulationCondition(program, *prismDepth, condition, errors,
                                   depth + 1u))
        return false;
      if (polygon->definitionId != "values/polygon") {
        errors.push_back("Prism polygon must come from a polygon value block.");
        return false;
      }
      const auto polygonValue = polygon->fields.find("value");
      const auto plane = volume->fields.find("plane");
      if (polygonValue == polygon->fields.end() || plane == volume->fields.end()) {
        errors.push_back("Prism is missing its plane or polygon value.");
        return false;
      }
      const auto vertices = ParsePolygonVertices(polygonValue->second, errors);
      if (!vertices) return false;
      PhysicsSandboxCudaExpressionPrism prism;
      if (plane->second == "xy")
        prism.plane = PhysicsSandboxCudaExpressionPlane::XY;
      else if (plane->second == "xz")
        prism.plane = PhysicsSandboxCudaExpressionPlane::XZ;
      else if (plane->second == "yz")
        prism.plane = PhysicsSandboxCudaExpressionPlane::YZ;
      else {
        errors.push_back("Prism has an invalid projection plane.");
        return false;
      }
      if (condition->cuda.prisms.size() >= 256u ||
          condition->cuda.prismVertices.size() + vertices->size() > 65536u) {
        errors.push_back("Simulation predicate prism geometry is too large.");
        return false;
      }
      prism.vertexOffset = static_cast<std::uint32_t>(
          condition->cuda.prismVertices.size());
      prism.vertexCount = static_cast<std::uint32_t>(vertices->size());
      const std::size_t prismIndex = condition->cuda.prisms.size();
      condition->cuda.prisms.push_back(prism);
      for (const auto &[x, y] : *vertices)
        condition->cuda.prismVertices.push_back(
            PhysicsSandboxCudaExpressionPoint2{x, y});
      output.push_back(PhysicsSandboxCudaConditionInstruction{
          Op::InsidePrism, Value::Speed, static_cast<double>(prismIndex)});
      return true;
    }
    errors.push_back("Inside expects a box or prism volume.");
    return false;
  }

  errors.push_back("Block '" + node.definitionId +
                   "' is not yet supported inside simulate where.");
  return false;
}

std::optional<ConditionProgram>
CompileSimulationCondition(const VisualProgram &program, const VisualNode &node,
                           std::vector<std::string> &errors) {
  if (node.definitionId == "conditions/legacy-script") {
    const auto source = node.fields.find("source");
    if (source == node.fields.end()) {
      errors.push_back("Legacy condition block is missing its source text.");
      return std::nullopt;
    }
    ConditionCompileResult compiled = CompileConditionScript(source->second);
    if (compiled.error) {
      errors.push_back(*compiled.error);
      return std::nullopt;
    }
    return std::move(compiled.program);
  }

  ConditionProgram result;
  if (!EmitSimulationCondition(program, node, &result, errors))
    return std::nullopt;
  if (result.cuda.instructions.size() > 256u) {
    errors.push_back("Simulation predicate exceeds the 256-instruction limit.");
    return std::nullopt;
  }
  return result;
}

std::optional<OptionConfiguration>
CompileFirstTimeObjective(const VisualProgram &program, const VisualNode &score,
                          std::vector<std::string> &errors) {
  const VisualNode *const condition =
      InputNode(program, score, "condition", errors);
  const VisualNode *const rangeNode =
      InputNode(program, score, "range", errors);
  if (condition == nullptr || rangeNode == nullptr)
    return std::nullopt;
  const auto range = CompileTimeRange(program, *rangeNode, errors);
  if (!range)
    return std::nullopt;
  if (range->kind != TimeRangeKind::All) {
    errors.push_back("Native first-entry objectives currently use the whole "
                     "simulation range.");
    return std::nullopt;
  }
  if (condition->definitionId != "conditions/inside") {
    errors.push_back(
        "Native first-time lowering currently expects an inside condition.");
    return std::nullopt;
  }
  const VisualNode *const position =
      InputNode(program, *condition, "position", errors);
  const VisualNode *const volume =
      InputNode(program, *condition, "volume", errors);
  if (position == nullptr || volume == nullptr)
    return std::nullopt;
  if (position->definitionId != "simulation/car-position") {
    errors.push_back("First-entry lowering currently tracks car position.");
    return std::nullopt;
  }

  if (volume->definitionId == "targets/box") {
    const VisualNode *const center =
        InputNode(program, *volume, "center", errors);
    const VisualNode *const size = InputNode(program, *volume, "size", errors);
    if (center == nullptr || size == nullptr)
      return std::nullopt;
    const auto point = CompilePoint(program, *center, errors);
    if (!point || size->definitionId != "targets/size") {
      if (size->definitionId != "targets/size")
        errors.push_back("Box size must be assembled with a size block.");
      return std::nullopt;
    }
    const VisualNode *const sx = InputNode(program, *size, "x", errors);
    const VisualNode *const sy = InputNode(program, *size, "y", errors);
    const VisualNode *const sz = InputNode(program, *size, "z", errors);
    if (sx == nullptr || sy == nullptr || sz == nullptr)
      return std::nullopt;
    const auto x = ConstantScalar(program, *sx, errors);
    const auto y = ConstantScalar(program, *sy, errors);
    const auto z = ConstantScalar(program, *sz, errors);
    if (!x || !y || !z)
      return std::nullopt;
    OptionSettings settings = DefaultVolumeEntryOptionSettings();
    settings["centerX"] = point->x;
    settings["centerY"] = point->y;
    settings["centerZ"] = point->z;
    settings["sizeX"] = CompactNumber(*x);
    settings["sizeY"] = CompactNumber(*y);
    settings["sizeZ"] = CompactNumber(*z);
    return OptionConfiguration{kVolumeEntryEvaluationId, std::move(settings)};
  }

  if (volume->definitionId == "targets/prism") {
    const VisualNode *const origin =
        InputNode(program, *volume, "origin", errors);
    const VisualNode *const depth =
        InputNode(program, *volume, "depth", errors);
    const VisualNode *const polygon =
        InputNode(program, *volume, "polygon", errors);
    if (origin == nullptr || depth == nullptr || polygon == nullptr)
      return std::nullopt;
    const auto point = CompilePoint(program, *origin, errors);
    const auto depthValue = ConstantScalar(program, *depth, errors);
    if (!point || !depthValue || polygon->definitionId != "values/polygon") {
      if (polygon->definitionId != "values/polygon")
        errors.push_back("Prism polygon must come from a polygon value block.");
      return std::nullopt;
    }
    const auto polygonValue = polygon->fields.find("value");
    const auto plane = volume->fields.find("plane");
    if (polygonValue == polygon->fields.end() ||
        plane == volume->fields.end()) {
      errors.push_back("Prism is missing its plane or polygon value.");
      return std::nullopt;
    }
    OptionSettings settings = DefaultCustomVolumeEntryOptionSettings();
    settings["plane"] = plane->second;
    settings["originX"] = point->x;
    settings["originY"] = point->y;
    settings["originZ"] = point->z;
    settings["depth"] = CompactNumber(*depthValue);
    settings["polygon"] = polygonValue->second;
    return OptionConfiguration{kCustomVolumeEntryEvaluationId,
                               std::move(settings)};
  }

  errors.push_back(
      "Inside currently lowers natively for box and prism volumes.");
  return std::nullopt;
}

std::optional<OptionConfiguration>
TryCompileNativeObjective(const VisualProgram &program, const VisualNode &score,
                          std::vector<std::string> &errors) {
  if (score.definitionId == "objective/first-time")
    return CompileFirstTimeObjective(program, score, errors);

  const VisualNode *objective = &score;
  std::optional<AlignmentGate> alignment;
  if (score.definitionId == "objective/only-when") {
    const VisualNode *const inner = InputNode(program, score, "score", errors);
    const VisualNode *const condition =
        InputNode(program, score, "condition", errors);
    if (inner == nullptr || condition == nullptr)
      return std::nullopt;
    alignment = CompileAlignmentGate(program, *condition, errors);
    if (!alignment)
      return std::nullopt;
    objective = inner;
  }

  const bool minimize = objective->definitionId == "objective/minimize";
  const bool maximize = objective->definitionId == "objective/maximize";
  if (!minimize && !maximize) {
    errors.push_back("Select-best needs minimize, maximize, or first-time "
                     "objective blocks.");
    return std::nullopt;
  }
  const VisualNode *const value =
      InputNode(program, *objective, "value", errors);
  const VisualNode *const rangeNode =
      InputNode(program, *objective, "range", errors);
  if (value == nullptr || rangeNode == nullptr)
    return std::nullopt;
  const auto range = CompileTimeRange(program, *rangeNode, errors);
  if (!range)
    return std::nullopt;

  if (value->definitionId == "math/distance" && !alignment) {
    if (!minimize) {
      errors.push_back(
          "Distance-to-point objectives are minimized, not maximized.");
      return std::nullopt;
    }
    if (range->kind == TimeRangeKind::All) {
      errors.push_back("Point distance needs an explicit evaluation range.");
      return std::nullopt;
    }
    const auto point = PointDistanceTarget(program, *value, errors);
    if (!point) {
      errors.push_back("The native distance fast path currently needs car "
                       "position on one side.");
      return std::nullopt;
    }
    OptionSettings settings = DefaultPointTargetOptionSettings();
    settings["minTimeMs"] = range->from;
    settings["maxTimeMs"] = range->to;
    settings["x"] = point->x;
    settings["y"] = point->y;
    settings["z"] = point->z;
    return OptionConfiguration{kPointTargetEvaluationId, std::move(settings)};
  }

  const bool totalSpeed =
      value->definitionId == "simulation/car-speed" ||
      (value->definitionId == "math/magnitude" && [&] {
        const VisualNode *const inner =
            InputNode(program, *value, "value", errors);
        return inner != nullptr &&
               inner->definitionId == "simulation/car-velocity";
      }());
  std::optional<DirectionValue> projectedDirection;
  if (value->definitionId == "math/dot")
    projectedDirection = ProjectedVelocityDirection(program, *value, errors);

  if (totalSpeed || projectedDirection) {
    if (!maximize) {
      errors.push_back("Velocity objectives are maximized, not minimized.");
      return std::nullopt;
    }
    if (range->kind == TimeRangeKind::All) {
      errors.push_back(
          "Velocity objectives need an explicit evaluation range.");
      return std::nullopt;
    }
    if (alignment && projectedDirection &&
        !SameDirection(alignment->direction, *projectedDirection)) {
      errors.push_back("Projected velocity and its alignment gate must use the "
                       "same direction.");
      return std::nullopt;
    }
    OptionSettings settings = DefaultVelocityOptionSettings();
    settings["minTimeMs"] = range->from;
    settings["maxTimeMs"] = range->to;
    settings["mode"] = projectedDirection ? "projected" : "total";
    settings["alignmentEnabled"] = alignment ? "true" : "false";
    const DirectionValue *direction = projectedDirection ? &*projectedDirection
                                      : alignment        ? &alignment->direction
                                                         : nullptr;
    if (direction != nullptr) {
      settings["directionX"] = CompactNumber(direction->x);
      settings["directionY"] = CompactNumber(direction->y);
      settings["directionZ"] = CompactNumber(direction->z);
    }
    settings["minAlignmentPercent"] =
        alignment ? CompactNumber(alignment->percent) : "-100";
    return OptionConfiguration{kVelocityEvaluationId, std::move(settings)};
  }

  if (value->definitionId == "math/weighted-blend" && !alignment) {
    if (!minimize || range->kind == TimeRangeKind::All) {
      errors.push_back("Pose error is minimized over an explicit time range.");
      return std::nullopt;
    }
    const VisualNode *const first = InputNode(program, *value, "a", errors);
    const VisualNode *const second = InputNode(program, *value, "b", errors);
    const VisualNode *const weightNode =
        InputNode(program, *value, "weight", errors);
    if (first == nullptr || second == nullptr || weightNode == nullptr)
      return std::nullopt;
    const auto point = PointDistanceTarget(program, *first, errors);
    const auto rotation = RotationDistanceTarget(program, *second, errors);
    const auto weight = ConstantScalar(program, *weightNode, errors);
    if (!point || !rotation || !weight)
      return std::nullopt;
    if (*weight < 0.0 || *weight > 100.0) {
      errors.push_back(
          "Pose rotation weight must be between 0 and 100 percent.");
      return std::nullopt;
    }
    OptionSettings settings = DefaultPoseTargetOptionSettings();
    settings["minTimeMs"] = range->from;
    settings["maxTimeMs"] = range->to;
    settings["x"] = point->x;
    settings["y"] = point->y;
    settings["z"] = point->z;
    settings["yawDegrees"] = rotation->yaw;
    settings["pitchDegrees"] = rotation->pitch;
    settings["rollDegrees"] = rotation->roll;
    settings["rotationWeightPercent"] = CompactNumber(*weight);
    return OptionConfiguration{kPoseTargetEvaluationId, std::move(settings)};
  }

  if (value->definitionId == "simulation/stunt-points" && !alignment) {
    if (!maximize || range->kind == TimeRangeKind::All ||
        range->from != range->to) {
      errors.push_back("Stunt points are maximized at one evaluation time.");
      return std::nullopt;
    }
    OptionSettings settings = DefaultStuntPointsOptionSettings();
    settings["targetTimeMs"] = range->from;
    return OptionConfiguration{kStuntPointsEvaluationId, std::move(settings)};
  }

  if (value->definitionId == "simulation/finish-time" && !alignment) {
    if (!minimize || range->kind != TimeRangeKind::All) {
      errors.push_back(
          "Precise finish time is minimized across the whole simulation.");
      return std::nullopt;
    }
    return OptionConfiguration{kPreciseFinishTimeEvaluationId,
                               DefaultPreciseFinishTimeOptionSettings()};
  }

  errors.push_back("This objective composition is type-correct but has no "
                   "native runtime lowering yet.");
  return std::nullopt;
}

std::optional<OptionConfiguration>
CompileGenericObjective(const VisualProgram &program, const VisualNode &score,
                        std::vector<std::string> &errors) {
  const VisualNode *objective = &score;
  const VisualNode *condition = nullptr;
  if (score.definitionId == "objective/only-when") {
    objective = InputNode(program, score, "score", errors);
    condition = InputNode(program, score, "condition", errors);
    if (objective == nullptr || condition == nullptr) return std::nullopt;
  }
  const bool minimize = objective->definitionId == "objective/minimize";
  const bool maximize = objective->definitionId == "objective/maximize";
  if (!minimize && !maximize) {
    errors.push_back(
        "Generic runtime objectives currently support minimize or maximize.");
    return std::nullopt;
  }
  const VisualNode *const value = InputNode(program, *objective, "value", errors);
  const VisualNode *const rangeNode =
      InputNode(program, *objective, "range", errors);
  if (value == nullptr || rangeNode == nullptr) return std::nullopt;
  const auto range = CompileTimeRange(program, *rangeNode, errors);
  if (!range) return std::nullopt;
  const auto expression = SerializeRuntimeExpression(program, *value, errors);
  if (!expression) return std::nullopt;
  std::string conditionExpression = "bool 1";
  if (condition != nullptr) {
    const auto compiledCondition =
        SerializeRuntimeExpression(program, *condition, errors);
    if (!compiledCondition) return std::nullopt;
    conditionExpression = *compiledCondition;
  }

  OptionSettings settings = DefaultVisualExpressionOptionSettings();
  settings["direction"] = minimize ? "minimize" : "maximize";
  settings["rangeKind"] =
      range->kind == TimeRangeKind::All ? "all" : "window";
  settings["minTimeMs"] =
      range->kind == TimeRangeKind::All ? "0" : range->from;
  settings["maxTimeMs"] =
      range->kind == TimeRangeKind::All ? "0" : range->to;
  settings["expression"] = *expression;
  settings["condition"] = std::move(conditionExpression);
  return OptionConfiguration{kVisualExpressionEvaluationId, std::move(settings)};
}

std::optional<OptionConfiguration>
CompileGenericFirstTimeObjective(const VisualProgram &program,
                                 const VisualNode &score,
                                 std::vector<std::string> &errors) {
  const VisualNode *const condition =
      InputNode(program, score, "condition", errors);
  const VisualNode *const rangeNode = InputNode(program, score, "range", errors);
  if (condition == nullptr || rangeNode == nullptr) return std::nullopt;
  const auto range = CompileTimeRange(program, *rangeNode, errors);
  if (!range) return std::nullopt;
  const auto conditionExpression =
      SerializeRuntimeExpression(program, *condition, errors);
  if (!conditionExpression) return std::nullopt;

  OptionSettings settings = DefaultVisualExpressionOptionSettings();
  settings["mode"] = "first-time";
  settings["direction"] = "minimize";
  settings["rangeKind"] =
      range->kind == TimeRangeKind::All ? "all" : "window";
  settings["minTimeMs"] =
      range->kind == TimeRangeKind::All ? "0" : range->from;
  settings["maxTimeMs"] =
      range->kind == TimeRangeKind::All ? "0" : range->to;
  settings["expression"] = "num 0";
  settings["condition"] = *conditionExpression;
  return OptionConfiguration{kVisualExpressionEvaluationId, std::move(settings)};
}

std::optional<OptionConfiguration>
CompileObjective(const VisualProgram &program, const VisualNode &score,
                 std::vector<std::string> &errors) {
  std::vector<std::string> nativeErrors;
  if (const auto native =
          TryCompileNativeObjective(program, score, nativeErrors)) {
    return native;
  }
  if (score.definitionId == "objective/first-time") {
    std::vector<std::string> genericErrors;
    if (const auto generic =
            CompileGenericFirstTimeObjective(program, score, genericErrors)) {
      return generic;
    }
    errors.insert(errors.end(), nativeErrors.begin(), nativeErrors.end());
    errors.insert(errors.end(), genericErrors.begin(), genericErrors.end());
    return std::nullopt;
  }
  return CompileGenericObjective(program, score, errors);
}

std::vector<OptionConfiguration>
CompileMutationWindow(const VisualProgram &program, const VisualNode &window,
                      std::vector<std::string> &errors) {
  const VisualNode *const rangeNode =
      InputNode(program, window, "range", errors);
  const VisualNode *const seedNode = InputNode(program, window, "seed", errors);
  if (rangeNode == nullptr || seedNode == nullptr)
    return {};
  const auto range = CompileTimeRange(program, *rangeNode, errors);
  const auto seed = ConstantScalar(program, *seedNode, errors);
  if (!range || !seed)
    return {};
  if (range->kind == TimeRangeKind::All) {
    errors.push_back("Mutation windows need explicit from/to times.");
    return {};
  }

  AtomFieldValues windowFields{{"minTimeMs", range->from},
                               {"maxTimeMs", range->to},
                               {"seed", CompactNumber(*seed)}};
  std::vector<std::pair<std::string, AtomFieldValues>> atoms;
  const auto body = window.statements.find("body");
  if (body != window.statements.end()) {
    for (const VisualNodeId childId : body->second) {
      const VisualNode *const child = program.find(childId);
      if (child == nullptr)
        continue;
      if (child->definitionId.rfind("mutate/", 0) != 0) {
        errors.push_back(
            "Only mutation commands belong inside a mutation window.");
        continue;
      }
      AtomFieldValues values = child->fields;
      const VisualBlockDefinition *const definition =
          FindVisualBlock(child->definitionId);
      if (definition == nullptr)
        continue;
      bool valid = true;
      for (const VisualInputDefinition &input : definition->inputs) {
        const VisualNode *const valueNode =
            InputNode(program, *child, input.key, errors);
        if (valueNode == nullptr) {
          valid = false;
          continue;
        }
        if (input.nativeSettingKeys.size() == 2u) {
          const bool integerRange = input.type == VisualValueType::IntegerRange;
          const std::string expected =
              integerRange ? "values/integer-range" : "values/number-range";
          if (valueNode->definitionId != expected) {
            errors.push_back("Mutation range parameter '" + input.key +
                             "' must use its compact range block.");
            valid = false;
            continue;
          }
          const auto minimum = valueNode->fields.find("minimum");
          const auto maximum = valueNode->fields.find("maximum");
          const auto minimumValue = minimum == valueNode->fields.end()
                                        ? std::nullopt
                                        : ParseNumberValue(minimum->second);
          const auto maximumValue = maximum == valueNode->fields.end()
                                        ? std::nullopt
                                        : ParseNumberValue(maximum->second);
          if (!minimumValue || !maximumValue ||
              !std::isfinite(*minimumValue) || !std::isfinite(*maximumValue) ||
              *minimumValue > *maximumValue ||
              (integerRange &&
               (std::floor(*minimumValue) != *minimumValue ||
                std::floor(*maximumValue) != *maximumValue))) {
            errors.push_back("Mutation range parameter '" + input.key +
                             "' contains an invalid min/max pair.");
            valid = false;
            continue;
          }
          values[input.nativeSettingKeys[0]] = CompactNumber(*minimumValue);
          values[input.nativeSettingKeys[1]] = CompactNumber(*maximumValue);
          continue;
        }
        const auto value = ConstantSettingValue(program, *valueNode, errors);
        if (!value) {
          valid = false;
          continue;
        }
        values[input.key] = *value;
      }
      if (valid)
        atoms.emplace_back(child->definitionId, std::move(values));
    }
  }
  if (atoms.empty()) {
    errors.push_back("Mutation window is empty.");
    return {};
  }
  const MutationGroupLowering lowered = LowerMutationGroup(atoms, windowFields);
  errors.insert(errors.end(), lowered.errors.begin(), lowered.errors.end());
  return lowered.configurations;
}

} // namespace

CompileResult
CompileVisualProgram(const VisualProgram &program,
                     const SearchComponentConfiguration &baseConfiguration) {
  CompileResult result;
  result.configuration = baseConfiguration;

  const VisualProgramValidation structural = ValidateVisualProgram(program);
  if (!structural.ok) {
    result.errors = structural.errors;
    return result;
  }

  const VisualNode *root = nullptr;
  for (const VisualNodeId id : program.topLevel) {
    const VisualNode *const node = program.find(id);
    if (node == nullptr || node->definitionId != "flow/when-start")
      continue;
    if (root != nullptr) {
      result.errors.push_back("Use exactly one 'when search starts' block.");
      return result;
    }
    root = node;
  }
  if (root == nullptr) {
    result.errors.push_back("Add a 'when search starts' block.");
    return result;
  }

  bool searchSeen = false;
  std::vector<OptionConfiguration> modifiers;
  std::vector<std::size_t> modifierWindowGroups;
  std::size_t mutationWindowIndex = 0u;
  const auto body = root->statements.find("body");
  if (body != root->statements.end()) {
    for (const VisualNodeId childId : body->second) {
      const VisualNode *const child = program.find(childId);
      if (child == nullptr)
        continue;
      if (child->definitionId.rfind("search/", 0) == 0) {
        if (searchSeen) {
          result.errors.push_back("A search can have only one search policy.");
          continue;
        }
        searchSeen = true;
        const std::string registrationId = child->definitionId.substr(7);
        const SearchAlgorithmRegistration *const registration =
            FindSearchAlgorithm(registrationId);
        const VisualBlockDefinition *const definition =
            FindVisualBlock(child->definitionId);
        if (registration == nullptr || definition == nullptr) {
          result.errors.push_back("Unknown search policy '" + registrationId +
                                  "'.");
          continue;
        }
        OptionSettings settings = registration->defaultSettings;
        for (const auto &[key, value] : child->fields)
          settings[key] = value;
        bool valid = true;
        for (const VisualInputDefinition &input : definition->inputs) {
          const VisualNode *const valueNode =
              InputNode(program, *child, input.key, result.errors);
          if (valueNode == nullptr) {
            valid = false;
            continue;
          }
          const auto value =
              ConstantSettingValue(program, *valueNode, result.errors);
          if (!value) {
            valid = false;
            continue;
          }
          settings[input.key] = *value;
        }
        if (valid)
          result.configuration.searchAlgorithm =
              OptionConfiguration{registration->id, std::move(settings)};
        bool objectiveSeen = false;
        bool simulationSeen = false;
        bool selectionFinished = false;
        const auto iteration = child->statements.find("body");
        if (iteration != child->statements.end()) {
          for (const VisualNodeId stepId : iteration->second) {
            const VisualNode *const step = program.find(stepId);
            if (step == nullptr)
              continue;
            if (step->definitionId == "flow/simulate") {
              if (selectionFinished) {
                result.errors.push_back(
                    "Simulation must happen before selecting the best result.");
                continue;
              }
              if (simulationSeen) {
                result.errors.push_back(
                    "A bruteforce iteration can have only one simulate step.");
                continue;
              }
              simulationSeen = true;
              const VisualNode *const until =
                  InputNode(program, *step, "until", result.errors);
              const VisualNode *const where =
                  InputNode(program, *step, "where", result.errors);
              if (until == nullptr || where == nullptr)
                continue;
              const auto horizon = ConstantScalar(program, *until, result.errors);
              if (!horizon)
                continue;
              if (*horizon <= 0.0 || std::floor(*horizon) != *horizon ||
                  *horizon > 4294967295.0) {
                result.errors.push_back(
                    "Simulation horizon must be a positive whole number of milliseconds.");
                continue;
              }
              result.simulationHorizonMs = CompactNumber(*horizon);
              const auto condition =
                  CompileSimulationCondition(program, *where, result.errors);
              if (condition)
                result.conditionProgram = *condition;
            } else if (step->definitionId == "flow/set-objective") {
              if (!simulationSeen) {
                result.errors.push_back(
                    "Select-best must come after the simulate step.");
              }
              if (objectiveSeen) {
                result.errors.push_back(
                    "A bruteforce iteration can have only one select-best step.");
                continue;
              }
              objectiveSeen = true;
              selectionFinished = true;
              const VisualNode *const score =
                  InputNode(program, *step, "score", result.errors);
              if (score != nullptr) {
                const auto compiled =
                    CompileObjective(program, *score, result.errors);
                if (compiled)
                  result.configuration.evaluationTarget = *compiled;
              }
            } else if (step->definitionId == "flow/mutation-window") {
              if (simulationSeen || selectionFinished) {
                result.errors.push_back(
                    "Mutation steps must come before simulation in each bruteforce iteration.");
                continue;
              }
              const auto compiled =
                  CompileMutationWindow(program, *step, result.errors);
              modifiers.insert(modifiers.end(), compiled.begin(), compiled.end());
              modifierWindowGroups.insert(modifierWindowGroups.end(),
                                          compiled.size(), mutationWindowIndex++);
            } else {
              result.errors.push_back("Unsupported bruteforce iteration step '" +
                                      step->definitionId + "'.");
            }
          }
        }
        if (!objectiveSeen) {
          result.errors.push_back(
              "Add a select-best step inside the bruteforce iteration.");
        }
        if (!simulationSeen) {
          result.errors.push_back(
              "Add a simulate step inside the bruteforce iteration.");
        }
      } else {
        result.errors.push_back(
            "Only a search process belongs under 'when search starts'.");
      }
    }
  }
  if (!searchSeen) {
    result.errors.push_back("Add a search policy to the search.");
  }
  if (modifiers.empty()) {
    result.errors.push_back(
        "Add at least one mutate step inside the bruteforce iteration.");
  }
  result.configuration.modifiers = std::move(modifiers);
  result.configuration.modifierWindowGroups = std::move(modifierWindowGroups);
  result.ok = result.errors.empty();
  return result;
}

} // namespace forevertas::blocks
