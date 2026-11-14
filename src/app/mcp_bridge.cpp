#include "app/mcp_bridge.h"

#include "app/app_settings.h"
#include "app/mcp_protocol.h"
#include "diagnostics/diagnostics_log.h"
#include "app/search_controller.h"
#include "evaluators/gate_entry_evaluator.h"
#include "input_timeline_time.h"
#include "mutations/input_event_formatter.h"
#include "mutations/input_event_utils.h"
#include "replay_file_io.h"
#include "searches/algorithm_registry.h"
#include "searches/search_runner.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTextStream>
#include <QThreadPool>
#include <QTimer>

#include <forevervalidator/experimental/physics_sandbox.h>
#include <forevervalidator/native.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <exception>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace forevertas::app {

namespace fv = forevervalidator;
namespace fvx = forevervalidator::experimental;

namespace {

constexpr std::uint32_t kTickMs = kInputTimelineTickDurationMs;
constexpr std::uint32_t kDefaultSimulateHorizonMs = 600000u;
constexpr int kMaximumCompareScripts = 256;
constexpr int kMaximumSimulationThreads = 32;
constexpr std::size_t kMaximumSnapshots = 64u;
constexpr qsizetype kMaximumMessageBytes = 64 * 1024 * 1024;
constexpr int kMaximumClients = 8;
// A simulate call computes for at most this long (a batch, the longer one).
constexpr auto kSimulationLimit = std::chrono::seconds(120);
constexpr auto kBatchSimulationLimit = std::chrono::seconds(300);

template<typename T, typename Error>
T Require(fv::DiscriminatedResult<T, Error> result, const char *operation) {
    if (!result) {
        std::string message = std::string(operation) + " failed";
        if (!result.Error().diagnostic.empty()) {
            message += ": " + result.Error().diagnostic;
        }
        throw std::runtime_error(std::move(message));
    }
    return std::move(result).Value();
}

QString Q(const std::string &text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

std::string S(const QString &text) {
    const QByteArray utf8 = text.toUtf8();
    return std::string(utf8.constData(), static_cast<std::size_t>(utf8.size()));
}

double Round(double value, int digits) {
    const double scale = std::pow(10.0, digits);
    return std::round(value * scale) / scale;
}

QJsonArray Vec(const fv::Vector3 &value, int digits = 3) {
    return QJsonArray{Round(value.x, digits), Round(value.y, digits),
                      Round(value.z, digits)};
}

float Length(const fv::Vector3 &value) {
    return std::sqrt(value.x * value.x + value.y * value.y +
                     value.z * value.z);
}

struct Euler {
    double yaw = 0.0;
    double pitch = 0.0;
    double roll = 0.0;
};

// Yaw about +Y (0 = facing +Z, 90 = facing +X), pitch nose-up positive,
// roll positive when the car's local +X side is lower.
Euler ToEuler(float x, float y, float z, float w) {
    constexpr double degrees = 57.29577951308232;
    const double fx = 2.0 * (x * z + w * y);
    const double fy = 2.0 * (y * z - w * x);
    const double fz = 1.0 - 2.0 * (x * x + y * y);
    const double ry = 2.0 * (x * y + w * z);
    const double uy = 1.0 - 2.0 * (x * x + z * z);
    Euler euler;
    euler.yaw = std::atan2(fx, fz) * degrees;
    euler.pitch = std::asin(std::clamp(fy, -1.0, 1.0)) * degrees;
    euler.roll = std::atan2(-ry, uy) * degrees;
    return euler;
}

QString SettingString(const QJsonValue &value) {
    if (value.isBool()) {
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    }
    if (value.isDouble()) {
        const double number = value.toDouble();
        if (std::floor(number) == number && std::fabs(number) < 9.0e15) {
            return QString::number(static_cast<qint64>(number));
        }
        return QString::number(number, 'g', 17);
    }
    return value.toString();
}

QJsonObject OptionSettingsJson(const OptionSettings &settings) {
    QJsonObject object;
    for (const auto &[key, value] : settings) object.insert(Q(key), Q(value));
    return object;
}

QJsonObject Error(const QString &message) {
    return QJsonObject{{QStringLiteral("error"), message}};
}

QString DefaultOutputPath(const QString &prefix, const QString &suffix) {
    const QString base = McpOutputDirectory();
    QDir().mkpath(base);
    return base + '/' + prefix +
            QDateTime::currentDateTime().toString(
                    QStringLiteral("yyyyMMdd-HHmmss-zzz")) +
            suffix;
}

// A file the client named: as given when absolute, else in the MCP output
// folder (never relative to wherever the app was started from).
QString OutputPath(const QString &given) {
    const QString path = QDir::fromNativeSeparators(given.trimmed());
    if (QDir::isAbsolutePath(path)) return QDir::cleanPath(path);
    return QDir::cleanPath(QDir(McpOutputDirectory()).filePath(path));
}

int ClampedInt(const QJsonValue &value, int fallback, int minimum, int maximum) {
    if (!value.isDouble()) return fallback;
    const double number = value.toDouble();
    if (!std::isfinite(number)) return fallback;
    return static_cast<int>(std::clamp<double>(number, minimum, maximum));
}

}  // namespace

// --------------------------------------------------------------------------
// Headless simulation (runs only on simulationPool_'s single thread).
// --------------------------------------------------------------------------

// A physics state captured part-way through a run, so later scripts that share
// the same inputs up to that point can continue from it instead of from 0.
struct McpSimulationSnapshot {
    fvx::PhysicsSandboxState state;
    // Every input event at or before simTimeMs; a resumed script must match.
    std::vector<fvx::PhysicsSandboxInputEvent> prefix;
    std::int64_t simTimeMs = 0;
    std::int64_t inputTimeMs = 0;
    QJsonArray checkpointsBefore;
};

struct McpSimulationCache {
    std::string packs;
    std::string scenario;
    std::uint32_t horizonMs = 0u;
    std::optional<fvx::PhysicsSandbox> sandbox;
    std::optional<fvx::PhysicsSandboxState> initialState;
    fvx::PhysicsSandboxStateView initialView;
    std::vector<fvx::PhysicsSandboxInputEvent> canonicalInputs;
    std::int64_t raceStartMs = 0;
    std::string mapName;
    // Clones of `sandbox` for parallel batch simulation, each with its own
    // captured initial state.
    std::deque<fvx::PhysicsSandbox> workers;
    std::vector<fvx::PhysicsSandboxState> workerInitialStates;
    std::map<std::string, std::shared_ptr<const McpSimulationSnapshot>> snapshots;
    std::deque<std::string> snapshotOrder;
    std::uint64_t nextSnapshot = 1u;

    void ensureWorkers(std::size_t count) {
        while (workers.size() < count) {
            Require(sandbox->RestoreState(*initialState), "restoring initial state");
            workers.push_back(Require(fvx::ClonePhysicsSandbox(*sandbox),
                                      "cloning simulation worker"));
            workerInitialStates.push_back(
                    Require(workers.back().CaptureState(), "capturing worker state"));
        }
    }

    std::string storeSnapshot(McpSimulationSnapshot snapshot) {
        const std::string id = "snap" + std::to_string(nextSnapshot++);
        snapshots[id] = std::make_shared<const McpSimulationSnapshot>(std::move(snapshot));
        snapshotOrder.push_back(id);
        while (snapshotOrder.size() > kMaximumSnapshots) {
            snapshots.erase(snapshotOrder.front());
            snapshotOrder.pop_front();
        }
        return id;
    }

    // The sandbox is built once per scenario with a horizon capacity of at
    // least `requiredHorizonMs`; shorter requested horizons are applied as a
    // stop time by the simulation loop, so changing horizon_ms between calls
    // never rebuilds the map. Rebuilds happen only for a new scenario or a
    // horizon beyond the capacity.
    void ensure(const std::string &packsDirectory,
                const std::string &scenarioPath,
                std::uint32_t requiredHorizonMs) {
        const bool sameScenario = sandbox && packs == packsDirectory &&
                scenario == scenarioPath;
        if (sameScenario && requiredHorizonMs <= horizonMs) return;
        const std::uint32_t simulationHorizonMs =
                std::max(requiredHorizonMs, kDefaultSimulateHorizonMs);
        workers.clear();
        workerInitialStates.clear();
        // Canonical-timeline states restore across horizons on the same
        // scenario, so snapshots survive a capacity increase.
        if (!sameScenario) {
            snapshots.clear();
            snapshotOrder.clear();
        }
        sandbox.reset();
        initialState.reset();
        if (packsDirectory.empty()) {
            throw std::runtime_error("No Packs directory is configured (use load_scenario with packs_directory)");
        }
        if (scenarioPath.empty()) {
            throw std::runtime_error("No replay or challenge is loaded (use load_scenario)");
        }
        const fv::ReplayIdentity identity{scenarioPath};
        fv::AssetSource source = Require(
                fv::OpenInstalledPackDirectory(packsDirectory),
                "opening Packs directory");
        fv::AssetBytes bytes = Require(
                ReadReplayFileUtf8(scenarioPath, identity), "reading scenario");
        fvx::PhysicsSandboxOptions options;
        options.backend = fv::SimulationBackend::OptimizedCpu;
        options.tickDurationMs = kTickMs;
        options.timelineMode = fvx::PhysicsSandboxTimelineMode::Canonical;
        options.simulationHorizonMs = simulationHorizonMs;
        fvx::PhysicsSandbox created = Require(
                fvx::CreatePhysicsSandbox(std::move(source), options),
                "creating simulation");
        initialView = Require(
                created.LoadScenario({bytes.data(), bytes.size()}, identity),
                "loading scenario");
        canonicalInputs = Require(created.ReadInputs(), "reading canonical inputs");
        raceStartMs = 0;
        for (const auto &event : canonicalInputs) {
            if (event.action == fvx::PhysicsSandboxInputAction::RaceRunning) {
                raceStartMs = event.timeMs;
                break;
            }
        }
        auto name = created.ReadMapName();
        mapName = name ? std::move(name).Value() : std::string();
        initialState = Require(created.CaptureState(), "capturing initial state");
        sandbox.emplace(std::move(created));
        packs = packsDirectory;
        scenario = scenarioPath;
        horizonMs = simulationHorizonMs;
    }

    std::int64_t inputTime(std::uint64_t simulationTimeMs) const {
        return UserTimelineTimeFromSimulationTime(
                static_cast<std::int64_t>(simulationTimeMs), kTickMs,
                raceStartMs);
    }
};

namespace {

struct SimulationOptions {
    int sampleIntervalMs = 250;
    std::int64_t fromMs = 0;
    std::int64_t toMs = std::numeric_limits<std::int64_t>::max();
    QString csvPath;
    bool samples = true;
    // Stop time (simulation clock); 0 = the cache capacity.
    std::uint32_t horizonMs = 0u;
    // Gates reported per run (independent first crossings) and scored in
    // order exactly as the gate-entry search target would score them.
    std::shared_ptr<const std::vector<GateSpec>> gates;
    std::shared_ptr<const IterationEvaluator> gateEvaluator;
    GateScoring gateScoring;
    std::int64_t gateFromMs = 0;  // input clock
    // Capture a snapshot when the run reaches this input-clock time.
    std::optional<std::int64_t> snapshotAtMs;
    // Continue from this snapshot instead of the start.
    std::shared_ptr<const McpSimulationSnapshot> fromSnapshot;
    // Stops the run at the next tick when set (request cancelled, MCP off).
    const std::atomic<bool> *cancelled = nullptr;
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
};

void CheckGoingOn(const SimulationOptions &options) {
    if (options.cancelled != nullptr && options.cancelled->load(std::memory_order_relaxed)) {
        throw std::runtime_error("Cancelled");
    }
    if (std::chrono::steady_clock::now() > options.deadline) {
        throw std::runtime_error("Took too long: a simulate call computes for at most 120 s (a batch, "
                                 "300 s); use a shorter horizon_ms or fewer scripts");
    }
}

const char *const kSampleFields =
        "t_ms,x,y,z,speed_kmh,vx,vy,vz,fwd_speed,side_speed,yaw,pitch,roll,"
        "steer,gas,brake,gear,rpm,wheels_on_ground,sliding,turbo,checkpoints,"
        "lateral_contact";

bool SameEvent(const fvx::PhysicsSandboxInputEvent &left,
               const fvx::PhysicsSandboxInputEvent &right) {
    return left.timeMs == right.timeMs && left.action == right.action &&
            left.value.kind == right.value.kind &&
            left.value.switchState == right.value.switchState &&
            left.value.analog == right.value.analog;
}

QJsonObject GateCrossingJson(const McpSimulationCache &cache,
                             const GateCrossing &crossing) {
    QJsonObject entry{
            {QStringLiteral("crossed"), true},
            {QStringLiteral("valid"), crossing.valid},
            {QStringLiteral("sim_ms"), Round(crossing.timeMs, 3)},
            {QStringLiteral("t_ms"),
             Round(crossing.timeMs - static_cast<double>(cache.raceStartMs + kTickMs), 3)},
            {QStringLiteral("speed_kmh"), Round(crossing.speedKmh, 3)},
            {QStringLiteral("yaw"), Round(crossing.yawDeg, 2)},
            {QStringLiteral("lat"), Round(crossing.lateral, 3)},
            {QStringLiteral("height"), Round(crossing.height, 3)},
            {QStringLiteral("wheels"), crossing.wheels}};
    if (!crossing.valid) {
        entry.insert(QStringLiteral("penalty"), Round(crossing.penalty, 3));
        entry.insert(QStringLiteral("violations"), Q(crossing.violations));
    }
    return entry;
}

QJsonObject SimulateScript(McpSimulationCache &cache,
                           fvx::PhysicsSandbox &sandbox,
                           const fvx::PhysicsSandboxState &initialState,
                           const std::string &script,
                           const SimulationOptions &options,
                           std::optional<McpSimulationSnapshot> *snapshotOut = nullptr) {
    const InputScriptParseResult parsed = ParseInputScript(script);
    if (!parsed) {
        return Error(QStringLiteral("Script parse error: %1").arg(Q(*parsed.error)));
    }
    InputScriptBaselineResult baseline = BuildInputScriptBaseline(
            cache.canonicalInputs, parsed.commands, kTickMs);
    if (!baseline) {
        return Error(QStringLiteral("Script error: %1").arg(Q(*baseline.error)));
    }
    ConvertKeyboardSteeringToAnalog(baseline.events);
    const std::vector<fvx::PhysicsSandboxInputEvent> events = baseline.events;

    QJsonArray checkpoints;
    fvx::PhysicsSandboxStateView view;
    if (options.fromSnapshot) {
        const McpSimulationSnapshot &snapshot = *options.fromSnapshot;
        std::size_t count = 0u;
        for (const auto &event : events) {
            if (event.timeMs > snapshot.simTimeMs) break;
            if (count >= snapshot.prefix.size() || !SameEvent(event, snapshot.prefix[count])) {
                return Error(QStringLiteral(
                        "Script differs from the snapshot before its time (%1 ms on the "
                        "script clock), first at %2 ms. Inputs at or before the snapshot "
                        "time must be identical; take the snapshot earlier.")
                                     .arg(snapshot.inputTimeMs)
                                     .arg(cache.inputTime(static_cast<std::uint64_t>(
                                             std::max(0, event.timeMs)))));
            }
            ++count;
        }
        if (count != snapshot.prefix.size()) {
            return Error(QStringLiteral(
                    "Script lacks inputs the snapshot had before its time (%1 ms)")
                                 .arg(snapshot.inputTimeMs));
        }
        view = Require(sandbox.RestoreState(snapshot.state), "restoring snapshot");
        checkpoints = snapshot.checkpointsBefore;
    } else {
        view = Require(sandbox.RestoreState(initialState), "restoring initial state");
    }
    Require(sandbox.ReplaceInputs(events), "applying inputs");

    std::unique_ptr<QFile> csv;
    std::unique_ptr<QTextStream> csvStream;
    if (!options.csvPath.isEmpty()) {
        QDir().mkpath(QFileInfo(options.csvPath).absolutePath());
        csv = std::make_unique<QFile>(options.csvPath);
        if (!csv->open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            return Error(QStringLiteral("Cannot write CSV %1: %2")
                                 .arg(options.csvPath, csv->errorString()));
        }
        csvStream = std::make_unique<QTextStream>(csv.get());
        *csvStream << "sim_ms," << kSampleFields << ",respawns\n";
    }

    const std::size_t gateCount = options.gates ? options.gates->size() : 0u;
    std::vector<std::optional<GateCrossing>> firstCrossings(gateCount);
    std::unique_ptr<IterationEvaluationSession> gateSession =
            options.gateEvaluator ? options.gateEvaluator->CreateSession() : nullptr;
    std::optional<EvaluationSample> gateBest;

    std::optional<std::int64_t> snapshotSimMs;
    if (options.snapshotAtMs) {
        snapshotSimMs = SimulationTimelineTimeFromUserTime(
                *options.snapshotAtMs, kTickMs, cache.raceStartMs);
    }
    QString snapshotError;

    QJsonArray samples;
    double maxSpeed = 0.0;
    double maxSpeedAt = 0.0;
    std::uint64_t airborneTicks = 0u;
    std::uint64_t slidingTicks = 0u;
    std::uint64_t lateralContactTicks = 0u;
    const std::uint32_t horizonMs = options.horizonMs == 0u
            ? cache.horizonMs : std::min(options.horizonMs, cache.horizonMs);

    const auto record = [&](const fvx::PhysicsSandboxStateView &state) {
        const auto &car = state.car;
        const std::int64_t t = cache.inputTime(state.timeMs);
        const double speedKmh = Length(car.linearSpeed) * 3.6;
        int onGround = 0;
        for (bool contact : car.wheelContact) onGround += contact ? 1 : 0;
        const bool sliding = car.sliding;
        if (speedKmh > maxSpeed) {
            maxSpeed = speedKmh;
            maxSpeedAt = static_cast<double>(t);
        }
        if (state.tick > 0u) {
            if (onGround == 0) ++airborneTicks;
            if (sliding) ++slidingTicks;
            if (car.lateralContact) ++lateralContactTicks;
        }
        for (const auto &event : state.acceptedCheckpointEvents) {
            checkpoints.append(QJsonObject{
                    {QStringLiteral("finish"), event.finish},
                    {QStringLiteral("number"), static_cast<qint64>(event.checkpointIndex) + 1},
                    {QStringLiteral("lap"), static_cast<qint64>(event.lap)},
                    {QStringLiteral("slot"), static_cast<qint64>(event.checkpointSlot)},
                    {QStringLiteral("event_index"), static_cast<qint64>(event.eventIndex)},
                    {QStringLiteral("t_ms"), static_cast<qint64>(cache.inputTime(event.timeMs))},
                    {QStringLiteral("race_time_ms"),
                     static_cast<qint64>(event.timeMs) - cache.raceStartMs},
                    {QStringLiteral("speed_kmh"), Round(speedKmh, 2)}});
        }
        const bool wantSample = options.samples && t >= options.fromMs &&
                t <= options.toMs &&
                (options.sampleIntervalMs <= 0 ? false
                         : t % options.sampleIntervalMs == 0);
        if (!wantSample && !csvStream) return;
        const Euler euler = ToEuler(car.rotationX, car.rotationY,
                                    car.rotationZ, car.rotationW);
        const int steer = static_cast<int>(std::lround(state.steering * 65536.0f));
        const QList<double> values{
                static_cast<double>(t),
                Round(car.position.x, 3), Round(car.position.y, 3), Round(car.position.z, 3),
                Round(speedKmh, 2),
                Round(car.linearSpeed.x, 3), Round(car.linearSpeed.y, 3), Round(car.linearSpeed.z, 3),
                Round(car.localSpeed.z, 3), Round(car.localSpeed.x, 3),
                Round(euler.yaw, 2), Round(euler.pitch, 2), Round(euler.roll, 2),
                static_cast<double>(steer), Round(state.accelerate, 3), Round(state.brake, 3),
                static_cast<double>(car.gear), Round(car.rpm, 1),
                static_cast<double>(onGround), sliding ? 1.0 : 0.0,
                Round(car.turbo, 3), static_cast<double>(state.checkpointsCollected),
                car.lateralContact ? 1.0 : 0.0};
        if (wantSample) {
            QJsonArray row;
            for (double value : values) row.append(value);
            samples.append(row);
        }
        if (csvStream) {
            *csvStream << state.timeMs;
            for (double value : values) *csvStream << ',' << value;
            *csvStream << ',' << state.respawnCount << '\n';
        }
    };

    const auto observeGates = [&](const fvx::PhysicsSandboxStateView &previous,
                                  const fvx::PhysicsSandboxStateView &current) {
        if (gateCount == 0u || cache.inputTime(current.timeMs) < options.gateFromMs) return;
        for (std::size_t index = 0u; index < gateCount; ++index) {
            if (firstCrossings[index]) continue;
            firstCrossings[index] = TestGateCrossing((*options.gates)[index],
                                                     options.gateScoring,
                                                     previous, current);
        }
        if (gateSession && !gateSession->IsComplete()) {
            if (auto sample = gateSession->Observe(previous, current)) {
                if (!gateBest || options.gateEvaluator->IsBetter(*sample, *gateBest)) {
                    gateBest = std::move(sample);
                }
            }
        }
    };

    const auto captureSnapshot = [&](const fvx::PhysicsSandboxStateView &state) {
        if (!snapshotOut || !snapshotSimMs ||
            static_cast<std::int64_t>(state.timeMs) != *snapshotSimMs) {
            return;
        }
        std::vector<fvx::PhysicsSandboxInputEvent> prefix;
        for (const auto &event : events) {
            if (event.timeMs > *snapshotSimMs) break;
            prefix.push_back(event);
        }
        snapshotOut->emplace(McpSimulationSnapshot{
                Require(sandbox.CaptureState(), "capturing snapshot"),
                std::move(prefix), *snapshotSimMs, *options.snapshotAtMs,
                checkpoints});
    };

    // A car that stays (almost) still for 10 s after the start will not
    // finish; stop there instead of simulating the whole horizon.
    constexpr std::uint64_t kStallTicks = 10000u / kTickMs;
    std::uint64_t stillTicks = 0u;
    bool stalled = false;
    record(view);
    captureSnapshot(view);
    while (static_cast<std::uint64_t>(view.timeMs) < horizonMs && !view.raceCompleted) {
        CheckGoingOn(options);
        const fvx::PhysicsSandboxStateView previous = view;
        view = Require(sandbox.AdvanceTicks(1u), "simulating");
        record(view);
        observeGates(previous, view);
        captureSnapshot(view);
        const bool raceStarted = static_cast<std::int64_t>(view.timeMs) > cache.raceStartMs + 1000;
        stillTicks = raceStarted && Length(view.car.linearSpeed) < 0.5f ? stillTicks + 1u : 0u;
        if (stillTicks >= kStallTicks) {
            stalled = true;
            break;
        }
    }
    if (csvStream) csvStream->flush();
    if (snapshotOut && snapshotSimMs && !snapshotOut->has_value()) {
        snapshotError = QStringLiteral("The run ended before the snapshot time");
    }

    QJsonObject result{
            {QStringLiteral("finished"), view.raceCompleted},
            {QStringLiteral("checkpoints_collected"), static_cast<qint64>(view.checkpointsCollected)},
            {QStringLiteral("checkpoints_total"), static_cast<qint64>(view.checkpointsTotal)},
            {QStringLiteral("laps"), QStringLiteral("%1/%2").arg(view.completedLaps).arg(view.totalLaps)},
            {QStringLiteral("respawns"), static_cast<qint64>(view.respawnCount)},
            {QStringLiteral("simulated_until_ms"), static_cast<qint64>(cache.inputTime(view.timeMs))},
            {QStringLiteral("max_speed_kmh"), Round(maxSpeed, 2)},
            {QStringLiteral("max_speed_at_ms"), maxSpeedAt},
            {QStringLiteral("airborne_ms"), static_cast<qint64>(airborneTicks * kTickMs)},
            {QStringLiteral("sliding_ms"), static_cast<qint64>(slidingTicks * kTickMs)},
            {QStringLiteral("lateral_contact_ms"), static_cast<qint64>(lateralContactTicks * kTickMs)},
            {QStringLiteral("checkpoint_times"), checkpoints}};
    if (options.fromSnapshot) {
        result.insert(QStringLiteral("resumed_from_ms"), static_cast<qint64>(options.fromSnapshot->inputTimeMs));
    }
    if (view.finishTimeMs) {
        result.insert(QStringLiteral("finish_time_ms"), static_cast<qint64>(*view.finishTimeMs));
    }
    if (view.finishTime && view.finishTime->IsValid()) {
        result.insert(QStringLiteral("finish_time_precise_ms"),
                      static_cast<double>(view.finishTime->estimatedNs) / 1.0e6);
    }
    if (view.stuntsScore) {
        result.insert(QStringLiteral("stunts_score"), static_cast<qint64>(*view.stuntsScore));
    }
    if (!view.raceCompleted) {
        result.insert(QStringLiteral("stalled"), stalled);
        result.insert(QStringLiteral("end_position"), Vec(view.car.position));
        result.insert(QStringLiteral("end_speed_kmh"), Round(Length(view.car.linearSpeed) * 3.6, 2));
    }
    if (gateCount > 0u) {
        QJsonArray gateReports;
        for (std::size_t index = 0u; index < gateCount; ++index) {
            QJsonObject entry = firstCrossings[index]
                    ? GateCrossingJson(cache, *firstCrossings[index])
                    : QJsonObject{{QStringLiteral("crossed"), false}};
            entry.insert(QStringLiteral("gate"), static_cast<qint64>(index));
            gateReports.append(entry);
        }
        result.insert(QStringLiteral("gates"), gateReports);
        if (gateBest) {
            const bool valid = gateBest->score < kGateMissScoreBase;
            result.insert(QStringLiteral("gate_target"),
                          QJsonObject{{QStringLiteral("valid"), valid},
                                      {QStringLiteral("score"), Round(gateBest->score, 4)},
                                      {QStringLiteral("sim_ms"), Round(gateBest->timeMs, 3)},
                                      {QStringLiteral("t_ms"),
                                       Round(gateBest->timeMs -
                                                     static_cast<double>(cache.raceStartMs + kTickMs), 3)},
                                      {QStringLiteral("description"), Q(gateBest->description)}});
        }
    }
    if (!snapshotError.isEmpty()) result.insert(QStringLiteral("snapshot_error"), snapshotError);
    if (options.samples && options.sampleIntervalMs > 0) {
        result.insert(QStringLiteral("sample_fields"), QString::fromLatin1(kSampleFields));
        result.insert(QStringLiteral("samples"), samples);
    }
    if (csv) result.insert(QStringLiteral("csv_path"), QDir::toNativeSeparators(options.csvPath));
    return result;
}

QJsonObject SimulateScript(McpSimulationCache &cache,
                           const std::string &script,
                           const SimulationOptions &options) {
    return SimulateScript(cache, *cache.sandbox, *cache.initialState, script, options);
}

// Runs `scripts` on up to `threads` cloned sandboxes in parallel. Results keep
// the input order.
QJsonArray SimulateBatch(McpSimulationCache &cache,
                         const std::vector<std::string> &scripts,
                         const SimulationOptions &options,
                         int threads) {
    const std::size_t workerCount = static_cast<std::size_t>(std::clamp(
            threads, 1, static_cast<int>(std::min<std::size_t>(scripts.size(), kMaximumSimulationThreads))));
    cache.ensureWorkers(workerCount);
    std::vector<QJsonObject> results(scripts.size());
    std::atomic<std::size_t> next{0u};
    const auto work = [&](std::size_t worker) {
        for (;;) {
            const std::size_t index = next.fetch_add(1u);
            if (index >= scripts.size()) return;
            try {
                results[index] = SimulateScript(cache, cache.workers[worker],
                                                cache.workerInitialStates[worker],
                                                scripts[index], options);
            } catch (const std::exception &error) {
                results[index] = Error(QString::fromUtf8(error.what()));
            }
        }
    };
    std::vector<std::thread> pool;
    for (std::size_t worker = 1u; worker < workerCount; ++worker) {
        pool.emplace_back(work, worker);
    }
    work(0u);
    for (auto &thread : pool) thread.join();
    QJsonArray array;
    for (std::size_t index = 0u; index < results.size(); ++index) {
        QJsonObject entry = results[index];
        entry.insert(QStringLiteral("index"), static_cast<qint64>(index));
        array.append(entry);
    }
    return array;
}

QJsonObject ScenarioInfo(const McpSimulationCache &cache) {
    const auto &view = cache.initialView;
    const Euler euler = ToEuler(view.car.rotationX, view.car.rotationY,
                                view.car.rotationZ, view.car.rotationW);
    return QJsonObject{
            {QStringLiteral("map_name"), Q(cache.mapName)},
            {QStringLiteral("checkpoints"), static_cast<qint64>(view.checkpointsTotal)},
            {QStringLiteral("laps"), static_cast<qint64>(view.totalLaps)},
            {QStringLiteral("environment"), static_cast<int>(view.mapEnvironment)},
            {QStringLiteral("vehicle"), static_cast<int>(view.vehicleModel)},
            {QStringLiteral("start_position"), Vec(view.car.position)},
            {QStringLiteral("start_yaw_degrees"), Round(euler.yaw, 2)}};
}

fv::Vector3 Transform(const fvx::PhysicsSandboxTransform &transform,
                      float x, float y, float z) {
    return {transform.basisX.x * x + transform.basisY.x * y + transform.basisZ.x * z +
                    transform.translation.x,
            transform.basisX.y * x + transform.basisY.y * y + transform.basisZ.y * z +
                    transform.translation.y,
            transform.basisX.z * x + transform.basisY.z * y + transform.basisZ.z * z +
                    transform.translation.z};
}

QString BlockKind(const QString &name) {
    const QString lower = name.toLower();
    const bool start = lower.contains(QStringLiteral("start"));
    const bool finish = lower.contains(QStringLiteral("finish"));
    if (lower.contains(QStringLiteral("multilap")) || (start && finish)) return QStringLiteral("start_finish");
    if (finish) return QStringLiteral("finish");
    if (start) return QStringLiteral("start");
    if (lower.contains(QStringLiteral("checkpoint"))) return QStringLiteral("checkpoint");
    if (lower.contains(QStringLiteral("turbo"))) return QStringLiteral("turbo");
    return {};
}

QJsonObject MapLayout(McpSimulationCache &cache, bool includeBlocks,
                      int maxBlocks) {
    const fvx::PhysicsSandboxRenderSceneHandle scene = Require(
            cache.sandbox->ReadRenderScene(), "reading map scene");
    struct Block {
        QString name;
        fv::Vector3 low{std::numeric_limits<float>::max(),
                        std::numeric_limits<float>::max(),
                        std::numeric_limits<float>::max()};
        fv::Vector3 high{-std::numeric_limits<float>::max(),
                         -std::numeric_limits<float>::max(),
                         -std::numeric_limits<float>::max()};
        fv::Vector3 origin{};
        double yaw = 0.0;
        bool trigger = false;
    };
    QHash<quint64, Block> blocks;
    QList<quint64> order;
    for (const auto &instance : scene->instances) {
        const bool trigger =
                instance.purpose == fvx::PhysicsSandboxScenePurpose::CheckpointTrigger;
        if ((instance.purpose != fvx::PhysicsSandboxScenePurpose::PlacedBlock && !trigger) ||
            instance.meshIndex >= scene->meshes.size()) {
            continue;
        }
        const auto &provenance = instance.provenance;
        const quint64 key = provenance.placementIdentity
                ? *provenance.placementIdentity
                : (provenance.blockInstanceId ? *provenance.blockInstanceId : instance.id);
        auto found = blocks.find(key);
        if (found == blocks.end()) {
            Block block;
            block.name = Q(provenance.blockName);
            block.origin = instance.worldTransform.translation;
            const auto &t = instance.worldTransform;
            block.yaw = std::atan2(t.basisZ.x, t.basisZ.z) * 57.29577951308232;
            found = blocks.insert(key, block);
            order.append(key);
        }
        Block &block = found.value();
        block.trigger = block.trigger || trigger;
        const auto &mesh = scene->meshes[instance.meshIndex];
        for (int corner = 0; corner < 8; ++corner) {
            const fv::Vector3 point = Transform(
                    instance.worldTransform,
                    (corner & 1) ? mesh.boundsMax.x : mesh.boundsMin.x,
                    (corner & 2) ? mesh.boundsMax.y : mesh.boundsMin.y,
                    (corner & 4) ? mesh.boundsMax.z : mesh.boundsMin.z);
            block.low = {std::min(block.low.x, point.x), std::min(block.low.y, point.y),
                         std::min(block.low.z, point.z)};
            block.high = {std::max(block.high.x, point.x), std::max(block.high.y, point.y),
                          std::max(block.high.z, point.z)};
        }
    }
    const auto describe = [](const Block &block, const QString &kind) {
        QJsonObject entry{
                {QStringLiteral("block"), block.name},
                {QStringLiteral("center"),
                 QJsonArray{Round((block.low.x + block.high.x) / 2, 1),
                            Round((block.low.y + block.high.y) / 2, 1),
                            Round((block.low.z + block.high.z) / 2, 1)}},
                {QStringLiteral("size"),
                 QJsonArray{Round(block.high.x - block.low.x, 1),
                            Round(block.high.y - block.low.y, 1),
                            Round(block.high.z - block.low.z, 1)}},
                {QStringLiteral("yaw"), Round(block.yaw, 1)}};
        if (!kind.isEmpty()) entry.insert(QStringLiteral("kind"), kind);
        return entry;
    };
    QJsonArray keyBlocks;
    QJsonArray allBlocks;
    for (const quint64 key : std::as_const(order)) {
        const Block &block = blocks[key];
        const QString kind = block.trigger && BlockKind(block.name).isEmpty()
                ? QStringLiteral("checkpoint") : BlockKind(block.name);
        if (!kind.isEmpty()) keyBlocks.append(describe(block, kind));
        if (includeBlocks && allBlocks.size() < maxBlocks) allBlocks.append(describe(block, kind));
    }
    QJsonObject result = ScenarioInfo(cache);
    result.insert(QStringLiteral("key_blocks"), keyBlocks);
    result.insert(QStringLiteral("block_count"), static_cast<qint64>(order.size()));
    if (includeBlocks) result.insert(QStringLiteral("blocks"), allBlocks);
    return result;
}

}  // namespace

// --------------------------------------------------------------------------
// Robustness.
// --------------------------------------------------------------------------

namespace {

constexpr char kEnabledSetting[] = "mcp/enabled";

QString IdKey(const QJsonValue &id) {
    return QString::fromUtf8(QJsonDocument(QJsonArray{id}).toJson(QJsonDocument::Compact));
}

// Runs `work`, turning anything it throws into a log line: a tool's mistake
// never reaches Qt's event loop (which would end the app).
template<typename Work>
void Guarded(const char *where, Work &&work) {
    try {
        work();
    } catch (const std::exception &error) {
        diagnostics::Log("mcp: %s failed: %s", where, error.what());
    } catch (...) {
        diagnostics::Log("mcp: %s failed", where);
    }
}

}  // namespace

// --------------------------------------------------------------------------
// Transport.
// --------------------------------------------------------------------------

McpBridge::McpBridge(SearchController &controller, QQmlApplicationEngine *engine, bool sessionEnabled,
                     QObject *parent)
    : QObject(parent), controller_(controller), engine_(engine) {
    enabled_ = sessionEnabled ||
            AppSettings::Instance().value(QLatin1String(kEnabledSetting), false).toBool();
    retry_.setSingleShot(true);
    retry_.setInterval(3000);
    connect(&retry_, &QTimer::timeout, this, [this] { Guarded("listening", [this] { startListening(); }); });
    // Simulation clones are dropped two minutes after the last simulation.
    workersIdle_.setSingleShot(true);
    workersIdle_.setInterval(120000);
    connect(&workersIdle_, &QTimer::timeout, this, [this] {
        if (!simulationPool_) return;
        auto cache = cache_;
        simulationPool_->start([cache] {
            cache->workers.clear();
            cache->workerInitialStates.clear();
        });
    });
    apply();
}

McpBridge::~McpBridge() {
    stopListening();
    cancelJobs();
    if (simulationPool_) {
        simulationPool_->clear();
        // a simulation stops at its next tick; never hold the app's exit
        // on one that does not
        if (!simulationPool_->waitForDone(3000)) static_cast<void>(simulationPool_.release());
    }
}

void McpBridge::setEnabled(bool enabled) {
    AppSettings::Instance().setValue(QLatin1String(kEnabledSetting), enabled);
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    diagnostics::Log("mcp: turned %s", enabled ? "on" : "off");
    apply();
    emit changed();
}

void McpBridge::apply() {
    if (enabled_) {
        if (!simulationPool_) {
            simulationPool_ = std::make_unique<QThreadPool>();
            simulationPool_->setMaxThreadCount(1);
            simulationPool_->setExpiryTimeout(30000);
            cache_ = std::make_shared<McpSimulationCache>();
        }
        Guarded("listening", [this] { startListening(); });
        return;
    }
    stopListening();
    retry_.stop();
    workersIdle_.stop();
    cancelJobs();
    if (simulationPool_) {
        // the thread ends with its simulation; nothing is kept while off
        simulationPool_->clear();
        if (!simulationPool_->waitForDone(3000)) static_cast<void>(simulationPool_.release());
        simulationPool_.reset();
    }
    cache_.reset();
    jobs_.clear();
    setStatus(tr("Off"));
}

bool McpBridge::listening() const {
    return server_ != nullptr && server_->isListening();
}

void McpBridge::setStatus(const QString &status) {
    if (status_ == status) return;
    status_ = status;
    emit changed();
}

void McpBridge::startListening() {
    if (!enabled_ || listening()) return;
    const QString name = McpSocketName();
    if (server_ == nullptr) {
        server_ = new QLocalServer(this);
        server_->setSocketOptions(QLocalServer::UserAccessOption);
        server_->setMaxPendingConnections(kMaximumClients);
        connect(server_, &QLocalServer::newConnection, this,
                [this] { Guarded("accepting a client", [this] { acceptConnections(); }); });
    }
    // Another ForeverTAS of this user answers there: leave it be and take
    // over when it closes. (Listening would not fail: Qt makes the socket
    // elsewhere and renames it over the other's, and Windows lets two servers
    // share a pipe name.)
    {
        QLocalSocket probe;
        probe.connectToServer(name);
        if (probe.waitForConnected(300)) {
            probe.abort();
            setStatus(tr("Another ForeverTAS window answers MCP clients; this one takes over when it closes."));
            retry_.start();
            return;
        }
    }
    // Nothing answers: a socket a crash left is in the way, if anything.
    QLocalServer::removeServer(name);
    if (!server_->listen(name)) {
        diagnostics::Log("mcp: cannot listen: %s", qUtf8Printable(server_->errorString()));
        setStatus(tr("Not reachable yet (%1); trying again.").arg(server_->errorString()));
        retry_.start();
        return;
    }
    diagnostics::Log("mcp: listening on %s", qUtf8Printable(server_->fullServerName()));
    setStatus(tr("On: MCP clients can drive ForeverTAS."));
}

void McpBridge::stopListening() {
    const auto sockets = buffers_.keys();
    buffers_.clear();
    for (QLocalSocket *socket : sockets) {
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    }
    if (server_ != nullptr) {
        server_->close();
        server_->deleteLater();
        server_ = nullptr;
    }
    emit changed();
}

void McpBridge::acceptConnections() {
    while (server_ != nullptr && server_->hasPendingConnections()) {
        QLocalSocket *socket = server_->nextPendingConnection();
        if (socket == nullptr) break;
        if (buffers_.size() >= kMaximumClients) {
            diagnostics::Log("mcp: refused a client (%d already)", kMaximumClients);
            socket->abort();
            socket->deleteLater();
            continue;
        }
        buffers_.insert(socket, {});
        diagnostics::Log("mcp: client connected (%lld)", static_cast<long long>(buffers_.size()));
        connect(socket, &QLocalSocket::readyRead, this,
                [this, socket] { Guarded("reading a client", [this, socket] { readSocket(socket); }); });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
            if (buffers_.remove(socket) > 0) {
                diagnostics::Log("mcp: client disconnected (%lld)", static_cast<long long>(buffers_.size()));
                emit changed();
            }
            socket->deleteLater();
        });
        emit changed();
    }
}

void McpBridge::readSocket(QLocalSocket *socket) {
    const auto found = buffers_.find(socket);
    if (found == buffers_.end()) return;
    found.value().append(socket->readAll());
    QByteArray &buffer = found.value();
    if (buffer.size() > kMaximumMessageBytes && buffer.indexOf('\n') < 0) {
        diagnostics::Log("mcp: a client sent a message over %lld bytes; disconnected",
                         static_cast<long long>(kMaximumMessageBytes));
        socket->abort();
        return;
    }
    const QPointer<QLocalSocket> guarded(socket);
    qsizetype newline = -1;
    while ((newline = buffer.indexOf('\n')) >= 0) {
        const QByteArray line = buffer.left(newline).trimmed();
        buffer.remove(0, newline + 1);
        if (line.isEmpty()) continue;
        QJsonParseError error{};
        const QJsonDocument document = QJsonDocument::fromJson(line, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            socket->write(QJsonDocument(McpErrorResponse(QJsonValue::Null, -32700, QStringLiteral("Parse error")))
                                  .toJson(QJsonDocument::Compact) + '\n');
            continue;
        }
        handleMessage(guarded, document.object());
        // the message may have closed the socket or turned MCP off
        if (!guarded || !buffers_.contains(socket)) return;
    }
}

void McpBridge::handleMessage(const QPointer<QLocalSocket> &socket, const QJsonObject &message) {
    const QString method = message.value(QStringLiteral("method")).toString();
    const QJsonValue id = message.value(QStringLiteral("id"));
    const QJsonObject params = message.value(QStringLiteral("params")).toObject();
    if (!message.contains(QStringLiteral("id"))) {
        // a client giving up on a request: its simulation stops
        if (method == QLatin1String("notifications/cancelled")) {
            const auto job = jobs_.constFind(IdKey(params.value(QStringLiteral("requestId"))));
            if (job != jobs_.constEnd()) job.value()->store(true);
        }
        return;
    }
    const auto send = [socket](const QJsonObject &response) {
        if (!socket || socket->state() != QLocalSocket::ConnectedState) return;
        socket->write(QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n');
        socket->flush();
    };
    if (method == QLatin1String("initialize")) {
        send(McpResponse(id, McpInitializeResult(params, QCoreApplication::applicationVersion())));
    } else if (method == QLatin1String("ping")) {
        send(McpResponse(id, {}));
    } else if (method == QLatin1String("tools/list")) {
        send(McpResponse(id, {{QStringLiteral("tools"), McpToolCatalog()}}));
    } else if (method == QLatin1String("tools/call")) {
        const QString name = params.value(QStringLiteral("name")).toString();
        if (!params.value(QStringLiteral("arguments")).isUndefined() &&
            !params.value(QStringLiteral("arguments")).isObject()) {
            send(McpErrorResponse(id, -32602, QStringLiteral("'arguments' must be an object")));
            return;
        }
        // one answer, whatever happens after
        auto answered = std::make_shared<bool>(false);
        auto started = std::make_shared<QElapsedTimer>();
        started->start();
        callTool(name, params.value(QStringLiteral("arguments")).toObject(), IdKey(id),
                 [send, id, name, answered, started](const QJsonObject &payload, bool isError) {
                     if (*answered) return;
                     *answered = true;
                     if (isError) {
                         diagnostics::Log("mcp: %s failed after %lld ms: %s", qUtf8Printable(name),
                                          static_cast<long long>(started->elapsed()),
                                          qUtf8Printable(payload.value(QStringLiteral("error")).toString().left(300)));
                     }
                     send(McpResponse(id, McpToolResult(payload, isError)));
                 });
    } else {
        send(McpErrorResponse(id, -32601, QStringLiteral("Method not found: %1").arg(method)));
    }
}

void McpBridge::callTool(const QString &name, const QJsonObject &arguments, const QString &requestKey,
                         Reply reply) {
    using Handler = void (McpBridge::*)(const QJsonObject &, Reply);
    static const QHash<QString, Handler> handlers{
            {QStringLiteral("get_status"), &McpBridge::toolGetStatus},
            {QStringLiteral("list_components"), &McpBridge::toolListComponents},
            {QStringLiteral("get_documentation"), &McpBridge::toolGetDocumentation},
            {QStringLiteral("load_scenario"), &McpBridge::toolLoadScenario},
            {QStringLiteral("get_map_layout"), &McpBridge::toolGetMapLayout},
            {QStringLiteral("get_input_script"), &McpBridge::toolGetInputScript},
            {QStringLiteral("set_input_script"), &McpBridge::toolSetInputScript},
            {QStringLiteral("use_best_as_base"), &McpBridge::toolUseBestAsBase},
            {QStringLiteral("simulate"), &McpBridge::toolSimulate},
            {QStringLiteral("configure_search"), &McpBridge::toolConfigureSearch},
            {QStringLiteral("evaluate_base"), &McpBridge::toolEvaluateBase},
            {QStringLiteral("start_search"), &McpBridge::toolStartSearch},
            {QStringLiteral("wait_for_search"), &McpBridge::toolWaitForSearch},
            {QStringLiteral("stop_search"), &McpBridge::toolStopSearch},
            {QStringLiteral("get_best_result"), &McpBridge::toolGetBestResult},
            {QStringLiteral("get_search_history"), &McpBridge::toolGetSearchHistory},
            {QStringLiteral("save_input_script"), &McpBridge::toolSaveInputScript},
            {QStringLiteral("capture_window"), &McpBridge::toolCaptureWindow}};
    const auto handler = handlers.constFind(name);
    if (handler == handlers.constEnd()) {
        reply(Error(QStringLiteral("Unknown tool: %1").arg(name)), true);
        return;
    }
    // Simulating tools find their request's cancel flag here.
    currentRequest_ = requestKey;
    try {
        (this->*handler.value())(arguments, reply);
    } catch (const std::exception &error) {
        reply(Error(QString::fromUtf8(error.what())), true);
    } catch (...) {
        reply(Error(QStringLiteral("Unexpected failure in %1").arg(name)), true);
    }
    currentRequest_.clear();
}

void McpBridge::waitUntil(std::function<bool()> done, int timeoutMs, std::function<void(bool)> finish) {
    if (done()) {
        finish(false);
        return;
    }
    auto *timer = new QTimer(this);
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    timer->setInterval(100);
    connect(timer, &QTimer::timeout, this,
            [this, timer, clock, timeoutMs, done = std::move(done), finish = std::move(finish)] {
                Guarded("waiting", [&] {
                    const bool satisfied = done();
                    if (!satisfied && clock->elapsed() < timeoutMs && enabled_) return;
                    timer->stop();
                    timer->deleteLater();
                    finish(!satisfied);
                });
            });
    timer->start();
}

void McpBridge::runSimulationJob(const QString &requestKey, SimulationWork work, Reply reply) {
    if (!simulationPool_ || !cache_) {
        reply(Error(QStringLiteral("MCP is off")), true);
        return;
    }
    workersIdle_.stop();
    // the flag a cancel, MCP turned off or the app closing sets
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    const QString key = requestKey.isEmpty() ? QStringLiteral("internal-%1").arg(++internalJobs_) : requestKey;
    jobs_.insert(key, cancelled);
    const QPointer<McpBridge> self(this);
    auto cache = cache_;
    simulationPool_->start([self, cache, cancelled, key, work = std::move(work), reply = std::move(reply)]() mutable {
        QJsonObject payload;
        bool failed = false;
        try {
            payload = work(*cache, *cancelled);
            failed = payload.size() == 1 && payload.contains(QStringLiteral("error"));
        } catch (const std::exception &error) {
            payload = Error(QString::fromUtf8(error.what()));
            failed = true;
        } catch (...) {
            payload = Error(QStringLiteral("Unexpected simulation failure"));
            failed = true;
        }
        if (!self) return;
        QMetaObject::invokeMethod(
                self.data(),
                [self, key, reply = std::move(reply), payload = std::move(payload), failed] {
                    if (!self) return;
                    Guarded("answering a simulation", [&] {
                        self->jobs_.remove(key);
                        self->workersIdle_.start();
                        reply(payload, failed);
                    });
                },
                Qt::QueuedConnection);
    });
}

void McpBridge::cancelJobs() {
    for (const auto &flag : std::as_const(jobs_)) flag->store(true);
}

// --------------------------------------------------------------------------
// State helpers.
// --------------------------------------------------------------------------

QString McpBridge::clientCommand() const {
    const QString appImage = qEnvironmentVariable("APPIMAGE");
    if (!appImage.isEmpty()) return QDir::toNativeSeparators(appImage) + QStringLiteral(" --mcp-stdio");
    const QDir here(QCoreApplication::applicationDirPath());
#if defined(Q_OS_WIN)
    if (QFileInfo(here.filePath(QStringLiteral("forevertas-mcp.exe"))).isFile()) {
        return QDir::toNativeSeparators(here.filePath(QStringLiteral("forevertas-mcp.exe")));
    }
#endif
    return QDir::toNativeSeparators(QCoreApplication::applicationFilePath()) + QStringLiteral(" --mcp-stdio");
}

QString McpBridge::clientConfig() const {
    const QString command = clientCommand();
    const bool separate = !command.endsWith(QStringLiteral(" --mcp-stdio"));
    const QString program = separate ? command : command.left(command.size() - 12);
    QJsonObject server{{QStringLiteral("command"), program}};
    if (!separate) server.insert(QStringLiteral("args"), QJsonArray{QStringLiteral("--mcp-stdio")});
    return QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("mcpServers"),
                                       QJsonObject{{QStringLiteral("forevertas"), server}}}})
                    .toJson(QJsonDocument::Indented));
}

std::uint32_t McpBridge::configuredHorizonMs() const {
    bool ok = false;
    const qint64 value = controller_.simulationHorizonMs().toLongLong(&ok);
    if (!ok || value <= 0) return kDefaultSimulationHorizonMs;
    return static_cast<std::uint32_t>(std::min<qint64>(value, kMaximumSimulationHorizonMs));
}

QJsonObject McpBridge::configurationObject() const {
    QJsonArray backends;
    for (const QVariant &option : controller_.simulationBackendOptions()) {
        QJsonObject backend = QJsonObject::fromVariantMap(option.toMap());
        const QString id = backend.value(QStringLiteral("id")).toString();
        if (id == QLatin1String("cuda")) {
            backend.insert(QStringLiteral("available"), controller_.cudaAvailable());
            backend.insert(QStringLiteral("status"), controller_.cudaStatusText());
        } else if (id == QLatin1String("hip")) {
            backend.insert(QStringLiteral("available"), controller_.hipAvailable());
            backend.insert(QStringLiteral("status"), controller_.hipStatusText());
        } else if (id == QLatin1String("gpus")) {
            backend.insert(QStringLiteral("available"), controller_.multipleGpus());
        } else if (!backend.contains(QStringLiteral("available"))) {
            backend.insert(QStringLiteral("available"), true);
        }
        backends.append(backend);
    }
    return QJsonObject{
            {QStringLiteral("backend"), controller_.simulationBackendId()},
            {QStringLiteral("backend_options"), backends},
            {QStringLiteral("approximate_physics"), controller_.approximatePhysics()},
            {QStringLiteral("approximate_physics_available"), controller_.approximatePhysicsAvailable()},
            {QStringLiteral("simulation_horizon_ms"), controller_.simulationHorizonMs()},
            {QStringLiteral("required_horizon_ms"), static_cast<qint64>(controller_.requiredSimulationHorizonMs())},
            {QStringLiteral("condition_script"), controller_.conditionScript()},
            {QStringLiteral("algorithm"),
             QJsonObject{{QStringLiteral("id"), controller_.searchAlgorithmId()},
                         {QStringLiteral("settings"),
                          QJsonObject::fromVariantMap(controller_.searchAlgorithmSettings())}}},
            {QStringLiteral("modifiers"), QJsonArray::fromVariantList(controller_.modifierPasses())},
            {QStringLiteral("target"),
             QJsonObject{{QStringLiteral("id"), controller_.evaluationTargetId()},
                         {QStringLiteral("settings"),
                          QJsonObject::fromVariantMap(controller_.evaluationTargetSettings())}}},
            {QStringLiteral("cpu_workers"), controller_.cpuWorkerCount()},
            {QStringLiteral("cuda_batch_size"), controller_.cudaParallelSampleCount()},
            {QStringLiteral("cuda_calibration"), controller_.cudaCalibrationEnabled()},
            {QStringLiteral("hip_batch_size"), controller_.hipParallelSampleCount()},
            {QStringLiteral("hip_calibration"), controller_.hipCalibrationEnabled()},
            {QStringLiteral("randomize_seeds"), controller_.randomizeSeedsOnStart()},
            {QStringLiteral("autorestart"),
             QJsonObject{{QStringLiteral("mode"), controller_.autoRestartMode()},
                         {QStringLiteral("duration"), controller_.autoRestartDuration()},
                         {QStringLiteral("attempts"), controller_.autoRestartAttempts()}}}};
}

QJsonObject McpBridge::searchProgressObject() const {
    return QJsonObject{
            {QStringLiteral("running"), controller_.running()},
            {QStringLiteral("stopping"), controller_.stopping()},
            {QStringLiteral("status_text"), controller_.statusText()},
            {QStringLiteral("attempts"), controller_.iterationCountRawText()},
            {QStringLiteral("attempts_per_second"), controller_.throughputText()},
            {QStringLiteral("elapsed"), controller_.elapsedText()},
            {QStringLiteral("best_summary"), controller_.resultText()},
            {QStringLiteral("failure"), QJsonObject::fromVariantMap(controller_.failureDiagnostic())}};
}

QJsonObject McpBridge::statusObject(bool includeInputs) const {
    const QString script = controller_.baseInputScript();
    QJsonObject status{
            {QStringLiteral("version"), QCoreApplication::applicationVersion()},
            {QStringLiteral("packs_directory"), controller_.packsDirectory()},
            {QStringLiteral("auto_detected_packs_directory"), controller_.autoDetectedPacksDirectory()},
            {QStringLiteral("scenario_path"), controller_.replayPath()},
            {QStringLiteral("base_script_lines"), script.isEmpty() ? 0 : static_cast<int>(script.count('\n') + 1)},
            {QStringLiteral("base_script_error"), controller_.baseInputScriptError()},
            {QStringLiteral("replay_input_status"), controller_.replayInputStatusText()},
            {QStringLiteral("configuration"), configurationObject()},
            {QStringLiteral("validation_message"), controller_.validationMessage()},
            {QStringLiteral("can_start"), controller_.canStart()},
            {QStringLiteral("search"), searchProgressObject()},
            {QStringLiteral("output_directory"), QDir::toNativeSeparators(McpOutputDirectory())},
            {QStringLiteral("base_evaluation"),
             QJsonObject{{QStringLiteral("text"), controller_.baseEvaluationText()},
                         {QStringLiteral("result"), QJsonObject::fromVariantMap(controller_.baseEvaluationResult())}}}};
    if (includeInputs) {
        status.insert(QStringLiteral("base_script"), script);
        status.insert(QStringLiteral("best_script"), controller_.bestInputsText());
    }
    return status;
}

// --------------------------------------------------------------------------
// Tools.
// --------------------------------------------------------------------------

namespace {

// A string argument, or an error naming it when it is there but not a string.
std::optional<QString> StringArgument(const QJsonObject &arguments, const char *name, QString *error) {
    const QJsonValue value = arguments.value(QLatin1String(name));
    if (value.isUndefined() || value.isNull()) return std::nullopt;
    if (!value.isString()) {
        *error = QStringLiteral("'%1' must be a string").arg(QLatin1String(name));
        return std::nullopt;
    }
    return value.toString();
}

}  // namespace

void McpBridge::toolGetStatus(const QJsonObject &arguments, Reply reply) {
    reply(statusObject(arguments.value(QStringLiteral("include_inputs")).toBool()), false);
}

void McpBridge::toolListComponents(const QJsonObject &arguments, Reply reply) {
    QJsonArray algorithms;
    for (const auto &registration : SearchAlgorithmRegistry()) {
        algorithms.append(QJsonObject{
                {QStringLiteral("id"), Q(registration.id)},
                {QStringLiteral("name"), Q(registration.displayName)},
                {QStringLiteral("gpu"), registration.supportsGpuBackends},
                {QStringLiteral("multi_threaded_cpu"), registration.supportsMultiThreadedCpu},
                {QStringLiteral("default_settings"), OptionSettingsJson(registration.defaultSettings)}});
    }
    QJsonArray modifiers;
    for (const auto &registration : ModifierRegistry()) {
        modifiers.append(QJsonObject{
                {QStringLiteral("id"), Q(registration.id)},
                {QStringLiteral("name"), Q(registration.displayName)},
                {QStringLiteral("default_settings"), OptionSettingsJson(registration.defaultSettings)}});
    }
    QJsonArray targets;
    for (const auto &registration : EvaluationTargetRegistry()) {
        QJsonObject target{{QStringLiteral("id"), Q(registration.id)},
                           {QStringLiteral("name"), Q(registration.displayName)},
                           {QStringLiteral("default_settings"), OptionSettingsJson(registration.defaultSettings)}};
        if (registration.id == kGateEntryEvaluationId) {
            target.insert(QStringLiteral("cpu_only"), true);
        }
        targets.append(target);
    }
    QJsonObject result{{QStringLiteral("algorithms"), algorithms},
                       {QStringLiteral("modifiers"), modifiers},
                       {QStringLiteral("targets"), targets},
                       {QStringLiteral("notes"),
                        QStringLiteral("Modifier settings ending in TimeMs use the input timeline "
                                       "(script clock). maxTimeMode='horizon' (where supported) "
                                       "follows the search end. Steering values in settings are "
                                       "normalised -1..1; scripts use -65536..65536. cpu_only targets "
                                       "need a CPU backend and exact physics.")}};
    if (arguments.value(QStringLiteral("include_condition_reference")).toBool()) {
        result.insert(QStringLiteral("condition_reference"),
                      QJsonArray::fromVariantList(controller_.conditionReference()));
    }
    reply(result, false);
}

void McpBridge::toolGetDocumentation(const QJsonObject &arguments, Reply reply) {
    static const QHash<QString, QString> files{
            {QStringLiteral("mcp"), QStringLiteral("MCP.md")},
            {QStringLiteral("readme"), QStringLiteral("README.md")},
            {QStringLiteral("search_components"), QStringLiteral("SEARCH_COMPONENTS.md")},
            {QStringLiteral("conditions"), QStringLiteral("CONDITIONS.md")},
            {QStringLiteral("checkpoint_time"), QStringLiteral("CHECKPOINT_TIME.md")},
            {QStringLiteral("input_insertion"), QStringLiteral("INPUT_INSERTION.md")},
            {QStringLiteral("gpu_backends"), QStringLiteral("GPU_BACKENDS.md")},
            {QStringLiteral("gpu_safety"), QStringLiteral("GPU_SAFETY.md")},
            {QStringLiteral("gates"), QStringLiteral("GATES.md")}};
    const QString topic = arguments.value(QStringLiteral("topic")).toString();
    const auto file = files.constFind(topic);
    if (file == files.constEnd()) {
        QStringList topics = files.keys();
        topics.sort();
        reply(Error(QStringLiteral("Unknown topic. Use one of: %1").arg(topics.join(QStringLiteral(", ")))), true);
        return;
    }
    QFile resource(QStringLiteral(":/mcp-docs/") + file.value());
    if (!resource.open(QIODevice::ReadOnly)) {
        reply(Error(QStringLiteral("Documentation is not bundled in this build")), true);
        return;
    }
    reply(QJsonObject{{QStringLiteral("topic"), topic},
                      {QStringLiteral("markdown"), QString::fromUtf8(resource.readAll())}},
          false);
}

void McpBridge::toolLoadScenario(const QJsonObject &arguments, Reply reply) {
    if (controller_.running()) {
        reply(Error(QStringLiteral("Stop the running search before loading another scenario")), true);
        return;
    }
    QString argumentError;
    const QString path = QDir::fromNativeSeparators(
            StringArgument(arguments, "path", &argumentError).value_or(QString()));
    if (!argumentError.isEmpty()) {
        reply(Error(argumentError), true);
        return;
    }
    if (path.isEmpty() || !QFileInfo(path).isFile()) {
        reply(Error(QStringLiteral("Scenario file not found: %1").arg(path)), true);
        return;
    }
    const QString packs = StringArgument(arguments, "packs_directory", &argumentError).value_or(QString());
    if (!argumentError.isEmpty()) {
        reply(Error(argumentError), true);
        return;
    }
    if (!packs.isEmpty()) {
        if (!QFileInfo(packs).isDir()) {
            reply(Error(QStringLiteral("Packs directory not found: %1").arg(packs)), true);
            return;
        }
        controller_.setPacksDirectory(QDir::fromNativeSeparators(packs));
    } else if (controller_.packsDirectory().isEmpty() &&
               !controller_.autoDetectedPacksDirectory().isEmpty()) {
        controller_.applyAutoDetectedPacksDirectory();
    }
    controller_.setReplayPath(path);

    const bool extract = arguments.value(QStringLiteral("extract_inputs")).toBool();
    const QString request = currentRequest_;
    const auto finish = [this, reply, extract, request](bool timedOut) {
        if (timedOut) {
            reply(Error(QStringLiteral("Timed out extracting replay inputs")), true);
            return;
        }
        const std::string packsDirectory = S(controller_.packsDirectory());
        const std::string scenario = S(controller_.replayPath());
        const std::string script = S(controller_.baseInputScript());
        const QString extraction = controller_.replayInputStatusText();
        const std::uint32_t horizon = std::max(configuredHorizonMs(), kDefaultSimulateHorizonMs);
        runSimulationJob(
                request,
                [packsDirectory, scenario, script, extract, extraction, horizon](
                        McpSimulationCache &cache, const std::atomic<bool> &cancelled) {
                    cache.ensure(packsDirectory, scenario, horizon);
                    QJsonObject result = ScenarioInfo(cache);
                    result.insert(QStringLiteral("packs_directory"), Q(packsDirectory));
                    result.insert(QStringLiteral("scenario_path"), Q(scenario));
                    if (extract) {
                        result.insert(QStringLiteral("extraction"), extraction);
                        result.insert(QStringLiteral("base_script"), Q(script));
                        SimulationOptions options;
                        options.samples = false;
                        options.cancelled = &cancelled;
                        options.deadline = std::chrono::steady_clock::now() + kSimulationLimit;
                        result.insert(QStringLiteral("base_script_result"), SimulateScript(cache, script, options));
                    }
                    return result;
                },
                reply);
    };
    if (!extract) {
        finish(false);
        return;
    }
    if (!controller_.canExtractReplayInputs()) {
        reply(Error(QStringLiteral("Inputs cannot be extracted from this scenario (standalone challenges have no "
                                   "inputs, or the Packs directory is missing)")),
              true);
        return;
    }
    controller_.extractReplayInputs();
    waitUntil([this] { return !controller_.extractingReplayInputs(); }, 120000, finish);
}

void McpBridge::toolGetMapLayout(const QJsonObject &arguments, Reply reply) {
    const std::string packsDirectory = S(controller_.packsDirectory());
    const std::string scenario = S(controller_.replayPath());
    const bool blocks = arguments.value(QStringLiteral("include_blocks")).toBool();
    const int maxBlocks = ClampedInt(arguments.value(QStringLiteral("max_blocks")), 400, 1, 100000);
    const std::uint32_t horizon = std::max(configuredHorizonMs(), kDefaultSimulateHorizonMs);
    runSimulationJob(
            currentRequest_,
            [=](McpSimulationCache &cache, const std::atomic<bool> &) {
                cache.ensure(packsDirectory, scenario,
                             cache.sandbox && cache.packs == packsDirectory && cache.scenario == scenario
                                     ? cache.horizonMs : horizon);
                return MapLayout(cache, blocks, maxBlocks);
            },
            reply);
}

void McpBridge::toolGetInputScript(const QJsonObject &, Reply reply) {
    reply(QJsonObject{{QStringLiteral("script"), controller_.baseInputScript()},
                      {QStringLiteral("parse_error"), controller_.baseInputScriptError()}},
          false);
}

void McpBridge::toolSetInputScript(const QJsonObject &arguments, Reply reply) {
    if (controller_.running()) {
        reply(Error(QStringLiteral("Stop the running search before changing the base script")), true);
        return;
    }
    if (!arguments.value(QStringLiteral("script")).isString()) {
        reply(Error(QStringLiteral("'script' must be a string")), true);
        return;
    }
    const QString script = arguments.value(QStringLiteral("script")).toString();
    const InputScriptParseResult parsed = ParseInputScript(S(script));
    if (!parsed) {
        // the window keeps the script it had
        reply(Error(QStringLiteral("Script parse error (not applied): %1").arg(Q(*parsed.error))), true);
        return;
    }
    controller_.setBaseInputScript(script);
    if (arguments.value(QStringLiteral("preview")).toBool()) controller_.previewBaseInputs();
    reply(QJsonObject{{QStringLiteral("applied"), true},
                      {QStringLiteral("commands"), static_cast<qint64>(parsed.commands.size())},
                      {QStringLiteral("validation_message"), controller_.validationMessage()}},
          false);
}

void McpBridge::toolUseBestAsBase(const QJsonObject &arguments, Reply reply) {
    if (controller_.running()) {
        reply(Error(QStringLiteral("Stop the running search first (stop_search)")), true);
        return;
    }
    const QString source = arguments.value(QStringLiteral("source")).toString(QStringLiteral("best"));
    if (source != QLatin1String("best") && source != QLatin1String("selected_cycle")) {
        reply(Error(QStringLiteral("'source' must be 'best' or 'selected_cycle'")), true);
        return;
    }
    const QString script = source == QLatin1String("selected_cycle") ? controller_.selectedInputsText()
                                                                      : controller_.bestInputsText();
    if (script.trimmed().isEmpty()) {
        reply(Error(QStringLiteral("There is no %1 script yet").arg(source)), true);
        return;
    }
    controller_.setBaseInputScript(script);
    reply(QJsonObject{{QStringLiteral("applied"), true},
                      {QStringLiteral("source"), source},
                      {QStringLiteral("base_script_error"), controller_.baseInputScriptError()},
                      {QStringLiteral("lines"), static_cast<int>(script.count('\n') + 1)}},
          false);
}

namespace {

// Accepts gates as the compact target string, one object, or an array of
// objects/strings ({"x":..,"y":..,"z":..,"dir":..,"hw":..,...}) and returns
// the compact "k=v,...|k=v,..." form used by the gate-entry target.
std::optional<QString> GateListText(const QJsonValue &value, QString *error) {
    static const char *const keys[] = {"x", "y", "z", "dir", "hw", "lmin", "lmax", "ymin", "ymax",
                                       "vmin", "vmax", "yawmin", "yawmax", "wheels"};
    const auto one = [&](const QJsonValue &gate) -> std::optional<QString> {
        if (gate.isString()) return gate.toString();
        if (!gate.isObject()) {
            *error = QStringLiteral("each gate must be an object or a 'k=v,...' string");
            return std::nullopt;
        }
        const QJsonObject object = gate.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (std::find_if(std::begin(keys), std::end(keys),
                             [&](const char *key) { return it.key() == QLatin1String(key); }) == std::end(keys)) {
                *error = QStringLiteral("unknown gate field '%1'").arg(it.key());
                return std::nullopt;
            }
        }
        QStringList fields;
        for (const char *key : keys) {
            const QJsonValue field = object.value(QLatin1String(key));
            if (field.isUndefined() || field.isNull()) continue;
            if (!field.isDouble()) {
                *error = QStringLiteral("gate field '%1' must be a number").arg(QLatin1String(key));
                return std::nullopt;
            }
            fields.append(QStringLiteral("%1=%2").arg(QLatin1String(key)).arg(QString::number(field.toDouble(), 'g', 12)));
        }
        return fields.join(',');
    };
    if (value.isString()) return value.toString();
    if (value.isObject()) return one(value);
    if (!value.isArray()) {
        *error = QStringLiteral("'gates' must be an array of gate objects or a string");
        return std::nullopt;
    }
    QStringList gates;
    for (const QJsonValue &gate : value.toArray()) {
        const auto text = one(gate);
        if (!text) return std::nullopt;
        gates.append(*text);
    }
    return gates.join('|');
}

int DefaultSimulationThreads() {
    const unsigned hardware = std::thread::hardware_concurrency();
    return std::clamp(hardware > 1u ? static_cast<int>(hardware) - 1 : 1, 1, kMaximumSimulationThreads);
}

}  // namespace

void McpBridge::toolSimulate(const QJsonObject &arguments, Reply reply) {
    std::vector<std::string> scripts;
    const bool compare = arguments.contains(QStringLiteral("scripts"));
    if (compare) {
        if (!arguments.value(QStringLiteral("scripts")).isArray()) {
            reply(Error(QStringLiteral("'scripts' must be an array of strings")), true);
            return;
        }
        const QJsonArray list = arguments.value(QStringLiteral("scripts")).toArray();
        if (list.isEmpty() || list.size() > kMaximumCompareScripts) {
            reply(Error(QStringLiteral("'scripts' must hold 1..%1 scripts").arg(kMaximumCompareScripts)), true);
            return;
        }
        for (const QJsonValue &value : list) {
            if (!value.isString()) {
                reply(Error(QStringLiteral("every entry of 'scripts' must be a string")), true);
                return;
            }
            scripts.push_back(S(value.toString()));
        }
    } else {
        if (arguments.contains(QStringLiteral("script")) && !arguments.value(QStringLiteral("script")).isString()) {
            reply(Error(QStringLiteral("'script' must be a string")), true);
            return;
        }
        scripts.push_back(S(arguments.contains(QStringLiteral("script"))
                                    ? arguments.value(QStringLiteral("script")).toString()
                                    : controller_.baseInputScript()));
    }
    QString argumentError;
    const QString scenarioOverride = StringArgument(arguments, "replay_path", &argumentError).value_or(QString());
    if (!argumentError.isEmpty()) {
        reply(Error(argumentError), true);
        return;
    }
    const std::string packsDirectory = S(controller_.packsDirectory());
    const std::string scenario =
            S(scenarioOverride.isEmpty() ? controller_.replayPath() : QDir::fromNativeSeparators(scenarioOverride));
    std::uint32_t horizon = kDefaultSimulateHorizonMs;
    if (arguments.contains(QStringLiteral("horizon_ms"))) {
        const double requested = arguments.value(QStringLiteral("horizon_ms")).toDouble(-1.0);
        if (!(requested >= kTickMs && requested <= kMaximumSimulationHorizonMs)) {
            reply(Error(QStringLiteral("horizon_ms must be a number from 10 to %1").arg(kMaximumSimulationHorizonMs)), true);
            return;
        }
        horizon = static_cast<std::uint32_t>(requested) / kTickMs * kTickMs;
    }
    SimulationOptions options;
    options.horizonMs = horizon;
    options.sampleIntervalMs = ClampedInt(arguments.value(QStringLiteral("sample_interval_ms")), 250, 0, 3600000);
    if (options.sampleIntervalMs % static_cast<int>(kTickMs) != 0) {
        reply(Error(QStringLiteral("sample_interval_ms must be a multiple of 10")), true);
        return;
    }
    options.fromMs = ClampedInt(arguments.value(QStringLiteral("from_ms")), 0, 0, std::numeric_limits<int>::max());
    options.toMs = arguments.value(QStringLiteral("to_ms")).isDouble()
            ? static_cast<std::int64_t>(arguments.value(QStringLiteral("to_ms")).toDouble())
            : std::numeric_limits<std::int64_t>::max();
    QString csv = StringArgument(arguments, "csv_path", &argumentError).value_or(QString());
    if (!argumentError.isEmpty()) {
        reply(Error(argumentError), true);
        return;
    }
    if (csv == QLatin1String("auto")) csv = DefaultOutputPath(QStringLiteral("sim-"), QStringLiteral(".csv"));
    else if (!csv.trimmed().isEmpty()) csv = OutputPath(csv);
    options.csvPath = compare ? QString() : csv;
    options.samples = !compare || arguments.value(QStringLiteral("include_samples")).toBool();

    if (arguments.contains(QStringLiteral("gates"))) {
        QString error;
        const auto text = GateListText(arguments.value(QStringLiteral("gates")), &error);
        if (!text) {
            reply(Error(error), true);
            return;
        }
        std::string parseError;
        auto gates = ParseGateList(S(*text), &parseError);
        if (!gates) {
            reply(Error(QStringLiteral("gates: %1").arg(Q(parseError))), true);
            return;
        }
        OptionSettings settings = DefaultGateEntryOptionSettings();
        settings["gates"] = S(*text);
        settings["soft"] = arguments.value(QStringLiteral("gate_soft")).toBool(true) ? "true" : "false";
        if (arguments.value(QStringLiteral("yaw_weight")).isDouble()) {
            options.gateScoring.yawWeight = arguments.value(QStringLiteral("yaw_weight")).toDouble();
            settings["yawWeight"] = S(SettingString(arguments.value(QStringLiteral("yaw_weight"))));
        }
        if (arguments.value(QStringLiteral("speed_weight")).isDouble()) {
            options.gateScoring.speedWeight = arguments.value(QStringLiteral("speed_weight")).toDouble();
            settings["speedWeight"] = S(SettingString(arguments.value(QStringLiteral("speed_weight"))));
        }
        try {
            options.gateEvaluator = std::shared_ptr<const IterationEvaluator>(CreateGateEntryEvaluator(settings, kTickMs));
        } catch (const std::exception &failure) {
            reply(Error(QStringLiteral("gates: %1").arg(QString::fromUtf8(failure.what()))), true);
            return;
        }
        options.gates = std::make_shared<const std::vector<GateSpec>>(std::move(*gates));
        options.gateFromMs = ClampedInt(arguments.value(QStringLiteral("gate_from_ms")), 0, 0,
                                        std::numeric_limits<int>::max());
    }
    if (arguments.contains(QStringLiteral("snapshot_at_ms"))) {
        if (compare) {
            reply(Error(QStringLiteral("snapshot_at_ms works with a single 'script'")), true);
            return;
        }
        const double at = arguments.value(QStringLiteral("snapshot_at_ms")).toDouble(-1.0);
        if (!(at >= 0.0) || std::fmod(at, kTickMs) != 0.0) {
            reply(Error(QStringLiteral("snapshot_at_ms must be a non-negative multiple of 10")), true);
            return;
        }
        options.snapshotAtMs = static_cast<std::int64_t>(at);
    }
    const std::string fromSnapshot = S(StringArgument(arguments, "from_snapshot", &argumentError).value_or(QString()));
    if (!argumentError.isEmpty()) {
        reply(Error(argumentError), true);
        return;
    }
    const int threads = ClampedInt(arguments.value(QStringLiteral("threads")), DefaultSimulationThreads(), 1,
                                   kMaximumSimulationThreads);
    runSimulationJob(
            currentRequest_,
            [packsDirectory, scenario, horizon, scripts = std::move(scripts), options, compare, fromSnapshot,
             threads](McpSimulationCache &cache, const std::atomic<bool> &cancelled) mutable {
                options.cancelled = &cancelled;
                options.deadline = std::chrono::steady_clock::now() +
                        (compare ? std::chrono::duration_cast<std::chrono::steady_clock::duration>(kBatchSimulationLimit)
                                 : std::chrono::duration_cast<std::chrono::steady_clock::duration>(kSimulationLimit));
                cache.ensure(packsDirectory, scenario, horizon);
                if (!fromSnapshot.empty()) {
                    const auto found = cache.snapshots.find(fromSnapshot);
                    if (found == cache.snapshots.end()) {
                        return Error(QStringLiteral("Unknown snapshot '%1' (snapshots are dropped when another "
                                                    "scenario loads; at most %2 are kept)")
                                             .arg(Q(fromSnapshot))
                                             .arg(kMaximumSnapshots));
                    }
                    options.fromSnapshot = found->second;
                }
                if (!compare) {
                    std::optional<McpSimulationSnapshot> snapshot;
                    QJsonObject result = SimulateScript(cache, *cache.sandbox, *cache.initialState, scripts.front(),
                                                        options, options.snapshotAtMs ? &snapshot : nullptr);
                    if (snapshot) {
                        const qint64 at = snapshot->inputTimeMs;
                        result.insert(QStringLiteral("snapshot_id"), Q(cache.storeSnapshot(std::move(*snapshot))));
                        result.insert(QStringLiteral("snapshot_at_ms"), at);
                    }
                    return result;
                }
                QJsonObject result{{QStringLiteral("results"), SimulateBatch(cache, scripts, options, threads)}};
                result.insert(QStringLiteral("threads"),
                              static_cast<qint64>(std::min<std::size_t>(scripts.size(), static_cast<std::size_t>(threads))));
                return result;
            },
            reply);
}

void McpBridge::toolConfigureSearch(const QJsonObject &arguments, Reply reply) {
    if (controller_.running()) {
        reply(Error(QStringLiteral("Stop the running search before reconfiguring it")), true);
        return;
    }
    QJsonArray problems;
    const auto note = [&problems](const QString &text) { problems.append(text); };
    const auto applySettings = [&](const QJsonValue &requestedValue,
                                   const std::function<void(const QString &, const QString &)> &set,
                                   const std::function<QVariantMap()> &read, const QString &label) {
        if (requestedValue.isUndefined()) return;
        if (!requestedValue.isObject()) {
            note(QStringLiteral("%1: 'settings' must be an object").arg(label));
            return;
        }
        const QJsonObject requested = requestedValue.toObject();
        for (auto it = requested.begin(); it != requested.end(); ++it) {
            const QString value = SettingString(it.value());
            set(it.key(), value);
            const QVariantMap current = read();
            if (!current.contains(it.key())) {
                note(QStringLiteral("%1: unknown setting '%2'").arg(label, it.key()));
            } else if (current.value(it.key()).toString() != value) {
                note(QStringLiteral("%1: '%2' kept '%3' (rejected '%4')")
                             .arg(label, it.key(), current.value(it.key()).toString(), value));
            }
        }
    };

    if (arguments.contains(QStringLiteral("backend"))) {
        const QString backend = arguments.value(QStringLiteral("backend")).toString();
        controller_.setSimulationBackendId(backend);
        if (controller_.simulationBackendId() != backend) {
            note(QStringLiteral("backend '%1' is not available here (kept '%2')")
                         .arg(backend, controller_.simulationBackendId()));
        }
    }
    if (arguments.contains(QStringLiteral("approximate_physics"))) {
        const bool wanted = arguments.value(QStringLiteral("approximate_physics")).toBool();
        controller_.setApproximatePhysics(wanted);
        if (controller_.approximatePhysics() != wanted) note(QStringLiteral("approximate physics is not available here"));
    }
    if (arguments.contains(QStringLiteral("simulation_horizon_ms"))) {
        controller_.setSimulationHorizonMs(SettingString(arguments.value(QStringLiteral("simulation_horizon_ms"))));
    }
    if (arguments.contains(QStringLiteral("condition_script"))) {
        controller_.setConditionScript(arguments.value(QStringLiteral("condition_script")).toString());
    }
    if (arguments.contains(QStringLiteral("algorithm"))) {
        const QJsonObject algorithm = arguments.value(QStringLiteral("algorithm")).toObject();
        if (algorithm.contains(QStringLiteral("id"))) {
            const QString id = algorithm.value(QStringLiteral("id")).toString();
            if (!FindSearchAlgorithm(S(id))) note(QStringLiteral("unknown algorithm '%1'").arg(id));
            else controller_.setSearchAlgorithmId(id);
        }
        applySettings(algorithm.value(QStringLiteral("settings")),
                      [this](const QString &k, const QString &v) { controller_.setSearchAlgorithmSetting(k, v); },
                      [this] { return controller_.searchAlgorithmSettings(); }, QStringLiteral("algorithm"));
    }
    if (arguments.contains(QStringLiteral("target"))) {
        const QJsonObject target = arguments.value(QStringLiteral("target")).toObject();
        if (target.contains(QStringLiteral("id"))) {
            const QString id = target.value(QStringLiteral("id")).toString();
            if (!FindEvaluationTarget(S(id))) note(QStringLiteral("unknown target '%1'").arg(id));
            else controller_.setEvaluationTargetId(id);
        }
        QJsonObject targetSettings = target.value(QStringLiteral("settings")).toObject();
        if (targetSettings.contains(QStringLiteral("gates")) && !targetSettings.value(QStringLiteral("gates")).isString()) {
            QString error;
            const auto text = GateListText(targetSettings.value(QStringLiteral("gates")), &error);
            if (text) {
                targetSettings.insert(QStringLiteral("gates"), *text);
            } else {
                note(QStringLiteral("target: gates rejected (%1)").arg(error));
                targetSettings.remove(QStringLiteral("gates"));
            }
        }
        if (target.contains(QStringLiteral("settings"))) {
            applySettings(targetSettings,
                          [this](const QString &k, const QString &v) { controller_.setEvaluationTargetSetting(k, v); },
                          [this] { return controller_.evaluationTargetSettings(); }, QStringLiteral("target"));
        }
    }
    if (arguments.contains(QStringLiteral("modifiers"))) {
        if (!arguments.value(QStringLiteral("modifiers")).isArray()) {
            note(QStringLiteral("'modifiers' must be an array; the passes were kept"));
        } else {
            const QJsonArray passes = arguments.value(QStringLiteral("modifiers")).toArray();
            for (int index = static_cast<int>(controller_.modifierPasses().size()) - 1; index >= 0; --index) {
                controller_.removeModifierPass(index);
            }
            for (const QJsonValue &value : passes) {
                const QJsonObject pass = value.toObject();
                const QString id = pass.value(QStringLiteral("id")).toString();
                if (!FindModifier(S(id))) {
                    note(QStringLiteral("unknown modifier '%1'").arg(id));
                    continue;
                }
                const int before = static_cast<int>(controller_.modifierPasses().size());
                controller_.addModifierPass(id);
                const int index = static_cast<int>(controller_.modifierPasses().size()) - 1;
                if (index < before) {
                    note(QStringLiteral("modifier '%1' could not be added").arg(id));
                    continue;
                }
                if (pass.contains(QStringLiteral("enabled"))) {
                    controller_.setModifierPassEnabled(index, pass.value(QStringLiteral("enabled")).toBool());
                }
                applySettings(pass.value(QStringLiteral("settings")),
                              [this, index](const QString &k, const QString &v) {
                                  controller_.setModifierPassSetting(index, k, v);
                              },
                              [this, index] {
                                  return controller_.modifierPasses().value(index).toMap()
                                          .value(QStringLiteral("settings")).toMap();
                              },
                              QStringLiteral("modifier %1 (%2)").arg(index).arg(id));
            }
        }
    }
    if (arguments.contains(QStringLiteral("cpu_workers"))) {
        controller_.setCpuWorkerCount(SettingString(arguments.value(QStringLiteral("cpu_workers"))));
    }
    if (arguments.contains(QStringLiteral("gpu_batch_size"))) {
        const QString size = SettingString(arguments.value(QStringLiteral("gpu_batch_size")));
        controller_.setCudaParallelSampleCount(size);
        controller_.setHipParallelSampleCount(size);
    }
    if (arguments.contains(QStringLiteral("gpu_calibration"))) {
        const bool enabled = arguments.value(QStringLiteral("gpu_calibration")).toBool();
        controller_.setCudaCalibrationEnabled(enabled);
        controller_.setHipCalibrationEnabled(enabled);
    }
    if (arguments.contains(QStringLiteral("randomize_seeds"))) {
        controller_.setRandomizeSeedsOnStart(arguments.value(QStringLiteral("randomize_seeds")).toBool());
    }
    if (arguments.value(QStringLiteral("autorestart")).isObject()) {
        const QJsonObject restart = arguments.value(QStringLiteral("autorestart")).toObject();
        if (restart.contains(QStringLiteral("mode")))
            controller_.setAutoRestartMode(restart.value(QStringLiteral("mode")).toString());
        if (restart.contains(QStringLiteral("duration")))
            controller_.setAutoRestartDuration(restart.value(QStringLiteral("duration")).toString());
        if (restart.contains(QStringLiteral("attempts")))
            controller_.setAutoRestartAttempts(SettingString(restart.value(QStringLiteral("attempts"))));
    }
    if (arguments.value(QStringLiteral("extend_horizon")).toBool()) controller_.extendSimulationHorizon();
    reply(QJsonObject{{QStringLiteral("configuration"), configurationObject()},
                      {QStringLiteral("problems"), problems},
                      {QStringLiteral("validation_message"), controller_.validationMessage()},
                      {QStringLiteral("can_start"), controller_.canStart()}},
          false);
}

void McpBridge::toolEvaluateBase(const QJsonObject &arguments, Reply reply) {
    if (!controller_.canEvaluateBase()) {
        reply(Error(QStringLiteral("Base evaluation is not possible now: %1")
                            .arg(controller_.running() ? QStringLiteral("a search is running")
                                                       : controller_.validationMessage())),
              true);
        return;
    }
    const int timeoutMs = static_cast<int>(
            std::clamp(arguments.value(QStringLiteral("timeout_seconds")).toDouble(60.0), 1.0, 290.0) * 1000.0);
    controller_.evaluateBase();
    waitUntil([this] { return !controller_.evaluatingBase(); }, timeoutMs, [this, reply](bool timedOut) {
        reply(QJsonObject{{QStringLiteral("timed_out"), timedOut},
                          {QStringLiteral("text"), controller_.baseEvaluationText()},
                          {QStringLiteral("result"), QJsonObject::fromVariantMap(controller_.baseEvaluationResult())}},
              false);
    });
}

void McpBridge::toolStartSearch(const QJsonObject &, Reply reply) {
    if (controller_.running()) {
        reply(Error(QStringLiteral("A search is already running")), true);
        return;
    }
    if (!controller_.canStart()) {
        reply(Error(QStringLiteral("Cannot start: %1").arg(controller_.validationMessage())), true);
        return;
    }
    controller_.startSearch();
    waitUntil([this] { return controller_.running(); }, 2000, [this, reply](bool) {
        reply(QJsonObject{{QStringLiteral("started"), controller_.running()},
                          {QStringLiteral("search"), searchProgressObject()},
                          {QStringLiteral("configuration"), configurationObject()}},
              !controller_.running());
    });
}

void McpBridge::toolWaitForSearch(const QJsonObject &arguments, Reply reply) {
    const int timeoutMs = static_cast<int>(
            std::clamp(arguments.value(QStringLiteral("timeout_seconds")).toDouble(30.0), 0.0, 290.0) * 1000.0);
    const bool untilFinished = arguments.value(QStringLiteral("until")).toString() == QLatin1String("finished");
    // a new best changes the best script (the improvement count is shown
    // rounded, so it can stay the same)
    const QString initialBest = controller_.bestInputsText();
    waitUntil(
            [this, untilFinished, initialBest] {
                if (!controller_.running()) return true;
                return !untilFinished && controller_.bestInputsText() != initialBest;
            },
            timeoutMs,
            [this, reply, initialBest](bool timedOut) {
                QJsonObject result = searchProgressObject();
                result.insert(QStringLiteral("timed_out"), timedOut);
                result.insert(QStringLiteral("improved"), controller_.bestInputsText() != initialBest);
                reply(result, false);
            });
}

void McpBridge::toolStopSearch(const QJsonObject &arguments, Reply reply) {
    if (!controller_.running()) {
        reply(QJsonObject{{QStringLiteral("stopped"), true}, {QStringLiteral("search"), searchProgressObject()}}, false);
        return;
    }
    controller_.stopSearch();
    const int timeoutMs = static_cast<int>(
            std::clamp(arguments.value(QStringLiteral("wait_seconds")).toDouble(30.0), 0.0, 290.0) * 1000.0);
    waitUntil([this] { return !controller_.running(); }, timeoutMs, [this, reply](bool timedOut) {
        reply(QJsonObject{{QStringLiteral("stopped"), !controller_.running()},
                          {QStringLiteral("timed_out"), timedOut},
                          {QStringLiteral("search"), searchProgressObject()}},
              false);
    });
}

void McpBridge::toolGetBestResult(const QJsonObject &arguments, Reply reply) {
    const QString best = controller_.bestInputsText();
    QJsonObject result = searchProgressObject();
    result.insert(QStringLiteral("best_script"), best);
    if (best.trimmed().isEmpty() || !arguments.value(QStringLiteral("simulate")).toBool(true)) {
        reply(result, false);
        return;
    }
    const std::string packsDirectory = S(controller_.packsDirectory());
    const std::string scenario = S(controller_.replayPath());
    const std::string script = S(best);
    const std::string base = S(controller_.baseInputScript());
    runSimulationJob(
            currentRequest_,
            [=](McpSimulationCache &cache, const std::atomic<bool> &cancelled) {
                cache.ensure(packsDirectory, scenario,
                             cache.sandbox && cache.packs == packsDirectory && cache.scenario == scenario
                                     ? cache.horizonMs : kDefaultSimulateHorizonMs);
                SimulationOptions options;
                options.samples = false;
                options.cancelled = &cancelled;
                options.deadline = std::chrono::steady_clock::now() + kSimulationLimit;
                QJsonObject withSimulation = result;
                withSimulation.insert(QStringLiteral("simulation"), SimulateScript(cache, script, options));
                if (base != script) {
                    withSimulation.insert(QStringLiteral("base_simulation"), SimulateScript(cache, base, options));
                }
                return withSimulation;
            },
            reply);
}

void McpBridge::toolGetSearchHistory(const QJsonObject &arguments, Reply reply) {
    if (arguments.value(QStringLiteral("session_index")).isDouble()) {
        controller_.selectSession(arguments.value(QStringLiteral("session_index")).toInt());
    }
    if (arguments.value(QStringLiteral("cycle_index")).isDouble()) {
        controller_.selectCycle(arguments.value(QStringLiteral("cycle_index")).toInt());
    }
    reply(QJsonObject{{QStringLiteral("sessions"), QJsonArray::fromVariantList(controller_.sessionOptions())},
                      {QStringLiteral("selected_session"), controller_.selectedSessionDirectory()},
                      {QStringLiteral("cycles"), QJsonArray::fromVariantList(controller_.cycleRows())},
                      {QStringLiteral("selected_cycle_restart"), static_cast<qint64>(controller_.selectedCycleRestart())},
                      {QStringLiteral("selected_cycle_script"), controller_.selectedInputsText()}},
          false);
}

void McpBridge::toolSaveInputScript(const QJsonObject &arguments, Reply reply) {
    QString argumentError;
    const QString given = StringArgument(arguments, "path", &argumentError).value_or(QString());
    if (!argumentError.isEmpty()) {
        reply(Error(argumentError), true);
        return;
    }
    QString script;
    if (arguments.contains(QStringLiteral("script"))) {
        script = arguments.value(QStringLiteral("script")).toString();
    } else {
        const QString source = arguments.value(QStringLiteral("source")).toString(QStringLiteral("best"));
        script = source == QLatin1String("base")             ? controller_.baseInputScript()
                : source == QLatin1String("selected_cycle") ? controller_.selectedInputsText()
                                                            : controller_.bestInputsText();
    }
    if (given.trimmed().isEmpty() || script.trimmed().isEmpty()) {
        reply(Error(QStringLiteral("Need a path and a non-empty script")), true);
        return;
    }
    const QString path = OutputPath(given);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text) || file.write(script.toUtf8()) < 0 || !file.commit()) {
        reply(Error(QStringLiteral("Could not write %1").arg(QDir::toNativeSeparators(path))), true);
        return;
    }
    reply(QJsonObject{{QStringLiteral("saved"), QDir::toNativeSeparators(path)},
                      {QStringLiteral("lines"), static_cast<int>(script.count('\n') + 1)}},
          false);
}

void McpBridge::toolCaptureWindow(const QJsonObject &arguments, Reply reply) {
    QQuickWindow *window = engine_ ? qobject_cast<QQuickWindow *>(engine_->rootObjects().value(0)) : nullptr;
    if (window == nullptr) {
        reply(Error(QStringLiteral("The main window is not available")), true);
        return;
    }
    QString argumentError;
    const QString given = StringArgument(arguments, "path", &argumentError).value_or(QString());
    if (!argumentError.isEmpty()) {
        reply(Error(argumentError), true);
        return;
    }
    const QString path = given.trimmed().isEmpty() ? DefaultOutputPath(QStringLiteral("window-"), QStringLiteral(".png"))
                                                   : OutputPath(given);
    QDir().mkpath(QFileInfo(path).absolutePath());
    const QImage image = window->grabWindow();
    if (image.isNull() || !image.save(path, "PNG")) {
        reply(Error(QStringLiteral("Could not capture the window to %1").arg(QDir::toNativeSeparators(path))), true);
        return;
    }
    reply(QJsonObject{{QStringLiteral("path"), QDir::toNativeSeparators(path)},
                      {QStringLiteral("width"), image.width()},
                      {QStringLiteral("height"), image.height()}},
          false);
}

}  // namespace forevertas::app
