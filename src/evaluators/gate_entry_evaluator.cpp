#include "evaluators/gate_entry_evaluator.h"

#include "evaluators/evaluator_utils.h"
#include "searches/option_settings_utils.h"
#include "time_format.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace forevertas {
namespace {

using forevervalidator::experimental::PhysicsSandboxStateView;

constexpr double kDegrees = 57.29577951308232;
constexpr double kNearLateralMargin = 10.0;
constexpr double kNearHeightMargin = 8.0;
// Misses at the same gate are banded: a crossing that violates the gate's
// limits (penalty 0..4999) always ranks above never reaching the gate
// (5000 + closest approach distance).
constexpr double kViolationPenaltyCap = 4999.0;
constexpr double kApproachPenaltyBase = 5000.0;
constexpr double kMaximumPenalty = 9999.0;
constexpr std::size_t kMaximumGates = 32u;

double Wrap360(double degrees) {
    double wrapped = std::fmod(degrees, 360.0);
    if (wrapped < 0.0) wrapped += 360.0;
    return wrapped;
}

double Wrap180(double degrees) {
    return Wrap360(degrees + 180.0) - 180.0;
}

struct GateFrame {
    double along = 0.0;
    double lateral = 0.0;
    double height = 0.0;
};

GateFrame FrameOf(const GateSpec &gate, double x, double y, double z) {
    const double radians = gate.headingDeg / kDegrees;
    const double dx = std::sin(radians);
    const double dz = std::cos(radians);
    const double ox = x - gate.x;
    const double oz = z - gate.z;
    // Left of travel is (dz, -dx): facing +Z, left is +X.
    return {ox * dx + oz * dz, ox * dz - oz * dx, y - gate.y};
}

double Excess(double value, double minimum, double maximum) {
    return std::max({0.0, minimum - value, value - maximum});
}

// Degrees outside [yawMin, yawMax] measured counter-clockwise from yawMin.
double YawExcess(const GateSpec &gate, double yaw) {
    if (!gate.yawMinDeg || !gate.yawMaxDeg) return 0.0;
    const double width = Wrap360(*gate.yawMaxDeg - *gate.yawMinDeg);
    const double offset = Wrap360(yaw - *gate.yawMinDeg);
    if (offset <= width) return 0.0;
    return std::min(offset - width, 360.0 - offset);
}

double SpeedKmh(const PhysicsSandboxStateView &state) {
    return Length(VelocityOf(state)) * 3.6;
}

int WheelsOnGround(const PhysicsSandboxStateView &state) {
    int count = 0;
    for (const bool contact : state.car.wheelContact) count += contact ? 1 : 0;
    return count;
}

std::string Fixed(double value, int digits) {
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(digits);
    stream << value;
    return stream.str();
}

std::string Signed(double value, int digits) {
    return (value >= 0.0 ? "+" : "") + Fixed(value, digits);
}

std::optional<bool> ParseBool(const std::string &text) {
    if (text == "true" || text == "1" || text == "yes") return true;
    if (text == "false" || text == "0" || text == "no") return false;
    return std::nullopt;
}

struct GateConfig {
    std::vector<GateSpec> gates;
    GateScoring scoring;
    bool soft = true;
};

std::optional<GateConfig> ParseConfig(const OptionSettings &settings,
                                      std::string *error) {
    GateConfig config;
    const auto gates = settings.find("gates");
    if (gates == settings.end()) {
        if (error) *error = "gate target needs a 'gates' setting";
        return std::nullopt;
    }
    auto parsed = ParseGateList(gates->second, error);
    if (!parsed) return std::nullopt;
    config.gates = std::move(*parsed);
    if (const auto soft = settings.find("soft"); soft != settings.end()) {
        const auto value = ParseBool(soft->second);
        if (!value) {
            if (error) *error = "gate target 'soft' must be true or false";
            return std::nullopt;
        }
        config.soft = *value;
    }
    for (const auto &[key, target] :
         {std::pair<const char *, double *>{"yawWeight", &config.scoring.yawWeight},
          std::pair<const char *, double *>{"speedWeight", &config.scoring.speedWeight}}) {
        const auto found = settings.find(key);
        if (found == settings.end()) continue;
        const auto value = ParseFiniteDouble(found->second);
        if (!value || *value < 0.0 || *value > 1000.0) {
            if (error) *error = std::string("gate target '") + key + "' must be 0..1000";
            return std::nullopt;
        }
        *target = *value;
    }
    return config;
}

EvaluationSample MissSample(const GateConfig &config,
                            std::size_t passed,
                            double penalty,
                            double timeMs,
                            const std::string &detail) {
    const std::size_t remaining = config.gates.size() - passed;
    const double clamped = std::min(penalty, kMaximumPenalty);
    const double score = kGateMissScoreBase +
            1.0e6 * static_cast<double>(remaining) + 100.0 * clamped;
    std::string description = "Gate miss: " + std::to_string(passed) + "/" +
            std::to_string(config.gates.size()) + " passed, gate " +
            std::to_string(passed + 1u) + " " + detail + " (penalty " +
            Fixed(clamped, 2) + ")";
    return EvaluationSample{score, timeMs, std::move(description)};
}

class GateSession final : public IterationEvaluationSession {
public:
    explicit GateSession(std::shared_ptr<const GateConfig> config)
        : config_(std::move(config)) {}

    std::unique_ptr<IterationEvaluationSession> Clone() const override {
        return std::make_unique<GateSession>(*this);
    }

    bool IsComplete() const override { return complete_; }

    std::optional<EvaluationSample> Observe(
            const std::optional<PhysicsSandboxStateView> &previous,
            const PhysicsSandboxStateView &current) override {
        if (complete_) return std::nullopt;
        const GateConfig &config = *config_;
        if (previous) {
            const GateSpec &gate = config.gates[index_];
            if (auto crossing = TestGateCrossing(gate, config.scoring,
                                                 *previous, current)) {
                if (crossing->valid) {
                    if (index_ + 1u == config.gates.size()) {
                        complete_ = true;
                        std::string description =
                                "Gate entry: " +
                                FormatInterpolatedTimeMilliseconds(crossing->timeMs) +
                                ", " + Fixed(crossing->speedKmh, 2) + " km/h, yaw " +
                                Fixed(crossing->yawDeg, 1) + ", lat " +
                                Signed(crossing->lateral, 2) + " m";
                        return EvaluationSample{crossing->timeMs,
                                                crossing->timeMs,
                                                std::move(description)};
                    }
                    ++index_;
                    bestApproach_ = std::numeric_limits<double>::infinity();
                } else {
                    complete_ = true;
                    if (!config.soft) return std::nullopt;
                    return MissSample(config, index_,
                                      std::min(crossing->penalty, kViolationPenaltyCap),
                                      crossing->timeMs,
                                      "crossed outside its limits: " +
                                              crossing->violations);
                }
            }
        }
        if (current.raceCompleted) complete_ = true;
        if (!config.soft) return std::nullopt;
        const double distance =
                GateApproachDistance(config.gates[index_], current);
        if (!(distance < bestApproach_ - 1e-4)) return std::nullopt;
        bestApproach_ = distance;
        return MissSample(config, index_, kApproachPenaltyBase + distance,
                          static_cast<double>(current.timeMs),
                          "not crossed, closest approach " +
                                  Fixed(distance, 2) + " m");
    }

private:
    std::shared_ptr<const GateConfig> config_;
    std::size_t index_ = 0u;
    bool complete_ = false;
    double bestApproach_ = std::numeric_limits<double>::infinity();
};

class GateEvaluator final : public IterationEvaluator {
public:
    explicit GateEvaluator(GateConfig config)
        : config_(std::make_shared<const GateConfig>(std::move(config))) {}

    EvaluationPlan Plan(std::int64_t simulationHorizonMs,
                        std::int64_t earliestMutationTimeMs,
                        std::uint32_t tickDurationMs) const override {
        static_cast<void>(tickDurationMs);
        return {earliestMutationTimeMs, simulationHorizonMs};
    }

    std::unique_ptr<IterationEvaluationSession> CreateSession()
            const override {
        return std::make_unique<GateSession>(config_);
    }

    bool IsBetter(const EvaluationSample &iteration,
                  const EvaluationSample &incumbent) const override {
        return iteration.score < incumbent.score;
    }

    // A valid crossing reports its own time, so later attempts cannot win.
    // A miss can still be beaten by anything that gets further.
    std::optional<double> LatestImprovingTimeMs(
            const EvaluationSample &incumbent) const override {
        if (incumbent.score < kGateMissScoreBase) return incumbent.timeMs;
        return std::nullopt;
    }

private:
    std::shared_ptr<const GateConfig> config_;
};

std::optional<double> ParseGateNumber(const std::string &text) {
    const auto value = ParseFiniteDouble(text);
    if (!value || std::abs(*value) > 1.0e7) return std::nullopt;
    return value;
}

}  // namespace

double CarYawDegrees(const PhysicsSandboxStateView &state) {
    const double x = state.car.rotationX;
    const double y = state.car.rotationY;
    const double z = state.car.rotationZ;
    const double w = state.car.rotationW;
    return std::atan2(2.0 * (x * z + w * y), 1.0 - 2.0 * (x * x + y * y)) *
            kDegrees;
}

std::optional<std::vector<GateSpec>> ParseGateList(const std::string &text,
                                                   std::string *error) {
    const auto fail = [error](const std::string &message)
            -> std::optional<std::vector<GateSpec>> {
        if (error) *error = message;
        return std::nullopt;
    };
    std::vector<GateSpec> gates;
    std::size_t start = 0u;
    while (start <= text.size()) {
        std::size_t end = text.find('|', start);
        if (end == std::string::npos) end = text.size();
        const std::string chunk = text.substr(start, end - start);
        start = end + 1u;
        if (chunk.find_first_not_of(" \t\r\n") == std::string::npos) {
            if (end == text.size()) break;
            continue;
        }
        GateSpec gate;
        bool hasX = false, hasY = false, hasZ = false, hasDir = false;
        std::optional<double> yawMin, yawMax;
        std::size_t position = 0u;
        while (position <= chunk.size()) {
            std::size_t comma = chunk.find(',', position);
            if (comma == std::string::npos) comma = chunk.size();
            std::string field = chunk.substr(position, comma - position);
            position = comma + 1u;
            field.erase(0, field.find_first_not_of(" \t\r\n"));
            field.erase(field.find_last_not_of(" \t\r\n") + 1u);
            if (field.empty()) {
                if (comma == chunk.size()) break;
                continue;
            }
            const std::size_t equals = field.find('=');
            if (equals == std::string::npos) {
                return fail("gate field '" + field + "' is not key=value");
            }
            const std::string key = field.substr(0u, equals);
            const auto value = ParseGateNumber(field.substr(equals + 1u));
            if (!value) return fail("gate field '" + field + "' has an invalid number");
            if (key == "x") { gate.x = *value; hasX = true; }
            else if (key == "y") { gate.y = *value; hasY = true; }
            else if (key == "z") { gate.z = *value; hasZ = true; }
            else if (key == "dir") { gate.headingDeg = *value; hasDir = true; }
            else if (key == "lmin") gate.lateralMin = *value;
            else if (key == "lmax") gate.lateralMax = *value;
            else if (key == "hw") { gate.lateralMin = -*value; gate.lateralMax = *value; }
            else if (key == "ymin") gate.heightMin = *value;
            else if (key == "ymax") gate.heightMax = *value;
            else if (key == "vmin") gate.speedMinKmh = *value;
            else if (key == "vmax") gate.speedMaxKmh = *value;
            else if (key == "yawmin") yawMin = *value;
            else if (key == "yawmax") yawMax = *value;
            else if (key == "wheels") gate.minimumWheels = static_cast<int>(std::lround(*value));
            else return fail("unknown gate field '" + key + "'");
            if (comma == chunk.size()) break;
        }
        if (!hasX || !hasY || !hasZ || !hasDir) {
            return fail("every gate needs x, y, z and dir");
        }
        if (yawMin.has_value() != yawMax.has_value()) {
            return fail("give both yawmin and yawmax, or neither");
        }
        gate.yawMinDeg = yawMin;
        gate.yawMaxDeg = yawMax;
        if (!(gate.lateralMin < gate.lateralMax)) return fail("gate needs lmin < lmax");
        if (!(gate.heightMin < gate.heightMax)) return fail("gate needs ymin < ymax");
        if (gate.speedMinKmh > gate.speedMaxKmh) return fail("gate needs vmin <= vmax");
        if (gate.minimumWheels < 0 || gate.minimumWheels > 4) return fail("gate wheels must be 0..4");
        gates.push_back(gate);
        if (gates.size() > kMaximumGates) return fail("at most 32 gates");
        if (end == text.size()) break;
    }
    if (gates.empty()) return fail("no gates given");
    return gates;
}

std::string FormatGateList(const std::vector<GateSpec> &gates) {
    std::ostringstream stream;
    stream.precision(10);
    for (std::size_t index = 0u; index < gates.size(); ++index) {
        const GateSpec &gate = gates[index];
        if (index > 0u) stream << '|';
        stream << "x=" << gate.x << ",y=" << gate.y << ",z=" << gate.z
               << ",dir=" << gate.headingDeg << ",lmin=" << gate.lateralMin
               << ",lmax=" << gate.lateralMax << ",ymin=" << gate.heightMin
               << ",ymax=" << gate.heightMax;
        if (gate.speedMinKmh > 0.0) stream << ",vmin=" << gate.speedMinKmh;
        if (gate.speedMaxKmh < 1.0e9) stream << ",vmax=" << gate.speedMaxKmh;
        if (gate.yawMinDeg) stream << ",yawmin=" << *gate.yawMinDeg << ",yawmax=" << *gate.yawMaxDeg;
        if (gate.minimumWheels > 0) stream << ",wheels=" << gate.minimumWheels;
    }
    return stream.str();
}

std::optional<GateCrossing> TestGateCrossing(
        const GateSpec &gate,
        const GateScoring &scoring,
        const PhysicsSandboxStateView &previous,
        const PhysicsSandboxStateView &current) {
    const auto before = PositionOf(previous);
    const auto after = PositionOf(current);
    const GateFrame a = FrameOf(gate, before.x, before.y, before.z);
    const GateFrame b = FrameOf(gate, after.x, after.y, after.z);
    if (!(a.along < 0.0 && b.along >= 0.0)) return std::nullopt;
    const double fraction = -a.along / (b.along - a.along);
    GateCrossing crossing;
    crossing.lateral = a.lateral + fraction * (b.lateral - a.lateral);
    crossing.height = a.height + fraction * (b.height - a.height);
    if (crossing.lateral < gate.lateralMin - kNearLateralMargin ||
        crossing.lateral > gate.lateralMax + kNearLateralMargin ||
        crossing.height < gate.heightMin - kNearHeightMargin ||
        crossing.height > gate.heightMax + kNearHeightMargin) {
        return std::nullopt;  // the plane was crossed somewhere else
    }
    crossing.timeMs = static_cast<double>(previous.timeMs) +
            fraction * static_cast<double>(current.timeMs - previous.timeMs);
    const double speedBefore = SpeedKmh(previous);
    crossing.speedKmh = speedBefore + fraction * (SpeedKmh(current) - speedBefore);
    const double yawBefore = CarYawDegrees(previous);
    crossing.yawDeg = Wrap180(yawBefore + fraction *
                              Wrap180(CarYawDegrees(current) - yawBefore));
    crossing.wheels = WheelsOnGround(current);

    const double lateralExcess = Excess(crossing.lateral, gate.lateralMin, gate.lateralMax);
    const double heightExcess = Excess(crossing.height, gate.heightMin, gate.heightMax);
    const double speedExcess = Excess(crossing.speedKmh, gate.speedMinKmh, gate.speedMaxKmh);
    const double yawExcess = YawExcess(gate, crossing.yawDeg);
    const int wheelShortfall = std::max(0, gate.minimumWheels - crossing.wheels);
    crossing.penalty = lateralExcess + heightExcess +
            scoring.speedWeight * speedExcess + scoring.yawWeight * yawExcess +
            2.0 * wheelShortfall;
    crossing.valid = crossing.penalty <= 0.0;
    std::string violations;
    const auto add = [&violations](const std::string &text) {
        if (!violations.empty()) violations += ", ";
        violations += text;
    };
    if (lateralExcess > 0.0) add("lateral " + Signed(crossing.lateral, 2) + " m");
    if (heightExcess > 0.0) add("height " + Signed(crossing.height, 2) + " m");
    if (speedExcess > 0.0) add("speed " + Fixed(crossing.speedKmh, 2) + " km/h");
    if (yawExcess > 0.0) add("yaw " + Fixed(crossing.yawDeg, 1) + " (" + Fixed(yawExcess, 1) + " outside)");
    if (wheelShortfall > 0) add("wheels " + std::to_string(crossing.wheels));
    crossing.violations = std::move(violations);
    return crossing;
}

double GateApproachDistance(const GateSpec &gate,
                            const PhysicsSandboxStateView &state) {
    const auto position = PositionOf(state);
    const GateFrame frame = FrameOf(gate, position.x, position.y, position.z);
    const double lateral = Excess(frame.lateral, gate.lateralMin, gate.lateralMax);
    const double height = Excess(frame.height, gate.heightMin, gate.heightMax);
    return std::sqrt(frame.along * frame.along + lateral * lateral +
                     height * height);
}

OptionSettings DefaultGateEntryOptionSettings() {
    return {{"gates", "x=0,y=0,z=0,dir=0,hw=8"},
            {"soft", "true"},
            {"yawWeight", "0.5"},
            {"speedWeight", "0.5"}};
}

std::optional<std::string> ValidateGateEntryOptionSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) {
    static_cast<void>(tickDurationMs);
    if (const auto keyError = ValidateOptionSettingKeys(
                settings, DefaultGateEntryOptionSettings())) {
        return keyError;
    }
    std::string error;
    if (!ParseConfig(settings, &error)) return error;
    return std::nullopt;
}

std::unique_ptr<IterationEvaluator> CreateGateEntryEvaluator(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) {
    if (const auto error = ValidateGateEntryOptionSettings(settings, tickDurationMs)) {
        throw std::invalid_argument(*error);
    }
    std::string error;
    return std::make_unique<GateEvaluator>(*ParseConfig(settings, &error));
}

}  // namespace forevertas
