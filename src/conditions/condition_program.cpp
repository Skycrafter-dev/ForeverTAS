#include "conditions/condition_program.h"

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

    bool EmitScalarVariable(const std::string &name) {
        struct Entry { const char *name; PhysicsSandboxCudaConditionValue value; int component; };
        static const Entry entries[] = {
            {"car.x", PhysicsSandboxCudaConditionValue::Position, 1}, {"car.position.x", PhysicsSandboxCudaConditionValue::Position, 1},
            {"car.y", PhysicsSandboxCudaConditionValue::Position, 2}, {"car.position.y", PhysicsSandboxCudaConditionValue::Position, 2},
            {"car.z", PhysicsSandboxCudaConditionValue::Position, 3}, {"car.position.z", PhysicsSandboxCudaConditionValue::Position, 3},
            {"car.prev.x", PhysicsSandboxCudaConditionValue::PreviousPosition, 1}, {"car.prev.position.x", PhysicsSandboxCudaConditionValue::PreviousPosition, 1},
            {"car.prev.y", PhysicsSandboxCudaConditionValue::PreviousPosition, 2}, {"car.prev.position.y", PhysicsSandboxCudaConditionValue::PreviousPosition, 2},
            {"car.prev.z", PhysicsSandboxCudaConditionValue::PreviousPosition, 3}, {"car.prev.position.z", PhysicsSandboxCudaConditionValue::PreviousPosition, 3},
            {"car.vel.x", PhysicsSandboxCudaConditionValue::Velocity, 1}, {"car.velocity.x", PhysicsSandboxCudaConditionValue::Velocity, 1},
            {"car.vel.y", PhysicsSandboxCudaConditionValue::Velocity, 2}, {"car.velocity.y", PhysicsSandboxCudaConditionValue::Velocity, 2},
            {"car.vel.z", PhysicsSandboxCudaConditionValue::Velocity, 3}, {"car.velocity.z", PhysicsSandboxCudaConditionValue::Velocity, 3},
            {"car.prev.vel.x", PhysicsSandboxCudaConditionValue::PreviousVelocity, 1}, {"car.prev.velocity.x", PhysicsSandboxCudaConditionValue::PreviousVelocity, 1},
            {"car.prev.vel.y", PhysicsSandboxCudaConditionValue::PreviousVelocity, 2}, {"car.prev.velocity.y", PhysicsSandboxCudaConditionValue::PreviousVelocity, 2},
            {"car.prev.vel.z", PhysicsSandboxCudaConditionValue::PreviousVelocity, 3}, {"car.prev.velocity.z", PhysicsSandboxCudaConditionValue::PreviousVelocity, 3},
            {"car.vel.pitch", PhysicsSandboxCudaConditionValue::AngularVelocity, 1}, {"car.velocity.pitch", PhysicsSandboxCudaConditionValue::AngularVelocity, 1},
            {"car.vel.yaw", PhysicsSandboxCudaConditionValue::AngularVelocity, 2}, {"car.velocity.yaw", PhysicsSandboxCudaConditionValue::AngularVelocity, 2},
            {"car.vel.roll", PhysicsSandboxCudaConditionValue::AngularVelocity, 3}, {"car.velocity.roll", PhysicsSandboxCudaConditionValue::AngularVelocity, 3},
            {"car.prev.vel.pitch", PhysicsSandboxCudaConditionValue::PreviousAngularVelocity, 1}, {"car.prev.velocity.pitch", PhysicsSandboxCudaConditionValue::PreviousAngularVelocity, 1},
            {"car.prev.vel.yaw", PhysicsSandboxCudaConditionValue::PreviousAngularVelocity, 2}, {"car.prev.velocity.yaw", PhysicsSandboxCudaConditionValue::PreviousAngularVelocity, 2},
            {"car.prev.vel.roll", PhysicsSandboxCudaConditionValue::PreviousAngularVelocity, 3}, {"car.prev.velocity.roll", PhysicsSandboxCudaConditionValue::PreviousAngularVelocity, 3},
            {"car.localvel.x", PhysicsSandboxCudaConditionValue::LocalVelocity, 1}, {"car.localvelocity.x", PhysicsSandboxCudaConditionValue::LocalVelocity, 1},
            {"car.localvel.y", PhysicsSandboxCudaConditionValue::LocalVelocity, 2}, {"car.localvelocity.y", PhysicsSandboxCudaConditionValue::LocalVelocity, 2},
            {"car.localvel.z", PhysicsSandboxCudaConditionValue::LocalVelocity, 3}, {"car.localvelocity.z", PhysicsSandboxCudaConditionValue::LocalVelocity, 3},
            {"car.prev.localvel.x", PhysicsSandboxCudaConditionValue::PreviousLocalVelocity, 1}, {"car.prev.localvelocity.x", PhysicsSandboxCudaConditionValue::PreviousLocalVelocity, 1},
            {"car.prev.localvel.y", PhysicsSandboxCudaConditionValue::PreviousLocalVelocity, 2}, {"car.prev.localvelocity.y", PhysicsSandboxCudaConditionValue::PreviousLocalVelocity, 2},
            {"car.prev.localvel.z", PhysicsSandboxCudaConditionValue::PreviousLocalVelocity, 3}, {"car.prev.localvelocity.z", PhysicsSandboxCudaConditionValue::PreviousLocalVelocity, 3},
        };
        for (const Entry &entry : entries) if (name == entry.name) {
            EmitSource(PhysicsSandboxCudaConditionOpcode::Scalar, entry.value, entry.component);
            return true;
        }
        const auto scalar = [&](PhysicsSandboxCudaConditionValue value) {
            EmitSource(PhysicsSandboxCudaConditionOpcode::Scalar, value); return true;
        };
        if (name == "car.speed") return scalar(PhysicsSandboxCudaConditionValue::Speed);
        if (name == "car.prev.speed") return scalar(PhysicsSandboxCudaConditionValue::PreviousSpeed);
        if (name == "car.localspeed") return scalar(PhysicsSandboxCudaConditionValue::LocalSpeed);
        if (name == "car.prev.localspeed") return scalar(PhysicsSandboxCudaConditionValue::PreviousLocalSpeed);
        if (name == "car.yaw" || name == "car.rotation.yaw") return scalar(PhysicsSandboxCudaConditionValue::Yaw);
        if (name == "car.pitch" || name == "car.rotation.pitch") return scalar(PhysicsSandboxCudaConditionValue::Pitch);
        if (name == "car.roll" || name == "car.rotation.roll") return scalar(PhysicsSandboxCudaConditionValue::Roll);
        if (name == "car.prev.yaw" || name == "car.prev.rotation.yaw") return scalar(PhysicsSandboxCudaConditionValue::PreviousYaw);
        if (name == "car.prev.pitch" || name == "car.prev.rotation.pitch") return scalar(PhysicsSandboxCudaConditionValue::PreviousPitch);
        if (name == "car.prev.roll" || name == "car.prev.rotation.roll") return scalar(PhysicsSandboxCudaConditionValue::PreviousRoll);
        if (name == "car.freewheel") return scalar(PhysicsSandboxCudaConditionValue::FreeWheeling);
        if (name == "car.lateralcontact") return scalar(PhysicsSandboxCudaConditionValue::LateralContact);
        if (name == "car.sliding" || name == "car.is_sliding" || name == "car.is") return scalar(PhysicsSandboxCudaConditionValue::Sliding);
        if (name == "car.gear") return scalar(PhysicsSandboxCudaConditionValue::Gear);
        if (name == "car.rpm") return scalar(PhysicsSandboxCudaConditionValue::Rpm);
        if (name == "car.turning_rate" || name == "car.tr") return scalar(PhysicsSandboxCudaConditionValue::TurningRate);
        if (name == "car.turbo_type" || name == "car.tt") return scalar(PhysicsSandboxCudaConditionValue::TurboType);
        if (name == "car.turbo_boost_factor" || name == "car.tbf") return scalar(PhysicsSandboxCudaConditionValue::TurboBoostFactor);
        if (name == "car.cps") return scalar(PhysicsSandboxCudaConditionValue::CheckpointCount);
        if (name == "iterations") return scalar(PhysicsSandboxCudaConditionValue::Iterations);
        if (name == "last_improvement.time") return scalar(PhysicsSandboxCudaConditionValue::LastImprovementTime);
        if (name == "last_restart.time") return scalar(PhysicsSandboxCudaConditionValue::LastRestartTime);
        static const char *wheelNames[] = {"frontleft", "frontright", "backleft", "backright"};
        for (std::uint32_t i = 0u; i < 4u; ++i) {
            const std::string prefix = "car.wheels." + std::string(wheelNames[i]);
            if (name == prefix + ".groundcontact") return scalar(static_cast<PhysicsSandboxCudaConditionValue>(static_cast<std::uint32_t>(PhysicsSandboxCudaConditionValue::WheelGroundContact0) + i));
            if (name == prefix + ".is_sliding" || name == prefix + ".is") return scalar(static_cast<PhysicsSandboxCudaConditionValue>(static_cast<std::uint32_t>(PhysicsSandboxCudaConditionValue::WheelSliding0) + i));
            if (name == prefix + ".surface") return scalar(static_cast<PhysicsSandboxCudaConditionValue>(static_cast<std::uint32_t>(PhysicsSandboxCudaConditionValue::WheelSurface0) + i));
        }
        return false;
    }

    bool EmitVectorVariable(const std::string &name) {
        PhysicsSandboxCudaConditionValue value;
        if (name == "car.pos" || name == "car.position") value = PhysicsSandboxCudaConditionValue::Position;
        else if (name == "car.prev.pos" || name == "car.prev.position") value = PhysicsSandboxCudaConditionValue::PreviousPosition;
        else if (name == "car.vel" || name == "car.velocity") value = PhysicsSandboxCudaConditionValue::Velocity;
        else if (name == "car.prev.vel" || name == "car.prev.velocity") value = PhysicsSandboxCudaConditionValue::PreviousVelocity;
        else if (name == "car.localvel" || name == "car.localvelocity") value = PhysicsSandboxCudaConditionValue::LocalVelocity;
        else if (name == "car.prev.localvel" || name == "car.prev.localvelocity") value = PhysicsSandboxCudaConditionValue::PreviousLocalVelocity;
        else return false;
        EmitSource(PhysicsSandboxCudaConditionOpcode::Vector, value);
        return true;
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
    const double sinr = 2.0 * (w * x + y * z);
    const double cosr = 1.0 - 2.0 * (x * x + y * y);
    const double sinp = 2.0 * (w * y - z * x);
    const double siny = 2.0 * (w * z + x * y);
    const double cosy = 1.0 - 2.0 * (y * y + z * z);
    return {std::atan2(siny, cosy), std::abs(sinp) >= 1.0 ? std::copysign(1.5707963267948966, sinp) : std::asin(sinp), std::atan2(sinr, cosr), true};
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

bool ConditionProgram::Evaluate(
        const PhysicsSandboxStateView &previous,
        const PhysicsSandboxStateView &current,
        const ConditionExecutionContext &context) const {
    std::vector<Value> stack;
    stack.reserve(32u);
    for (const auto &instruction : cuda.instructions) {
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
            if (stack.empty() || stack.back().vector) return false;
            stack.back().x *= instruction.opcode == PhysicsSandboxCudaConditionOpcode::KilometersPerHour ? 3.6 : 57.29577951308232;
        } else {
            if (stack.size() < 2u) return false;
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
            default: return false;
            }
        }
        if (stack.size() > 32u) return false;
    }
    return stack.size() == 1u && !stack[0].vector && stack[0].x != 0.0;
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
