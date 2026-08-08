#include "conditions/condition_program.h"
#include "conditions/condition_ast_compiler.h"
#include "conditions/condition_syntax.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
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
        const ConditionVariables &variables,
        ConditionGateMode gateMode) {
    ConditionCompileResult compiled;
    ConditionProgram result;
    std::istringstream lines(source);
    std::string line;
    std::size_t lineNumber = 0u;
    std::size_t count = 0u;
    while (std::getline(lines, line)) {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (std::all_of(line.begin(), line.end(), [](unsigned char c) { return std::isspace(c); })) continue;
        const auto firstContent = std::find_if(
                line.begin(), line.end(),
                [](unsigned char c) { return !std::isspace(c); });
        if (firstContent != line.end() &&
            std::distance(firstContent, line.end()) >= 2 &&
            firstContent[0] == '/' && firstContent[1] == '/') {
            continue;
        }
        std::vector<PhysicsSandboxCudaConditionInstruction> lineInstructions;
        const ConditionParsedLine parsed = ParseConditionLine(line);
        if (parsed.error) {
            const ConditionSourceRange range = parsed.error->range;
            compiled.diagnostics.push_back(
                    {static_cast<std::uint32_t>(lineNumber),
                     static_cast<std::uint32_t>(range.begin + 1u),
                     static_cast<std::uint32_t>(
                             std::max<std::size_t>(range.end - range.begin,
                                                   1u)),
                     parsed.error->message});
            continue;
        }
        ConditionAstCompileError astError;
        if (!parsed.root ||
            !CompileConditionAst(*parsed.root,
                                 variables,
                                 &lineInstructions,
                                 &astError)) {
            compiled.diagnostics.push_back(
                    {static_cast<std::uint32_t>(lineNumber),
                     static_cast<std::uint32_t>(astError.range.begin + 1u),
                     static_cast<std::uint32_t>(std::max<std::size_t>(
                             astError.range.end - astError.range.begin, 1u)),
                     std::move(astError.message)});
            continue;
        }

        const std::size_t finalizerSize =
                gateMode == ConditionGateMode::Any ? 2u : 0u;
        const std::size_t combinedSize = result.cuda.instructions.size() +
                lineInstructions.size() + (count == 0u ? 0u : 1u) +
                finalizerSize;
        if (combinedSize > 256u) {
            compiled.diagnostics.push_back(
                    {static_cast<std::uint32_t>(lineNumber),
                     1u,
                     static_cast<std::uint32_t>(
                             std::max<std::size_t>(line.size(), 1u)),
                     "condition script exceeds the 256-instruction limit"});
            continue;
        }
        result.cuda.instructions.insert(result.cuda.instructions.end(),
                                        lineInstructions.begin(),
                                        lineInstructions.end());
        if (count++ != 0u) {
            result.cuda.instructions.push_back(
                    {gateMode == ConditionGateMode::All
                             ? PhysicsSandboxCudaConditionOpcode::LogicalAnd
                             : PhysicsSandboxCudaConditionOpcode::Add});
        }
    }
    if (!compiled.diagnostics.empty()) {
        const ConditionDiagnostic &first = compiled.diagnostics.front();
        compiled.error = "Condition line " + std::to_string(first.line) +
                ": " + first.message + " at column " +
                std::to_string(first.column);
        return compiled;
    }
    if (count != 0u) {
        if (gateMode == ConditionGateMode::Any) {
            result.cuda.instructions.push_back(
                    {PhysicsSandboxCudaConditionOpcode::Constant, {}, 0.0});
            result.cuda.instructions.push_back(
                    {PhysicsSandboxCudaConditionOpcode::Greater});
        }
        compiled.program = std::move(result);
    }
    return compiled;
}

}  // namespace forevertas
