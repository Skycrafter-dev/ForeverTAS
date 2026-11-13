#ifndef FOREVERTAS_EVALUATORS_AROUND_WALLS_DISTANCE_H
#define FOREVERTAS_EVALUATORS_AROUND_WALLS_DISTANCE_H

#include <forevervalidator/experimental/physics_sandbox.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace forevertas {

// Shortest path from a sphere to a trigger surface when the map's walls
// exist only within `reach` metres of the trigger's box: straight lines
// elsewhere, so the distance is continuous everywhere. Inside that box the
// path bends at graph nodes placed around the walls' edges; each node knows
// its own path length to the trigger.
class AroundWallsDistance {
public:
    struct Stats {
        std::size_t triangles = 0u;  // after clipping to the box
        std::size_t nodes = 0u;      // nodes with a path to the trigger
        std::size_t sweeps = 0u;     // sphere sweeps while building
        double buildSeconds = 0.0;
    };

    AroundWallsDistance(
            const std::vector<forevervalidator::experimental::PhysicsSandboxCollisionTriangle>
                    &map,
            const std::vector<forevervalidator::experimental::PhysicsSandboxCollisionTriangle>
                    &trigger,
            // Map triangles that are not walls (every checkpoint's and
            // finish's trigger comes with the map's collision triangles);
            // NotCollidable ones are not walls either.
            const std::vector<forevervalidator::experimental::PhysicsSandboxCollisionTriangle>
                    &notWalls,
            double radius,
            double reach);
    ~AroundWallsDistance();

    // The same, kept for the process: a later search on the same walls,
    // trigger, radius and reach reuses it, with the candidates its queries
    // filled. The last kSharedGraphs stay.
    static std::shared_ptr<const AroundWallsDistance> Shared(
            const std::vector<forevervalidator::experimental::PhysicsSandboxCollisionTriangle>
                    &map,
            const std::vector<forevervalidator::experimental::PhysicsSandboxCollisionTriangle>
                    &trigger,
            const std::vector<forevervalidator::experimental::PhysicsSandboxCollisionTriangle>
                    &notWalls,
            double radius,
            double reach);
    static constexpr std::size_t kSharedGraphs = 8u;
    AroundWallsDistance(const AroundWallsDistance &) = delete;
    AroundWallsDistance &operator=(const AroundWallsDistance &) = delete;

    static constexpr std::uint32_t kNoNode = 0xffffffffu;

    // What one query leaves the next one, a moment later: the node it went
    // through and the walls that blocked the others (a wall that blocked a
    // node usually still does, and one triangle is quick to test). Only
    // speeds queries up; the answers stay the same.
    struct Memory {
        static constexpr std::size_t kBlockers = 16u;
        std::uint32_t node = kNoNode;
        std::uint32_t directBlocker = kNoNode;  // what hid the trigger
        std::uint32_t blockedNodes[kBlockers];
        std::uint32_t blockers[kBlockers];
        std::size_t next = 0u;

        Memory() {
            std::fill(std::begin(blockedNodes), std::end(blockedNodes), kNoNode);
        }
    };

    struct Path {
        double length = 0.0;
        // Where the path first bends, or the trigger's closest point when
        // that is in plain sight (inSight).
        forevervalidator::Vector3 via{};
        bool inSight = false;
    };

    // Path from a sphere of the radius at `center` until it touches the
    // trigger.
    Path PathTo(const forevervalidator::Vector3 &center, Memory &memory) const;
    // The same through every node, without the precomputed candidates:
    // slow, for tests.
    Path SearchPathTo(const forevervalidator::Vector3 &center) const;

    const Stats &stats() const;

    // The graph as the GPU's table (forevervalidator/experimental/
    // around_walls_table.h), packed once and shared by every search on it:
    // the corners PathTo has filled come filled, the others are filled on
    // demand by the searches that need them. Its query gives PathTo's
    // answers without the memory of earlier queries.
    std::shared_ptr<forevervalidator::experimental::PhysicsSandboxAroundWallsTable> GpuTable() const;
    // The table with every corner filled here, as PathTo fills them: slow,
    // for tests.
    std::vector<std::uint8_t> GpuTableFilledHere() const;

private:
    struct Impl;
    static std::vector<std::uint8_t> PackGpuTable(const Impl &impl, bool fillAll);
    std::unique_ptr<const Impl> impl_;
};

}  // namespace forevertas

#endif
