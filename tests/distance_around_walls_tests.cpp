#include "evaluators/around_walls_distance.h"
#include "evaluators/distance_target_evaluator.h"
#include "mutations/input_event_formatter.h"
#include "mutations/replay_input_script.h"
#include "replay_file_io.h"

#include <forevervalidator/experimental/around_walls_table.h>
#include <forevervalidator/native.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace forevertas;
using namespace forevervalidator;
using namespace forevervalidator::experimental;

void Check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

template<typename Result>
auto Require(Result result) {
    if (!result) throw std::runtime_error(result.Error().diagnostic);
    return std::move(result.Value());
}

double Seconds(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}

OptionSettings Settings(const std::string &route, std::uint32_t slot) {
    return {{"minTimeMs", "0"}, {"maxTimeMs", "600000"}, {"eventType", "checkpoint"},
            {"checkpointIndex", "2"}, {"lap", "1"}, {"checkpointSlot", std::to_string(slot)},
            {"eventIndex", "0"}, {"route", route}, {"reach", "32"}};
}

// The distance a fresh session reports for one state: its first sample is
// always its best so far.
double DistanceAt(const IterationEvaluator &evaluator, const PhysicsSandboxStateView &state) {
    const auto sample = evaluator.CreateSession()->Observe(std::nullopt, state);
    Check(sample.has_value(), "a fresh distance session reported nothing");
    return std::fmod(sample->score - 1e12, 1e6);
}

// The Distance target around walls, on a real replay, measured to the
// second checkpoint the run takes: it reaches it exactly when the run does,
// is never shorter than the straight line, and changes smoothly as the car
// moves, also where a wall starts or stops hiding the trigger and where the
// walls stop counting.
void TestAroundWalls(const char *packs, const char *path) {
    PhysicsSandboxOptions options;
    options.backend = SimulationBackend::OptimizedCpu;
    options.timelineMode = PhysicsSandboxTimelineMode::Canonical;
    options.simulationHorizonMs = 600000;
    auto sandbox = Require(CreatePhysicsSandbox(Require(OpenInstalledPackDirectory(packs)), options));
    ReplayIdentity identity{path};
    const auto bytes = Require(ReadReplayFileUtf8(path, identity));
    Require(sandbox.LoadReplay({bytes.data(), bytes.size()}, identity));
    const auto parsed = ParseInputScript(ExtractReplayInputScript(packs, path));
    Check(!parsed.error, "replay script failed to parse");
    const auto baseline = BuildInputScriptBaseline(Require(sandbox.ReadInputs()), parsed.commands, 10);
    Check(!baseline.error, "replay input conversion failed");
    Require(sandbox.ReplaceInputs(baseline.events));

    std::vector<PhysicsSandboxStateView> history;
    std::optional<PhysicsSandboxAcceptedCheckpointEvent> second;
    std::size_t checkpoints = 0u;
    for (int tick = 0; tick < 60000 && !second; ++tick) {
        history.push_back(Require(sandbox.AdvanceTicks(1)));
        for (const auto &event : history.back().acceptedCheckpointEvents) {
            if (!event.finish && ++checkpoints == 2u) second = event;
        }
        if (history.back().raceCompleted) break;
    }
    Check(second.has_value(), "the replay took no second checkpoint");

    auto straight = CreateDistanceTargetEvaluator(Settings("straight", second->checkpointSlot), 10);
    auto around = CreateDistanceTargetEvaluator(Settings("around", second->checkpointSlot), 10);
    straight->BindScene(sandbox);
    around->BindScene(sandbox);

    // One session over the run, as a search runs it: the first state builds
    // the graph.
    auto session = around->CreateSession();
    auto started = std::chrono::steady_clock::now();
    session->Observe(std::nullopt, history.front());
    const double buildSeconds = Seconds(started);
    started = std::chrono::steady_clock::now();
    std::optional<double> reachedMs;
    for (std::size_t i = 1; i < history.size() && !reachedMs; ++i) {
        if (const auto sample = session->Observe(std::nullopt, history[i]);
            sample && sample->score < 1e12) {
            reachedMs = sample->timeMs;
        }
    }
    const double tickNs = Seconds(started) * 1e9 / static_cast<double>(history.size() - 1u);
    Check(reachedMs && *reachedMs == static_cast<double>(second->timeMs),
          "around walls did not reach the checkpoint when the run took it");

    // Every state before the checkpoint, each measured on its own.
    std::vector<double> straightDistances;
    std::vector<double> aroundDistances;
    for (std::size_t i = 0; i + 1 < history.size(); ++i) {
        straightDistances.push_back(DistanceAt(*straight, history[i]));
        aroundDistances.push_back(DistanceAt(*around, history[i]));
    }
    // A rise beyond the car's move would hold searches back (the trap of
    // a score that grows as the car gets nearer); a drop is a new shortcut
    // coming into sight, no more abrupt than the nodes are apart.
    double largestRise = 0.0;
    double largestDrop = 0.0;
    for (std::size_t i = 0; i < aroundDistances.size(); ++i) {
        Check(aroundDistances[i] >= straightDistances[i] - 1e-9,
              "around walls was shorter than the straight line");
        if (i == 0u) continue;
        const auto &from = history[i - 1u].car.position;
        const auto &to = history[i].car.position;
        const double moved = std::sqrt((to.x - from.x) * (to.x - from.x) +
                                       (to.y - from.y) * (to.y - from.y) +
                                       (to.z - from.z) * (to.z - from.z));
        // Beyond the car's own move, only its turning and the graph's nodes
        // being points may change the distance.
        const double change = aroundDistances[i] - aroundDistances[i - 1u];
        largestRise = std::max(largestRise, change - moved);
        largestDrop = std::max(largestDrop, -change - moved);
        if (std::fabs(change) - moved > 0.5) {
            std::cerr << "jump at " << history[i].timeMs << " ms: " << aroundDistances[i - 1u]
                      << " -> " << aroundDistances[i] << " m (straight " << straightDistances[i - 1u]
                      << " -> " << straightDistances[i] << ", moved " << moved << " m)\n";
        }
    }
    std::cerr << "around walls to checkpoint slot " << second->checkpointSlot << ": graph built in "
              << buildSeconds << " s, " << tickNs << " ns per tick over " << history.size() - 1u
              << " ticks; beyond the car's move, largest rise " << largestRise
              << " m, largest drop " << largestDrop << " m\n";
    Check(largestRise < 1.0, "around walls rose faster than the car moved");
    Check(largestDrop < 2.5, "around walls dropped faster than the car moved");

    // The fast way (candidates precomputed around the point) against the
    // search through every node, one radius over the car on every tick of
    // the run: the same answer, give or take where the nodes are.
    const auto scene = Require(sandbox.ReadScene());
    std::vector<PhysicsSandboxCollisionTriangle> triggers;
    const PhysicsSandboxCheckpointTrigger *target = nullptr;
    for (const auto &trigger : scene.checkpointTriggers) {
        triggers.insert(triggers.end(), trigger.triangles.begin(), trigger.triangles.end());
        if (trigger.role == PhysicsSandboxCheckpointTriggerRole::Checkpoint &&
            trigger.checkpointSlot == second->checkpointSlot) {
            target = &trigger;
        }
    }
    Check(target != nullptr, "the second checkpoint's trigger was not read");
    constexpr double kRadius = 0.59;
    const AroundWallsDistance graph(scene.collisionTriangles, target->triangles, triggers,
                                    kRadius, 32.0);
    AroundWallsDistance::Memory memory;
    double worse = 0.0;
    double fastSeconds = 0.0;
    double searchSeconds = 0.0;
    for (const auto &state : history) {
        const Vector3 point{state.car.position.x,
                            state.car.position.y + static_cast<float>(kRadius),
                            state.car.position.z};
        started = std::chrono::steady_clock::now();
        const auto fast = graph.PathTo(point, memory);
        fastSeconds += Seconds(started);
        started = std::chrono::steady_clock::now();
        const auto search = graph.SearchPathTo(point);
        searchSeconds += Seconds(started);
        Check(fast.inSight == search.inSight, "the fast way saw the trigger differently");
        Check(fast.length >= search.length - 1e-9, "the fast way was shorter than the shortest");
        worse = std::max(worse, fast.length - search.length);
    }
    std::cerr << "fast way at most " << worse << " m longer than the search through all "
              << graph.stats().nodes << " nodes; " << fastSeconds * 1e9 / history.size()
              << " ns against " << searchSeconds * 1e9 / history.size()
              << " ns per point (first visits fill the candidates)\n";
    Check(worse < 1.0, "the fast way was much longer than the shortest");

    // The GPU's table and query: PathTo's answers (without the memory of
    // earlier queries) on every tick of the run and at points all around
    // the trigger, near and far.
    // It comes with the corners PathTo filled so far; the table's own fill
    // (run on demand by GPU searches, here on every thread for the rest)
    // must give the CPU's corners byte for byte.
    started = std::chrono::steady_clock::now();
    const std::vector<std::uint8_t> packed = graph.GpuTable()->bytes;
    const double tableSeconds = Seconds(started);
    Check(around_walls::Valid(packed.data(), packed.size()), "the GPU table is not readable");
    const auto &header = *reinterpret_cast<const around_walls::Header *>(packed.data());
    const auto unfilled = [&header](const std::vector<std::uint8_t> &bytes, std::uint64_t c) {
        return reinterpret_cast<const around_walls::Corner *>(bytes.data() + header.corners)[c]
                       .direct == around_walls::kCornerUnfilled;
    };
    std::vector<std::uint8_t> table = packed;
    started = std::chrono::steady_clock::now();
    std::uint64_t filledBefore = 0u;
    {
        std::atomic<std::uint64_t> next{0u};
        std::vector<std::thread> fillers;
        for (unsigned t = 0; t < std::max(1u, std::thread::hardware_concurrency()); ++t) {
            fillers.emplace_back([&] {
                for (std::uint64_t c; (c = next.fetch_add(1u)) < header.cornerCount;) {
                    if (unfilled(table, c)) around_walls::FillCorner(table.data(), c);
                }
            });
        }
        for (auto &filler : fillers) filler.join();
        for (std::uint64_t c = 0; c < header.cornerCount; ++c) filledBefore += !unfilled(packed, c);
    }
    const double fillSeconds = Seconds(started);
    started = std::chrono::steady_clock::now();
    const std::vector<std::uint8_t> filledHere = graph.GpuTableFilledHere();
    const double cpuFillSeconds = Seconds(started);
    Check(filledHere == table, "the table's corner fill differs from the CPU's");
    std::vector<Vector3> points;
    for (const auto &state : history) {
        points.push_back({state.car.position.x, state.car.position.y + static_cast<float>(kRadius),
                          state.car.position.z});
    }
    std::uint32_t random = 12345u;
    const auto next = [&random] {
        random = random * 1664525u + 1013904223u;
        return static_cast<double>(random >> 8) / static_cast<double>(1u << 24);
    };
    const Vector3 anchor = target->triangles.front().a;
    for (int i = 0; i < 3000; ++i) {
        const double spread = i < 2000 ? 48.0 : 400.0;
        points.push_back({static_cast<float>(anchor.x + (next() * 2.0 - 1.0) * spread),
                          static_cast<float>(anchor.y + (next() * 2.0 - 1.0) * spread * 0.25),
                          static_cast<float>(anchor.z + (next() * 2.0 - 1.0) * spread)});
    }
    // On demand, as a GPU search asks: a query that needed corners not
    // filled yet marks them; they are filled and it asks again.
    std::vector<std::uint8_t> onDemand = packed;
    std::size_t onDemandRounds = 0u;
    std::size_t onDemandDiffer = 0u;
    const auto askFilling = [&](const Vector3 &point) {
        auto *words = reinterpret_cast<std::uint32_t *>(onDemand.data() + header.missed);
        for (;;) {
            const auto path = around_walls::PathTo(onDemand.data(), point.x, point.y, point.z);
            if (words[0] == 0u) return path;
            ++onDemandRounds;
            for (std::uint64_t c = 0; c < header.cornerCount; ++c) {
                if ((words[1u + c / 32u] >> (c % 32u) & 1u) != 0u && unfilled(onDemand, c)) {
                    around_walls::FillCorner(onDemand.data(), c);
                }
            }
            std::fill(words, words + around_walls::MissedBytes(header.cornerCount) / 4u, 0u);
        }
    };
    std::size_t differ = 0u;
    for (const Vector3 &point : points) {
        AroundWallsDistance::Memory fresh;
        const auto cpu = graph.PathTo(point, fresh);
        const auto gpu = around_walls::PathTo(table.data(), point.x, point.y, point.z);
        const auto later = askFilling(point);
        onDemandDiffer += later.length != gpu.length || later.inSight != gpu.inSight ||
                later.via[0] != gpu.via[0] || later.via[1] != gpu.via[1] ||
                later.via[2] != gpu.via[2];
        const bool same = cpu.inSight == gpu.inSight &&
                std::fabs(cpu.length - gpu.length) <= 1e-9 &&
                std::fabs(cpu.via.x - gpu.via[0]) <= 1e-3 &&
                std::fabs(cpu.via.y - gpu.via[1]) <= 1e-3 &&
                std::fabs(cpu.via.z - gpu.via[2]) <= 1e-3;
        if (!same && differ++ < 5u) {
            std::cerr << "table query differs at " << point.x << ' ' << point.y << ' ' << point.z
                      << ": " << cpu.length << (cpu.inSight ? " in sight" : "") << " vs "
                      << gpu.length << (gpu.inSight ? " in sight" : "") << '\n';
        }
    }
    std::cerr << "GPU table " << table.size() / 1024 << " KiB, packed in " << tableSeconds
              << " s with " << filledBefore << "/" << header.cornerCount
              << " corners filled; the rest filled by the table's code in " << fillSeconds
              << " s (all by the CPU's: " << cpuFillSeconds << " s); "
              << points.size() - differ << "/" << points.size() << " points as PathTo; on demand "
              << onDemandRounds << " fills, " << onDemandDiffer << " answers differ\n";
    Check(differ == 0u, "the GPU table's query differs from PathTo");
    Check(onDemandDiffer == 0u, "the table filled on demand answers differently");
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    try {
        TestAroundWalls(argv[1], argv[2]);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
