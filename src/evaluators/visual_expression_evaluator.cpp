#include "evaluators/visual_expression_evaluator.h"

#include "evaluators/evaluator_utils.h"
#include "searches/option_settings_utils.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace forevertas {
namespace {

enum class ValueKind { Scalar, Vector, Rotation, Boolean };

struct ExpressionPoint2 {
  double x = 0.0;
  double y = 0.0;
};

enum class ExpressionPlane { XY, XZ, YZ };

struct ExpressionPrism {
  ExpressionPlane plane = ExpressionPlane::XZ;
  std::vector<ExpressionPoint2> polygon;
};

double Cross(const ExpressionPoint2 &left, const ExpressionPoint2 &right) {
  return left.x * right.y - left.y * right.x;
}

ExpressionPoint2 Subtract(const ExpressionPoint2 &left,
                          const ExpressionPoint2 &right) {
  return {left.x - right.x, left.y - right.y};
}

bool OnSegment(const ExpressionPoint2 &point, const ExpressionPoint2 &from,
               const ExpressionPoint2 &to) {
  constexpr double tolerance = 1e-9;
  if (std::abs(Cross(Subtract(point, from), Subtract(to, from))) > tolerance)
    return false;
  return point.x >= std::min(from.x, to.x) - tolerance &&
         point.x <= std::max(from.x, to.x) + tolerance &&
         point.y >= std::min(from.y, to.y) - tolerance &&
         point.y <= std::max(from.y, to.y) + tolerance;
}

bool Contains2D(const std::vector<ExpressionPoint2> &polygon,
                const ExpressionPoint2 &point) {
  bool inside = false;
  for (std::size_t index = 0u, previous = polygon.size() - 1u;
       index < polygon.size(); previous = index++) {
    const ExpressionPoint2 &a = polygon[previous];
    const ExpressionPoint2 &b = polygon[index];
    if (OnSegment(point, a, b)) return true;
    const bool crosses = (a.y > point.y) != (b.y > point.y);
    if (crosses) {
      const double crossingX =
          (b.x - a.x) * (point.y - a.y) / (b.y - a.y) + a.x;
      if (point.x < crossingX) inside = !inside;
    }
  }
  return inside;
}

int Orientation(const ExpressionPoint2 &a, const ExpressionPoint2 &b,
                const ExpressionPoint2 &c) {
  const double value = Cross(Subtract(b, a), Subtract(c, a));
  if (std::abs(value) <= 1e-9) return 0;
  return value > 0.0 ? 1 : -1;
}

bool ProperlyIntersects(const ExpressionPoint2 &a, const ExpressionPoint2 &b,
                        const ExpressionPoint2 &c, const ExpressionPoint2 &d) {
  const int first = Orientation(a, b, c);
  const int second = Orientation(a, b, d);
  const int third = Orientation(c, d, a);
  const int fourth = Orientation(c, d, b);
  if (first == 0 && OnSegment(c, a, b)) return true;
  if (second == 0 && OnSegment(d, a, b)) return true;
  if (third == 0 && OnSegment(a, c, d)) return true;
  if (fourth == 0 && OnSegment(b, c, d)) return true;
  return first != second && third != fourth;
}

bool IsSimplePolygon(const std::vector<ExpressionPoint2> &polygon) {
  if (polygon.size() < 3u || polygon.size() > 256u) return false;
  double areaTwice = 0.0;
  for (std::size_t index = 0u; index < polygon.size(); ++index) {
    const ExpressionPoint2 &a = polygon[index];
    const ExpressionPoint2 &b = polygon[(index + 1u) % polygon.size()];
    const ExpressionPoint2 &previous =
        polygon[(index + polygon.size() - 1u) % polygon.size()];
    areaTwice += Cross(a, b);
    if (std::hypot(b.x - a.x, b.y - a.y) <= 1e-9) return false;
    if (std::abs(Cross(Subtract(a, previous), Subtract(b, a))) <= 1e-9)
      return false;
    for (std::size_t other = index + 1u; other < polygon.size(); ++other) {
      if (other == (index + 1u) % polygon.size() ||
          (other + 1u) % polygon.size() == index)
        continue;
      if (ProperlyIntersects(a, b, polygon[other],
                             polygon[(other + 1u) % polygon.size()]))
        return false;
    }
  }
  return std::abs(areaTwice) > 1e-8;
}

struct EvaluationQuaternion {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  double w = 1.0;
};

double QuaternionLength(const EvaluationQuaternion &value) {
  return std::sqrt(value.x * value.x + value.y * value.y +
                   value.z * value.z + value.w * value.w);
}

std::optional<EvaluationQuaternion>
NormalizeQuaternion(const EvaluationQuaternion &value) {
  const double length = QuaternionLength(value);
  if (!std::isfinite(length) || length <= 1e-12)
    return std::nullopt;
  return EvaluationQuaternion{value.x / length, value.y / length,
                              value.z / length, value.w / length};
}

std::optional<EvaluationQuaternion> RotationOf(
    const forevervalidator::experimental::PhysicsSandboxStateView &state) {
  return NormalizeQuaternion({state.car.rotationX, state.car.rotationY,
                              state.car.rotationZ, state.car.rotationW});
}

std::optional<double>
RotationDistanceRadians(const EvaluationQuaternion &left,
                        const EvaluationQuaternion &right) {
  const auto a = NormalizeQuaternion(left);
  const auto b = NormalizeQuaternion(right);
  if (!a || !b)
    return std::nullopt;
  const double dot = std::abs(a->x * b->x + a->y * b->y +
                              a->z * b->z + a->w * b->w);
  return 2.0 * std::acos(std::clamp(dot, 0.0, 1.0));
}

enum class Op {
  Number,
  Boolean,
  CarPosition,
  CarVelocity,
  CarLocalVelocity,
  CarSpeed,
  StuntPoints,
  FinishTime,
  SimulationTime,
  CheckpointCount,
  RaceCompleted,
  Sliding,
  FreeWheeling,
  Vector,
  Direction,
  Rotation,
  Distance,
  Magnitude,
  Normalize,
  Dot,
  RotationDistance,
  WeightedBlend,
  PercentRatio,
  KilometersPerHour,
  Absolute,
  Clamp,
  Add,
  Subtract,
  Multiply,
  Divide,
  Minimum,
  Maximum,
  Less,
  LessEqual,
  Equal,
  GreaterEqual,
  Greater,
  And,
  Or,
  Not,
  InsideBox,
  InsidePrism,
};

struct ExpressionNode {
  Op op = Op::Number;
  ValueKind kind = ValueKind::Scalar;
  double number = 0.0;
  bool boolean = false;
  int a = -1;
  int b = -1;
  int c = -1;
  int payload = -1;
};

struct ExpressionProgram {
  std::vector<ExpressionNode> nodes;
  std::vector<ExpressionPrism> prisms;
  int root = -1;
};

struct RuntimeValue {
  ValueKind kind = ValueKind::Scalar;
  double scalar = 0.0;
  EvaluationVector3 vector;
  EvaluationQuaternion rotation;
  bool boolean = false;
};

struct ParseResult {
  std::optional<ExpressionProgram> program;
  std::string error;
};

class ExpressionParser final {
public:
  explicit ExpressionParser(std::string text) : stream_(std::move(text)) {}

  ParseResult Parse(ValueKind expected) {
    ExpressionProgram program;
    std::string error;
    const int root = ParseNode(program, error, 0u);
    if (root < 0) return {{}, std::move(error)};
    std::string trailing;
    if (stream_ >> trailing) {
      return {{}, "visual expression contains trailing token '" + trailing +
                      "'"};
    }
    if (program.nodes[static_cast<std::size_t>(root)].kind != expected) {
      return {{}, "visual expression root has the wrong value type"};
    }
    program.root = root;
    return {std::move(program), {}};
  }

private:
  int Append(ExpressionProgram &program, ExpressionNode node) {
    program.nodes.push_back(std::move(node));
    return static_cast<int>(program.nodes.size() - 1u);
  }

  int Child(ExpressionProgram &program, std::string &error, std::size_t depth,
            ValueKind kind) {
    const int id = ParseNode(program, error, depth + 1u);
    if (id < 0) return -1;
    if (program.nodes[static_cast<std::size_t>(id)].kind != kind) {
      error = "visual expression child has the wrong value type";
      return -1;
    }
    return id;
  }

  int Unary(ExpressionProgram &program, std::string &error, std::size_t depth,
            Op op, ValueKind input, ValueKind output) {
    const int a = Child(program, error, depth, input);
    if (a < 0) return -1;
    ExpressionNode node;
    node.op = op;
    node.kind = output;
    node.a = a;
    return Append(program, node);
  }

  int Binary(ExpressionProgram &program, std::string &error, std::size_t depth,
             Op op, ValueKind input, ValueKind output) {
    const int a = Child(program, error, depth, input);
    if (a < 0) return -1;
    const int b = Child(program, error, depth, input);
    if (b < 0) return -1;
    ExpressionNode node;
    node.op = op;
    node.kind = output;
    node.a = a;
    node.b = b;
    return Append(program, node);
  }

  int ParseNode(ExpressionProgram &program, std::string &error,
                std::size_t depth) {
    if (depth > 128u) {
      error = "visual expression exceeds the nesting limit";
      return -1;
    }
    if (program.nodes.size() >= 4096u) {
      error = "visual expression exceeds the node limit";
      return -1;
    }
    std::string token;
    if (!(stream_ >> token)) {
      error = "visual expression ended unexpectedly";
      return -1;
    }
    if (token == "num") {
      std::string value;
      if (!(stream_ >> value)) {
        error = "numeric expression literal has no value";
        return -1;
      }
      const auto parsed = ParseFiniteDouble(value);
      if (!parsed) {
        error = "numeric expression literal is invalid";
        return -1;
      }
      ExpressionNode node;
      node.op = Op::Number;
      node.kind = ValueKind::Scalar;
      node.number = *parsed;
      return Append(program, node);
    }
    if (token == "bool") {
      std::string value;
      if (!(stream_ >> value) || (value != "0" && value != "1")) {
        error = "boolean expression literal is invalid";
        return -1;
      }
      ExpressionNode node;
      node.op = Op::Boolean;
      node.kind = ValueKind::Boolean;
      node.boolean = value == "1";
      return Append(program, node);
    }
    const auto state = [&](Op op, ValueKind kind) {
      ExpressionNode node;
      node.op = op;
      node.kind = kind;
      return Append(program, node);
    };
    if (token == "carpos") return state(Op::CarPosition, ValueKind::Vector);
    if (token == "carvel") return state(Op::CarVelocity, ValueKind::Vector);
    if (token == "carlocalvel")
      return state(Op::CarLocalVelocity, ValueKind::Vector);
    if (token == "carspeed") return state(Op::CarSpeed, ValueKind::Scalar);
    if (token == "stunts") return state(Op::StuntPoints, ValueKind::Scalar);
    if (token == "finishms") return state(Op::FinishTime, ValueKind::Scalar);
    if (token == "simtime") return state(Op::SimulationTime, ValueKind::Scalar);
    if (token == "checkpoints")
      return state(Op::CheckpointCount, ValueKind::Scalar);
    if (token == "completed")
      return state(Op::RaceCompleted, ValueKind::Boolean);
    if (token == "sliding") return state(Op::Sliding, ValueKind::Boolean);
    if (token == "freewheeling")
      return state(Op::FreeWheeling, ValueKind::Boolean);
    if (token == "carrot") return state(Op::Rotation, ValueKind::Rotation);

    if (token == "vec" || token == "dir" || token == "rot") {
      const int a = Child(program, error, depth, ValueKind::Scalar);
      if (a < 0) return -1;
      const int b = Child(program, error, depth, ValueKind::Scalar);
      if (b < 0) return -1;
      const int c = Child(program, error, depth, ValueKind::Scalar);
      if (c < 0) return -1;
      ExpressionNode node;
      node.op = token == "rot" ? Op::Rotation
                                : token == "dir" ? Op::Direction : Op::Vector;
      node.kind = token == "rot" ? ValueKind::Rotation : ValueKind::Vector;
      node.a = a;
      node.b = b;
      node.c = c;
      return Append(program, node);
    }
    if (token == "distance")
      return Binary(program, error, depth, Op::Distance, ValueKind::Vector,
                    ValueKind::Scalar);
    if (token == "magnitude")
      return Unary(program, error, depth, Op::Magnitude, ValueKind::Vector,
                   ValueKind::Scalar);
    if (token == "normalize")
      return Unary(program, error, depth, Op::Normalize, ValueKind::Vector,
                   ValueKind::Vector);
    if (token == "dot")
      return Binary(program, error, depth, Op::Dot, ValueKind::Vector,
                    ValueKind::Scalar);
    if (token == "rotdistance")
      return Binary(program, error, depth, Op::RotationDistance,
                    ValueKind::Rotation, ValueKind::Scalar);
    if (token == "ratio")
      return Unary(program, error, depth, Op::PercentRatio, ValueKind::Scalar,
                   ValueKind::Scalar);
    if (token == "kmh")
      return Unary(program, error, depth, Op::KilometersPerHour,
                   ValueKind::Scalar, ValueKind::Scalar);
    if (token == "abs")
      return Unary(program, error, depth, Op::Absolute, ValueKind::Scalar,
                   ValueKind::Scalar);
    if (token == "clamp") {
      const int value = Child(program, error, depth, ValueKind::Scalar);
      if (value < 0) return -1;
      const int minimum = Child(program, error, depth, ValueKind::Scalar);
      if (minimum < 0) return -1;
      const int maximum = Child(program, error, depth, ValueKind::Scalar);
      if (maximum < 0) return -1;
      ExpressionNode node;
      node.op = Op::Clamp;
      node.kind = ValueKind::Scalar;
      node.a = value;
      node.b = minimum;
      node.c = maximum;
      return Append(program, node);
    }
    if (token == "add")
      return Binary(program, error, depth, Op::Add, ValueKind::Scalar,
                    ValueKind::Scalar);
    if (token == "sub")
      return Binary(program, error, depth, Op::Subtract, ValueKind::Scalar,
                    ValueKind::Scalar);
    if (token == "mul")
      return Binary(program, error, depth, Op::Multiply, ValueKind::Scalar,
                    ValueKind::Scalar);
    if (token == "div")
      return Binary(program, error, depth, Op::Divide, ValueKind::Scalar,
                    ValueKind::Scalar);
    if (token == "min")
      return Binary(program, error, depth, Op::Minimum, ValueKind::Scalar,
                    ValueKind::Scalar);
    if (token == "max")
      return Binary(program, error, depth, Op::Maximum, ValueKind::Scalar,
                    ValueKind::Scalar);
    if (token == "lt")
      return Binary(program, error, depth, Op::Less, ValueKind::Scalar,
                    ValueKind::Boolean);
    if (token == "le")
      return Binary(program, error, depth, Op::LessEqual, ValueKind::Scalar,
                    ValueKind::Boolean);
    if (token == "eq")
      return Binary(program, error, depth, Op::Equal, ValueKind::Scalar,
                    ValueKind::Boolean);
    if (token == "ge")
      return Binary(program, error, depth, Op::GreaterEqual, ValueKind::Scalar,
                    ValueKind::Boolean);
    if (token == "gt")
      return Binary(program, error, depth, Op::Greater, ValueKind::Scalar,
                    ValueKind::Boolean);
    if (token == "and")
      return Binary(program, error, depth, Op::And, ValueKind::Boolean,
                    ValueKind::Boolean);
    if (token == "or")
      return Binary(program, error, depth, Op::Or, ValueKind::Boolean,
                    ValueKind::Boolean);
    if (token == "not")
      return Unary(program, error, depth, Op::Not, ValueKind::Boolean,
                   ValueKind::Boolean);
    if (token == "insidebox") {
      const int position = Child(program, error, depth, ValueKind::Vector);
      if (position < 0) return -1;
      const int center = Child(program, error, depth, ValueKind::Vector);
      if (center < 0) return -1;
      const int size = Child(program, error, depth, ValueKind::Vector);
      if (size < 0) return -1;
      ExpressionNode node;
      node.op = Op::InsideBox;
      node.kind = ValueKind::Boolean;
      node.a = position;
      node.b = center;
      node.c = size;
      return Append(program, node);
    }
    if (token == "insideprism") {
      const int position = Child(program, error, depth, ValueKind::Vector);
      if (position < 0) return -1;
      const int origin = Child(program, error, depth, ValueKind::Vector);
      if (origin < 0) return -1;
      const int prismDepth = Child(program, error, depth, ValueKind::Scalar);
      if (prismDepth < 0) return -1;
      std::string planeToken;
      std::string countToken;
      if (!(stream_ >> planeToken >> countToken)) {
        error = "inside-prism expression is missing geometry";
        return -1;
      }
      ExpressionPlane plane;
      if (planeToken == "xy")
        plane = ExpressionPlane::XY;
      else if (planeToken == "xz")
        plane = ExpressionPlane::XZ;
      else if (planeToken == "yz")
        plane = ExpressionPlane::YZ;
      else {
        error = "inside-prism expression has an invalid plane";
        return -1;
      }
      std::uint32_t count = 0u;
      const auto countResult = std::from_chars(
          countToken.data(), countToken.data() + countToken.size(), count);
      if (countResult.ec != std::errc{} ||
          countResult.ptr != countToken.data() + countToken.size() || count < 3u ||
          count > 256u) {
        error = "inside-prism polygon must contain between 3 and 256 vertices";
        return -1;
      }
      ExpressionPrism prism;
      prism.plane = plane;
      prism.polygon.reserve(count);
      for (std::uint32_t index = 0u; index < count; ++index) {
        std::string xText;
        std::string yText;
        if (!(stream_ >> xText >> yText)) {
          error = "inside-prism polygon ended unexpectedly";
          return -1;
        }
        const auto x = ParseFiniteDouble(xText);
        const auto y = ParseFiniteDouble(yText);
        if (!x || !y || std::abs(*x) > 10000000.0 ||
            std::abs(*y) > 10000000.0) {
          error = "inside-prism polygon contains an invalid coordinate";
          return -1;
        }
        prism.polygon.push_back({*x, *y});
      }
      if (!IsSimplePolygon(prism.polygon)) {
        error = "inside-prism polygon must be simple and non-degenerate";
        return -1;
      }
      const int payload = static_cast<int>(program.prisms.size());
      program.prisms.push_back(std::move(prism));
      ExpressionNode node;
      node.op = Op::InsidePrism;
      node.kind = ValueKind::Boolean;
      node.a = position;
      node.b = origin;
      node.c = prismDepth;
      node.payload = payload;
      return Append(program, node);
    }
    if (token == "blend") {
      const int a = Child(program, error, depth, ValueKind::Scalar);
      if (a < 0) return -1;
      const int b = Child(program, error, depth, ValueKind::Scalar);
      if (b < 0) return -1;
      const int c = Child(program, error, depth, ValueKind::Scalar);
      if (c < 0) return -1;
      ExpressionNode node;
      node.op = Op::WeightedBlend;
      node.kind = ValueKind::Scalar;
      node.a = a;
      node.b = b;
      node.c = c;
      return Append(program, node);
    }
    error = "unsupported visual expression opcode '" + token + "'";
    return -1;
  }

  std::istringstream stream_;
};

std::optional<RuntimeValue>
EvaluateNode(const ExpressionProgram &program, int id,
             const forevervalidator::experimental::PhysicsSandboxStateView &state) {
  const ExpressionNode &node = program.nodes[static_cast<std::size_t>(id)];
  const auto child = [&](int childId) {
    return EvaluateNode(program, childId, state);
  };
  const auto scalar = [&](int childId) -> std::optional<double> {
    const auto value = child(childId);
    return value && value->kind == ValueKind::Scalar
               ? std::optional<double>(value->scalar)
               : std::nullopt;
  };
  const auto vector = [&](int childId) -> std::optional<EvaluationVector3> {
    const auto value = child(childId);
    return value && value->kind == ValueKind::Vector
               ? std::optional<EvaluationVector3>(value->vector)
               : std::nullopt;
  };
  const auto rotation =
      [&](int childId) -> std::optional<EvaluationQuaternion> {
    const auto value = child(childId);
    return value && value->kind == ValueKind::Rotation
               ? std::optional<EvaluationQuaternion>(value->rotation)
               : std::nullopt;
  };
  const auto boolean = [&](int childId) -> std::optional<bool> {
    const auto value = child(childId);
    return value && value->kind == ValueKind::Boolean
               ? std::optional<bool>(value->boolean)
               : std::nullopt;
  };

  RuntimeValue result;
  result.kind = node.kind;
  switch (node.op) {
  case Op::Number:
    result.scalar = node.number;
    return result;
  case Op::Boolean:
    result.boolean = node.boolean;
    return result;
  case Op::CarPosition:
    result.vector = PositionOf(state);
    return result;
  case Op::CarVelocity:
    result.vector = VelocityOf(state);
    return result;
  case Op::CarLocalVelocity:
    result.vector = {state.car.localSpeed.x, state.car.localSpeed.y,
                     state.car.localSpeed.z};
    return result;
  case Op::CarSpeed:
    result.scalar = Length(VelocityOf(state));
    return result;
  case Op::StuntPoints:
    result.scalar = static_cast<double>(state.stuntsScore.value_or(0u));
    return result;
  case Op::FinishTime:
    if (!state.raceCompleted || !state.finishTime ||
        !state.finishTime->IsValid())
      return std::nullopt;
    result.scalar = static_cast<double>(state.finishTime->upperBoundNs) / 1.0e6;
    return result;
  case Op::SimulationTime:
    result.scalar = static_cast<double>(state.timeMs);
    return result;
  case Op::CheckpointCount:
    result.scalar = static_cast<double>(state.checkpointsCollected);
    return result;
  case Op::RaceCompleted:
    result.boolean = state.raceCompleted;
    return result;
  case Op::Sliding:
    result.boolean = state.car.sliding;
    return result;
  case Op::FreeWheeling:
    result.boolean = state.car.freeWheeling;
    return result;
  case Op::Vector:
  case Op::Direction: {
    const auto x = scalar(node.a);
    const auto y = scalar(node.b);
    const auto z = scalar(node.c);
    if (!x || !y || !z) return std::nullopt;
    result.vector = {*x, *y, *z};
    if (node.op == Op::Direction) result.vector = Normalize(result.vector);
    return result;
  }
  case Op::Rotation: {
    if (node.a < 0) {
      const auto rotation = RotationOf(state);
      if (!rotation)
        return std::nullopt;
      result.rotation = *rotation;
      return result;
    }
    const auto yaw = scalar(node.a);
    const auto pitch = scalar(node.b);
    const auto roll = scalar(node.c);
    if (!yaw || !pitch || !roll) return std::nullopt;
    constexpr double degreesToRadians = 3.14159265358979323846 / 180.0;
    const double hy = *yaw * degreesToRadians * 0.5;
    const double hp = *pitch * degreesToRadians * 0.5;
    const double hr = *roll * degreesToRadians * 0.5;
    const double cy = std::cos(hy);
    const double sy = std::sin(hy);
    const double cp = std::cos(hp);
    const double sp = std::sin(hp);
    const double cr = std::cos(hr);
    const double sr = std::sin(hr);
    const auto rotation = NormalizeQuaternion(EvaluationQuaternion{
        sr * cp * cy - cr * sp * sy, cr * sp * cy + sr * cp * sy,
        cr * cp * sy - sr * sp * cy, cr * cp * cy + sr * sp * sy});
    if (!rotation)
      return std::nullopt;
    result.rotation = *rotation;
    return result;
  }
  case Op::Distance: {
    const auto a = vector(node.a);
    const auto b = vector(node.b);
    if (!a || !b) return std::nullopt;
    result.scalar = Distance(*a, *b);
    return result;
  }
  case Op::Magnitude: {
    const auto value = vector(node.a);
    if (!value) return std::nullopt;
    result.scalar = Length(*value);
    return result;
  }
  case Op::Normalize: {
    const auto value = vector(node.a);
    if (!value) return std::nullopt;
    result.vector = Normalize(*value);
    return result;
  }
  case Op::Dot: {
    const auto a = vector(node.a);
    const auto b = vector(node.b);
    if (!a || !b) return std::nullopt;
    result.scalar = Dot(*a, *b);
    return result;
  }
  case Op::RotationDistance: {
    const auto a = rotation(node.a);
    const auto b = rotation(node.b);
    if (!a || !b) return std::nullopt;
    const auto distance = RotationDistanceRadians(*a, *b);
    if (!distance)
      return std::nullopt;
    result.scalar = *distance;
    return result;
  }
  case Op::PercentRatio: {
    const auto value = scalar(node.a);
    if (!value) return std::nullopt;
    result.scalar = *value / 100.0;
    return result;
  }
  case Op::KilometersPerHour: {
    const auto value = scalar(node.a);
    if (!value) return std::nullopt;
    result.scalar = *value * 3.6;
    return std::isfinite(result.scalar) ? std::optional<RuntimeValue>(result)
                                        : std::nullopt;
  }
  case Op::Absolute: {
    const auto value = scalar(node.a);
    if (!value) return std::nullopt;
    result.scalar = std::abs(*value);
    return result;
  }
  case Op::Clamp: {
    const auto value = scalar(node.a);
    const auto minimum = scalar(node.b);
    const auto maximum = scalar(node.c);
    if (!value || !minimum || !maximum || *minimum > *maximum)
      return std::nullopt;
    result.scalar = std::clamp(*value, *minimum, *maximum);
    return result;
  }
  case Op::Add:
  case Op::Subtract:
  case Op::Multiply:
  case Op::Divide:
  case Op::Minimum:
  case Op::Maximum:
  case Op::Less:
  case Op::LessEqual:
  case Op::Equal:
  case Op::GreaterEqual:
  case Op::Greater: {
    const auto a = scalar(node.a);
    const auto b = scalar(node.b);
    if (!a || !b) return std::nullopt;
    if (node.op == Op::Less) {
      result.boolean = *a < *b;
      return result;
    }
    if (node.op == Op::LessEqual) {
      result.boolean = *a <= *b;
      return result;
    }
    if (node.op == Op::Equal) {
      result.boolean = *a == *b;
      return result;
    }
    if (node.op == Op::GreaterEqual) {
      result.boolean = *a >= *b;
      return result;
    }
    if (node.op == Op::Greater) {
      result.boolean = *a > *b;
      return result;
    }
    if (node.op == Op::Divide && std::abs(*b) <= 1e-15)
      return std::nullopt;
    if (node.op == Op::Add) result.scalar = *a + *b;
    if (node.op == Op::Subtract) result.scalar = *a - *b;
    if (node.op == Op::Multiply) result.scalar = *a * *b;
    if (node.op == Op::Divide) result.scalar = *a / *b;
    if (node.op == Op::Minimum) result.scalar = std::min(*a, *b);
    if (node.op == Op::Maximum) result.scalar = std::max(*a, *b);
    if (!std::isfinite(result.scalar)) return std::nullopt;
    return result;
  }
  case Op::And:
  case Op::Or: {
    const auto a = boolean(node.a);
    const auto b = boolean(node.b);
    if (!a || !b) return std::nullopt;
    result.boolean = node.op == Op::And ? (*a && *b) : (*a || *b);
    return result;
  }
  case Op::Not: {
    const auto value = boolean(node.a);
    if (!value) return std::nullopt;
    result.boolean = !*value;
    return result;
  }
  case Op::InsideBox: {
    const auto position = child(node.a);
    const auto center = child(node.b);
    const auto size = child(node.c);
    if (!position || !center || !size ||
        position->kind != ValueKind::Vector ||
        center->kind != ValueKind::Vector || size->kind != ValueKind::Vector ||
        !std::isfinite(size->vector.x) || !std::isfinite(size->vector.y) ||
        !std::isfinite(size->vector.z) || size->vector.x <= 0.0 ||
        size->vector.y <= 0.0 || size->vector.z <= 0.0) {
      return std::nullopt;
    }
    result.boolean =
        std::abs(position->vector.x - center->vector.x) <= size->vector.x * 0.5 &&
        std::abs(position->vector.y - center->vector.y) <= size->vector.y * 0.5 &&
        std::abs(position->vector.z - center->vector.z) <= size->vector.z * 0.5;
    return result;
  }
  case Op::InsidePrism: {
    const auto position = vector(node.a);
    const auto origin = vector(node.b);
    const auto depth = scalar(node.c);
    if (!position || !origin || !depth || !std::isfinite(*depth) || *depth <= 0.0 ||
        node.payload < 0 ||
        static_cast<std::size_t>(node.payload) >= program.prisms.size()) {
      return std::nullopt;
    }
    const ExpressionPrism &prism =
        program.prisms[static_cast<std::size_t>(node.payload)];
    ExpressionPoint2 projected;
    double normal = 0.0;
    switch (prism.plane) {
    case ExpressionPlane::XY:
      projected = {position->x - origin->x, position->y - origin->y};
      normal = position->z - origin->z;
      break;
    case ExpressionPlane::XZ:
      projected = {position->x - origin->x, position->z - origin->z};
      normal = position->y - origin->y;
      break;
    case ExpressionPlane::YZ:
      projected = {position->y - origin->y, position->z - origin->z};
      normal = position->x - origin->x;
      break;
    }
    result.boolean = normal >= 0.0 && normal <= *depth &&
                     Contains2D(prism.polygon, projected);
    return result;
  }
  case Op::WeightedBlend: {
    const auto a = scalar(node.a);
    const auto b = scalar(node.b);
    const auto percent = scalar(node.c);
    if (!a || !b || !percent) return std::nullopt;
    const double weight = *percent / 100.0;
    result.scalar = *a * (1.0 - weight) + *b * weight;
    return std::isfinite(result.scalar) ? std::optional<RuntimeValue>(result)
                                        : std::nullopt;
  }
  }
  return std::nullopt;
}

class VisualExpressionSession final : public IterationEvaluationSession {
public:
  VisualExpressionSession(bool firstTime, ExpressionProgram score,
                          ExpressionProgram condition)
      : firstTime_(firstTime), score_(std::move(score)),
        condition_(std::move(condition)) {}

  std::optional<EvaluationSample> Observe(
      const std::optional<forevervalidator::experimental::PhysicsSandboxStateView>
          &previous,
      const forevervalidator::experimental::PhysicsSandboxStateView &current)
      override {
    static_cast<void>(previous);
    const auto condition = EvaluateNode(condition_, condition_.root, current);
    if (!condition || condition->kind != ValueKind::Boolean ||
        !condition->boolean) {
      return std::nullopt;
    }
    const double timeMs = static_cast<double>(current.timeMs);
    if (firstTime_) {
      if (reported_) return std::nullopt;
      reported_ = true;
      return EvaluationSample{
          timeMs, timeMs, TimeMetricDescription("First matching time", timeMs)};
    }
    const auto score = EvaluateNode(score_, score_.root, current);
    if (!score || score->kind != ValueKind::Scalar ||
        !std::isfinite(score->scalar)) {
      return std::nullopt;
    }
    return EvaluationSample{
        score->scalar, timeMs,
        MetricDescription("Visual expression", score->scalar, "", timeMs)};
  }

private:
  bool firstTime_ = false;
  bool reported_ = false;
  ExpressionProgram score_;
  ExpressionProgram condition_;
};

class VisualExpressionEvaluator final : public IterationEvaluator {
public:
  VisualExpressionEvaluator(bool firstTime, bool minimize, bool allTime,
                            std::int64_t minimumTimeMs,
                            std::int64_t maximumTimeMs,
                            ExpressionProgram score,
                            ExpressionProgram condition)
      : firstTime_(firstTime), minimize_(minimize), allTime_(allTime),
        minimumTimeMs_(minimumTimeMs),
        maximumTimeMs_(maximumTimeMs), score_(std::move(score)),
        condition_(std::move(condition)) {}

  EvaluationPlan Plan(std::int64_t simulationHorizonMs,
                      std::int64_t earliestMutationTimeMs,
                      std::uint32_t tickDurationMs) const override {
    static_cast<void>(tickDurationMs);
    if (allTime_) {
      return {earliestMutationTimeMs, simulationHorizonMs};
    }
    return {std::max(minimumTimeMs_, earliestMutationTimeMs), maximumTimeMs_};
  }

  std::unique_ptr<IterationEvaluationSession> CreateSession() const override {
    return std::make_unique<VisualExpressionSession>(firstTime_, score_, condition_);
  }

  bool IsBetter(const EvaluationSample &candidate,
                const EvaluationSample &incumbent) const override {
    return minimize_ ? candidate.score < incumbent.score
                     : candidate.score > incumbent.score;
  }

private:
  bool firstTime_ = false;
  bool minimize_ = false;
  bool allTime_ = false;
  std::int64_t minimumTimeMs_ = 0;
  std::int64_t maximumTimeMs_ = 0;
  ExpressionProgram score_;
  ExpressionProgram condition_;
};

ParseResult ParseExpression(const std::string &text, ValueKind kind) {
  return ExpressionParser(text).Parse(kind);
}

using CudaExpressionInstruction = forevervalidator::experimental::
    PhysicsSandboxCudaExpressionInstruction;
using CudaExpressionOpcode = forevervalidator::experimental::
    PhysicsSandboxCudaExpressionOpcode;
using CudaExpressionSource = forevervalidator::experimental::
    PhysicsSandboxCudaExpressionSource;
using CudaExpressionEvaluator = forevervalidator::experimental::
    PhysicsSandboxCudaExpressionEvaluator;
using CudaExpressionPlane = forevervalidator::experimental::
    PhysicsSandboxCudaExpressionPlane;
using CudaExpressionPrism = forevervalidator::experimental::
    PhysicsSandboxCudaExpressionPrism;
using CudaExpressionPoint2 = forevervalidator::experimental::
    PhysicsSandboxCudaExpressionPoint2;

bool EmitCudaExpressionNode(const ExpressionProgram &program, int id,
                            CudaExpressionEvaluator *evaluator,
                            std::vector<CudaExpressionInstruction> *output,
                            std::string *error) {
  const ExpressionNode &node = program.nodes[static_cast<std::size_t>(id)];
  const auto child = [&](int childId) {
    return childId < 0 ||
           EmitCudaExpressionNode(program, childId, evaluator, output, error);
  };
  if (node.a >= 0 && !child(node.a)) return false;
  if (node.b >= 0 && !child(node.b)) return false;
  if (node.c >= 0 && !child(node.c)) return false;

  CudaExpressionInstruction instruction;
  switch (node.op) {
  case Op::Number:
    instruction.opcode = CudaExpressionOpcode::Constant;
    instruction.value = node.number;
    break;
  case Op::Boolean:
    instruction.opcode = CudaExpressionOpcode::Boolean;
    instruction.value = node.boolean ? 1.0 : 0.0;
    break;
  case Op::CarPosition:
    instruction.opcode = CudaExpressionOpcode::Source;
    instruction.source = CudaExpressionSource::CarPosition;
    break;
  case Op::CarVelocity:
    instruction.opcode = CudaExpressionOpcode::Source;
    instruction.source = CudaExpressionSource::CarVelocity;
    break;
  case Op::CarLocalVelocity:
    instruction.opcode = CudaExpressionOpcode::Source;
    instruction.source = CudaExpressionSource::CarLocalVelocity;
    break;
  case Op::CarSpeed:
    instruction.opcode = CudaExpressionOpcode::Source;
    instruction.source = CudaExpressionSource::CarSpeed;
    break;
  case Op::StuntPoints:
    instruction.opcode = CudaExpressionOpcode::Source;
    instruction.source = CudaExpressionSource::StuntPoints;
    break;
  case Op::FinishTime:
    if (error != nullptr) {
      *error = "Generic CUDA expression composition cannot use precise finish "
               "time yet; use the native finish-time objective.";
    }
    return false;
  case Op::SimulationTime:
    instruction.opcode = CudaExpressionOpcode::Source;
    instruction.source = CudaExpressionSource::SimulationTime;
    break;
  case Op::CheckpointCount:
    instruction.opcode = CudaExpressionOpcode::Source;
    instruction.source = CudaExpressionSource::CheckpointCount;
    break;
  case Op::RaceCompleted:
    instruction.opcode = CudaExpressionOpcode::Source;
    instruction.source = CudaExpressionSource::RaceCompleted;
    break;
  case Op::Sliding:
    instruction.opcode = CudaExpressionOpcode::Source;
    instruction.source = CudaExpressionSource::Sliding;
    break;
  case Op::FreeWheeling:
    instruction.opcode = CudaExpressionOpcode::Source;
    instruction.source = CudaExpressionSource::FreeWheeling;
    break;
  case Op::Vector: instruction.opcode = CudaExpressionOpcode::Vector; break;
  case Op::Direction:
    instruction.opcode = CudaExpressionOpcode::Direction;
    break;
  case Op::Rotation:
    if (node.a < 0) {
      instruction.opcode = CudaExpressionOpcode::Source;
      instruction.source = CudaExpressionSource::CarRotation;
    } else {
      instruction.opcode = CudaExpressionOpcode::Rotation;
    }
    break;
  case Op::Distance: instruction.opcode = CudaExpressionOpcode::Distance; break;
  case Op::Magnitude:
    instruction.opcode = CudaExpressionOpcode::Magnitude;
    break;
  case Op::Normalize: instruction.opcode = CudaExpressionOpcode::Normalize; break;
  case Op::Dot: instruction.opcode = CudaExpressionOpcode::Dot; break;
  case Op::RotationDistance:
    instruction.opcode = CudaExpressionOpcode::RotationDistance;
    break;
  case Op::WeightedBlend:
    instruction.opcode = CudaExpressionOpcode::WeightedBlend;
    break;
  case Op::PercentRatio:
    instruction.opcode = CudaExpressionOpcode::PercentRatio;
    break;
  case Op::KilometersPerHour:
    instruction.opcode = CudaExpressionOpcode::KilometersPerHour;
    break;
  case Op::Absolute: instruction.opcode = CudaExpressionOpcode::Absolute; break;
  case Op::Clamp: instruction.opcode = CudaExpressionOpcode::Clamp; break;
  case Op::Add: instruction.opcode = CudaExpressionOpcode::Add; break;
  case Op::Subtract: instruction.opcode = CudaExpressionOpcode::Subtract; break;
  case Op::Multiply: instruction.opcode = CudaExpressionOpcode::Multiply; break;
  case Op::Divide: instruction.opcode = CudaExpressionOpcode::Divide; break;
  case Op::Minimum: instruction.opcode = CudaExpressionOpcode::Minimum; break;
  case Op::Maximum: instruction.opcode = CudaExpressionOpcode::Maximum; break;
  case Op::Less: instruction.opcode = CudaExpressionOpcode::Less; break;
  case Op::LessEqual:
    instruction.opcode = CudaExpressionOpcode::LessOrEqual;
    break;
  case Op::Equal: instruction.opcode = CudaExpressionOpcode::Equal; break;
  case Op::GreaterEqual:
    instruction.opcode = CudaExpressionOpcode::GreaterOrEqual;
    break;
  case Op::Greater: instruction.opcode = CudaExpressionOpcode::Greater; break;
  case Op::And: instruction.opcode = CudaExpressionOpcode::LogicalAnd; break;
  case Op::Or: instruction.opcode = CudaExpressionOpcode::LogicalOr; break;
  case Op::Not: instruction.opcode = CudaExpressionOpcode::LogicalNot; break;
  case Op::InsideBox: instruction.opcode = CudaExpressionOpcode::InsideBox; break;
  case Op::InsidePrism: {
    if (evaluator == nullptr || node.payload < 0 ||
        static_cast<std::size_t>(node.payload) >= program.prisms.size()) {
      if (error != nullptr) *error = "CUDA inside-prism payload is invalid.";
      return false;
    }
    const ExpressionPrism &prism =
        program.prisms[static_cast<std::size_t>(node.payload)];
    if (evaluator->prisms.size() >= 256u ||
        evaluator->prismVertices.size() + prism.polygon.size() > 65536u) {
      if (error != nullptr) *error = "CUDA expression prism geometry is too large.";
      return false;
    }
    CudaExpressionPrism cudaPrism;
    switch (prism.plane) {
    case ExpressionPlane::XY: cudaPrism.plane = CudaExpressionPlane::XY; break;
    case ExpressionPlane::XZ: cudaPrism.plane = CudaExpressionPlane::XZ; break;
    case ExpressionPlane::YZ: cudaPrism.plane = CudaExpressionPlane::YZ; break;
    }
    cudaPrism.vertexOffset =
        static_cast<std::uint32_t>(evaluator->prismVertices.size());
    cudaPrism.vertexCount = static_cast<std::uint32_t>(prism.polygon.size());
    const std::size_t prismIndex = evaluator->prisms.size();
    evaluator->prisms.push_back(cudaPrism);
    for (const ExpressionPoint2 &vertex : prism.polygon) {
      evaluator->prismVertices.push_back({vertex.x, vertex.y});
    }
    instruction.opcode = CudaExpressionOpcode::InsidePrism;
    instruction.value = static_cast<double>(prismIndex);
    break;
  }
  }
  output->push_back(instruction);
  return true;
}

bool ValidateCudaExpressionStack(
    const std::vector<CudaExpressionInstruction> &program,
    std::string *error) {
  std::size_t depth = 0u;
  std::size_t maximum = 0u;
  for (const CudaExpressionInstruction &instruction : program) {
    int consumed = 0;
    int produced = 1;
    switch (instruction.opcode) {
    case CudaExpressionOpcode::Constant:
    case CudaExpressionOpcode::Boolean:
    case CudaExpressionOpcode::Source:
      consumed = 0;
      break;
    case CudaExpressionOpcode::Magnitude:
    case CudaExpressionOpcode::Normalize:
    case CudaExpressionOpcode::PercentRatio:
    case CudaExpressionOpcode::KilometersPerHour:
    case CudaExpressionOpcode::Absolute:
    case CudaExpressionOpcode::LogicalNot:
      consumed = 1;
      break;
    case CudaExpressionOpcode::Distance:
    case CudaExpressionOpcode::Dot:
    case CudaExpressionOpcode::RotationDistance:
    case CudaExpressionOpcode::Add:
    case CudaExpressionOpcode::Subtract:
    case CudaExpressionOpcode::Multiply:
    case CudaExpressionOpcode::Divide:
    case CudaExpressionOpcode::Minimum:
    case CudaExpressionOpcode::Maximum:
    case CudaExpressionOpcode::Less:
    case CudaExpressionOpcode::LessOrEqual:
    case CudaExpressionOpcode::Equal:
    case CudaExpressionOpcode::GreaterOrEqual:
    case CudaExpressionOpcode::Greater:
    case CudaExpressionOpcode::LogicalAnd:
    case CudaExpressionOpcode::LogicalOr:
      consumed = 2;
      break;
    case CudaExpressionOpcode::Vector:
    case CudaExpressionOpcode::Direction:
    case CudaExpressionOpcode::Rotation:
    case CudaExpressionOpcode::Clamp:
    case CudaExpressionOpcode::InsideBox:
    case CudaExpressionOpcode::InsidePrism:
    case CudaExpressionOpcode::WeightedBlend:
      consumed = 3;
      break;
    }
    if (depth < static_cast<std::size_t>(consumed)) {
      if (error != nullptr) *error = "CUDA expression stack underflow.";
      return false;
    }
    depth -= static_cast<std::size_t>(consumed);
    depth += static_cast<std::size_t>(produced);
    maximum = std::max(maximum, depth);
  }
  if (depth != 1u) {
    if (error != nullptr) *error = "CUDA expression has an invalid final stack.";
    return false;
  }
  if (maximum > 32u) {
    if (error != nullptr) {
      *error = "CUDA expression requires more than 32 temporary values.";
    }
    return false;
  }
  return true;
}

} // namespace

OptionSettings DefaultVisualExpressionOptionSettings() {
  return {{"mode", "score"},
          {"direction", "maximize"},
          {"rangeKind", "window"},
          {"minTimeMs", "1000"},
          {"maxTimeMs", "6000"},
          {"expression", "num 0"},
          {"condition", "bool 1"}};
}

std::optional<std::string> ValidateVisualExpressionOptionSettings(
    const OptionSettings &settings, std::uint32_t tickDurationMs) {
  const OptionSettings defaults = DefaultVisualExpressionOptionSettings();
  if (const auto error = ValidateOptionSettingKeys(settings, defaults))
    return error;
  if (settings.at("mode") != "score" &&
      settings.at("mode") != "first-time") {
    return "visual expression mode must be score or first-time";
  }
  if (settings.at("direction") != "minimize" &&
      settings.at("direction") != "maximize") {
    return "visual expression direction must be minimize or maximize";
  }
  if (settings.at("rangeKind") != "window" &&
      settings.at("rangeKind") != "all") {
    return "visual expression range kind must be window or all";
  }
  const auto minimum = ParseSignedDecimal(settings.at("minTimeMs"));
  const auto maximum = ParseSignedDecimal(settings.at("maxTimeMs"));
  if (!minimum || !maximum)
    return "visual expression times must be whole milliseconds";
  if (settings.at("rangeKind") == "window") {
    if (const auto error = ValidateTimeWindow(*minimum, *maximum, tickDurationMs,
                                              "visual expression", true)) {
      return error;
    }
  }
  const ParseResult score =
      ParseExpression(settings.at("expression"), ValueKind::Scalar);
  if (!score.program) return score.error;
  const ParseResult condition =
      ParseExpression(settings.at("condition"), ValueKind::Boolean);
  if (!condition.program) return condition.error;
  return std::nullopt;
}

std::unique_ptr<IterationEvaluator> CreateVisualExpressionEvaluator(
    const OptionSettings &settings, std::uint32_t tickDurationMs) {
  if (const auto error =
          ValidateVisualExpressionOptionSettings(settings, tickDurationMs)) {
    throw std::invalid_argument(*error);
  }
  ParseResult score =
      ParseExpression(settings.at("expression"), ValueKind::Scalar);
  ParseResult condition =
      ParseExpression(settings.at("condition"), ValueKind::Boolean);
  return std::make_unique<VisualExpressionEvaluator>(
      settings.at("mode") == "first-time",
      settings.at("direction") == "minimize",
      settings.at("rangeKind") == "all",
      *ParseSignedDecimal(settings.at("minTimeMs")),
      *ParseSignedDecimal(settings.at("maxTimeMs")), std::move(*score.program),
      std::move(*condition.program));
}

std::optional<forevervalidator::experimental::
                  PhysicsSandboxCudaExpressionEvaluator>
BuildCudaVisualExpressionEvaluator(const OptionSettings &settings,
                                   std::uint32_t tickDurationMs,
                                   std::string *error) {
  if (const auto validation =
          ValidateVisualExpressionOptionSettings(settings, tickDurationMs)) {
    if (error != nullptr) *error = *validation;
    return std::nullopt;
  }
  ParseResult score =
      ParseExpression(settings.at("expression"), ValueKind::Scalar);
  ParseResult condition =
      ParseExpression(settings.at("condition"), ValueKind::Boolean);
  forevervalidator::experimental::PhysicsSandboxCudaExpressionEvaluator result;
  result.maximize = settings.at("direction") == "maximize";
  result.firstTime = settings.at("mode") == "first-time";
  if (!EmitCudaExpressionNode(*score.program, score.program->root,
                              &result, &result.score, error) ||
      !EmitCudaExpressionNode(*condition.program, condition.program->root,
                              &result, &result.condition, error) ||
      !ValidateCudaExpressionStack(result.score, error) ||
      !ValidateCudaExpressionStack(result.condition, error)) {
    return std::nullopt;
  }
  return result;
}

} // namespace forevertas
