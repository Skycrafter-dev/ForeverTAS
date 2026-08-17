#include "evaluators/custom_volume_entry_evaluator.h"

#include "evaluators/evaluator_utils.h"
#include "searches/option_settings_utils.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace forevertas {
namespace {

constexpr double kMaximumCoordinate = 10000000.0;

struct Point2 {
    double x = 0.0;
    double y = 0.0;
};

enum class Plane {
    XY,
    XZ,
    YZ,
};

struct Prism {
    Plane plane = Plane::XZ;
    EvaluationVector3 origin;
    double depth = 1.0;
    std::vector<Point2> polygon;
};

struct ProjectedPoint {
    Point2 plane;
    double normal = 0.0;
};

double Cross(const Point2 &left, const Point2 &right) {
    return left.x * right.y - left.y * right.x;
}

Point2 Subtract(const Point2 &left, const Point2 &right) {
    return {left.x - right.x, left.y - right.y};
}

bool OnSegment(const Point2 &point,
               const Point2 &from,
               const Point2 &to) {
    constexpr double tolerance = 1e-9;
    if (std::abs(Cross(Subtract(point, from), Subtract(to, from))) >
        tolerance) {
        return false;
    }
    return point.x >= std::min(from.x, to.x) - tolerance &&
            point.x <= std::max(from.x, to.x) + tolerance &&
            point.y >= std::min(from.y, to.y) - tolerance &&
            point.y <= std::max(from.y, to.y) + tolerance;
}

bool Contains2D(const std::vector<Point2> &polygon,
                const Point2 &point) {
    bool inside = false;
    for (std::size_t index = 0u, previous = polygon.size() - 1u;
         index < polygon.size();
         previous = index++) {
        const Point2 &a = polygon[previous];
        const Point2 &b = polygon[index];
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

ProjectedPoint Project(const Prism &prism,
                       const EvaluationVector3 &point) {
    switch (prism.plane) {
    case Plane::XY:
        return {{point.x - prism.origin.x, point.y - prism.origin.y},
                point.z - prism.origin.z};
    case Plane::XZ:
        return {{point.x - prism.origin.x, point.z - prism.origin.z},
                point.y - prism.origin.y};
    case Plane::YZ:
        return {{point.y - prism.origin.y, point.z - prism.origin.z},
                point.x - prism.origin.x};
    }
    return {};
}

bool Contains(const Prism &prism, const EvaluationVector3 &point) {
    const ProjectedPoint projected = Project(prism, point);
    return projected.normal >= 0.0 && projected.normal <= prism.depth &&
            Contains2D(prism.polygon, projected.plane);
}

void AddCandidate(std::vector<double> *candidates, double value) {
    constexpr double tolerance = 1e-9;
    if (value < -tolerance || value > 1.0 + tolerance) return;
    candidates->push_back(std::clamp(value, 0.0, 1.0));
}

std::optional<double> SegmentEntryFraction(
        const Prism &prism,
        const EvaluationVector3 &from,
        const EvaluationVector3 &to) {
    const ProjectedPoint a = Project(prism, from);
    const ProjectedPoint b = Project(prism, to);
    std::vector<double> candidates{0.0, 1.0};

    const double normalDelta = b.normal - a.normal;
    if (std::abs(normalDelta) > 1e-12) {
        AddCandidate(&candidates, -a.normal / normalDelta);
        AddCandidate(
                &candidates, (prism.depth - a.normal) / normalDelta);
    }

    const Point2 path = Subtract(b.plane, a.plane);
    for (std::size_t index = 0u; index < prism.polygon.size(); ++index) {
        const Point2 edgeStart = prism.polygon[index];
        const Point2 edgeEnd =
                prism.polygon[(index + 1u) % prism.polygon.size()];
        const Point2 edge = Subtract(edgeEnd, edgeStart);
        const double denominator = Cross(path, edge);
        if (std::abs(denominator) <= 1e-12) continue;
        const Point2 offset = Subtract(edgeStart, a.plane);
        const double pathFraction = Cross(offset, edge) / denominator;
        const double edgeFraction = Cross(offset, path) / denominator;
        if (edgeFraction >= -1e-9 && edgeFraction <= 1.0 + 1e-9) {
            AddCandidate(&candidates, pathFraction);
        }
    }

    std::sort(candidates.begin(), candidates.end());
    candidates.erase(
            std::unique(
                    candidates.begin(),
                    candidates.end(),
                    [](double left, double right) {
                        return std::abs(left - right) <= 1e-9;
                    }),
            candidates.end());
    for (std::size_t index = 0u; index < candidates.size(); ++index) {
        const double candidate = candidates[index];
        const EvaluationVector3 point{
                from.x + (to.x - from.x) * candidate,
                from.y + (to.y - from.y) * candidate,
                from.z + (to.z - from.z) * candidate};
        if (Contains(prism, point)) return candidate;
        if (index + 1u >= candidates.size()) continue;
        const double midpoint = (candidate + candidates[index + 1u]) * 0.5;
        const EvaluationVector3 middle{
                from.x + (to.x - from.x) * midpoint,
                from.y + (to.y - from.y) * midpoint,
                from.z + (to.z - from.z) * midpoint};
        if (Contains(prism, middle)) return candidate;
    }
    return std::nullopt;
}

bool ParseNumber(std::string_view text, double *value) {
    if (text.empty()) return false;
    const char *const begin = text.data();
    const char *const end = begin + text.size();
    const auto result = std::from_chars(begin, end, *value);
    return result.ec == std::errc{} && result.ptr == end &&
            std::isfinite(*value) &&
            std::abs(*value) <= kMaximumCoordinate;
}

std::optional<std::vector<Point2>> ParsePolygon(
        const std::string &encoded) {
    std::vector<Point2> polygon;
    std::size_t position = 0u;
    while (position < encoded.size()) {
        const std::size_t comma = encoded.find(',', position);
        const std::size_t semicolon = encoded.find(';', position);
        const std::size_t end = semicolon == std::string::npos
                ? encoded.size()
                : semicolon;
        if (comma == std::string::npos || comma >= end) return std::nullopt;
        Point2 point;
        if (!ParseNumber(
                    std::string_view(encoded).substr(
                            position, comma - position),
                    &point.x) ||
            !ParseNumber(
                    std::string_view(encoded).substr(
                            comma + 1u, end - comma - 1u),
                    &point.y)) {
            return std::nullopt;
        }
        polygon.push_back(point);
        position = end + 1u;
    }
    if (polygon.size() < 3u || polygon.size() > 256u) return std::nullopt;
    return polygon;
}

int Orientation(const Point2 &a, const Point2 &b, const Point2 &c) {
    const double value = Cross(Subtract(b, a), Subtract(c, a));
    if (std::abs(value) <= 1e-9) return 0;
    return value > 0.0 ? 1 : -1;
}

bool ProperlyIntersects(const Point2 &a,
                        const Point2 &b,
                        const Point2 &c,
                        const Point2 &d) {
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

bool IsSimplePolygon(const std::vector<Point2> &polygon) {
    double areaTwice = 0.0;
    for (std::size_t index = 0u; index < polygon.size(); ++index) {
        const Point2 &a = polygon[index];
        const Point2 &b = polygon[(index + 1u) % polygon.size()];
        const Point2 &previous = polygon[
                (index + polygon.size() - 1u) % polygon.size()];
        areaTwice += Cross(a, b);
        if (std::hypot(b.x - a.x, b.y - a.y) <= 1e-9) return false;
        if (std::abs(Cross(Subtract(a, previous), Subtract(b, a))) <=
            1e-9) {
            return false;
        }
        for (std::size_t other = index + 1u;
             other < polygon.size();
             ++other) {
            if (other == index ||
                other == (index + 1u) % polygon.size() ||
                (other + 1u) % polygon.size() == index) {
                continue;
            }
            if (ProperlyIntersects(
                        a,
                        b,
                        polygon[other],
                        polygon[(other + 1u) % polygon.size()])) {
                return false;
            }
        }
    }
    return std::abs(areaTwice) > 1e-8;
}

std::optional<Prism> ParsePrism(const OptionSettings &settings) {
    const auto origin = ReadVector3Settings(
            settings, "originX", "originY", "originZ");
    const auto depthSetting = settings.find("depth");
    const auto polygonSetting = settings.find("polygon");
    const auto planeSetting = settings.find("plane");
    if (!origin || depthSetting == settings.end() ||
        polygonSetting == settings.end() || planeSetting == settings.end()) {
        return std::nullopt;
    }
    const auto depth = ParseFiniteDouble(depthSetting->second);
    if (!depth || *depth <= 0.0 ||
        *depth > kMaximumCoordinate ||
        std::abs(origin->x) > kMaximumCoordinate ||
        std::abs(origin->y) > kMaximumCoordinate ||
        std::abs(origin->z) > kMaximumCoordinate) {
        return std::nullopt;
    }
    const auto polygon = ParsePolygon(polygonSetting->second);
    if (!polygon || !IsSimplePolygon(*polygon)) return std::nullopt;
    Plane plane;
    if (planeSetting->second == "xy") {
        plane = Plane::XY;
    } else if (planeSetting->second == "xz") {
        plane = Plane::XZ;
    } else if (planeSetting->second == "yz") {
        plane = Plane::YZ;
    } else {
        return std::nullopt;
    }
    return Prism{plane, *origin, *depth, *polygon};
}

class CustomVolumeEntrySession final
    : public IterationEvaluationSession {
public:
    explicit CustomVolumeEntrySession(Prism prism)
        : prism_(std::move(prism)) {}

    std::optional<EvaluationSample> Observe(
            const std::optional<
                    forevervalidator::experimental::PhysicsSandboxStateView>
                    &previous,
            const forevervalidator::experimental::PhysicsSandboxStateView
                    &current) override {
        if (reported_) return std::nullopt;
        const EvaluationVector3 currentPosition = PositionOf(current);
        if (!previous) {
            if (!Contains(prism_, currentPosition)) return std::nullopt;
            reported_ = true;
            const double time = static_cast<double>(current.timeMs);
            return EvaluationSample{
                    time,
                    time,
                    TimeMetricDescription(
                            "Custom volume entry time", time)};
        }
        const EvaluationVector3 previousPosition = PositionOf(*previous);
        if (Contains(prism_, previousPosition)) return std::nullopt;
        const auto fraction = SegmentEntryFraction(
                prism_, previousPosition, currentPosition);
        if (!fraction) return std::nullopt;
        reported_ = true;
        const double time = static_cast<double>(previous->timeMs) +
                *fraction * static_cast<double>(
                        current.timeMs - previous->timeMs);
        return EvaluationSample{
                time,
                time,
                TimeMetricDescription("Custom volume entry time", time)};
    }

private:
    Prism prism_;
    bool reported_ = false;
};

class CustomVolumeEntryEvaluator final : public IterationEvaluator {
public:
    explicit CustomVolumeEntryEvaluator(Prism prism)
        : prism_(std::move(prism)) {}

    EvaluationPlan Plan(std::int64_t simulationHorizonMs,
                        std::int64_t earliestMutationTimeMs,
                        std::uint32_t tickDurationMs) const override {
        static_cast<void>(tickDurationMs);
        return {earliestMutationTimeMs, simulationHorizonMs};
    }

    std::unique_ptr<IterationEvaluationSession> CreateSession()
            const override {
        return std::make_unique<CustomVolumeEntrySession>(prism_);
    }

    bool IsBetter(const EvaluationSample &iteration,
                  const EvaluationSample &incumbent) const override {
        return iteration.score < incumbent.score;
    }

private:
    Prism prism_;
};

}  // namespace

OptionFieldList CustomVolumeEntryOptionFields() {
    return {
            MirroredField("plane", "custom-volume", "xz"),
            MirroredField("originX", "custom-volume", "0"),
            MirroredField("originY", "custom-volume", "0"),
            MirroredField("originZ", "custom-volume", "0"),
            MirroredField("depth", "custom-volume", "5"),
            MirroredField("polygon", "custom-volume", "-5,-5;5,-5;0,5")};
}

OptionSettings DefaultCustomVolumeEntryOptionSettings() {
    return {{"plane", "xz"},
            {"originX", "0"},
            {"originY", "0"},
            {"originZ", "0"},
            {"depth", "5"},
            {"polygon", "-5,-5;5,-5;0,5"}};
}

std::optional<std::string> ValidateCustomVolumeEntryOptionSettings(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) {
    static_cast<void>(tickDurationMs);
    if (const auto error = ValidateOptionSettingKeys(
                settings, DefaultCustomVolumeEntryOptionSettings())) {
        return error;
    }
    if (!ParsePrism(settings)) {
        return "custom volume requires a simple polygon, a valid plane, "
               "finite origin, and positive extrusion depth";
    }
    return std::nullopt;
}

std::unique_ptr<IterationEvaluator> CreateCustomVolumeEntryEvaluator(
        const OptionSettings &settings,
        std::uint32_t tickDurationMs) {
    if (const auto error = ValidateCustomVolumeEntryOptionSettings(
                settings, tickDurationMs)) {
        throw std::invalid_argument(*error);
    }
    return std::make_unique<CustomVolumeEntryEvaluator>(
            *ParsePrism(settings));
}

}  // namespace forevertas
