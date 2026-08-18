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

enum class ValueKind { Scalar, Vector, Rotation };

struct Value {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double w = 0.0;
    ValueKind kind = ValueKind::Scalar;
    bool valid = true;
};

Value Scalar(double value) {
    return {value, 0.0, 0.0, 0.0, ValueKind::Scalar, std::isfinite(value)};
}

Value Vector(double x, double y, double z) {
    return {x, y, z, 0.0, ValueKind::Vector,
            std::isfinite(x) && std::isfinite(y) && std::isfinite(z)};
}

Value Rotation(double x, double y, double z, double w) {
    return {x, y, z, w, ValueKind::Rotation,
            std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
                    std::isfinite(w)};
}

Value InvalidValue() {
    Value result;
    result.valid = false;
    return result;
}

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
    return Vector(
            std::atan2(siny, cosy),
            std::abs(sinp) >= 1.0
                    ? std::copysign(1.5707963267948966, sinp)
                    : std::asin(sinp),
            std::atan2(sinr, cosr));
}

Value Source(PhysicsSandboxCudaConditionValue source,
             const PhysicsSandboxStateView &previous,
             const PhysicsSandboxStateView &current,
             const ConditionExecutionContext &context) {
    const auto vec = [](const forevervalidator::Vector3 &v) {
        return Vector(v.x, v.y, v.z);
    };
    const auto length = [](const forevervalidator::Vector3 &v) {
        return std::sqrt(static_cast<double>(v.x) * v.x +
                         static_cast<double>(v.y) * v.y +
                         static_cast<double>(v.z) * v.z);
    };
    switch (source) {
    case PhysicsSandboxCudaConditionValue::Position: return vec(current.car.position);
    case PhysicsSandboxCudaConditionValue::PreviousPosition: return vec(previous.car.position);
    case PhysicsSandboxCudaConditionValue::Velocity: return vec(current.car.linearSpeed);
    case PhysicsSandboxCudaConditionValue::PreviousVelocity: return vec(previous.car.linearSpeed);
    case PhysicsSandboxCudaConditionValue::LocalVelocity: return vec(current.car.localSpeed);
    case PhysicsSandboxCudaConditionValue::PreviousLocalVelocity: return vec(previous.car.localSpeed);
    case PhysicsSandboxCudaConditionValue::AngularVelocity: return vec(current.car.angularSpeed);
    case PhysicsSandboxCudaConditionValue::PreviousAngularVelocity: return vec(previous.car.angularSpeed);
    case PhysicsSandboxCudaConditionValue::Yaw:
        return Scalar(Angles(current.car.rotationX, current.car.rotationY,
                             current.car.rotationZ, current.car.rotationW).x);
    case PhysicsSandboxCudaConditionValue::Pitch:
        return Scalar(Angles(current.car.rotationX, current.car.rotationY,
                             current.car.rotationZ, current.car.rotationW).y);
    case PhysicsSandboxCudaConditionValue::Roll:
        return Scalar(Angles(current.car.rotationX, current.car.rotationY,
                             current.car.rotationZ, current.car.rotationW).z);
    case PhysicsSandboxCudaConditionValue::PreviousYaw:
        return Scalar(Angles(previous.car.rotationX, previous.car.rotationY,
                             previous.car.rotationZ, previous.car.rotationW).x);
    case PhysicsSandboxCudaConditionValue::PreviousPitch:
        return Scalar(Angles(previous.car.rotationX, previous.car.rotationY,
                             previous.car.rotationZ, previous.car.rotationW).y);
    case PhysicsSandboxCudaConditionValue::PreviousRoll:
        return Scalar(Angles(previous.car.rotationX, previous.car.rotationY,
                             previous.car.rotationZ, previous.car.rotationW).z);
    case PhysicsSandboxCudaConditionValue::Speed:
        return Scalar(length(current.car.linearSpeed));
    case PhysicsSandboxCudaConditionValue::PreviousSpeed:
        return Scalar(length(previous.car.linearSpeed));
    case PhysicsSandboxCudaConditionValue::LocalSpeed:
        return Scalar(length(current.car.localSpeed));
    case PhysicsSandboxCudaConditionValue::PreviousLocalSpeed:
        return Scalar(length(previous.car.localSpeed));
    case PhysicsSandboxCudaConditionValue::FreeWheeling:
        return Scalar(current.car.freeWheeling ? 1.0 : 0.0);
    case PhysicsSandboxCudaConditionValue::LateralContact:
        return Scalar(current.car.lateralContact ? 1.0 : 0.0);
    case PhysicsSandboxCudaConditionValue::Sliding:
        return Scalar(current.car.sliding ? 1.0 : 0.0);
    case PhysicsSandboxCudaConditionValue::Gear:
        return Scalar(static_cast<double>(current.car.gear));
    case PhysicsSandboxCudaConditionValue::Rpm: return Scalar(current.car.rpm);
    case PhysicsSandboxCudaConditionValue::TurningRate:
        return Scalar(current.car.turningRate);
    case PhysicsSandboxCudaConditionValue::TurboType:
        return Scalar(static_cast<double>(current.car.turboType));
    case PhysicsSandboxCudaConditionValue::TurboBoostFactor:
        return Scalar(current.car.turboBoostFactor);
    case PhysicsSandboxCudaConditionValue::Iterations:
        return Scalar(static_cast<double>(context.iterations));
    case PhysicsSandboxCudaConditionValue::LastImprovementTime:
        return Scalar(context.lastImprovementTimeSeconds);
    case PhysicsSandboxCudaConditionValue::LastRestartTime:
        return Scalar(context.lastRestartTimeSeconds);
    case PhysicsSandboxCudaConditionValue::CurrentTime:
        return Scalar(context.currentTimeSeconds);
    case PhysicsSandboxCudaConditionValue::CheckpointCount:
        return Scalar(static_cast<double>(current.checkpointsCollected));
    case PhysicsSandboxCudaConditionValue::StuntPoints:
        return Scalar(static_cast<double>(current.stuntsScore.value_or(0u)));
    case PhysicsSandboxCudaConditionValue::FinishTime:
        if (!current.raceCompleted || !current.finishTime ||
            !current.finishTime->IsValid())
            return InvalidValue();
        return Scalar(static_cast<double>(current.finishTime->upperBoundNs) /
                      1.0e6);
    case PhysicsSandboxCudaConditionValue::SimulationTime:
        return Scalar(static_cast<double>(current.timeMs));
    case PhysicsSandboxCudaConditionValue::RaceCompleted:
        return Scalar(current.raceCompleted ? 1.0 : 0.0);
    case PhysicsSandboxCudaConditionValue::CarRotation:
        return Rotation(current.car.rotationX, current.car.rotationY,
                        current.car.rotationZ, current.car.rotationW);
    default: break;
    }
    const std::uint32_t raw = static_cast<std::uint32_t>(source);
    const std::uint32_t ground = static_cast<std::uint32_t>(PhysicsSandboxCudaConditionValue::WheelGroundContact0);
    const std::uint32_t sliding = static_cast<std::uint32_t>(PhysicsSandboxCudaConditionValue::WheelSliding0);
    const std::uint32_t surface = static_cast<std::uint32_t>(PhysicsSandboxCudaConditionValue::WheelSurface0);
    if (raw >= ground && raw < ground + 4u)
        return Scalar(current.car.wheelContact[raw-ground] ? 1.0 : 0.0);
    if (raw >= sliding && raw < sliding + 4u)
        return Scalar(current.car.wheelSliding[raw-sliding] ? 1.0 : 0.0);
    if (raw >= surface && raw < surface + 4u)
        return Scalar(static_cast<double>(current.car.wheelSurface[raw-surface]));
    return InvalidValue();
}

double ValueLength(const Value &value) {
    return std::sqrt(value.x * value.x + value.y * value.y +
                     value.z * value.z);
}

bool NormalizeVector(Value *value) {
    const double length = ValueLength(*value);
    if (!std::isfinite(length)) return false;
    if (length <= 1e-12) {
        value->x = value->y = value->z = 0.0;
    } else {
        value->x /= length;
        value->y /= length;
        value->z /= length;
    }
    return true;
}

bool NormalizeRotation(Value *value) {
    const double length = std::sqrt(value->x * value->x + value->y * value->y +
                                    value->z * value->z + value->w * value->w);
    if (!std::isfinite(length) || length <= 1e-12) return false;
    value->x /= length;
    value->y /= length;
    value->z /= length;
    value->w /= length;
    return true;
}

bool PointOnSegment(double px, double py, double ax, double ay,
                    double bx, double by) {
    constexpr double tolerance = 1e-9;
    const double cross = (px - ax) * (by - ay) - (py - ay) * (bx - ax);
    if (std::abs(cross) > tolerance) return false;
    return px >= std::min(ax, bx) - tolerance &&
           px <= std::max(ax, bx) + tolerance &&
           py >= std::min(ay, by) - tolerance &&
           py <= std::max(ay, by) + tolerance;
}

bool ContainsPrism(const PhysicsSandboxCudaExpressionPrism &prism,
                   const std::vector<PhysicsSandboxCudaExpressionPoint2> &vertices,
                   const Value &position, const Value &origin, double depth) {
    if (prism.vertexCount < 3u || !std::isfinite(depth) || depth <= 0.0 ||
        static_cast<std::uint64_t>(prism.vertexOffset) + prism.vertexCount >
                vertices.size())
        return false;
    double planeX = 0.0;
    double planeY = 0.0;
    double normal = 0.0;
    switch (prism.plane) {
    case PhysicsSandboxCudaExpressionPlane::XY:
        planeX = position.x - origin.x;
        planeY = position.y - origin.y;
        normal = position.z - origin.z;
        break;
    case PhysicsSandboxCudaExpressionPlane::XZ:
        planeX = position.x - origin.x;
        planeY = position.z - origin.z;
        normal = position.y - origin.y;
        break;
    case PhysicsSandboxCudaExpressionPlane::YZ:
        planeX = position.y - origin.y;
        planeY = position.z - origin.z;
        normal = position.x - origin.x;
        break;
    }
    if (normal < 0.0 || normal > depth) return false;
    bool inside = false;
    std::uint32_t previous = prism.vertexCount - 1u;
    for (std::uint32_t index = 0u; index < prism.vertexCount;
         previous = index++) {
        const auto &a = vertices[prism.vertexOffset + previous];
        const auto &b = vertices[prism.vertexOffset + index];
        if (PointOnSegment(planeX, planeY, a.x, a.y, b.x, b.y)) return true;
        const bool crosses = (a.y > planeY) != (b.y > planeY);
        if (crosses) {
            const double crossingX =
                    (b.x - a.x) * (planeY - a.y) / (b.y - a.y) + a.x;
            if (planeX < crossingX) inside = !inside;
        }
    }
    return inside;
}

}  // namespace

bool ConditionProgram::Evaluate(
        const PhysicsSandboxStateView &previous,
        const PhysicsSandboxStateView &current,
        const ConditionExecutionContext &context) const {
    using Op = PhysicsSandboxCudaConditionOpcode;
    std::vector<Value> stack;
    stack.reserve(32u);
    const auto push = [&](Value value) {
        if (!value.valid || stack.size() >= 32u) return false;
        stack.push_back(value);
        return true;
    };
    const auto pop = [&](ValueKind kind, Value *value) {
        if (stack.empty() || stack.back().kind != kind || !stack.back().valid)
            return false;
        *value = stack.back();
        stack.pop_back();
        return true;
    };
    for (const auto &instruction : cuda.instructions) {
        if (instruction.opcode == Op::Constant) {
            if (!push(Scalar(instruction.x))) return false;
            continue;
        }
        if (instruction.opcode == Op::ConstantVector) {
            if (!push(Vector(instruction.x, instruction.y, instruction.z)))
                return false;
            continue;
        }
        if (instruction.opcode == Op::Scalar || instruction.opcode == Op::Vector ||
            instruction.opcode == Op::RotationSource) {
            Value value = Source(instruction.value, previous, current, context);
            if (!value.valid) return false;
            if (instruction.opcode == Op::Scalar && value.kind == ValueKind::Vector) {
                const int component = static_cast<int>(instruction.x);
                value = Scalar(component == 1 ? value.x
                               : component == 2 ? value.y
                               : component == 3 ? value.z : 0.0);
            }
            if ((instruction.opcode == Op::Scalar &&
                 value.kind != ValueKind::Scalar) ||
                (instruction.opcode == Op::Vector &&
                 value.kind != ValueKind::Vector) ||
                (instruction.opcode == Op::RotationSource &&
                 value.kind != ValueKind::Rotation) ||
                !push(value))
                return false;
            continue;
        }
        if (instruction.opcode == Op::KilometersPerHour ||
            instruction.opcode == Op::Degrees ||
            instruction.opcode == Op::PercentRatio ||
            instruction.opcode == Op::Absolute ||
            instruction.opcode == Op::LogicalNot) {
            if (stack.empty() || stack.back().kind != ValueKind::Scalar)
                return false;
            if (instruction.opcode == Op::LogicalNot) {
                stack.back().x = stack.back().x == 0.0 ? 1.0 : 0.0;
                continue;
            }
            if (instruction.opcode == Op::KilometersPerHour)
                stack.back().x *= 3.6;
            else if (instruction.opcode == Op::Degrees)
                stack.back().x *= 57.29577951308232;
            else if (instruction.opcode == Op::PercentRatio)
                stack.back().x *= 0.01;
            else
                stack.back().x = std::abs(stack.back().x);
            if (!std::isfinite(stack.back().x)) return false;
            continue;
        }
        if (instruction.opcode == Op::ComposeVector ||
            instruction.opcode == Op::Direction ||
            instruction.opcode == Op::Rotation) {
            Value z;
            Value y;
            Value x;
            if (!pop(ValueKind::Scalar, &z) || !pop(ValueKind::Scalar, &y) ||
                !pop(ValueKind::Scalar, &x))
                return false;
            if (instruction.opcode == Op::Rotation) {
                constexpr double degreesToRadians =
                        3.14159265358979323846 / 180.0;
                const double hy = x.x * degreesToRadians * 0.5;
                const double hp = y.x * degreesToRadians * 0.5;
                const double hr = z.x * degreesToRadians * 0.5;
                const double cy = std::cos(hy);
                const double sy = std::sin(hy);
                const double cp = std::cos(hp);
                const double sp = std::sin(hp);
                const double cr = std::cos(hr);
                const double sr = std::sin(hr);
                Value rotation = Rotation(
                        sr * cp * cy - cr * sp * sy,
                        cr * sp * cy + sr * cp * sy,
                        cr * cp * sy - sr * sp * cy,
                        cr * cp * cy + sr * sp * sy);
                if (!NormalizeRotation(&rotation) || !push(rotation)) return false;
                continue;
            }
            Value vector = Vector(x.x, y.x, z.x);
            if (instruction.opcode == Op::Direction && !NormalizeVector(&vector))
                return false;
            if (!push(vector)) return false;
            continue;
        }
        if (instruction.opcode == Op::Magnitude ||
            instruction.opcode == Op::Normalize) {
            Value value;
            if (!pop(ValueKind::Vector, &value)) return false;
            if (instruction.opcode == Op::Magnitude) {
                if (!push(Scalar(ValueLength(value)))) return false;
            } else {
                if (!NormalizeVector(&value) || !push(value)) return false;
            }
            continue;
        }
        if (instruction.opcode == Op::Distance || instruction.opcode == Op::Dot) {
            Value right;
            Value left;
            if (!pop(ValueKind::Vector, &right) ||
                !pop(ValueKind::Vector, &left))
                return false;
            const double value = instruction.opcode == Op::Distance
                    ? std::sqrt((left.x - right.x) * (left.x - right.x) +
                                (left.y - right.y) * (left.y - right.y) +
                                (left.z - right.z) * (left.z - right.z))
                    : left.x * right.x + left.y * right.y + left.z * right.z;
            if (!push(Scalar(value))) return false;
            continue;
        }
        if (instruction.opcode == Op::RotationDistance) {
            Value right;
            Value left;
            if (!pop(ValueKind::Rotation, &right) ||
                !pop(ValueKind::Rotation, &left) ||
                !NormalizeRotation(&left) || !NormalizeRotation(&right))
                return false;
            const double dot = std::clamp(
                    std::abs(left.x * right.x + left.y * right.y +
                             left.z * right.z + left.w * right.w),
                    0.0, 1.0);
            if (!push(Scalar(2.0 * std::acos(dot)))) return false;
            continue;
        }
        if (instruction.opcode == Op::InsideBox ||
            instruction.opcode == Op::InsidePrism) {
            if (instruction.opcode == Op::InsideBox) {
                Value size;
                Value center;
                Value position;
                if (!pop(ValueKind::Vector, &size) ||
                    !pop(ValueKind::Vector, &center) ||
                    !pop(ValueKind::Vector, &position) || size.x <= 0.0 ||
                    size.y <= 0.0 || size.z <= 0.0)
                    return false;
                const bool inside =
                        std::abs(position.x - center.x) <= size.x * 0.5 &&
                        std::abs(position.y - center.y) <= size.y * 0.5 &&
                        std::abs(position.z - center.z) <= size.z * 0.5;
                if (!push(Scalar(inside ? 1.0 : 0.0))) return false;
                continue;
            }
            Value depth;
            Value origin;
            Value position;
            if (!pop(ValueKind::Scalar, &depth) ||
                !pop(ValueKind::Vector, &origin) ||
                !pop(ValueKind::Vector, &position) ||
                !std::isfinite(instruction.x) || instruction.x < 0.0 ||
                instruction.x >= static_cast<double>(cuda.prisms.size()))
                return false;
            const auto prismIndex = static_cast<std::size_t>(instruction.x);
            const bool inside = ContainsPrism(cuda.prisms[prismIndex],
                                              cuda.prismVertices, position,
                                              origin, depth.x);
            if (!push(Scalar(inside ? 1.0 : 0.0))) return false;
            continue;
        }
        if (instruction.opcode == Op::Clamp ||
            instruction.opcode == Op::WeightedBlend) {
            Value c;
            Value b;
            Value a;
            if (!pop(ValueKind::Scalar, &c) || !pop(ValueKind::Scalar, &b) ||
                !pop(ValueKind::Scalar, &a))
                return false;
            if (instruction.opcode == Op::Clamp) {
                if (b.x > c.x) return false;
                a.x = std::clamp(a.x, b.x, c.x);
            } else {
                const double weight = c.x / 100.0;
                a.x = a.x * (1.0 - weight) + b.x * weight;
            }
            if (!push(a)) return false;
            continue;
        }
        if (stack.size() < 2u) return false;
        Value right = stack.back();
        stack.pop_back();
        Value &left = stack.back();
        if (left.kind != ValueKind::Scalar || right.kind != ValueKind::Scalar ||
            !left.valid || !right.valid)
            return false;
        switch (instruction.opcode) {
        case Op::Add: left.x += right.x; break;
        case Op::Subtract: left.x -= right.x; break;
        case Op::Multiply: left.x *= right.x; break;
        case Op::Divide: left.x = right.x == 0.0 ? 0.0 : left.x / right.x; break;
        case Op::Minimum: left.x = std::min(left.x, right.x); break;
        case Op::Maximum: left.x = std::max(left.x, right.x); break;
        case Op::Greater: left = Scalar(left.x > right.x ? 1.0 : 0.0); break;
        case Op::Less: left = Scalar(left.x < right.x ? 1.0 : 0.0); break;
        case Op::GreaterOrEqual:
            left = Scalar(left.x >= right.x ? 1.0 : 0.0);
            break;
        case Op::LessOrEqual:
            left = Scalar(left.x <= right.x ? 1.0 : 0.0);
            break;
        case Op::Equal: left = Scalar(left.x == right.x ? 1.0 : 0.0); break;
        case Op::LogicalAnd:
            left = Scalar(left.x != 0.0 && right.x != 0.0 ? 1.0 : 0.0);
            break;
        case Op::LogicalOr:
            left = Scalar(left.x != 0.0 || right.x != 0.0 ? 1.0 : 0.0);
            break;
        default: return false;
        }
        if (!std::isfinite(left.x)) return false;
    }
    return stack.size() == 1u && stack[0].valid &&
           stack[0].kind == ValueKind::Scalar && stack[0].x != 0.0;
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
    // Reject statically-too-deep expressions at compile time; the
    // runtime guard would otherwise silently evaluate them to false.
    {
        std::size_t depth = 0u;
        std::size_t deepest = 0u;
        for (const auto &instruction : result.cuda.instructions) {
            const auto opcode = instruction.opcode;
            if (opcode == PhysicsSandboxCudaConditionOpcode::Constant ||
                opcode == PhysicsSandboxCudaConditionOpcode::ConstantVector ||
                opcode == PhysicsSandboxCudaConditionOpcode::Scalar ||
                opcode == PhysicsSandboxCudaConditionOpcode::Vector) {
                ++depth;
            } else if (opcode ==
                               PhysicsSandboxCudaConditionOpcode::
                                       KilometersPerHour ||
                       opcode ==
                               PhysicsSandboxCudaConditionOpcode::Degrees) {
                // net stack effect zero
            } else {
                if (depth < 2u) return {{}, "Condition script is malformed"};
                --depth;
            }
            deepest = std::max(deepest, depth);
        }
        if (deepest > 32u) {
            return {{},
                    "Condition line nesting exceeds the 32-value stack "
                    "limit"};
        }
    }
    return {std::move(result), std::nullopt};
}

}  // namespace forevertas
