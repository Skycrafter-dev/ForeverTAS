#include "blocks/visual_runtime.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace forevertas::blocks {
namespace {

struct Property {
  std::string key, label;
  std::function<VisualValue(const VisualState &)> read;
};

VisualValue Value(bool value) { return VisualValue(value); }
template<class T> VisualValue Value(T value) { return VisualValue(static_cast<double>(value)); }
VisualValue Value(forevervalidator::Vector3 value) {
  return VisualValue(VisualVector{value.x, value.y, value.z});
}
template<class T> VisualValue Value(const std::optional<T> &value) {
  return value ? Value(*value) : VisualValue{};
}
template<class T, std::size_t N> VisualValue Value(const std::array<T, N> &values) {
  auto list = std::make_shared<VisualList>();
  for (const auto &value : values) list->push_back(Value(value));
  return VisualValue(std::shared_ptr<const VisualList>(list));
}

const std::vector<Property> &Properties() {
  // One registry feeds both the dropdown and execution, so there cannot be a
  // cosmetic reporter for a state field that the runtime cannot actually read.
  static const std::vector<Property> properties{
#define STATE(key, label, member) {key, label, [](const VisualState &s) { return Value(s.member); }}
      STATE("time", "time (ms)", timeMs),
      STATE("tick", "tick", tick),
      STATE("duration", "duration (ms)", durationMs),
      STATE("position", "position (m)", car.position),
      STATE("velocity", "velocity (m/s)", car.linearSpeed),
      STATE("local-velocity", "local velocity (m/s)", car.localSpeed),
      STATE("angular-velocity", "angular velocity", car.angularSpeed),
      STATE("force", "force", car.force),
      STATE("torque", "torque", car.torque),
      {"rotation", "rotation", [](const VisualState &s) {
        return VisualValue(VisualRotation{s.car.rotationX, s.car.rotationY, s.car.rotationZ, s.car.rotationW});
      }},
      STATE("rotation-x", "rotation quaternion x", car.rotationX),
      STATE("rotation-y", "rotation quaternion y", car.rotationY),
      STATE("rotation-z", "rotation quaternion z", car.rotationZ),
      STATE("rotation-w", "rotation quaternion w", car.rotationW),
      {"speed", "speed (m/s)", [](const VisualState &s) {
        const auto &v = s.car.linearSpeed;
        return Value(std::hypot(static_cast<double>(v.x), v.y, v.z));
      }},
      STATE("signed-speed", "signed speed", car.signedSpeed),
      STATE("accelerate", "accelerate", accelerate),
      STATE("brake", "brake", brake),
      STATE("steering", "steering", steering),
      STATE("gear", "gear", car.gear),
      STATE("rpm", "engine RPM", car.rpm),
      STATE("turning-rate", "turning rate", car.turningRate),
      STATE("sliding", "sliding", car.sliding),
      STATE("freewheeling", "freewheeling", car.freeWheeling),
      STATE("lateral-contact", "lateral contact", car.lateralContact),
      STATE("turbo", "turbo", car.turbo),
      STATE("turbo-type", "turbo type", car.turboType),
      STATE("turbo-boost", "turbo boost factor", car.turboBoostFactor),
      STATE("burning", "burning", car.burning),
      STATE("gear-changed", "gear changed", car.gearChanged),
      STATE("wheel-contact", "wheel contact (list)", car.wheelContact),
      STATE("wheel-surface", "wheel surface IDs (list)", car.wheelSurface),
      STATE("wheel-has-surface", "wheel has surface (list)", car.wheelHasSurface),
      STATE("wheel-sliding", "wheel sliding (list)", car.wheelSliding),
      STATE("camera-up", "camera support up", car.cameraSupportUp),
      STATE("camera-flight", "camera flight transition", car.cameraFlightTransition),
      STATE("checkpoints", "checkpoints collected", checkpointsCollected),
      STATE("total-checkpoints", "total checkpoints", checkpointsTotal),
      STATE("laps", "completed laps", completedLaps),
      STATE("total-laps", "total laps", totalLaps),
      STATE("finished", "race completed", raceCompleted),
      STATE("finish-time", "finish time (ms, may be absent)", finishTimeMs),
      {"precise-finish-time", "precise finish time (ms, may be absent)", [](const VisualState &s) {
        return s.finishTime ? Value(static_cast<double>(s.finishTime->estimatedNs)/1000000.0) : VisualValue{};
      }},
      {"finish-lower-bound", "finish lower bound (ns, may be absent)", [](const VisualState &s) {
        return s.finishTime ? Value(s.finishTime->lowerBoundNs) : VisualValue{};
      }},
      {"finish-upper-bound", "finish upper bound (ns, may be absent)", [](const VisualState &s) {
        return s.finishTime ? Value(s.finishTime->upperBoundNs) : VisualValue{};
      }},
      STATE("respawns", "respawn count", respawnCount),
      STATE("stunt-points", "stunt points (may be absent)", stuntsScore),
      STATE("environment", "map environment ID", mapEnvironment),
      STATE("vehicle", "vehicle model ID", vehicleModel),
      STATE("play-mode", "play mode ID (may be absent)", playMode),
#undef STATE
  };
  return properties;
}
} // namespace

const std::vector<std::pair<std::string, std::string>> &VisualStateProperties() {
  static const auto choices = [] {
    std::vector<std::pair<std::string, std::string>> result;
    for (const auto &property : Properties()) result.emplace_back(property.key, property.label);
    return result;
  }();
  return choices;
}

VisualValue ReadVisualStateProperty(const VisualState &state, const std::string &property) {
  const auto &properties = Properties();
  const auto found = std::find_if(properties.begin(), properties.end(),
      [&](const Property &item) { return item.key == property; });
  if (found == properties.end()) throw std::runtime_error("Unknown simulation property: " + property);
  return found->read(state);
}

VisualValue ReadVisualStateProperty(const VisualState &state, std::size_t property) {
  const auto &properties=Properties();
  if (property>=properties.size()) throw std::runtime_error("Unknown simulation property index.");
  return properties[property].read(state);
}
} // namespace forevertas::blocks
