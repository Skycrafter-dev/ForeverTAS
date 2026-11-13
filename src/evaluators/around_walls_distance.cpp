#include "evaluators/around_walls_distance.h"

#include <forevervalidator/experimental/around_walls_table.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace forevertas {
namespace {

namespace sandbox = forevervalidator::experimental;

constexpr double kInfinity = std::numeric_limits<double>::infinity();
// EPlugSurfaceMaterialId_NotCollidable: decoration the car passes through.
constexpr std::uint8_t kNotCollidable = 28u;
// Nodes sit this far beyond the sphere's radius from the edge they bend
// around, and need at least kClearanceMargin of room to every wall.
constexpr double kNodeOffsetMargin = 0.05;
constexpr double kClearanceMargin = 0.01;
// Nodes along an edge at most this far apart, at most one per voxel.
constexpr double kNodeSpacing = 2.0;
constexpr double kNodeVoxel = 2.0;
// Trigger sample spacing, and the room a sample needs, as a share of the
// radius: a car on the road has its sphere's centre about one radius up.
constexpr double kTriggerSpacing = 1.0;
constexpr double kSampleClearance = 0.75;
// A segment that only grazes a wall within this of its ends does not cross
// it: a trigger resting on the road ends exactly on the road's surface.
constexpr double kCrossingTolerance = 0.01;
// How much nearer than at its ends a sweep must come to a wall to be
// stopped by it.
constexpr double kNearerOnTheWay = 1e-6;

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
double Length(Vec a) { return std::sqrt(Dot(a, a)); }
double Component(Vec a, int axis) {
    return axis == 0 ? a.x : axis == 1 ? a.y : a.z;
}

std::optional<Vec> Normalized(Vec a) {
    const double length = Length(a);
    if (length < 1e-9) return std::nullopt;
    return a * (1.0 / length);
}

Vec Of(const forevervalidator::Vector3 &v) { return {v.x, v.y, v.z}; }

struct Triangle {
    Vec a;
    Vec b;
    Vec c;
};

struct Box {
    Vec minimum{kInfinity, kInfinity, kInfinity};
    Vec maximum{-kInfinity, -kInfinity, -kInfinity};

    void Add(Vec v) {
        minimum = {std::min(minimum.x, v.x), std::min(minimum.y, v.y),
                   std::min(minimum.z, v.z)};
        maximum = {std::max(maximum.x, v.x), std::max(maximum.y, v.y),
                   std::max(maximum.z, v.z)};
    }
    void Add(const Box &box) {
        Add(box.minimum);
        Add(box.maximum);
    }
    Box Grown(double amount) const {
        return {minimum - Vec{amount, amount, amount},
                maximum + Vec{amount, amount, amount}};
    }
    bool Contains(Vec p) const {
        return p.x >= minimum.x && p.x <= maximum.x && p.y >= minimum.y &&
               p.y <= maximum.y && p.z >= minimum.z && p.z <= maximum.z;
    }
    bool Overlaps(const Box &box) const {
        return minimum.x <= box.maximum.x && box.minimum.x <= maximum.x &&
               minimum.y <= box.maximum.y && box.minimum.y <= maximum.y &&
               minimum.z <= box.maximum.z && box.minimum.z <= maximum.z;
    }
    double DistanceTo(Vec p) const {
        const double dx = std::max({minimum.x - p.x, 0.0, p.x - maximum.x});
        const double dy = std::max({minimum.y - p.y, 0.0, p.y - maximum.y});
        const double dz = std::max({minimum.z - p.z, 0.0, p.z - maximum.z});
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    // Where a ray from origin along direction enters the box, if before
    // limit (slab test).
    std::optional<double> RayEnter(Vec origin, Vec direction, double limit) const {
        double enter = 0.0;
        double leave = limit;
        for (int axis = 0; axis < 3; ++axis) {
            const double start = Component(origin, axis);
            const double step = Component(direction, axis);
            const double low = Component(minimum, axis);
            const double high = Component(maximum, axis);
            if (std::fabs(step) < 1e-12) {
                if (start < low || start > high) return std::nullopt;
                continue;
            }
            double t0 = (low - start) / step;
            double t1 = (high - start) / step;
            if (t0 > t1) std::swap(t0, t1);
            enter = std::max(enter, t0);
            leave = std::min(leave, t1);
            if (enter > leave) return std::nullopt;
        }
        return enter;
    }

    // Whether a segment from p along step (q - p), with inverse step
    // components, passes through the box grown by `grow` (slab test).
    bool Segment(Vec p, Vec step, Vec inverse, double grow) const {
        double enter = 0.0;
        double leave = 1.0;
        for (int axis = 0; axis < 3; ++axis) {
            const double start = Component(p, axis);
            const double low = Component(minimum, axis) - grow;
            const double high = Component(maximum, axis) + grow;
            if (std::fabs(Component(step, axis)) < 1e-12) {
                if (start < low || start > high) return false;
                continue;
            }
            const double scale = Component(inverse, axis);
            double t0 = (low - start) * scale;
            double t1 = (high - start) * scale;
            if (t0 > t1) std::swap(t0, t1);
            enter = std::max(enter, t0);
            leave = std::min(leave, t1);
            if (enter > leave) return false;
        }
        return true;
    }
};

Box BoxOf(const Triangle &t) {
    Box box;
    box.Add(t.a);
    box.Add(t.b);
    box.Add(t.c);
    return box;
}

// Closest point of triangle abc to p (Ericson, Real-Time Collision
// Detection, 5.1.5).
Vec ClosestOnTriangle(Vec p, const Triangle &t) {
    const Vec ab = t.b - t.a;
    const Vec ac = t.c - t.a;
    const Vec ap = p - t.a;
    const double d1 = Dot(ab, ap);
    const double d2 = Dot(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0) return t.a;
    const Vec bp = p - t.b;
    const double d3 = Dot(ab, bp);
    const double d4 = Dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3) return t.b;
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
        return t.a + ab * (d1 / (d1 - d3));
    }
    const Vec cp = p - t.c;
    const double d5 = Dot(ab, cp);
    const double d6 = Dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6) return t.c;
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
        return t.a + ac * (d2 / (d2 - d6));
    }
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
        return t.b + (t.c - t.b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    }
    const double denominator = 1.0 / (va + vb + vc);
    return t.a + ab * (vb * denominator) + ac * (vc * denominator);
}

double PointTriangleDistance(Vec p, const Triangle &t) {
    return Length(p - ClosestOnTriangle(p, t));
}

// Distance between segments p1q1 and p2q2 (Ericson 5.1.9).
double SegmentSegmentDistance(Vec p1, Vec q1, Vec p2, Vec q2) {
    const Vec d1 = q1 - p1;
    const Vec d2 = q2 - p2;
    const Vec r = p1 - p2;
    const double a = Dot(d1, d1);
    const double e = Dot(d2, d2);
    const double f = Dot(d2, r);
    double s = 0.0;
    double t = 0.0;
    if (a <= 1e-12 && e <= 1e-12) return Length(r);
    if (a <= 1e-12) {
        t = std::clamp(f / e, 0.0, 1.0);
    } else {
        const double c = Dot(d1, r);
        if (e <= 1e-12) {
            s = std::clamp(-c / a, 0.0, 1.0);
        } else {
            const double b = Dot(d1, d2);
            const double denominator = a * e - b * b;
            s = denominator > 1e-12
                    ? std::clamp((b * f - c * e) / denominator, 0.0, 1.0)
                    : 0.0;
            t = (b * s + f) / e;
            if (t < 0.0) {
                t = 0.0;
                s = std::clamp(-c / a, 0.0, 1.0);
            } else if (t > 1.0) {
                t = 1.0;
                s = std::clamp((b - c) / a, 0.0, 1.0);
            }
        }
    }
    return Length((p1 + d1 * s) - (p2 + d2 * t));
}

// Whether segment pq crosses the triangle away from its ends
// (Moller-Trumbore).
bool Crosses(Vec p, Vec q, const Triangle &t) {
    const Vec direction = q - p;
    const double length = Length(direction);
    if (length <= 2.0 * kCrossingTolerance) return false;
    const Vec e1 = t.b - t.a;
    const Vec e2 = t.c - t.a;
    const Vec h = Cross(direction, e2);
    const double a = Dot(e1, h);
    if (std::fabs(a) < 1e-12) return false;
    const double f = 1.0 / a;
    const Vec s = p - t.a;
    const double u = f * Dot(s, h);
    if (u < 0.0 || u > 1.0) return false;
    const Vec qv = Cross(s, e1);
    const double v = f * Dot(direction, qv);
    if (v < 0.0 || u + v > 1.0) return false;
    const double along = f * Dot(e2, qv);
    const double margin = kCrossingTolerance / length;
    return along > margin && along < 1.0 - margin;
}

// How far along the unit direction a ray from origin meets the triangle
// (Moller-Trumbore), or infinity.
double RayDistance(Vec origin, Vec direction, const Triangle &t) {
    const Vec e1 = t.b - t.a;
    const Vec e2 = t.c - t.a;
    const Vec h = Cross(direction, e2);
    const double a = Dot(e1, h);
    if (std::fabs(a) < 1e-12) return kInfinity;
    const double f = 1.0 / a;
    const Vec s = origin - t.a;
    const double u = f * Dot(s, h);
    if (u < 0.0 || u > 1.0) return kInfinity;
    const Vec q = Cross(s, e1);
    const double v = f * Dot(direction, q);
    if (v < 0.0 || u + v > 1.0) return kInfinity;
    const double along = f * Dot(e2, q);
    return along > 0.0 ? along : kInfinity;
}

// A triangle's plane (unit normal, none for a sliver), box and bounding
// sphere, for sweeps.
struct WallPlane {
    std::optional<Vec> normal;
    double offset = 0.0;
    Box box;
    Vec center;
    double bound = 0.0;
};

WallPlane PlaneOf(const Triangle &t) {
    WallPlane plane;
    plane.normal = Normalized(Cross(t.b - t.a, t.c - t.a));
    if (plane.normal) plane.offset = Dot(*plane.normal, t.a);
    plane.box = BoxOf(t);
    plane.center = (t.a + t.b + t.c) * (1.0 / 3.0);
    plane.bound = std::max({Length(t.a - plane.center), Length(t.b - plane.center),
                            Length(t.c - plane.center)});
    return plane;
}

double PointSegmentDistance(Vec point, Vec p, Vec q) {
    const Vec d = q - p;
    const double length = Dot(d, d);
    const double t = length > 1e-12 ? std::clamp(Dot(point - p, d) / length, 0.0, 1.0) : 0.0;
    return Length(point - (p + d * t));
}

// A sphere moving from p to q is stopped by a wall it crosses, or one it
// comes nearer than the radius to on the way, nearer than at either end:
// a car rolls along the road under it, whatever its height over it.
// `reach` is the segment's box grown by the radius.
bool Stops(Vec p, Vec q, double radius, const Box &reach, const Triangle &t,
           const WallPlane &plane) {
    if (!plane.box.Overlaps(reach)) return false;
    if (plane.normal) {
        // Both ends a radius or more off the same side of the plane.
        const double fromP = Dot(*plane.normal, p) - plane.offset;
        const double fromQ = Dot(*plane.normal, q) - plane.offset;
        if ((fromP >= radius && fromQ >= radius) ||
            (fromP <= -radius && fromQ <= -radius)) {
            return false;
        }
        if ((fromP > 0.0) != (fromQ > 0.0) && Crosses(p, q, t)) return true;
    } else if (Crosses(p, q, t)) {
        return true;
    }
    // Not crossing: the nearest points are an end against the triangle or
    // an edge against the segment; only an edge can be nearer on the way.
    if (PointSegmentDistance(plane.center, p, q) >= plane.bound + radius) return false;
    const double edges = std::min({SegmentSegmentDistance(p, q, t.a, t.b),
                                   SegmentSegmentDistance(p, q, t.b, t.c),
                                   SegmentSegmentDistance(p, q, t.c, t.a)});
    if (edges >= radius) return false;
    return edges < std::min(PointTriangleDistance(p, t), PointTriangleDistance(q, t)) -
                           kNearerOnTheWay;
}

// Bounding volume tree over the walls: nodes in depth-first order, a node's
// first child right after it.
class Walls {
public:
    explicit Walls(std::vector<Triangle> triangles)
        : triangles_(std::move(triangles)) {
        if (triangles_.empty()) return;
        std::vector<std::uint32_t> order(triangles_.size());
        for (std::uint32_t i = 0; i < order.size(); ++i) order[i] = i;
        Build(order, 0u, static_cast<std::uint32_t>(order.size()));
        std::vector<Triangle> sorted;
        sorted.reserve(order.size());
        for (const std::uint32_t index : order) {
            sorted.push_back(triangles_[index]);
            planes_.push_back(PlaneOf(triangles_[index]));
        }
        triangles_ = std::move(sorted);
    }

    const std::vector<Triangle> &triangles() const { return triangles_; }
    const std::vector<WallPlane> &planes() const { return planes_; }

    struct Node {
        Box box;
        std::uint32_t first = 0u;
        std::uint32_t count = 0u;  // leaf when non-zero
        std::uint32_t right = 0u;
    };
    const std::vector<Node> &tree() const { return nodes_; }

    bool Blocked(Vec p, Vec q, double radius) const {
        return Blocker(p, q, radius) != kNoWall;
    }

    static constexpr std::uint32_t kNoWall = 0xffffffffu;

    // Whether one wall stops a sphere moving from p to q.
    bool StoppedBy(std::uint32_t wall, Vec p, Vec q, double radius) const {
        Box reach;
        reach.Add(p);
        reach.Add(q);
        return Stops(p, q, radius, reach.Grown(radius), triangles_[wall], planes_[wall]);
    }

    // Whether a sphere moving from p to q is stopped, trying `last` (the
    // wall that stopped a nearby sweep, updated) before all of them.
    bool Blocked(Vec p, Vec q, double radius, std::uint32_t &last) const {
        if (last != kNoWall && StoppedBy(last, p, q, radius)) return true;
        const std::uint32_t wall = Blocker(p, q, radius);
        if (wall == kNoWall) return false;
        last = wall;
        return true;
    }

    // A wall that stops a sphere moving from p to q, or kNoWall.
    std::uint32_t Blocker(Vec p, Vec q, double radius) const {
        if (nodes_.empty()) return kNoWall;
        Box reach;
        reach.Add(p);
        reach.Add(q);
        reach = reach.Grown(radius);
        const Vec step = q - p;
        const auto invert = [](double v) { return std::fabs(v) < 1e-12 ? 0.0 : 1.0 / v; };
        const Vec inverse{invert(step.x), invert(step.y), invert(step.z)};
        std::uint32_t stack[64];
        int depth = 0;
        stack[depth++] = 0u;
        while (depth > 0) {
            const std::uint32_t index = stack[--depth];
            const Node &node = nodes_[index];
            if (!node.box.Segment(p, step, inverse, radius)) continue;
            if (node.count != 0u) {
                for (std::uint32_t i = node.first; i < node.first + node.count; ++i) {
                    if (Stops(p, q, radius, reach, triangles_[i], planes_[i])) return i;
                }
                continue;
            }
            stack[depth++] = node.right;
            stack[depth++] = index + 1u;
        }
        return kNoWall;
    }

    // Distance to the first wall along a ray (unit direction), or infinity.
    double RayHit(Vec origin, Vec direction) const {
        double best = kInfinity;
        if (nodes_.empty()) return best;
        std::uint32_t stack[64];
        int depth = 0;
        stack[depth++] = 0u;
        while (depth > 0) {
            const std::uint32_t index = stack[--depth];
            const Node &node = nodes_[index];
            if (!node.box.RayEnter(origin, direction, best)) continue;
            if (node.count != 0u) {
                for (std::uint32_t i = node.first; i < node.first + node.count; ++i) {
                    best = std::min(best, RayDistance(origin, direction, triangles_[i]));
                }
                continue;
            }
            // Nearer child first: its hit cuts the farther one short.
            const std::uint32_t left = index + 1u;
            const auto toLeft = nodes_[left].box.RayEnter(origin, direction, best);
            const auto toRight = nodes_[node.right].box.RayEnter(origin, direction, best);
            if (toLeft && toRight) {
                const bool leftFirst = *toLeft <= *toRight;
                stack[depth++] = leftFirst ? node.right : left;
                stack[depth++] = leftFirst ? left : node.right;
            } else if (toLeft) {
                stack[depth++] = left;
            } else if (toRight) {
                stack[depth++] = node.right;
            }
        }
        return best;
    }

    bool AnyWithin(Vec p, double distance) const {
        if (nodes_.empty()) return false;
        std::uint32_t stack[64];
        int depth = 0;
        stack[depth++] = 0u;
        while (depth > 0) {
            const Node &node = nodes_[stack[--depth]];
            if (node.box.DistanceTo(p) >= distance) continue;
            if (node.count != 0u) {
                for (std::uint32_t i = node.first; i < node.first + node.count; ++i) {
                    if (PointTriangleDistance(p, triangles_[i]) < distance) return true;
                }
                continue;
            }
            stack[depth++] = node.right;
            stack[depth++] = static_cast<std::uint32_t>(&node - nodes_.data()) + 1u;
        }
        return false;
    }

private:
    void Build(std::vector<std::uint32_t> &order, std::uint32_t first,
               std::uint32_t count) {
        const std::uint32_t index = static_cast<std::uint32_t>(nodes_.size());
        nodes_.push_back({});
        Box box;
        Box centers;
        for (std::uint32_t i = first; i < first + count; ++i) {
            const Triangle &t = triangles_[order[i]];
            box.Add(BoxOf(t));
            centers.Add((t.a + t.b + t.c) * (1.0 / 3.0));
        }
        nodes_[index].box = box;
        if (count <= 4u) {
            nodes_[index].first = first;
            nodes_[index].count = count;
            return;
        }
        const Vec extent = centers.maximum - centers.minimum;
        const int axis = extent.x >= extent.y && extent.x >= extent.z ? 0
                : extent.y >= extent.z ? 1 : 2;
        const std::uint32_t half = count / 2u;
        std::nth_element(
                order.begin() + first, order.begin() + first + half,
                order.begin() + first + count,
                [&](std::uint32_t left, std::uint32_t right) {
                    const Triangle &l = triangles_[left];
                    const Triangle &r = triangles_[right];
                    return Component(l.a + l.b + l.c, axis) <
                           Component(r.a + r.b + r.c, axis);
                });
        Build(order, first, half);
        nodes_[index].right = static_cast<std::uint32_t>(nodes_.size());
        Build(order, first + half, count - half);
    }

    std::vector<Triangle> triangles_;
    std::vector<WallPlane> planes_;
    std::vector<Node> nodes_;
};

// The part of a triangle inside the box, as a fan (Sutherland-Hodgman).
void ClipToBox(const Triangle &triangle, const Box &box,
               std::vector<Triangle> &out) {
    std::vector<Vec> polygon{triangle.a, triangle.b, triangle.c};
    std::vector<Vec> next;
    for (int axis = 0; axis < 3 && !polygon.empty(); ++axis) {
        for (const double sign : {1.0, -1.0}) {
            const double limit = sign > 0.0 ? Component(box.maximum, axis)
                                            : -Component(box.minimum, axis);
            const auto inside = [&](Vec v) {
                return sign * Component(v, axis) <= limit;
            };
            next.clear();
            for (std::size_t i = 0; i < polygon.size(); ++i) {
                const Vec current = polygon[i];
                const Vec following = polygon[(i + 1) % polygon.size()];
                if (inside(current)) next.push_back(current);
                if (inside(current) != inside(following)) {
                    const double from = sign * Component(current, axis);
                    const double to = sign * Component(following, axis);
                    next.push_back(current +
                                   (following - current) * ((limit - from) / (to - from)));
                }
            }
            polygon.swap(next);
            if (polygon.size() < 3u) return;
        }
    }
    for (std::size_t i = 1; i + 1 < polygon.size(); ++i) {
        const Triangle piece{polygon[0], polygon[i], polygon[i + 1]};
        if (Length(Cross(piece.b - piece.a, piece.c - piece.a)) > 1e-9) {
            out.push_back(piece);
        }
    }
}

struct VoxelKey {
    std::int64_t x;
    std::int64_t y;
    std::int64_t z;
    bool operator==(const VoxelKey &other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct VoxelHash {
    std::size_t operator()(const VoxelKey &k) const {
        return std::hash<std::int64_t>()(k.x * 73856093 ^ k.y * 19349663 ^
                                          k.z * 83492791);
    }
};

VoxelKey VoxelOf(Vec p, double size) {
    return {static_cast<std::int64_t>(std::floor(p.x / size)),
            static_cast<std::int64_t>(std::floor(p.y / size)),
            static_cast<std::int64_t>(std::floor(p.z / size))};
}

// A triangle's corners to the millimetre, in a fixed order.
struct TriangleKey {
    VoxelKey corners[3];
    bool operator==(const TriangleKey &other) const {
        return corners[0] == other.corners[0] && corners[1] == other.corners[1] &&
               corners[2] == other.corners[2];
    }
};

struct TriangleKeyHash {
    std::size_t operator()(const TriangleKey &k) const {
        const VoxelHash hash;
        return hash(k.corners[0]) * 31u + hash(k.corners[1]) * 17u + hash(k.corners[2]);
    }
};

TriangleKey KeyOf(const sandbox::PhysicsSandboxCollisionTriangle &t) {
    TriangleKey key{{VoxelOf(Of(t.a), 1e-3), VoxelOf(Of(t.b), 1e-3), VoxelOf(Of(t.c), 1e-3)}};
    std::sort(std::begin(key.corners), std::end(key.corners),
              [](const VoxelKey &l, const VoxelKey &r) {
                  return std::tie(l.x, l.y, l.z) < std::tie(r.x, r.y, r.z);
              });
    return key;
}

// Points just off every edge a path can bend around: off each face's side
// of the edge, in and out of the face's plane. Flat seams between two faces
// of one plane are skipped; points without room for the sphere are dropped.
std::vector<Vec> PlaceNodes(const Walls &walls, const Box &box, double radius) {
    const std::vector<Triangle> &triangles = walls.triangles();
    std::unordered_map<VoxelKey, std::uint32_t, VoxelHash> vertexIds;
    std::vector<Vec> vertices;
    const auto vertexId = [&](Vec v) {
        const auto [found, added] = vertexIds.try_emplace(
                VoxelOf(v, 1e-3), static_cast<std::uint32_t>(vertices.size()));
        if (added) vertices.push_back(v);
        return found->second;
    };
    // Edge (lower vertex id, higher) -> the faces' third vertices.
    std::unordered_map<std::uint64_t, std::vector<Vec>> edges;
    for (const Triangle &t : triangles) {
        const std::uint32_t ids[3] = {vertexId(t.a), vertexId(t.b), vertexId(t.c)};
        const Vec corners[3] = {t.a, t.b, t.c};
        for (int i = 0; i < 3; ++i) {
            const std::uint32_t from = ids[i];
            const std::uint32_t to = ids[(i + 1) % 3];
            if (from == to) continue;
            const std::uint64_t key = std::uint64_t{std::min(from, to)} << 32u |
                                      std::max(from, to);
            edges[key].push_back(corners[(i + 2) % 3]);
        }
    }
    std::unordered_set<VoxelKey, VoxelHash> taken;
    std::vector<Vec> nodes;
    const double offset = radius + kNodeOffsetMargin;
    std::vector<Vec> directions;
    for (const auto &[key, opposites] : edges) {
        const Vec start = vertices[key >> 32u];
        const Vec end = vertices[key & 0xffffffffu];
        const auto along = Normalized(end - start);
        if (!along) continue;
        directions.clear();
        std::vector<Vec> sides;
        for (const Vec opposite : opposites) {
            const Vec toward = opposite - start;
            const auto side = Normalized(toward - *along * Dot(toward, *along));
            if (!side) continue;
            sides.push_back(*side);
            const Vec normal = Cross(*along, *side);
            const Vec away = *side * -1.0;
            directions.push_back(away);
            directions.push_back(normal);
            directions.push_back(normal * -1.0);
            directions.push_back(*Normalized(away + normal));
            directions.push_back(*Normalized(away - normal));
        }
        if (sides.size() == 2u) {
            if (Dot(sides[0], sides[1]) < -0.9999) continue;
            if (const auto outside = Normalized((sides[0] + sides[1]) * -1.0)) {
                directions.push_back(*outside);
            }
        }
        const double length = Length(end - start);
        const int segments = std::max(1, static_cast<int>(std::ceil(length / kNodeSpacing)));
        for (int k = 0; k <= segments; ++k) {
            const Vec point = start + (end - start) * (static_cast<double>(k) / segments);
            for (const Vec direction : directions) {
                const Vec node = point + direction * offset;
                if (!box.Contains(node)) continue;
                const VoxelKey voxel = VoxelOf(node, kNodeVoxel);
                if (taken.count(voxel) != 0u) continue;
                if (walls.AnyWithin(node, radius + kClearanceMargin)) continue;
                taken.insert(voxel);
                nodes.push_back(node);
            }
        }
    }
    return nodes;
}

// Points on the triangles at most `spacing` apart (on a trigger, a sphere
// centred on one overlaps the trigger by its radius).
std::vector<Vec> SampleTrigger(const std::vector<Triangle> &trigger,
                               double spacing = kTriggerSpacing) {
    std::unordered_set<VoxelKey, VoxelHash> taken;
    std::vector<Vec> samples;
    for (const Triangle &t : trigger) {
        const double longest = std::max({Length(t.b - t.a), Length(t.c - t.b),
                                         Length(t.a - t.c)});
        const int steps = std::max(1, static_cast<int>(std::ceil(longest / spacing)));
        for (int i = 0; i <= steps; ++i) {
            for (int j = 0; i + j <= steps; ++j) {
                const Vec point = t.a + (t.b - t.a) * (static_cast<double>(i) / steps) +
                                  (t.c - t.a) * (static_cast<double>(j) / steps);
                if (taken.insert(VoxelOf(point, spacing * 0.5)).second) {
                    samples.push_back(point);
                }
            }
        }
    }
    return samples;
}

// Candidate nodes are precomputed at the corners of a grid this fine over
// the walls' box, and outside it at directions from the box's centre (a
// cube map this fine per face) times distances beyond the box. Each corner
// keeps its few best nodes; a query tries the ones of the corners around
// it, and searches every node only when none of those is in sight.
constexpr double kGridCell = 2.0;
constexpr int kOuterResolution = 24;
constexpr double kBands[] = {0.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0, 128.0,
                             256.0, 512.0, 1024.0, 2048.0, 4096.0};
constexpr int kBandCount = static_cast<int>(sizeof(kBands) / sizeof(kBands[0]));
constexpr int kCornerNodes = 3;
// A car drives in a thin layer over the road that corners above and below
// see differently from: more candidates come from points one radius over
// every floor (any surface facing up enough) within kFloorMargin of the
// walls' box, kGridCell apart, at most kFloorPoints of them per query.
constexpr double kFloorMargin = 64.0;
constexpr double kFloorFacing = 0.5;
constexpr int kFloorPoints = 8;
// With none of its candidates in sight, a query searches every node and
// gives up after this many sweeps (a point inside walls sees nothing).
constexpr int kFallbackSweeps = 48;
// A corner's search gives up after this many sweeps; corners without the
// room a trigger sample needs are skipped (inside walls they see nothing).
constexpr int kCornerSweeps = 512;

// How far a point sees in each direction: a cube map around it, each texel
// holding the farthest a ray gets before a wall among the directions
// through the texel's centre and corners. A point beyond that in the
// texel's directions is hidden; nearer ones still need a sphere sweep.
constexpr int kSightResolution = 4;
constexpr std::size_t kSightTexels = 6u * kSightResolution * kSightResolution;
constexpr double kSightUnit = 0.01;  // metres per step
constexpr std::uint16_t kSightOpen = 0xffffu;

Vec FaceDirection(int face, double u, double v) {
    const double sign = face % 2 == 0 ? 1.0 : -1.0;
    switch (face / 2) {
    case 0: return *Normalized({sign, u, v});
    case 1: return *Normalized({u, sign, v});
    default: return *Normalized({u, v, sign});
    }
}

std::size_t SightTexel(Vec d) {
    const double ax = std::fabs(d.x);
    const double ay = std::fabs(d.y);
    const double az = std::fabs(d.z);
    int face;
    double u;
    double v;
    if (ax >= ay && ax >= az) {
        face = d.x >= 0.0 ? 0 : 1;
        u = d.y / ax;
        v = d.z / ax;
    } else if (ay >= az) {
        face = d.y >= 0.0 ? 2 : 3;
        u = d.x / ay;
        v = d.z / ay;
    } else {
        face = d.z >= 0.0 ? 4 : 5;
        u = d.x / az;
        v = d.y / az;
    }
    const auto cell = [](double c) {
        return std::clamp(static_cast<int>((c + 1.0) * 0.5 * kSightResolution), 0,
                          kSightResolution - 1);
    };
    return (static_cast<std::size_t>(face) * kSightResolution + cell(u)) *
                   kSightResolution + cell(v);
}

void MeasureSight(const Walls &walls, Vec origin, std::uint16_t *texels) {
    constexpr int n = kSightResolution;
    const auto depth = [&](int face, double u, double v) {
        return walls.RayHit(origin, FaceDirection(face, u, v));
    };
    for (int face = 0; face < 6; ++face) {
        double corners[n + 1][n + 1];
        for (int i = 0; i <= n; ++i) {
            for (int j = 0; j <= n; ++j) {
                corners[i][j] = depth(face, 2.0 * i / n - 1.0, 2.0 * j / n - 1.0);
            }
        }
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                const double farthest = std::max(
                        {corners[i][j], corners[i + 1][j], corners[i][j + 1],
                         corners[i + 1][j + 1],
                         depth(face, (2.0 * i + 1.0) / n - 1.0, (2.0 * j + 1.0) / n - 1.0)});
                texels[(static_cast<std::size_t>(face) * n + i) * n + j] =
                        farthest >= (kSightOpen - 1) * kSightUnit
                                ? kSightOpen
                                : static_cast<std::uint16_t>(std::ceil(farthest / kSightUnit));
            }
        }
    }
}

bool Hidden(const std::uint16_t *texels, Vec from, Vec to) {
    const Vec d = to - from;
    const std::uint16_t depth = texels[SightTexel(d)];
    return depth != kSightOpen && Length(d) > depth * kSightUnit;
}

// Runs body(begin, end) over chunks of [0, count) on a few threads.
class Workers {
public:
    Workers() {
        const unsigned count = std::max(1u, std::thread::hardware_concurrency());
        for (unsigned i = 1; i < count; ++i) {
            threads_.emplace_back([this] { Loop(); });
        }
    }
    ~Workers() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        for (std::thread &thread : threads_) thread.join();
    }

    void For(std::size_t count,
             const std::function<void(std::size_t, std::size_t)> &body) {
        if (threads_.empty() || count < 256u) {
            body(0u, count);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            body_ = &body;
            count_ = count;
            next_ = 0u;
            busy_ = threads_.size();
            ++generation_;
        }
        wake_.notify_all();
        Work();
        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait(lock, [this] { return busy_ == 0u; });
    }

private:
    static constexpr std::size_t kChunk = 64u;

    void Work() {
        for (;;) {
            const std::size_t begin = next_.fetch_add(kChunk);
            if (begin >= count_) return;
            (*body_)(begin, std::min(count_, begin + kChunk));
        }
    }

    void Loop() {
        std::uint64_t seen = 0u;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [&] { return stopping_ || generation_ != seen; });
                if (stopping_) return;
                seen = generation_;
            }
            Work();
            std::lock_guard<std::mutex> lock(mutex_);
            if (--busy_ == 0u) done_.notify_one();
        }
    }

    std::vector<std::thread> threads_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable done_;
    const std::function<void(std::size_t, std::size_t)> *body_ = nullptr;
    std::size_t count_ = 0u;
    std::atomic<std::size_t> next_{0u};
    std::size_t busy_ = 0u;
    std::uint64_t generation_ = 0u;
    bool stopping_ = false;
};

}  // namespace

struct AroundWallsDistance::Impl {
    double radius = 0.0;
    std::vector<Triangle> trigger;
    Walls walls{{}};
    // Graph nodes and trigger samples with a path to the trigger, and that
    // path's length: a sample's is minus the radius, as a sphere centred on
    // it already overlaps the trigger by that much.
    std::vector<Vec> nodes;
    std::vector<double> lengths;
    std::vector<std::uint16_t> sight;  // kSightTexels per node


    Stats stats;

    // Candidate tables: inner grid corners first, then outer ones.
    Box box;
    Vec center;
    int cells[3] = {0, 0, 0};
    std::size_t outerStart = 0u;
    std::size_t floorStart = 0u;
    // Floor points' indices (from floorStart) by kGridCell voxel.
    std::unordered_map<VoxelKey, std::vector<std::uint32_t>, VoxelHash> floorCells;
    std::vector<Vec> floors;

    // A corner's best nodes and whether the trigger is in plain sight from
    // it, filled the first time a query needs it: a pure function of the
    // map, so the order queries come in changes nothing.
    struct CornerFound {
        bool direct = false;
        std::uint32_t nodes[kCornerNodes];
    };
    struct CornerCell {
        std::atomic<std::uint8_t> state{0};  // kCornerEmpty, kCornerFilling, kCornerReady
        CornerFound found;
    };
    static constexpr std::uint8_t kCornerEmpty = 0u;
    static constexpr std::uint8_t kCornerFilling = 1u;
    static constexpr std::uint8_t kCornerReady = 2u;
    mutable std::unique_ptr<CornerCell[]> corners;
    // The GPU's table, packed the first time a search asks.
    mutable std::once_flag gpuTableOnce;
    mutable std::shared_ptr<sandbox::PhysicsSandboxAroundWallsTable> gpuTable;

    Vec CornerPoint(std::size_t corner) const {
        if (corner >= floorStart) return floors[corner - floorStart];
        if (corner < outerStart) {
            const std::size_t layer = static_cast<std::size_t>(cells[1] + 1) * (cells[2] + 1);
            const int i = static_cast<int>(corner / layer);
            const int j = static_cast<int>(corner % layer / (cells[2] + 1));
            const int k = static_cast<int>(corner % (cells[2] + 1));
            return InnerPoint(i, j, k);
        }
        std::size_t rest = corner - outerStart;
        const int band = static_cast<int>(rest % kBandCount);
        rest /= kBandCount;
        const int v = static_cast<int>(rest % (kOuterResolution + 1));
        rest /= kOuterResolution + 1;
        const int u = static_cast<int>(rest % (kOuterResolution + 1));
        const int face = static_cast<int>(rest / (kOuterResolution + 1));
        return OuterPoint(face, u, v, band);
    }

    CornerFound Corner(std::size_t corner) const {
        CornerCell &cell = corners[corner];
        if (cell.state.load(std::memory_order_acquire) == kCornerReady) return cell.found;
        CornerFound found;
        std::fill(std::begin(found.nodes), std::end(found.nodes), kNoNode);
        const Vec point = CornerPoint(corner);
        // Inside walls a corner sees nothing.
        if (!walls.AnyWithin(point, radius * kSampleClearance)) {
            Search(point, found.direct, found.nodes, kCornerNodes, kCornerSweeps);
        }
        std::uint8_t empty = kCornerEmpty;
        if (cell.state.compare_exchange_strong(empty, kCornerFilling,
                                               std::memory_order_acq_rel)) {
            cell.found = found;
            cell.state.store(kCornerReady, std::memory_order_release);
        }
        return found;
    }

    std::size_t InnerCorner(int i, int j, int k) const {
        return (static_cast<std::size_t>(i) * (cells[1] + 1) + j) * (cells[2] + 1) + k;
    }
    Vec InnerPoint(int i, int j, int k) const {
        return box.minimum + Vec{i * kGridCell, j * kGridCell, k * kGridCell};
    }
    std::size_t OuterCorner(int face, int u, int v, int band) const {
        return outerStart +
               ((static_cast<std::size_t>(face) * (kOuterResolution + 1) + u) *
                        (kOuterResolution + 1) + v) * kBandCount + band;
    }
    // Where a ray from the box's centre leaves the box.
    Vec Exit(Vec direction) const {
        double nearest = kInfinity;
        for (int axis = 0; axis < 3; ++axis) {
            const double step = Component(direction, axis);
            if (std::fabs(step) < 1e-12) continue;
            const double wall = step > 0.0 ? Component(box.maximum, axis)
                                           : Component(box.minimum, axis);
            nearest = std::min(nearest, (wall - Component(center, axis)) / step);
        }
        return center + direction * nearest;
    }
    Vec OuterPoint(int face, int u, int v, int band) const {
        const Vec direction = FaceDirection(face, 2.0 * u / kOuterResolution - 1.0,
                                            2.0 * v / kOuterResolution - 1.0);
        return Exit(direction) + direction * kBands[band];
    }

    // The corners around p.
    void CornersAround(Vec p, std::size_t (&corners)[8]) const {
        if (box.Contains(p)) {
            int base[3];
            for (int axis = 0; axis < 3; ++axis) {
                base[axis] = std::clamp(
                        static_cast<int>((Component(p, axis) - Component(box.minimum, axis)) /
                                         kGridCell),
                        0, cells[axis] - 1);
            }
            for (int c = 0; c < 8; ++c) {
                corners[c] = InnerCorner(base[0] + (c & 1), base[1] + (c >> 1 & 1),
                                         base[2] + (c >> 2 & 1));
            }
            return;
        }
        const Vec direction = *Normalized(p - center);
        const double beyond = Length(p - Exit(direction));
        int band = 0;
        while (band + 2 < kBandCount && kBands[band + 1] <= beyond) ++band;
        const double ax = std::fabs(direction.x);
        const double ay = std::fabs(direction.y);
        const double az = std::fabs(direction.z);
        int face;
        double u;
        double v;
        if (ax >= ay && ax >= az) {
            face = direction.x >= 0.0 ? 0 : 1;
            u = direction.y / ax;
            v = direction.z / ax;
        } else if (ay >= az) {
            face = direction.y >= 0.0 ? 2 : 3;
            u = direction.x / ay;
            v = direction.z / ay;
        } else {
            face = direction.z >= 0.0 ? 4 : 5;
            u = direction.x / az;
            v = direction.y / az;
        }
        const auto cell = [](double c) {
            return std::clamp(static_cast<int>((c + 1.0) * 0.5 * kOuterResolution), 0,
                              kOuterResolution - 1);
        };
        const int i = cell(u);
        const int j = cell(v);
        for (int c = 0; c < 8; ++c) {
            corners[c] = OuterCorner(face, i + (c & 1), j + (c >> 1 & 1), band + (c >> 2 & 1));
        }
    }

    // Up to kFloorPoints floor points in the 2x2x2 voxels nearest p.
    int FloorPointsAround(Vec p, std::size_t (&points)[kFloorPoints]) const {
        int count = 0;
        const VoxelKey base = VoxelOf(p - Vec{kGridCell, kGridCell, kGridCell} * 0.5, kGridCell);
        for (int c = 0; c < 8; ++c) {
            const auto found = floorCells.find(
                    {base.x + (c & 1), base.y + (c >> 1 & 1), base.z + (c >> 2 & 1)});
            if (found == floorCells.end()) continue;
            for (const std::uint32_t point : found->second) {
                if (count == kFloorPoints) return count;
                points[count++] = floorStart + point;
            }
        }
        return count;
    }

    double Key(Vec p, std::uint32_t node) const {
        return Length(p - nodes[node]) + lengths[node];
    }

    // Every node, cheapest first: up to `want` in sight of p, and whether
    // the trigger's closest point is in plain sight. Gives up after
    // `sweeps` sweeps.
    int Search(Vec p, bool &direct, std::uint32_t *found, int want, int sweeps) const {
        const Vec closest = ClosestOnTrigger(p);
        direct = !walls.Blocked(p, closest, radius);
        thread_local std::vector<std::pair<double, std::uint32_t>> heap;
        heap.clear();
        for (std::uint32_t i = 0; i < nodes.size(); ++i) heap.push_back({Key(p, i), i});
        const auto later = [](const auto &a, const auto &b) { return a.first > b.first; };
        std::make_heap(heap.begin(), heap.end(), later);
        int count = 0;
        std::uint32_t last = Walls::kNoWall;
        while (!heap.empty() && count < want && sweeps > 0) {
            std::pop_heap(heap.begin(), heap.end(), later);
            const std::uint32_t node = heap.back().second;
            heap.pop_back();
            if (Hidden(&sight[node * kSightTexels], nodes[node], p)) continue;
            --sweeps;
            if (!walls.Blocked(nodes[node], p, radius, last)) found[count++] = node;
        }
        return count;
    }

    Vec ClosestOnTrigger(Vec p) const {
        Vec best;
        double bestDistance = kInfinity;
        for (const Triangle &t : trigger) {
            const Vec closest = ClosestOnTriangle(p, t);
            const double distance = Length(p - closest);
            if (distance < bestDistance) {
                bestDistance = distance;
                best = closest;
            }
        }
        return best;
    }
};

AroundWallsDistance::AroundWallsDistance(
        const std::vector<sandbox::PhysicsSandboxCollisionTriangle> &map,
        const std::vector<sandbox::PhysicsSandboxCollisionTriangle> &trigger,
        const std::vector<sandbox::PhysicsSandboxCollisionTriangle> &notWalls,
        double radius,
        double reach) {
    const auto started = std::chrono::steady_clock::now();
    auto impl = std::make_unique<Impl>();
    impl->radius = radius;
    Box triggerBox;
    for (const auto &t : trigger) {
        impl->trigger.push_back({Of(t.a), Of(t.b), Of(t.c)});
        triggerBox.Add(BoxOf(impl->trigger.back()));
    }
    const Box box = triggerBox.Grown(reach);
    std::unordered_set<TriangleKey, TriangleKeyHash> skipped;
    for (const auto &t : notWalls) skipped.insert(KeyOf(t));
    std::vector<Triangle> clipped;
    for (const auto &t : map) {
        const Triangle triangle{Of(t.a), Of(t.b), Of(t.c)};
        if (t.material != kNotCollidable && BoxOf(triangle).Overlaps(box) &&
            skipped.count(KeyOf(t)) == 0u) {
            ClipToBox(triangle, box, clipped);
        }
    }
    impl->stats.triangles = clipped.size();
    impl->walls = Walls(std::move(clipped));
    // Samples inside a pillar or under the road can never be reached.
    std::vector<Vec> placed;
    for (const Vec sample : SampleTrigger(impl->trigger)) {
        if (!impl->walls.AnyWithin(sample, radius * kSampleClearance)) {
            placed.push_back(sample);
        }
    }
    const std::size_t samples = placed.size();
    for (const Vec node : PlaceNodes(impl->walls, box, radius)) placed.push_back(node);

    // Dijkstra from the trigger samples over the points in sight of each
    // other. A pair is swept only when it would shorten the farther point's
    // path.
    Workers workers;
    std::vector<std::uint16_t> sight(placed.size() * kSightTexels);
    workers.For(placed.size(), [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            MeasureSight(impl->walls, placed[i], &sight[i * kSightTexels]);
        }
    });
    std::atomic<std::size_t> sweeps{0u};
    std::vector<double> length(placed.size(), kInfinity);
    std::vector<char> settled(placed.size(), 0);
    std::vector<char> improved(placed.size(), 0);
    using Entry = std::pair<double, std::uint32_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> queue;
    for (std::uint32_t i = 0; i < samples; ++i) {
        length[i] = -radius;
        queue.push({length[i], i});
    }
    while (!queue.empty()) {
        // Not a structured binding: the lambda below captures them.
        const double distance = queue.top().first;
        const std::uint32_t from = queue.top().second;
        queue.pop();
        if (settled[from] || distance > length[from]) continue;
        settled[from] = 1;
        const Vec origin = placed[from];
        workers.For(placed.size(), [&](std::size_t begin, std::size_t end) {
            std::size_t swept = 0u;
            std::uint32_t last = Walls::kNoWall;
            for (std::size_t to = begin; to < end; ++to) {
                if (settled[to]) continue;
                const double candidate = distance + Length(placed[to] - origin);
                if (candidate >= length[to]) continue;
                if (Hidden(&sight[from * kSightTexels], origin, placed[to]) ||
                    Hidden(&sight[to * kSightTexels], placed[to], origin)) {
                    continue;
                }
                ++swept;
                if (impl->walls.Blocked(origin, placed[to], radius, last)) continue;
                length[to] = candidate;
                improved[to] = 1;
            }
            sweeps += swept;
        });
        for (std::uint32_t to = 0; to < placed.size(); ++to) {
            if (!improved[to]) continue;
            improved[to] = 0;
            queue.push({length[to], to});
        }
    }
    for (std::size_t i = 0; i < placed.size(); ++i) {
        if (length[i] == kInfinity) continue;
        impl->nodes.push_back(placed[i]);
        impl->lengths.push_back(length[i]);
        impl->sight.insert(impl->sight.end(), sight.begin() + i * kSightTexels,
                           sight.begin() + (i + 1) * kSightTexels);
    }
    impl->box = box;
    impl->center = (box.minimum + box.maximum) * 0.5;
    for (int axis = 0; axis < 3; ++axis) {
        impl->cells[axis] = std::max(1, static_cast<int>(std::ceil(
                (Component(box.maximum, axis) - Component(box.minimum, axis)) / kGridCell)));
    }
    impl->outerStart = impl->InnerCorner(impl->cells[0], impl->cells[1], impl->cells[2]) + 1u;
    impl->floorStart =
            impl->OuterCorner(5, kOuterResolution, kOuterResolution, kBandCount - 1) + 1u;
    std::vector<Vec> floors;
    {
        const Box near = box.Grown(kFloorMargin);
        std::unordered_set<VoxelKey, VoxelHash> taken;
        std::vector<Triangle> one;
        for (const auto &t : map) {
            const Triangle triangle{Of(t.a), Of(t.b), Of(t.c)};
            if (t.material == kNotCollidable || !BoxOf(triangle).Overlaps(near) ||
                skipped.count(KeyOf(t)) != 0u) {
                continue;
            }
            const auto normal = Normalized(Cross(triangle.b - triangle.a, triangle.c - triangle.a));
            if (!normal || std::fabs(normal->y) < kFloorFacing) continue;
            one.clear();
            one.push_back(triangle);
            for (const Vec sample : SampleTrigger(one, kGridCell)) {
                const Vec point = sample + Vec{0.0, radius, 0.0};
                if (!near.Contains(point) || !taken.insert(VoxelOf(point, kGridCell * 0.5)).second) {
                    continue;
                }
                if (impl->walls.AnyWithin(point, radius * kSampleClearance)) continue;
                impl->floorCells[VoxelOf(point, kGridCell)].push_back(
                        static_cast<std::uint32_t>(floors.size()));
                floors.push_back(point);
            }
        }
    }
    impl->corners = std::make_unique<Impl::CornerCell[]>(impl->floorStart + floors.size());
    impl->floors = std::move(floors);
    impl->stats.nodes = impl->nodes.size();
    impl->stats.sweeps = sweeps;
    impl->stats.buildSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
    impl_ = std::move(impl);
}

AroundWallsDistance::~AroundWallsDistance() = default;

namespace {

// FNV-1a over everything a graph is built from.
class Fingerprint {
public:
    void Add(const void *data, std::size_t size) {
        const auto *bytes = static_cast<const unsigned char *>(data);
        for (std::size_t i = 0; i < size; ++i) {
            hash_ = (hash_ ^ bytes[i]) * 1099511628211ull;
        }
    }
    void Add(double value) { Add(&value, sizeof value); }
    void Add(const sandbox::PhysicsSandboxCollisionTriangle &t) {
        for (const auto &v : {t.a, t.b, t.c}) {
            Add(&v.x, sizeof v.x);
            Add(&v.y, sizeof v.y);
            Add(&v.z, sizeof v.z);
        }
        Add(&t.material, sizeof t.material);
    }
    std::uint64_t Value() const { return hash_; }

private:
    std::uint64_t hash_ = 14695981039346656037ull;
};

}  // namespace

std::shared_ptr<const AroundWallsDistance> AroundWallsDistance::Shared(
        const std::vector<sandbox::PhysicsSandboxCollisionTriangle> &map,
        const std::vector<sandbox::PhysicsSandboxCollisionTriangle> &trigger,
        const std::vector<sandbox::PhysicsSandboxCollisionTriangle> &notWalls,
        double radius,
        double reach) {
    // Only what lies near the trigger shapes the graph.
    Box triggerBox;
    for (const auto &t : trigger) triggerBox.Add(BoxOf({Of(t.a), Of(t.b), Of(t.c)}));
    const Box near = triggerBox.Grown(reach + kFloorMargin);
    Fingerprint fingerprint;
    fingerprint.Add(radius);
    fingerprint.Add(reach);
    for (const auto &t : trigger) fingerprint.Add(t);
    for (const auto *triangles : {&map, &notWalls}) {
        for (const auto &t : *triangles) {
            if (BoxOf({Of(t.a), Of(t.b), Of(t.c)}).Overlaps(near)) fingerprint.Add(t);
        }
        fingerprint.Add(0.0);
    }
    const std::uint64_t key = fingerprint.Value();

    static std::mutex mutex;
    // Most recently used last.
    static std::vector<std::pair<std::uint64_t, std::shared_ptr<const AroundWallsDistance>>>
            graphs;
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = std::find_if(graphs.begin(), graphs.end(),
                                    [&](const auto &entry) { return entry.first == key; });
    std::shared_ptr<const AroundWallsDistance> graph;
    if (found != graphs.end()) {
        graph = found->second;
        graphs.erase(found);
    } else {
        graph = std::make_shared<const AroundWallsDistance>(map, trigger, notWalls, radius, reach);
        if (graphs.size() == kSharedGraphs) graphs.erase(graphs.begin());
    }
    graphs.push_back({key, graph});
    return graph;
}

AroundWallsDistance::Path AroundWallsDistance::PathTo(
        const forevervalidator::Vector3 &point, Memory &memory) const {
    const Impl &impl = *impl_;
    const Vec center = Of(point);
    const auto at = [](Vec v) {
        return forevervalidator::Vector3{static_cast<float>(v.x), static_cast<float>(v.y),
                                         static_cast<float>(v.z)};
    };
    // The nodes best for the corners around the point, and the last
    // query's: a node's straight line plus its path can only underestimate,
    // so the first one in sight, cheapest first, is the answer.
    std::size_t corners[8];
    impl.CornersAround(center, corners);
    std::size_t floorPoints[kFloorPoints];
    const int floorCount = impl.FloorPointsAround(center, floorPoints);
    std::pair<double, std::uint32_t> candidates[(8 + kFloorPoints) * kCornerNodes + 1];
    int count = 0;
    bool direct = false;
    const auto add = [&](std::uint32_t node) {
        if (node == kNoNode) return;
        for (int i = 0; i < count; ++i) {
            if (candidates[i].second == node) return;
        }
        candidates[count++] = {impl.Key(center, node), node};
    };
    // Near a floor, the floor points see what the car sees; the corners
    // above and below it see past walls it does not (they still tell
    // whether the trigger may be in plain sight).
    for (const std::size_t corner : corners) {
        const Impl::CornerFound found = impl.Corner(corner);
        direct |= found.direct;
        if (floorCount == 0) {
            for (const std::uint32_t node : found.nodes) add(node);
        }
    }
    for (int i = 0; i < floorCount; ++i) {
        const Impl::CornerFound found = impl.Corner(floorPoints[i]);
        direct |= found.direct;
        for (const std::uint32_t node : found.nodes) add(node);
    }
    if (memory.node < impl.nodes.size()) add(memory.node);
    if (direct) {
        const Vec closest = impl.ClosestOnTrigger(center);
        if (memory.directBlocker == Walls::kNoWall ||
            !impl.walls.StoppedBy(memory.directBlocker, center, closest, impl.radius)) {
            memory.directBlocker = impl.walls.Blocker(center, closest, impl.radius);
            if (memory.directBlocker == Walls::kNoWall) {
                return {std::max(0.0, Length(center - closest) - impl.radius), at(closest), true};
            }
        }
    }
    std::sort(candidates, candidates + count);
    for (int i = 0; i < count; ++i) {
        const std::uint32_t node = candidates[i].second;
        if (Hidden(&impl.sight[node * kSightTexels], impl.nodes[node], center)) continue;
        // The wall that blocked this node last time, then all of them.
        const auto remembered = std::find(std::begin(memory.blockedNodes),
                                          std::end(memory.blockedNodes), node);
        if (remembered != std::end(memory.blockedNodes) &&
            impl.walls.StoppedBy(memory.blockers[remembered - std::begin(memory.blockedNodes)],
                                 impl.nodes[node], center, impl.radius)) {
            continue;
        }
        const std::uint32_t blocker = impl.walls.Blocker(impl.nodes[node], center, impl.radius);
        if (blocker != Walls::kNoWall) {
            const std::size_t slot = remembered != std::end(memory.blockedNodes)
                    ? static_cast<std::size_t>(remembered - std::begin(memory.blockedNodes))
                    : memory.next++ % Memory::kBlockers;
            memory.blockedNodes[slot] = node;
            memory.blockers[slot] = blocker;
            continue;
        }
        memory.node = node;
        return {std::max(0.0, candidates[i].first), at(impl.nodes[node]), false};
    }
    bool inSight = false;
    std::uint32_t found = kNoNode;
    const Vec closest = impl.ClosestOnTrigger(center);
    if (impl.Search(center, inSight, &found, 1, kFallbackSweeps) != 0 && !inSight) {
        memory.node = found;
        return {std::max(0.0, impl.Key(center, found)), at(impl.nodes[found]), false};
    }
    const Path straight{std::max(0.0, Length(center - closest) - impl.radius), at(closest), inSight};
    if (inSight) return straight;
    // Nothing found in sight: the cheapest way the points around saw.
    if (count != 0) {
        return {std::max(0.0, candidates[0].first), at(impl.nodes[candidates[0].second]), false};
    }
    return straight;
}

AroundWallsDistance::Path AroundWallsDistance::SearchPathTo(
        const forevervalidator::Vector3 &point) const {
    const Impl &impl = *impl_;
    const Vec center = Of(point);
    const auto at = [](Vec v) {
        return forevervalidator::Vector3{static_cast<float>(v.x), static_cast<float>(v.y),
                                         static_cast<float>(v.z)};
    };
    bool inSight = false;
    std::uint32_t found = kNoNode;
    const bool any = impl.Search(center, inSight, &found, 1, std::numeric_limits<int>::max()) != 0;
    if (any && !inSight) {
        return {std::max(0.0, impl.Key(center, found)), at(impl.nodes[found]), false};
    }
    const Vec closest = impl.ClosestOnTrigger(center);
    return {std::max(0.0, Length(center - closest) - impl.radius), at(closest), inSight};
}

namespace {

namespace table = forevervalidator::experimental::around_walls;
// The GPU's query is this one: same constants.
static_assert(table::kSightResolution == kSightResolution &&
              table::kSightTexels == static_cast<int>(kSightTexels) &&
              table::kSightUnit == kSightUnit && table::kSightOpen == kSightOpen &&
              table::kGridCell == kGridCell && table::kOuterResolution == kOuterResolution &&
              table::kBandCount == kBandCount && table::kCornerNodes == kCornerNodes &&
              table::kFloorPoints == kFloorPoints &&
              table::kFallbackSweeps == kFallbackSweeps &&
              table::kCrossingTolerance == kCrossingTolerance &&
              table::kNearerOnTheWay == kNearerOnTheWay &&
              table::kCornerSweeps == kCornerSweeps &&
              table::kSampleClearance == kSampleClearance);

}  // namespace

std::shared_ptr<sandbox::PhysicsSandboxAroundWallsTable> AroundWallsDistance::GpuTable() const {
    std::call_once(impl_->gpuTableOnce, [this] {
        impl_->gpuTable = std::make_shared<sandbox::PhysicsSandboxAroundWallsTable>();
        impl_->gpuTable->bytes = PackGpuTable(*impl_, false);
    });
    return impl_->gpuTable;
}

std::vector<std::uint8_t> AroundWallsDistance::GpuTableFilledHere() const {
    return PackGpuTable(*impl_, true);
}

std::vector<std::uint8_t> AroundWallsDistance::PackGpuTable(const Impl &impl, bool fillAll) {
    for (int band = 0; band < kBandCount; ++band) {
        if (table::Band(band) != kBands[band]) {
            throw std::logic_error("the GPU around-walls bands differ");
        }
    }
    // Every corner, as the queries that need them would fill them.
    const std::size_t cornerCount = impl.floorStart + impl.floors.size();
    if (fillAll) {
        Workers workers;
        workers.For(cornerCount, [&](std::size_t begin, std::size_t end) {
            for (std::size_t corner = begin; corner < end; ++corner) impl.Corner(corner);
        });
    }
    // Floor cells, sorted, and their points.
    std::vector<std::pair<VoxelKey, const std::vector<std::uint32_t> *>> cells;
    for (const auto &[key, points] : impl.floorCells) cells.push_back({key, &points});
    std::sort(cells.begin(), cells.end(), [](const auto &a, const auto &b) {
        return std::tie(a.first.x, a.first.y, a.first.z) <
               std::tie(b.first.x, b.first.y, b.first.z);
    });
    std::size_t floorIndexCount = 0u;
    for (const auto &cell : cells) floorIndexCount += cell.second->size();

    const auto &walls = impl.walls.triangles();
    const auto &planes = impl.walls.planes();
    const auto &tree = impl.walls.tree();
    table::Header header;
    header.radius = impl.radius;
    for (int axis = 0; axis < 3; ++axis) {
        header.boxMinimum[axis] = Component(impl.box.minimum, axis);
        header.boxMaximum[axis] = Component(impl.box.maximum, axis);
        header.center[axis] = Component(impl.center, axis);
        header.cells[axis] = impl.cells[axis];
    }
    header.triggerCount = static_cast<std::uint32_t>(impl.trigger.size());
    header.wallCount = static_cast<std::uint32_t>(walls.size());
    header.treeCount = static_cast<std::uint32_t>(tree.size());
    header.nodeCount = static_cast<std::uint32_t>(impl.nodes.size());
    header.floorCellCount = static_cast<std::uint32_t>(cells.size());
    header.outerStart = impl.outerStart;
    header.floorStart = impl.floorStart;
    header.cornerCount = cornerCount;
    std::uint64_t cursor = sizeof(table::Header);
    const auto place = [&cursor](std::uint64_t bytes) {
        cursor = (cursor + 15u) & ~std::uint64_t{15u};
        const std::uint64_t offset = cursor;
        cursor += bytes;
        return offset;
    };
    header.triggers = place(impl.trigger.size() * 9u * sizeof(double));
    header.walls = place(walls.size() * 9u * sizeof(double));
    header.planes = place(planes.size() * sizeof(table::Plane));
    header.tree = place(tree.size() * sizeof(table::TreeNode));
    header.nodes = place(impl.nodes.size() * 3u * sizeof(double));
    header.lengths = place(impl.lengths.size() * sizeof(double));
    header.sight = place(impl.sight.size() * sizeof(std::uint16_t));
    header.corners = place(cornerCount * sizeof(table::Corner));
    header.floors = place(impl.floors.size() * 3u * sizeof(double));
    header.floorCells = place(cells.size() * sizeof(table::FloorCell));
    header.floorIndices = place(floorIndexCount * sizeof(std::uint32_t));
    header.missed = place(table::MissedBytes(cornerCount));
    header.size = (cursor + 15u) & ~std::uint64_t{15u};

    std::vector<std::uint8_t> bytes(header.size, 0u);
    std::memcpy(bytes.data(), &header, sizeof header);
    const auto at = [&bytes](std::uint64_t offset) { return bytes.data() + offset; };
    const auto putTriangle = [](double *out, const Triangle &t) {
        const Vec corners[3] = {t.a, t.b, t.c};
        for (int c = 0; c < 3; ++c) {
            out[c * 3] = corners[c].x;
            out[c * 3 + 1] = corners[c].y;
            out[c * 3 + 2] = corners[c].z;
        }
    };
    for (std::size_t i = 0; i < impl.trigger.size(); ++i) {
        double triangle[9];
        putTriangle(triangle, impl.trigger[i]);
        std::memcpy(at(header.triggers + i * sizeof triangle), triangle, sizeof triangle);
    }
    for (std::size_t i = 0; i < walls.size(); ++i) {
        double triangle[9];
        putTriangle(triangle, walls[i]);
        std::memcpy(at(header.walls + i * sizeof triangle), triangle, sizeof triangle);
        table::Plane plane{};
        const WallPlane &wall = planes[i];
        if (wall.normal) {
            plane.normal[0] = wall.normal->x;
            plane.normal[1] = wall.normal->y;
            plane.normal[2] = wall.normal->z;
            plane.hasNormal = 1.0;
        }
        plane.offset = wall.offset;
        for (int axis = 0; axis < 3; ++axis) {
            plane.boxMinimum[axis] = Component(wall.box.minimum, axis);
            plane.boxMaximum[axis] = Component(wall.box.maximum, axis);
            plane.center[axis] = Component(wall.center, axis);
        }
        plane.bound = wall.bound;
        std::memcpy(at(header.planes + i * sizeof plane), &plane, sizeof plane);
    }
    for (std::size_t i = 0; i < tree.size(); ++i) {
        table::TreeNode node{};
        for (int axis = 0; axis < 3; ++axis) {
            node.boxMinimum[axis] = Component(tree[i].box.minimum, axis);
            node.boxMaximum[axis] = Component(tree[i].box.maximum, axis);
        }
        node.first = tree[i].first;
        node.count = tree[i].count;
        node.right = tree[i].right;
        std::memcpy(at(header.tree + i * sizeof node), &node, sizeof node);
    }
    for (std::size_t i = 0; i < impl.nodes.size(); ++i) {
        const double node[3] = {impl.nodes[i].x, impl.nodes[i].y, impl.nodes[i].z};
        std::memcpy(at(header.nodes + i * sizeof node), node, sizeof node);
    }
    std::memcpy(at(header.lengths), impl.lengths.data(), impl.lengths.size() * sizeof(double));
    std::memcpy(at(header.sight), impl.sight.data(), impl.sight.size() * sizeof(std::uint16_t));
    for (std::size_t i = 0; i < cornerCount; ++i) {
        table::Corner corner{};
        if (fillAll || impl.corners[i].state.load(std::memory_order_acquire) == Impl::kCornerReady) {
            const Impl::CornerFound found = impl.Corner(i);
            corner.direct = found.direct ? 1u : 0u;
            for (int n = 0; n < kCornerNodes; ++n) corner.nodes[n] = found.nodes[n];
        } else {
            corner.direct = table::kCornerUnfilled;
            for (int n = 0; n < kCornerNodes; ++n) corner.nodes[n] = table::kNone;
        }
        std::memcpy(at(header.corners + i * sizeof corner), &corner, sizeof corner);
    }
    for (std::size_t i = 0; i < impl.floors.size(); ++i) {
        const double point[3] = {impl.floors[i].x, impl.floors[i].y, impl.floors[i].z};
        std::memcpy(at(header.floors + i * sizeof point), point, sizeof point);
    }
    std::uint32_t first = 0u;
    for (std::size_t i = 0; i < cells.size(); ++i) {
        table::FloorCell cell{cells[i].first.x, cells[i].first.y, cells[i].first.z, first,
                              static_cast<std::uint32_t>(cells[i].second->size())};
        std::memcpy(at(header.floorCells + i * sizeof cell), &cell, sizeof cell);
        std::memcpy(at(header.floorIndices + first * sizeof(std::uint32_t)),
                    cells[i].second->data(), cells[i].second->size() * sizeof(std::uint32_t));
        first += cell.count;
    }
    return bytes;
}

const AroundWallsDistance::Stats &AroundWallsDistance::stats() const {
    return impl_->stats;
}

}  // namespace forevertas
