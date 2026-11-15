#ifndef FOREVERTAS_EVALUATORS_GATE_ENTRY_EVALUATOR_H
#define FOREVERTAS_EVALUATORS_GATE_ENTRY_EVALUATOR_H

// Gate entry target: an ordered list of oriented gates. Every gate but the
// last is a waypoint that must be passed honestly, in order; the last gate is
// scored by its interpolated crossing time. A gate is a vertical plane across
// the track through (x, y, z) facing heading `dir` (degrees, the telemetry yaw
// convention: 0 = +Z, 90 = +X). Only a genuine crossing counts: the previous
// tick is before the plane and the current tick is on or past it. The crossing
// instant is interpolated, and so are the position, speed and heading checked
// against the gate's limits.
//
// With soft scoring (default) every candidate gets a finite, ordered score,
// so a search can climb towards a gate that no candidate reaches yet:
//   valid final crossing        score = crossing simulation time (ms)
//   anything else (a "miss")    score = 1e7 + 1e6 * gates not passed
//                                        + 100 * penalty
// The penalty is the constraint violation of a crossing that missed its limits
// (lateral/height metres + yawWeight * degrees + speedWeight * km/h + 2 per
// missing wheel), or 25 + the closest approach (metres) to the next gate's
// opening when it was never crossed. Any valid result beats any miss.

#include "evaluators/iteration_evaluator.h"
#include "searches/option_configuration.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <forevervalidator/experimental/physics_sandbox.h>

namespace forevertas {

inline constexpr double kGateMissScoreBase = 1.0e7;

struct GateSpec {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double headingDeg = 0.0;
    // Lateral window, metres from the gate centre; + = left of travel.
    double lateralMin = -8.0;
    double lateralMax = 8.0;
    // Height window relative to y.
    double heightMin = -4.0;
    double heightMax = 8.0;
    double speedMinKmh = 0.0;
    double speedMaxKmh = 1.0e9;
    // Absolute heading window (telemetry yaw, degrees), wrap-aware.
    std::optional<double> yawMinDeg;
    std::optional<double> yawMaxDeg;
    int minimumWheels = 0;
};

struct GateCrossing {
    bool valid = false;
    double timeMs = 0.0;  // simulation time, interpolated
    double speedKmh = 0.0;
    double yawDeg = 0.0;
    double lateral = 0.0;
    double height = 0.0;
    int wheels = 0;
    double penalty = 0.0;  // 0 when valid
    std::string violations;  // empty when valid
};

// Parses "x=..,y=..,z=..,dir=..[,lmin=..,lmax=..|hw=..,ymin=..,ymax=..,
// vmin=..,vmax=..,yawmin=..,yawmax=..,wheels=..]" gates separated by '|'.
std::optional<std::vector<GateSpec>> ParseGateList(const std::string &text,
                                                   std::string *error);
std::string FormatGateList(const std::vector<GateSpec> &gates);

struct GateScoring {
    double yawWeight = 0.5;    // penalty metres per degree outside the window
    double speedWeight = 0.5;  // penalty metres per km/h outside the window
};

// A crossing of the gate plane near its opening (within 10 m laterally and
// 8 m vertically of the window) between `previous` and `current`, or nullopt.
std::optional<GateCrossing> TestGateCrossing(
        const GateSpec &gate,
        const GateScoring &scoring,
        const forevervalidator::experimental::PhysicsSandboxStateView &previous,
        const forevervalidator::experimental::PhysicsSandboxStateView &current);

// Distance (m) from the car to the gate's opening rectangle.
double GateApproachDistance(
        const GateSpec &gate,
        const forevervalidator::experimental::PhysicsSandboxStateView &state);

double CarYawDegrees(
        const forevervalidator::experimental::PhysicsSandboxStateView &state);

OptionSettings DefaultGateEntryOptionSettings();
std::optional<std::string> ValidateGateEntryOptionSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs);
std::unique_ptr<IterationEvaluator> CreateGateEntryEvaluator(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs);

}  // namespace forevertas

#endif
