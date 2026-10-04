#include "conditions/condition_catalog.h"

#include <array>
#include <utility>

namespace forevertas {
using Value = forevervalidator::experimental::PhysicsSandboxCudaConditionValue;

const std::vector<ConditionSymbol> &ConditionSymbols() {
    static const auto symbols = []() {
        std::vector<ConditionSymbol> result;
        const auto add = [&](std::string name, std::vector<std::string> aliases,
                             std::string type, std::string units, std::string description,
                             Value value, int component = 0, bool conditionsOnly = false) {
            result.push_back({std::move(name), std::move(aliases), std::move(type),
                              std::move(units), std::move(description), value, component, conditionsOnly});
        };
        for (bool previous : {false, true}) {
            const std::string p = previous ? "car.prev." : "car.";
            const std::string at = previous ? "Previous tick: " : "Current tick: ";
            const Value position = previous ? Value::PreviousPosition : Value::Position;
            const Value velocity = previous ? Value::PreviousVelocity : Value::Velocity;
            const Value local = previous ? Value::PreviousLocalVelocity : Value::LocalVelocity;
            const Value angular = previous ? Value::PreviousAngularVelocity : Value::AngularVelocity;
            add(p + "position", {p + "pos"}, "vector", "m", at + "world position", position);
            add(p + "velocity", {p + "vel"}, "vector", "m/s", at + "world velocity", velocity);
            add(p + "localvelocity", {p + "localvel"}, "vector", "m/s", at + "car-local velocity", local);
            const std::array<std::string, 3> axes{"x", "y", "z"};
            const std::array<std::string, 3> angles{"pitch", "yaw", "roll"};
            for (std::size_t i = 0; i < axes.size(); ++i) {
                const int component = static_cast<int>(i + 1);
                add(p + "position." + axes[i], {p + axes[i]}, "scalar", "m", at + "world position component", position, component);
                add(p + "velocity." + axes[i], {p + "vel." + axes[i]}, "scalar", "m/s", at + "world velocity component", velocity, component);
                add(p + "localvelocity." + axes[i], {p + "localvel." + axes[i]}, "scalar", "m/s", at + "car-local velocity component", local, component);
                add(p + "velocity." + angles[i], {p + "vel." + angles[i]}, "scalar", "rad/s", at + "angular velocity component", angular, component);
            }
            add(p + "speed", {}, "scalar", "m/s", at + "world velocity magnitude", previous ? Value::PreviousSpeed : Value::Speed);
            add(p + "localspeed", {}, "scalar", "m/s", at + "car-local velocity magnitude", previous ? Value::PreviousLocalSpeed : Value::LocalSpeed);
            add(p + "yaw", {p + "rotation.yaw"}, "scalar", "rad", at + "yaw angle", previous ? Value::PreviousYaw : Value::Yaw);
            add(p + "pitch", {p + "rotation.pitch"}, "scalar", "rad", at + "pitch angle", previous ? Value::PreviousPitch : Value::Pitch);
            add(p + "roll", {p + "rotation.roll"}, "scalar", "rad", at + "roll angle", previous ? Value::PreviousRoll : Value::Roll);
        }
        add("car.freewheel", {}, "boolean", "0 or 1", "Free-wheeling flag", Value::FreeWheeling);
        add("car.lateralcontact", {}, "boolean", "0 or 1", "Lateral-contact flag", Value::LateralContact);
        add("car.sliding", {"car.is_sliding", "car.is"}, "boolean", "0 or 1", "Car sliding flag", Value::Sliding);
        add("car.gear", {}, "integer", "gear index", "Current gear", Value::Gear);
        add("car.rpm", {}, "scalar", "native engine value", "Engine RPM telemetry", Value::Rpm);
        add("car.turning_rate", {"car.tr"}, "scalar", "native engine value", "Turning-rate telemetry", Value::TurningRate);
        add("car.turbo_type", {"car.tt"}, "integer", "engine enum", "Turbo type", Value::TurboType);
        add("car.turbo_boost_factor", {"car.tbf"}, "scalar", "factor", "Turbo boost multiplier", Value::TurboBoostFactor);
        add("car.cps", {}, "integer", "checkpoints", "Current-lap checkpoint count (not total across laps)", Value::CheckpointCount);
        add("iterations", {}, "integer", "attempts", "Search iteration counter", Value::Iterations, 0, true);
        add("last_improvement.time", {}, "scalar", "wall-clock s", "Search clock at last improvement; use time_since for elapsed time", Value::LastImprovementTime, 0, true);
        add("last_restart.time", {}, "scalar", "wall-clock s", "Search clock at last restart; not simulation time", Value::LastRestartTime, 0, true);
        const std::array<std::string, 4> wheels{"frontleft", "frontright", "backleft", "backright"};
        for (std::size_t i = 0; i < wheels.size(); ++i) {
            const auto offset = static_cast<unsigned int>(i);
            const std::string p = "car.wheels." + wheels[i];
            add(p + ".groundcontact", {}, "boolean", "0 or 1", "Wheel ground contact", static_cast<Value>(static_cast<unsigned int>(Value::WheelGroundContact0) + offset));
            add(p + ".is_sliding", {p + ".is"}, "boolean", "0 or 1", "Wheel sliding flag", static_cast<Value>(static_cast<unsigned int>(Value::WheelSliding0) + offset));
            add(p + ".surface", {}, "integer", "engine enum", "Wheel contact surface ID", static_cast<Value>(static_cast<unsigned int>(Value::WheelSurface0) + offset));
        }
        return result;
    }();
    return symbols;
}

const std::vector<ConditionFunction> &ConditionFunctions() {
    static const std::vector<ConditionFunction> functions{
        {"kmh", {}, "scalar", "km/h", "Convert m/s to km/h", "kmh(car.speed)"},
        {"deg", {}, "scalar", "degrees", "Convert radians to degrees", "deg(car.yaw)"},
        {"distance", {}, "scalar", "input vector units", "Euclidean distance between two vectors", "distance(car.position, (0,0,0))"},
        {"time_since", {}, "scalar", "wall-clock s", "Elapsed search time since a search timestamp; not simulation time", "time_since(last_improvement.time)", true},
        {"variable", {"var"}, "vector", "m", "Active point-target position; unavailable for other targets", "variable(\"bf_target_point\")", true, true}
    };
    return functions;
}

}  // namespace forevertas
