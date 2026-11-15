#include "evaluators/distance_target_evaluator.h"

#include "evaluators/around_walls_distance.h"
#include "evaluators/evaluator_utils.h"
#include "searches/option_settings_utils.h"
#include "time_format.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace forevertas {
namespace {

namespace sandbox = forevervalidator::experimental;

// Runs that never reach the target score this plus their closest
// distance; reached ones score their reach time, so any reach wins.
constexpr double kNotReached = 1e12;
// Each checkpoint still to come before the target outweighs any distance.
constexpr double kPerEvent = 1e6;
// Around walls: walls count up to this many metres from the trigger.
constexpr std::uint32_t kMaximumReach = 256u;
// Beyond this gap between the car's bounding sphere and a trigger's box,
// the box distance stands in for the exact one.
constexpr double kExactRange = 4.0;

struct Vec {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec operator*(Vec a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double Dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec Cross(Vec a, Vec b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}
Vec Scale(Vec a, Vec b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }

struct Quat {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double w = 1.0;
};

Quat operator*(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

Vec Rotate(Quat q, Vec v) {
    const Vec axis{q.x, q.y, q.z};
    const Vec t = Cross(axis, v) * 2.0;
    return v + t * q.w + Cross(axis, t);
}

Vec InverseRotate(Quat q, Vec v) { return Rotate({-q.x, -q.y, -q.z, q.w}, v); }

Vec Of(const forevervalidator::Vector3 &v) { return {v.x, v.y, v.z}; }

// Closest point of triangle abc to the origin (Ericson, Real-Time Collision
// Detection, 5.1.5).
Vec ClosestToOrigin(Vec a, Vec b, Vec c) {
    const Vec ab = b - a;
    const Vec ac = c - a;
    const Vec ap = a * -1.0;
    const double d1 = Dot(ab, ap);
    const double d2 = Dot(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0) return a;
    const Vec bp = b * -1.0;
    const double d3 = Dot(ab, bp);
    const double d4 = Dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3) return b;
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
        return a + ab * (d1 / (d1 - d3));
    }
    const Vec cp = c * -1.0;
    const double d5 = Dot(ab, cp);
    const double d6 = Dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6) return c;
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
        return a + ac * (d2 / (d2 - d6));
    }
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    }
    const double denominator = 1.0 / (va + vb + vc);
    return a + ab * (vb * denominator) + ac * (vc * denominator);
}

struct Triangle {
    Vec a;
    Vec b;
    Vec c;
};

struct Box {
    Vec minimum{std::numeric_limits<double>::max(),
                std::numeric_limits<double>::max(),
                std::numeric_limits<double>::max()};
    Vec maximum{std::numeric_limits<double>::lowest(),
                std::numeric_limits<double>::lowest(),
                std::numeric_limits<double>::lowest()};

    void Add(Vec v) {
        minimum = {std::min(minimum.x, v.x), std::min(minimum.y, v.y),
                   std::min(minimum.z, v.z)};
        maximum = {std::max(maximum.x, v.x), std::max(maximum.y, v.y),
                   std::max(maximum.z, v.z)};
    }

    double DistanceTo(Vec p) const {
        const double dx = std::max({minimum.x - p.x, 0.0, p.x - maximum.x});
        const double dy = std::max({minimum.y - p.y, 0.0, p.y - maximum.y});
        const double dz = std::max({minimum.z - p.z, 0.0, p.z - maximum.z});
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }
};

struct Surface {
    std::vector<Triangle> triangles;
    Box box;
};

struct Shape {
    Vec center;
    Quat rotation;
    Vec radii;
    Vec inverseRadii;
};

struct CarShape {
    std::vector<Shape> shapes;
    // Bounding sphere around the car origin.
    double radius = 0.0;
    // Around walls, the car is a sphere at the centre of its shapes' box,
    // as wide as the box's smallest half size: it never closes a gap the
    // car fits through.
    Vec bodyCenter;
    double bodyRadius = 0.0;
};

CarShape MakeCarShape(const std::vector<sandbox::PhysicsSandboxEllipsoid> &ellipsoids) {
    CarShape car;
    for (const sandbox::PhysicsSandboxEllipsoid &ellipsoid : ellipsoids) {
        Shape shape;
        shape.center = Of(ellipsoid.position);
        shape.rotation = {ellipsoid.rotationX, ellipsoid.rotationY,
                          ellipsoid.rotationZ, ellipsoid.rotationW};
        shape.radii = Of(ellipsoid.radii);
        if (shape.radii.x <= 0.0 || shape.radii.y <= 0.0 ||
            shape.radii.z <= 0.0) {
            continue;
        }
        shape.inverseRadii = {1.0 / shape.radii.x, 1.0 / shape.radii.y,
                              1.0 / shape.radii.z};
        car.radius = std::max(
                car.radius,
                std::sqrt(Dot(shape.center, shape.center)) +
                        std::max({shape.radii.x, shape.radii.y,
                                  shape.radii.z}));
        car.shapes.push_back(shape);
    }
    Vec low{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
            std::numeric_limits<double>::max()};
    Vec high = low * -1.0;
    for (const Shape &shape : car.shapes) {
        // Half extents of a rotated ellipsoid's box along each axis.
        const Vec ex = Rotate(shape.rotation, {shape.radii.x, 0.0, 0.0});
        const Vec ey = Rotate(shape.rotation, {0.0, shape.radii.y, 0.0});
        const Vec ez = Rotate(shape.rotation, {0.0, 0.0, shape.radii.z});
        const Vec half{std::sqrt(ex.x * ex.x + ey.x * ey.x + ez.x * ez.x),
                       std::sqrt(ex.y * ex.y + ey.y * ey.y + ez.y * ez.y),
                       std::sqrt(ex.z * ex.z + ey.z * ey.z + ez.z * ez.z)};
        low = {std::min(low.x, shape.center.x - half.x),
               std::min(low.y, shape.center.y - half.y),
               std::min(low.z, shape.center.z - half.z)};
        high = {std::max(high.x, shape.center.x + half.x),
                std::max(high.y, shape.center.y + half.y),
                std::max(high.z, shape.center.z + half.z)};
    }
    if (!car.shapes.empty()) {
        car.bodyCenter = (low + high) * 0.5;
        const Vec half = (high - low) * 0.5;
        car.bodyRadius = std::min({half.x, half.y, half.z});
    }
    return car;
}

Surface MakeSurface(const std::vector<sandbox::PhysicsSandboxCollisionTriangle> &triangles) {
    Surface surface;
    surface.triangles.reserve(triangles.size());
    for (const auto &triangle : triangles) {
        const Triangle t{Of(triangle.a), Of(triangle.b), Of(triangle.c)};
        surface.box.Add(t.a);
        surface.box.Add(t.b);
        surface.box.Add(t.c);
        surface.triangles.push_back(t);
    }
    return surface;
}

// Each shape becomes a unit sphere once the triangles are moved into its
// frame and scaled by its radii; the closest triangle point there is
// measured back in metres against the sphere point in its direction.
double ExactDistance(const CarShape &car, const Surface &surface,
                     Vec position, Quat rotation) {
    double best = std::numeric_limits<double>::max();
    for (const Shape &shape : car.shapes) {
        const Vec center = position + Rotate(rotation, shape.center);
        const Quat orientation = rotation * shape.rotation;
        const auto toUnit = [&](Vec v) {
            return Scale(InverseRotate(orientation, v - center),
                         shape.inverseRadii);
        };
        for (const Triangle &triangle : surface.triangles) {
            const Vec closest = ClosestToOrigin(
                    toUnit(triangle.a), toUnit(triangle.b), toUnit(triangle.c));
            const double length = std::sqrt(Dot(closest, closest));
            if (length <= 1.0) return 0.0;
            const Vec gap = Scale(closest - closest * (1.0 / length),
                                  shape.radii);
            best = std::min(best, std::sqrt(Dot(gap, gap)));
        }
    }
    return best;
}

// How far the car's shapes reach from its body centre along a unit
// direction (support function of the ellipsoids).
double Reach(const CarShape &car, Quat rotation, Vec direction) {
    const Vec local = InverseRotate(rotation, direction);
    double farthest = 0.0;
    for (const Shape &shape : car.shapes) {
        const Vec inShape = InverseRotate(shape.rotation, local);
        const Vec scaled = Scale(inShape, shape.radii);
        farthest = std::max(farthest, Dot(shape.center - car.bodyCenter, local) +
                                              std::sqrt(Dot(scaled, scaled)));
    }
    return farthest;
}

double DistanceToSurface(const CarShape &car, const Surface &surface,
                         Vec position, Quat rotation) {
    if (surface.triangles.empty()) return std::numeric_limits<double>::max();
    const double coarse = surface.box.DistanceTo(position) - car.radius;
    if (coarse > kExactRange) return coarse;
    return ExactDistance(car, surface, position, rotation);
}

// A trigger the target may be measured to: a checkpoint's map slot, or
// none for a finish line.
struct TargetSurface {
    std::optional<std::uint32_t> slot;
    Surface surface;
    std::vector<sandbox::PhysicsSandboxCollisionTriangle> triangles;
    // Around walls: built the first time a run needs it.
    std::shared_ptr<std::once_flag> routeBuilt = std::make_shared<std::once_flag>();
    std::shared_ptr<std::shared_ptr<const AroundWallsDistance>> route =
            std::make_shared<std::shared_ptr<const AroundWallsDistance>>();
};

struct Geometry {
    CarShape car;
    std::vector<TargetSurface> targets;
    // Around walls only: the map's walls, and every trigger (not walls).
    double reach = 0.0;
    std::vector<sandbox::PhysicsSandboxCollisionTriangle> walls;
    std::vector<sandbox::PhysicsSandboxCollisionTriangle> triggers;

    const AroundWallsDistance &Route(const TargetSurface &target) const {
        std::call_once(*target.routeBuilt, [&] {
            *target.route = AroundWallsDistance::Shared(
                    walls, target.triangles, triggers, car.bodyRadius, reach);
        });
        return **target.route;
    }
};

// The same selectors as the Checkpoint time target.
struct DistanceSettings {
    std::int64_t minimumTimeMs = 0;
    std::int64_t maximumTimeMs = 0;
    bool finish = false;
    std::uint32_t checkpointIndex = 0u;  // zero-based within the lap
    std::uint32_t lap = 1u;
    std::optional<std::uint32_t> checkpointSlot;
    std::uint64_t eventIndex = 0u;
    // Around walls within this many metres of the trigger; 0 = straight.
    std::uint32_t reach = 0u;
};

std::string TargetName(const DistanceSettings &settings) {
    std::string name = settings.finish
            ? std::string("the finish")
            : "checkpoint " + std::to_string(std::uint64_t{settings.checkpointIndex} + 1u);
    name += " of lap " + std::to_string(settings.lap);
    if (settings.checkpointSlot) {
        name += " (map slot " + std::to_string(*settings.checkpointSlot) + ")";
    }
    return name;
}

bool SlotPassed(const sandbox::PhysicsSandboxStateView &state,
                std::uint32_t slot) {
    return slot / 32u < state.passedCheckpointSlots.size() &&
           (state.passedCheckpointSlots[slot / 32u] >> (slot % 32u) & 1u) != 0u;
}

// Checkpoints (and finishes) still to come before the target can be
// reached: a run nearer in the race ranks first, then by distance.
std::uint32_t EventsBeforeTarget(const DistanceSettings &settings,
                                 const sandbox::PhysicsSandboxStateView &state) {
    const std::uint64_t perLap = std::uint64_t{state.checkpointsTotal} + 1u;
    const std::uint64_t target = (std::uint64_t{settings.lap} - 1u) * perLap +
            (settings.finish ? state.checkpointsTotal : settings.checkpointIndex);
    const std::uint64_t done = std::uint64_t{state.completedLaps} * perLap +
            state.currentLapCheckpoints;
    return target > done ? static_cast<std::uint32_t>(target - done) : 0u;
}

class DistanceSession final : public IterationEvaluationSession {
public:
    DistanceSession(DistanceSettings settings,
                    std::shared_ptr<const Geometry> geometry)
        : settings_(settings), geometry_(std::move(geometry)) {}

    std::unique_ptr<IterationEvaluationSession> Clone() const override {
        return std::make_unique<DistanceSession>(*this);
    }

    bool IsComplete() const override { return reached_; }

    std::optional<EvaluationSample> Observe(
            const std::optional<sandbox::PhysicsSandboxStateView> &,
            const sandbox::PhysicsSandboxStateView &current) override {
        if (reached_) return std::nullopt;
        if (const auto reachMs = ReachTime(current)) {
            reached_ = true;
            return EvaluationSample{
                    *reachMs, *reachMs,
                    "Reached " + TargetName(settings_) + " at " +
                            FormatRaceTimeMilliseconds(*reachMs)};
        }
        if (!geometry_) return std::nullopt;
        const Vec position = Of(current.car.position);
        const Quat rotation{current.car.rotationX, current.car.rotationY,
                            current.car.rotationZ, current.car.rotationW};
        // Any map slot: the nearest checkpoint not taken yet this lap (all of
        // them once every one is taken).
        bool anyOpen = false;
        for (const TargetSurface &target : geometry_->targets) {
            anyOpen |= !target.slot || !SlotPassed(current, *target.slot);
        }
        // Straight distances first: the way around walls is never shorter,
        // so only targets straight-nearer than the best way so far are
        // measured around walls (and have their graph built).
        thread_local std::vector<std::pair<double, std::size_t>> straight;
        straight.clear();
        for (std::size_t i = 0; i < geometry_->targets.size(); ++i) {
            const TargetSurface &target = geometry_->targets[i];
            if (anyOpen && target.slot && !settings_.checkpointSlot &&
                SlotPassed(current, *target.slot)) {
                continue;
            }
            straight.push_back({DistanceToSurface(geometry_->car, target.surface,
                                                  position, rotation),
                                i});
        }
        double distance = std::numeric_limits<double>::max();
        if (settings_.reach == 0u) {
            for (const auto &[length, index] : straight) distance = std::min(distance, length);
        } else {
            std::sort(straight.begin(), straight.end());
            const Vec body = position + Rotate(rotation, geometry_->car.bodyCenter);
            memories_.resize(geometry_->targets.size());
            for (const auto &[length, index] : straight) {
                if (length >= distance) break;
                const auto path = geometry_->Route(geometry_->targets[index])
                                          .PathTo({static_cast<float>(body.x),
                                                   static_cast<float>(body.y),
                                                   static_cast<float>(body.z)},
                                                  memories_[index]);
                // Near a trigger in plain sight, the exact distance.
                if (path.inSight && length <= kExactRange) {
                    distance = std::min(distance, length);
                    continue;
                }
                // The path starts at the sphere; the car's body reaches
                // farther towards where it first bends (or the trigger).
                const Vec toward = Of(path.via) - body;
                const double span = std::sqrt(Dot(toward, toward));
                const double reach = span > 1e-9
                        ? Reach(geometry_->car, rotation, toward * (1.0 / span))
                        : geometry_->car.bodyRadius;
                const double around = path.length -
                        std::min(span, std::max(0.0, reach - geometry_->car.bodyRadius));
                distance = std::min(distance, std::max(length, around));
            }
        }
        distance = std::max(0.0, distance);
        const std::uint32_t before = EventsBeforeTarget(settings_, current);
        const double score = kNotReached + before * kPerEvent + distance;
        // Only the best moment can be the candidate's sample.
        if (score >= best_) return std::nullopt;
        best_ = score;
        const double time = static_cast<double>(current.timeMs);
        std::ostringstream description;
        description.precision(6);
        description << (settings_.reach != 0u ? "Distance around walls to "
                                              : "Distance to ")
                    << TargetName(settings_) << ": "
                    << distance << " m";
        if (before > 1u) {
            description << " (" << before - 1u << " checkpoints before it)";
        }
        description << " at " << FormatRaceTimeMilliseconds(time);
        return EvaluationSample{score, time, description.str()};
    }

private:
    // As the Checkpoint time target selects events; the finish takes its
    // precise time.
    std::optional<double> ReachTime(
            const sandbox::PhysicsSandboxStateView &current) const {
        for (const auto &event : current.acceptedCheckpointEvents) {
            if (event.tick != current.tick || event.timeMs != current.timeMs ||
                event.finish != settings_.finish || event.lap != settings_.lap ||
                (!settings_.finish &&
                 event.checkpointIndex != settings_.checkpointIndex) ||
                (settings_.checkpointSlot &&
                 event.checkpointSlot != *settings_.checkpointSlot) ||
                (settings_.eventIndex != 0u &&
                 event.eventIndex != settings_.eventIndex)) {
                continue;
            }
            if (event.finish && current.finishTime &&
                current.finishTime->IsValid()) {
                return static_cast<double>(current.finishTime->upperBoundNs) /
                       1000000.0;
            }
            return static_cast<double>(event.timeMs);
        }
        return std::nullopt;
    }

    DistanceSettings settings_;
    std::shared_ptr<const Geometry> geometry_;
    // Around walls: what each target's last query leaves the next.
    std::vector<AroundWallsDistance::Memory> memories_;
    double best_ = std::numeric_limits<double>::infinity();
    bool reached_ = false;
};

class DistanceEvaluator final : public IterationEvaluator {
public:
    explicit DistanceEvaluator(DistanceSettings settings)
        : settings_(settings) {}

    void BindScene(const sandbox::PhysicsSandbox &scene) override {
        auto view = scene.ReadScene();
        if (!view) {
            throw std::runtime_error("the map's triggers could not be read");
        }
        auto geometry = std::make_shared<Geometry>();
        geometry->car = MakeCarShape(view.Value().carEllipsoids);
        for (const auto &trigger : view.Value().checkpointTriggers) {
            const bool finishLine =
                    trigger.role !=
                    sandbox::PhysicsSandboxCheckpointTriggerRole::Checkpoint;
            if (settings_.finish != finishLine ||
                (settings_.checkpointSlot &&
                 trigger.checkpointSlot != *settings_.checkpointSlot)) {
                continue;
            }
            TargetSurface target;
            target.slot = finishLine ? std::nullopt
                                     : std::optional<std::uint32_t>(trigger.checkpointSlot);
            target.surface = MakeSurface(trigger.triangles);
            if (settings_.reach != 0u) target.triangles = trigger.triangles;
            geometry->targets.push_back(std::move(target));
        }
        if (settings_.reach != 0u) {
            geometry->reach = settings_.reach;
            geometry->walls = view.Value().collisionTriangles;
            for (const auto &trigger : view.Value().checkpointTriggers) {
                geometry->triggers.insert(geometry->triggers.end(),
                                          trigger.triangles.begin(),
                                          trigger.triangles.end());
            }
        }
        if (geometry->targets.empty()) {
            throw std::invalid_argument(
                    "this map has no " + TargetName(settings_));
        }
        geometry_ = std::move(geometry);
    }

    void CompleteGpuEvaluator(sandbox::PhysicsSandboxCudaEvaluator &gpu) const override {
        auto *distance = std::get_if<sandbox::PhysicsSandboxCudaDistanceEvaluator>(&gpu);
        if (distance == nullptr || settings_.reach == 0u || !geometry_) return;
        // One table per target trigger, in the map's order, as the GPU
        // lists them.
        distance->aroundWalls.clear();
        for (const TargetSurface &target : geometry_->targets) {
            distance->aroundWalls.push_back(geometry_->Route(target).GpuTable());
        }
    }

    EvaluationPlan Plan(std::int64_t simulationHorizonMs,
                        std::int64_t earliestMutationTimeMs,
                        std::uint32_t tickDurationMs) const override {
        static_cast<void>(tickDurationMs);
        static_cast<void>(simulationHorizonMs);
        return {std::max(settings_.minimumTimeMs, earliestMutationTimeMs),
                settings_.maximumTimeMs};
    }

    std::unique_ptr<IterationEvaluationSession> CreateSession()
            const override {
        return std::make_unique<DistanceSession>(settings_, geometry_);
    }

    bool IsBetter(const EvaluationSample &iteration,
                  const EvaluationSample &incumbent) const override {
        return iteration.score < incumbent.score;
    }

    // Once a run reaches the target, later reaches cannot beat it.
    std::optional<double> LatestImprovingTimeMs(
            const EvaluationSample &incumbent) const override {
        if (incumbent.score >= kNotReached) return std::nullopt;
        return incumbent.timeMs;
    }

private:
    DistanceSettings settings_;
    std::shared_ptr<const Geometry> geometry_;
};

std::optional<DistanceSettings> ParseSettings(const OptionSettings &settings) {
    const auto minimum = ReadTimeSetting(settings, "minTimeMs");
    const auto maximum = ReadTimeSetting(settings, "maxTimeMs");
    const std::string &type = settings.at("eventType");
    const auto ordinal = ParseUnsignedDecimal32(settings.at("checkpointIndex"));
    const auto lap = ParseUnsignedDecimal32(settings.at("lap"));
    const std::string &slotText = settings.at("checkpointSlot");
    const auto slot = slotText == "-1" ? std::optional<std::uint32_t>()
                                       : ParseUnsignedDecimal32(slotText);
    const auto eventIndex = ParseUnsignedDecimal64(settings.at("eventIndex"));
    const std::string &route = settings.at("route");
    const auto reach = ParseUnsignedDecimal32(settings.at("reach"));
    if (!minimum || !maximum || !ordinal || *ordinal == 0u || !lap ||
        *lap == 0u || (slotText != "-1" && !slot) || !eventIndex ||
        (type != "finish" && type != "checkpoint") ||
        (route != "straight" && route != "around") || !reach ||
        *reach == 0u || *reach > kMaximumReach) {
        return std::nullopt;
    }
    return DistanceSettings{*minimum, *maximum, type == "finish",
                            *ordinal - 1u, *lap, slot, *eventIndex,
                            route == "around" ? *reach : 0u};
}

}  // namespace

double CarDistanceToTriangles(
        const std::vector<sandbox::PhysicsSandboxEllipsoid> &carEllipsoids,
        const std::vector<sandbox::PhysicsSandboxCollisionTriangle> &triangles,
        const sandbox::PhysicsSandboxCarState &car) {
    return std::max(0.0, DistanceToSurface(MakeCarShape(carEllipsoids),
                                           MakeSurface(triangles),
                                           Of(car.position),
                                           {car.rotationX, car.rotationY,
                                            car.rotationZ, car.rotationW}));
}

OptionSettings DefaultDistanceTargetOptionSettings() {
    return {{"minTimeMs", "1000"},
            {"maxTimeMs", "6000"},
            {"eventType", "checkpoint"},
            {"checkpointIndex", "1"},
            {"lap", "1"},
            {"checkpointSlot", "-1"},
            {"eventIndex", "0"},
            {"route", "straight"},
            {"reach", "32"}};
}

std::optional<std::string> ValidateDistanceTargetOptionSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) {
    if (const auto keyError = ValidateOptionSettingKeys(
                settings, DefaultDistanceTargetOptionSettings())) {
        return keyError;
    }
    const auto parsed = ParseSettings(settings);
    if (!parsed) {
        return kUnreadableSetting;
    }
    return ValidateTimeWindow(parsed->minimumTimeMs,
                              parsed->maximumTimeMs,
                              tickDurationMs,
                              "the minimum time",
                              true);
}

std::unique_ptr<IterationEvaluator> CreateDistanceTargetEvaluator(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) {
    if (const auto error = ValidateDistanceTargetOptionSettings(
                settings, tickDurationMs)) {
        throw std::invalid_argument(*error);
    }
    return std::make_unique<DistanceEvaluator>(*ParseSettings(settings));
}

}  // namespace forevertas
