#include "conditions/condition_program.h"
#include "conditions/condition_catalog.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <string_view>
#include <vector>

namespace forevertas {
namespace {

using namespace forevervalidator::experimental;

struct Value {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    bool vector = false;
};

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

class Parser {
public:
    Parser(std::string_view source,
           const ConditionVariables &variables,
           std::vector<PhysicsSandboxCudaConditionInstruction> *output)
        : source_(source), variables_(variables), output_(output) {}

    bool ParseComparison(std::string *error) {
        SkipSpaces();
        if (!ParseScalar(error)) return false;
        SkipSpaces();
        PhysicsSandboxCudaConditionOpcode opcode;
        if (Consume(">=")) opcode = PhysicsSandboxCudaConditionOpcode::GreaterOrEqual;
        else if (Consume("<=")) opcode = PhysicsSandboxCudaConditionOpcode::LessOrEqual;
        else if (Consume(">")) opcode = PhysicsSandboxCudaConditionOpcode::Greater;
        else if (Consume("<")) opcode = PhysicsSandboxCudaConditionOpcode::Less;
        else if (Consume("=")) opcode = PhysicsSandboxCudaConditionOpcode::Equal;
        else return Fail(error, "expected comparison operator");
        if (!ParseScalar(error)) return false;
        SkipSpaces();
        if (position_ != source_.size()) return Fail(error, "unexpected text after comparison");
        Emit(opcode);
        return true;
    }

    bool ParseExpression(std::string *error) {
        SkipSpaces();
        if (!ParseScalar(error)) return false;
        SkipSpaces();
        return position_ == source_.size() ||
                Fail(error, "unexpected text after expression");
    }

private:
    bool ParseScalar(std::string *error) {
        if (!ParseTerm(error)) return false;
        while (true) {
            SkipSpaces();
            if (Consume("+")) {
                if (!ParseTerm(error)) return false;
                Emit(PhysicsSandboxCudaConditionOpcode::Add);
            } else if (Consume("-")) {
                if (!ParseTerm(error)) return false;
                Emit(PhysicsSandboxCudaConditionOpcode::Subtract);
            } else break;
        }
        return true;
    }

    bool ParseTerm(std::string *error) {
        if (!ParseFactor(error)) return false;
        while (true) {
            SkipSpaces();
            if (Consume("*")) {
                if (!ParseFactor(error)) return false;
                Emit(PhysicsSandboxCudaConditionOpcode::Multiply);
            } else if (Consume("/")) {
                if (!ParseFactor(error)) return false;
                Emit(PhysicsSandboxCudaConditionOpcode::Divide);
            } else break;
        }
        return true;
    }

    bool ParseFactor(std::string *error) {
        SkipSpaces();
        if (Consume("(")) {
            const std::size_t saved = position_;
            double x = 0.0, y = 0.0, z = 0.0;
            if (ParseNumber(&x) && ConsumeComma() && ParseNumber(&y) &&
                ConsumeComma() && ParseNumber(&z)) {
                SkipSpaces();
                if (!Consume(")")) return Fail(error, "expected ')' after vector");
                EmitConstantVector(x, y, z);
                return true;
            }
            position_ = saved;
            if (!ParseScalar(error)) return false;
            SkipSpaces();
            if (!Consume(")")) return Fail(error, "expected ')'");
            return true;
        }
        double value = 0.0;
        if (ParseNumber(&value)) {
            EmitConstant(value);
            return true;
        }
        const std::size_t start = position_;
        std::string identifier;
        if (ParseIdentifier(&identifier)) {
            const std::string lower = Lower(identifier);
            SkipSpaces();
            if (Consume("(")) return ParseFunction(lower, error);
            if (EmitScalarVariable(lower)) return true;
            return FailAt(error, start, "unknown condition variable '" + identifier + "'");
        }
        return Fail(error, "expected number, variable, or function");
    }

    bool ParseFunction(const std::string &name, std::string *error) {
        if (name == "kmh" || name == "deg" || name == "time_since") {
            if (name == "time_since") {
                EmitSource(PhysicsSandboxCudaConditionOpcode::Scalar,
                           PhysicsSandboxCudaConditionValue::CurrentTime);
            }
            if (!ParseScalar(error)) return false;
            SkipSpaces();
            if (!Consume(")")) return Fail(error, "expected ')' after function argument");
            Emit(name == "kmh" ? PhysicsSandboxCudaConditionOpcode::KilometersPerHour
                 : name == "deg" ? PhysicsSandboxCudaConditionOpcode::Degrees
                 : PhysicsSandboxCudaConditionOpcode::Subtract);
            return true;
        }
        if (name == "distance") {
            if (!ParseVector(error) || !ConsumeComma() || !ParseVector(error)) return false;
            SkipSpaces();
            if (!Consume(")")) return Fail(error, "expected ')' after distance arguments");
            Emit(PhysicsSandboxCudaConditionOpcode::Distance);
            return true;
        }
        if (name == "variable" || name == "var") {
            SkipSpaces();
            bool quoted = Consume("\"");
            std::string variable;
            while (position_ < source_.size() &&
                   (quoted ? source_[position_] != '"' : source_[position_] != ')')) {
                variable.push_back(source_[position_++]);
            }
            if (quoted && !Consume("\"")) return Fail(error, "unterminated variable name");
            SkipSpaces();
            if (!Consume(")")) return Fail(error, "expected ')' after variable name");
            const auto found = variables_.find(Lower(variable));
            if (found == variables_.end()) {
                return Fail(error, "unknown external variable '" + variable + "'");
            }
            if (found->second.vector) {
                EmitConstantVector(found->second.x, found->second.y, found->second.z);
            } else {
                EmitConstant(found->second.x);
            }
            return true;
        }
        return Fail(error, "unknown condition function '" + name + "'");
    }

    bool ParseVector(std::string *error) {
        SkipSpaces();
        if (Consume("(")) {
            double x = 0.0, y = 0.0, z = 0.0;
            if (!ParseNumber(&x) || !ConsumeComma() || !ParseNumber(&y) ||
                !ConsumeComma() || !ParseNumber(&z)) {
                return Fail(error, "vector literal must contain three numbers");
            }
            SkipSpaces();
            if (!Consume(")")) return Fail(error, "expected ')' after vector");
            EmitConstantVector(x, y, z);
            return true;
        }
        std::string identifier;
        if (!ParseIdentifier(&identifier)) return Fail(error, "expected vector expression");
        const std::string lower = Lower(identifier);
        SkipSpaces();
        if ((lower == "variable" || lower == "var") && Consume("(")) {
            return ParseFunction(lower, error);
        }
        return EmitVectorVariable(lower) ||
                Fail(error, "unknown vector variable '" + identifier + "'");
    }

    bool EmitKnownVariable(const std::string &name, bool vector) {
        for (const ConditionSymbol &entry : ConditionSymbols()) {
            if ((entry.type == "vector") != vector) continue;
            if (name != entry.name &&
                std::find(entry.aliases.begin(), entry.aliases.end(), name) == entry.aliases.end()) continue;
            EmitSource(vector ? PhysicsSandboxCudaConditionOpcode::Vector
                              : PhysicsSandboxCudaConditionOpcode::Scalar,
                       entry.value, entry.component);
            return true;
        }
        return false;
    }

    bool EmitScalarVariable(const std::string &name) {
        return EmitKnownVariable(name, false);
    }

    bool EmitVectorVariable(const std::string &name) {
        return EmitKnownVariable(name, true);
    }

    bool ParseIdentifier(std::string *value) {
        SkipSpaces();
        const std::size_t start = position_;
        while (position_ < source_.size()) {
            const char c = source_[position_];
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '.') break;
            ++position_;
        }
        if (position_ == start) return false;
        *value = std::string(source_.substr(start, position_ - start));
        return true;
    }

    bool ParseNumber(double *value) {
        SkipSpaces();
        const std::string tail(source_.substr(position_));
        char *end = nullptr;
        *value = std::strtod(tail.c_str(), &end);
        if (end == tail.c_str() || !std::isfinite(*value)) return false;
        position_ += static_cast<std::size_t>(end - tail.c_str());
        return true;
    }

    bool ConsumeComma() { SkipSpaces(); return Consume(","); }
    bool Consume(std::string_view token) {
        if (source_.substr(position_, token.size()) != token) return false;
        position_ += token.size(); return true;
    }
    void SkipSpaces() { while (position_ < source_.size() && std::isspace(static_cast<unsigned char>(source_[position_]))) ++position_; }
    void Emit(PhysicsSandboxCudaConditionOpcode opcode) { output_->push_back({opcode}); }
    void EmitConstant(double value) { output_->push_back({PhysicsSandboxCudaConditionOpcode::Constant, {}, value}); }
    void EmitConstantVector(double x, double y, double z) { output_->push_back({PhysicsSandboxCudaConditionOpcode::ConstantVector, {}, x, y, z}); }
    void EmitSource(PhysicsSandboxCudaConditionOpcode opcode, PhysicsSandboxCudaConditionValue value, int component = 0) { output_->push_back({opcode, value, static_cast<double>(component)}); }
    bool Fail(std::string *error, std::string message) const { return FailAt(error, position_, std::move(message)); }
    bool FailAt(std::string *error, std::size_t position, std::string message) const { *error = std::move(message) + " at column " + std::to_string(position + 1u); return false; }

    std::string_view source_;
    const ConditionVariables &variables_;
    std::vector<PhysicsSandboxCudaConditionInstruction> *output_;
    std::size_t position_ = 0u;
};

Value Angles(float x, float y, float z, float w) {
    const double sinPitch = 2.0 * (w * x - y * z);
    const double sinYaw = 2.0 * (w * y + x * z);
    const double cosYaw = 1.0 - 2.0 * (x * x + y * y);
    const double sinRoll = 2.0 * (w * z + x * y);
    const double cosRoll = 1.0 - 2.0 * (x * x + z * z);
    return {std::atan2(sinYaw, cosYaw),
            std::abs(sinPitch) >= 1.0
                    ? std::copysign(1.5707963267948966, sinPitch)
                    : std::asin(sinPitch),
            std::atan2(sinRoll, cosRoll),
            true};
}

Value Source(PhysicsSandboxCudaConditionValue source,
             const PhysicsSandboxStateView &previous,
             const PhysicsSandboxStateView &current,
             const ConditionExecutionContext &context) {
    const auto vec = [](const forevervalidator::Vector3 &v) { return Value{v.x, v.y, v.z, true}; };
    const auto length = [](const forevervalidator::Vector3 &v) { return std::sqrt(static_cast<double>(v.x)*v.x + static_cast<double>(v.y)*v.y + static_cast<double>(v.z)*v.z); };
    switch (source) {
    case PhysicsSandboxCudaConditionValue::Position: return vec(current.car.position);
    case PhysicsSandboxCudaConditionValue::PreviousPosition: return vec(previous.car.position);
    case PhysicsSandboxCudaConditionValue::Velocity: return vec(current.car.linearSpeed);
    case PhysicsSandboxCudaConditionValue::PreviousVelocity: return vec(previous.car.linearSpeed);
    case PhysicsSandboxCudaConditionValue::LocalVelocity: return vec(current.car.localSpeed);
    case PhysicsSandboxCudaConditionValue::PreviousLocalVelocity: return vec(previous.car.localSpeed);
    case PhysicsSandboxCudaConditionValue::AngularVelocity: return vec(current.car.angularSpeed);
    case PhysicsSandboxCudaConditionValue::PreviousAngularVelocity: return vec(previous.car.angularSpeed);
    case PhysicsSandboxCudaConditionValue::Yaw: return {Angles(current.car.rotationX,current.car.rotationY,current.car.rotationZ,current.car.rotationW).x};
    case PhysicsSandboxCudaConditionValue::Pitch: return {Angles(current.car.rotationX,current.car.rotationY,current.car.rotationZ,current.car.rotationW).y};
    case PhysicsSandboxCudaConditionValue::Roll: return {Angles(current.car.rotationX,current.car.rotationY,current.car.rotationZ,current.car.rotationW).z};
    case PhysicsSandboxCudaConditionValue::PreviousYaw: return {Angles(previous.car.rotationX,previous.car.rotationY,previous.car.rotationZ,previous.car.rotationW).x};
    case PhysicsSandboxCudaConditionValue::PreviousPitch: return {Angles(previous.car.rotationX,previous.car.rotationY,previous.car.rotationZ,previous.car.rotationW).y};
    case PhysicsSandboxCudaConditionValue::PreviousRoll: return {Angles(previous.car.rotationX,previous.car.rotationY,previous.car.rotationZ,previous.car.rotationW).z};
    case PhysicsSandboxCudaConditionValue::Speed: return {length(current.car.linearSpeed)};
    case PhysicsSandboxCudaConditionValue::PreviousSpeed: return {length(previous.car.linearSpeed)};
    case PhysicsSandboxCudaConditionValue::LocalSpeed: return {length(current.car.localSpeed)};
    case PhysicsSandboxCudaConditionValue::PreviousLocalSpeed: return {length(previous.car.localSpeed)};
    case PhysicsSandboxCudaConditionValue::FreeWheeling: return {current.car.freeWheeling ? 1.0 : 0.0};
    case PhysicsSandboxCudaConditionValue::LateralContact: return {current.car.lateralContact ? 1.0 : 0.0};
    case PhysicsSandboxCudaConditionValue::Sliding: return {current.car.sliding ? 1.0 : 0.0};
    case PhysicsSandboxCudaConditionValue::Gear: return {static_cast<double>(current.car.gear)};
    case PhysicsSandboxCudaConditionValue::Rpm: return {current.car.rpm};
    case PhysicsSandboxCudaConditionValue::TurningRate: return {current.car.turningRate};
    case PhysicsSandboxCudaConditionValue::TurboType: return {static_cast<double>(current.car.turboType)};
    case PhysicsSandboxCudaConditionValue::TurboBoostFactor: return {current.car.turboBoostFactor};
    case PhysicsSandboxCudaConditionValue::Iterations: return {static_cast<double>(context.iterations)};
    case PhysicsSandboxCudaConditionValue::LastImprovementTime: return {context.lastImprovementTimeSeconds};
    case PhysicsSandboxCudaConditionValue::LastRestartTime: return {context.lastRestartTimeSeconds};
    case PhysicsSandboxCudaConditionValue::CurrentTime: return {context.currentTimeSeconds};
    case PhysicsSandboxCudaConditionValue::SimulationTimeMilliseconds: return {static_cast<double>(current.timeMs)};
    case PhysicsSandboxCudaConditionValue::CompletedLaps: return {static_cast<double>(current.completedLaps)};
    case PhysicsSandboxCudaConditionValue::CheckpointCount: return {static_cast<double>(current.checkpointsCollected)};
    default: break;
    }
    const std::uint32_t raw = static_cast<std::uint32_t>(source);
    const std::uint32_t ground = static_cast<std::uint32_t>(PhysicsSandboxCudaConditionValue::WheelGroundContact0);
    const std::uint32_t sliding = static_cast<std::uint32_t>(PhysicsSandboxCudaConditionValue::WheelSliding0);
    const std::uint32_t surface = static_cast<std::uint32_t>(PhysicsSandboxCudaConditionValue::WheelSurface0);
    if (raw >= ground && raw < ground + 4u) return {current.car.wheelContact[raw-ground] ? 1.0 : 0.0};
    if (raw >= sliding && raw < sliding + 4u) return {current.car.wheelSliding[raw-sliding] ? 1.0 : 0.0};
    if (raw >= surface && raw < surface + 4u) return {static_cast<double>(current.car.wheelSurface[raw-surface])};
    return {};
}

}  // namespace

std::optional<Value> EvaluateInstructions(
        const std::vector<PhysicsSandboxCudaConditionInstruction> &instructions,
        const PhysicsSandboxStateView &previous,
        const PhysicsSandboxStateView &current,
        const ConditionExecutionContext &context) {
    std::vector<Value> stack;
    stack.reserve(32u);
    for (const auto &instruction : instructions) {
        if (instruction.opcode == PhysicsSandboxCudaConditionOpcode::Constant) stack.push_back({instruction.x});
        else if (instruction.opcode == PhysicsSandboxCudaConditionOpcode::ConstantVector) stack.push_back({instruction.x,instruction.y,instruction.z,true});
        else if (instruction.opcode == PhysicsSandboxCudaConditionOpcode::Scalar || instruction.opcode == PhysicsSandboxCudaConditionOpcode::Vector) {
            Value value = Source(instruction.value, previous, current, context);
            if (instruction.opcode == PhysicsSandboxCudaConditionOpcode::Scalar && value.vector) {
                const int component = static_cast<int>(instruction.x);
                value = {component == 1 ? value.x : component == 2 ? value.y : component == 3 ? value.z : 0.0};
            }
            stack.push_back(value);
        } else if (instruction.opcode == PhysicsSandboxCudaConditionOpcode::KilometersPerHour || instruction.opcode == PhysicsSandboxCudaConditionOpcode::Degrees) {
            if (stack.empty() || stack.back().vector) return std::nullopt;
            stack.back().x *= instruction.opcode == PhysicsSandboxCudaConditionOpcode::KilometersPerHour ? 3.6 : 57.29577951308232;
        } else {
            if (stack.size() < 2u) return std::nullopt;
            Value right = stack.back(); stack.pop_back(); Value &left = stack.back();
            switch (instruction.opcode) {
            case PhysicsSandboxCudaConditionOpcode::Distance: left = {std::sqrt((left.x-right.x)*(left.x-right.x)+(left.y-right.y)*(left.y-right.y)+(left.z-right.z)*(left.z-right.z))}; break;
            case PhysicsSandboxCudaConditionOpcode::Add: left.x += right.x; break;
            case PhysicsSandboxCudaConditionOpcode::Subtract: left.x -= right.x; break;
            case PhysicsSandboxCudaConditionOpcode::Multiply: left.x *= right.x; break;
            case PhysicsSandboxCudaConditionOpcode::Divide: left.x = right.x == 0.0 ? 0.0 : left.x/right.x; break;
            case PhysicsSandboxCudaConditionOpcode::Greater: left={left.x>right.x?1.0:0.0}; break;
            case PhysicsSandboxCudaConditionOpcode::Less: left={left.x<right.x?1.0:0.0}; break;
            case PhysicsSandboxCudaConditionOpcode::GreaterOrEqual: left={left.x>=right.x?1.0:0.0}; break;
            case PhysicsSandboxCudaConditionOpcode::LessOrEqual: left={left.x<=right.x?1.0:0.0}; break;
            case PhysicsSandboxCudaConditionOpcode::Equal: left={left.x==right.x?1.0:0.0}; break;
            case PhysicsSandboxCudaConditionOpcode::LogicalAnd: left={left.x!=0.0&&right.x!=0.0?1.0:0.0}; break;
            default: return std::nullopt;
            }
        }
        if (stack.size() > 32u) return std::nullopt;
    }
    if (stack.size() != 1u || stack[0].vector ||
        !std::isfinite(stack[0].x)) {
        return std::nullopt;
    }
    return stack[0];
}

bool ConditionProgram::Evaluate(
        const PhysicsSandboxStateView &previous,
        const PhysicsSandboxStateView &current,
        const ConditionExecutionContext &context) const {
    const auto value = EvaluateInstructions(
            cuda.instructions, previous, current, context);
    return value && value->x != 0.0;
}

std::optional<double> ScalarExpressionProgram::Evaluate(
        const PhysicsSandboxStateView &previous,
        const PhysicsSandboxStateView &current,
        const ConditionExecutionContext &context) const {
    const auto value = EvaluateInstructions(
            instructions, previous, current, context);
    return value ? std::optional<double>(value->x) : std::nullopt;
}

ScalarExpressionCompileResult CompileScalarExpression(
        const std::string &source,
        const ConditionVariables &variables) {
    ScalarExpressionProgram program;
    std::string error;
    Parser parser(source, variables, &program.instructions);
    if (!parser.ParseExpression(&error)) return {{}, error};
    if (program.instructions.size() > 256u) {
        return {{}, "expression exceeds the 256-instruction limit"};
    }
    const PhysicsSandboxStateView emptyState;
    if (!program.Evaluate(emptyState, emptyState, {})) {
        return {{}, "expression must produce a finite scalar"};
    }
    return {std::move(program), std::nullopt};
}

ConditionCompileResult CompileConditionScript(
        const std::string &source,
        const ConditionVariables &variables) {
    ConditionProgram result;
    std::istringstream lines(source);
    std::string line;
    std::size_t lineNumber = 0u;
    std::size_t count = 0u;
    while (std::getline(lines, line)) {
        ++lineNumber;
        if (std::all_of(line.begin(), line.end(), [](unsigned char c) { return std::isspace(c); })) continue;
        std::string error;
        Parser parser(line, variables, &result.cuda.instructions);
        if (!parser.ParseComparison(&error)) return {{}, "Condition line " + std::to_string(lineNumber) + ": " + error};
        if (count++ != 0u) result.cuda.instructions.push_back({PhysicsSandboxCudaConditionOpcode::LogicalAnd});
        if (result.cuda.instructions.size() > 256u) return {{}, "Condition script exceeds the 256-instruction limit"};
    }
    if (count == 0u) return {std::nullopt, std::nullopt};
    return {std::move(result), std::nullopt};
}

}  // namespace forevertas
