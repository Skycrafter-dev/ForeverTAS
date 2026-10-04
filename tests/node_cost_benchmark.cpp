// Measures the per-node cost of a branching CPU search against one physics
// tick, using the persisted ForeverTAS search configuration.
//
// Run with XDG_CONFIG_HOME pointing at a copy of the ForeverTAS settings
// directory: SearchConfigurationModel may migrate and persist settings.

#include "app/search_configuration_model.h"
#include "conditions/condition_program.h"
#include "mutations/composite_input_mutator.h"
#include "mutations/input_event_formatter.h"
#include "mutations/input_event_utils.h"
#include "physics_backend.h"
#include "replay_file_io.h"
#include "searches/algorithm_registry.h"
#include "searches/search_runner.h"

#include <forevervalidator/experimental/physics_sandbox.h>
#include <forevervalidator/native.h>

#include <QCoreApplication>
#include <QDir>
#include <QSettings>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace forevertas;
using namespace forevervalidator::experimental;
using Clock = std::chrono::steady_clock;

template <typename T, typename Error>
T Require(forevervalidator::DiscriminatedResult<T, Error> result,
          const char *operation) {
    if (!result) {
        std::string message = std::string(operation) + " failed";
        if (!result.Error().diagnostic.empty()) {
            message += ": " + result.Error().diagnostic;
        }
        throw std::runtime_error(std::move(message));
    }
    return std::move(result).Value();
}

double ElapsedNs(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::nano>(end - start).count();
}

double Median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2u];
}

double WallSeconds() {
    return std::chrono::duration<double>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
}

struct Segment {
    std::int64_t firstInputMs = 0;
    std::int64_t lastInputMs = 0;
    std::uint32_t ticks = 0u;
};

std::vector<Segment> Segments(std::int64_t firstMs,
                              std::int64_t lastMs,
                              std::uint32_t count,
                              std::uint32_t tickMs) {
    const std::int64_t windowTicks = (lastMs - firstMs) / tickMs + 1;
    std::vector<Segment> segments;
    for (std::uint32_t index = 0u; index < count; ++index) {
        const std::int64_t begin = firstMs +
                windowTicks * index / count * tickMs;
        const std::int64_t end = firstMs +
                windowTicks * (index + 1u) / count * tickMs;
        segments.push_back({begin,
                            end - tickMs,
                            static_cast<std::uint32_t>((end - begin) / tickMs)});
    }
    return segments;
}

struct Patch {
    std::int64_t minimumTimeMs = 0;
    std::int64_t maximumTimeMs = 0;
    std::vector<PhysicsSandboxInputEvent> events;
};

std::vector<PhysicsSandboxInputEvent> Slice(const Patch &patch,
                                            const Segment &segment) {
    std::vector<PhysicsSandboxInputEvent> events;
    for (const PhysicsSandboxInputEvent &event : patch.events) {
        if (event.timeMs >= segment.firstInputMs &&
            event.timeMs <= segment.lastInputMs) {
            events.push_back(event);
        }
    }
    return events;
}

class Bench {
public:
    Bench(PhysicsSandbox &sandbox,
          const IterationEvaluator &evaluator,
          const ConditionProgram *condition,
          std::uint32_t tickMs)
        : sandbox_(sandbox),
          evaluator_(evaluator),
          condition_(condition),
          tickMs_(tickMs),
          started_(WallSeconds()) {}

    // Mirrors the per-tick work of BasicBruteForceSearch's evaluateTimeline.
    PhysicsSandboxStateView SimulateObserved(
            std::uint32_t ticks,
            PhysicsSandboxStateView previous,
            IterationEvaluationSession &session) {
        std::optional<PhysicsSandboxStateView> last = previous;
        for (std::uint32_t tick = 0u; tick < ticks; ++tick) {
            PhysicsSandboxStateView state = Require(
                    sandbox_.AdvanceTicks(1u), "advancing tick");
            const ConditionExecutionContext context{
                    1u, started_, started_, WallSeconds()};
            session.SetExecutionContext(context);
            const bool eligible = condition_ == nullptr ||
                    condition_->Evaluate(*last, state, context);
            if (eligible) {
                observed_ += session.Observe(last, state).has_value();
            }
            last = state;
        }
        return *last;
    }

    PhysicsSandbox &sandbox_;
    const IterationEvaluator &evaluator_;
    const ConditionProgram *condition_;
    std::uint32_t tickMs_;
    double started_;
    std::uint64_t observed_ = 0u;
};

QString Stored(const QSettings &settings, const char *key) {
    return settings.value(QString::fromLatin1(key)).toString();
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ForeverTAS"));
    QCoreApplication::setApplicationName(QStringLiteral("ForeverTAS"));

    const QByteArray configHome = qgetenv("XDG_CONFIG_HOME");
    if (configHome.isEmpty() ||
        QDir(QString::fromLocal8Bit(configHome)) ==
                QDir(QDir::homePath() + QStringLiteral("/.config"))) {
        std::fprintf(stderr,
                     "Set XDG_CONFIG_HOME to a copy of the settings "
                     "directory; the configuration model may persist.\n");
        return 2;
    }
    const double scale = argc > 1 ? std::atof(argv[1]) : 1.0;

    try {
        constexpr std::uint32_t tickMs = kSearchTickDurationMs;
        QSettings settings;
        const std::string packs =
                Stored(settings, "paths/packsDirectory").toStdString();
        const std::string replayPath =
                Stored(settings, "paths/replayPath").toStdString();
        const std::string baseScript =
                Stored(settings, "inputs/baseScript").toStdString();
        const std::uint32_t horizonMs =
                settings.value(QStringLiteral("search/simulationHorizonMs"),
                               kDefaultSimulationHorizonMs).toUInt();
        const std::string conditionScript =
                Stored(settings, "search/conditionScript").toStdString();

        const app::SearchConfigurationModel model;
        const app::SearchConfigurationValidation validation =
                model.validate(tickMs, horizonMs);
        if (!validation.configuration) {
            throw std::runtime_error(
                    "configuration: " + validation.error.toStdString());
        }
        const app::SearchComponentConfiguration &configuration =
                *validation.configuration;

        std::vector<std::unique_ptr<InputMutator>> passes;
        std::printf("modifiers:");
        for (const OptionConfiguration &modifier : configuration.modifiers) {
            passes.push_back(FindModifier(modifier.id)->create(
                    modifier.settings, tickMs));
            std::printf(" %s", modifier.id.c_str());
        }
        const CompositeInputMutator mutator(std::move(passes));
        const std::unique_ptr<IterationEvaluator> evaluator =
                FindEvaluationTarget(configuration.evaluationTarget.id)
                        ->create(configuration.evaluationTarget.settings,
                                 tickMs);
        std::printf("\nevaluation: %s\n",
                    configuration.evaluationTarget.id.c_str());

        ConditionCompileResult compiled = CompileConditionScript(
                conditionScript);
        if (compiled.error) {
            throw std::runtime_error("condition: " + *compiled.error);
        }
        const ConditionProgram *condition =
                compiled.program ? &*compiled.program : nullptr;

        PhysicsSandboxOptions options;
        options.backend = ToForeverValidatorBackend(
                PhysicsBackend::OptimizedCpu);
        options.tickDurationMs = tickMs;
        options.timelineMode = PhysicsSandboxTimelineMode::Canonical;
        options.simulationHorizonMs = horizonMs;
        const forevervalidator::ReplayIdentity identity{replayPath};
        forevervalidator::AssetBytes replay = Require(
                ReadReplayFileUtf8(replayPath, identity), "reading replay");
        PhysicsSandbox sandbox = Require(
                CreatePhysicsSandbox(
                        Require(forevervalidator::OpenInstalledPackDirectory(
                                        packs),
                                "opening packs"),
                        options),
                "creating sandbox");
        Require(sandbox.LoadScenario({replay.data(), replay.size()}, identity),
                "loading scenario");
        const InputScriptParseResult parsed = ParseInputScript(baseScript);
        if (!parsed) throw std::runtime_error("base script: " + *parsed.error);
        InputScriptBaselineResult baseline = BuildInputScriptBaseline(
                Require(sandbox.ReadInputs(), "reading inputs"),
                parsed.commands,
                tickMs);
        if (!baseline) throw std::runtime_error(*baseline.error);
        ConvertKeyboardSteeringToAnalog(baseline.events);
        Require(sandbox.ReplaceInputs(std::move(baseline.events)),
                "applying base script");

        const std::int64_t firstMs = std::min<std::int64_t>(
                mutator.EarliestMutationTimeMs(), horizonMs);
        const std::int64_t lastMs = std::min<std::int64_t>(
                mutator.AffectedTimeRange().maximumTimeMs, horizonMs);
        const std::uint32_t windowTicks =
                static_cast<std::uint32_t>((lastMs - firstMs) / tickMs + 1);
        const std::uint32_t tailTicks =
                static_cast<std::uint32_t>((horizonMs - lastMs) / tickMs);
        const PhysicsSandboxStateView start =
                Require(sandbox.ReadState(), "reading state");
        Require(sandbox.AdvanceTicks(static_cast<std::uint32_t>(
                        (firstMs - tickMs - start.timeMs) / tickMs)),
                "advancing to branch");
        const PhysicsSandboxState branch =
                Require(sandbox.CaptureState(), "capturing branch");
        const std::vector<PhysicsSandboxInputEvent> baselineInputs =
                Require(sandbox.ReadInputs(), "reading baseline");
        std::printf("mutation window %lld-%lld ms (%u ticks), tail to "
                    "horizon %u ticks, %zu baseline events, race completed "
                    "at branch: %s\n",
                    static_cast<long long>(firstMs),
                    static_cast<long long>(lastMs),
                    windowTicks,
                    tailTicks,
                    baselineInputs.size(),
                    branch.View().raceCompleted ? "yes" : "no");

        const auto scaled = [scale](std::uint32_t count) {
            return std::max<std::uint32_t>(
                    1u, static_cast<std::uint32_t>(count * scale));
        };

        // Candidate pool, drawn exactly as BasicBruteForceSearch draws them.
        constexpr std::uint32_t poolSize = 4096u;
        std::vector<Patch> pool;
        std::vector<double> mutateNs;
        std::size_t changed = 0u;
        for (std::uint32_t index = 0u; index < poolSize; ++index) {
            const auto t0 = Clock::now();
            MutationResult mutation = mutator.Mutate(
                    {baselineInputs, index, 0u, tickMs, firstMs, true, 0u});
            mutateNs.push_back(ElapsedNs(t0, Clock::now()));
            changed += mutation.mutationCount;
            Patch patch;
            if (mutation.windowPatch) {
                patch.minimumTimeMs = std::min<std::int64_t>(
                        mutation.windowPatch->minimumTimeMs, horizonMs);
                patch.maximumTimeMs = std::min<std::int64_t>(
                        mutation.windowPatch->maximumTimeMs, horizonMs);
                patch.events = std::move(mutation.windowPatch->events);
            } else {
                patch.minimumTimeMs = firstMs;
                patch.maximumTimeMs = lastMs;
                for (const PhysicsSandboxInputEvent &event : mutation.inputs) {
                    if (event.timeMs >= firstMs && event.timeMs <= lastMs) {
                        patch.events.push_back(event);
                    }
                }
            }
            for (PhysicsSandboxInputEvent &event : patch.events) {
                event.timeMs = std::min<std::int32_t>(
                        event.timeMs, static_cast<std::int32_t>(horizonMs));
            }
            pool.push_back(std::move(patch));
        }
        std::printf("mean effective changes per candidate: %.1f\n\n",
                    static_cast<double>(changed) / poolSize);

        Bench bench(sandbox, *evaluator, condition, tickMs);
        std::unique_ptr<IterationEvaluationSession> session =
                evaluator->CreateSession();

        // 1. One tick, raw and with the search loop's per-tick work.
        std::vector<double> rawTickNs;
        std::vector<double> searchTickNs;
        for (std::uint32_t rep = 0u; rep < scaled(200u); ++rep) {
            Require(sandbox.RestoreState(branch), "restoring");
            auto t0 = Clock::now();
            for (std::uint32_t tick = 0u; tick < windowTicks + tailTicks;
                 ++tick) {
                Require(sandbox.AdvanceTicks(1u), "advancing");
            }
            rawTickNs.push_back(ElapsedNs(t0, Clock::now()) /
                                (windowTicks + tailTicks));
            const PhysicsSandboxStateView view =
                    Require(sandbox.RestoreState(branch), "restoring");
            t0 = Clock::now();
            bench.SimulateObserved(windowTicks + tailTicks, view, *session);
            searchTickNs.push_back(ElapsedNs(t0, Clock::now()) /
                                   (windowTicks + tailTicks));
        }
        const double tickNs = Median(searchTickNs);

        // 2. Node operations at D=10 segment boundaries.
        const std::vector<Segment> segments10 =
                Segments(firstMs, lastMs, 10u, tickMs);
        std::vector<PhysicsSandboxState> nodeStates;
        {
            Require(sandbox.RestoreState(branch), "restoring");
            nodeStates.push_back(branch);
            for (std::size_t index = 0u; index + 1u < segments10.size();
                 ++index) {
                Require(sandbox.AdvanceTicks(segments10[index].ticks),
                        "advancing");
                nodeStates.push_back(
                        Require(sandbox.CaptureState(), "capturing"));
            }
        }
        std::vector<double> captureNs;
        std::vector<double> releaseNs;
        std::vector<double> restoreNs;
        std::vector<double> segmentPatchNs;
        std::vector<double> windowPatchNs;
        std::vector<double> sessionNs;
        std::uint32_t poolIndex = 0u;
        for (std::uint32_t rep = 0u; rep < scaled(100u); ++rep) {
            for (std::size_t index = 0u; index < nodeStates.size(); ++index) {
                auto t0 = Clock::now();
                Require(sandbox.RestoreState(nodeStates[index]), "restoring");
                restoreNs.push_back(ElapsedNs(t0, Clock::now()));

                std::vector<PhysicsSandboxInputEvent> slice =
                        Slice(pool[poolIndex++ % poolSize], segments10[index]);
                t0 = Clock::now();
                Require(sandbox.ReplaceInputWindow(
                                segments10[index].firstInputMs,
                                segments10[index].lastInputMs,
                                std::move(slice)),
                        "replacing segment");
                segmentPatchNs.push_back(ElapsedNs(t0, Clock::now()));

                t0 = Clock::now();
                std::optional<PhysicsSandboxState> captured =
                        Require(sandbox.CaptureState(), "capturing");
                captureNs.push_back(ElapsedNs(t0, Clock::now()));
                t0 = Clock::now();
                captured.reset();
                releaseNs.push_back(ElapsedNs(t0, Clock::now()));

                t0 = Clock::now();
                std::unique_ptr<IterationEvaluationSession> created =
                        evaluator->CreateSession();
                created.reset();
                sessionNs.push_back(ElapsedNs(t0, Clock::now()));
            }
            Require(sandbox.RestoreState(branch), "restoring");
            const Patch &patch = pool[poolIndex++ % poolSize];
            const auto t0 = Clock::now();
            Require(sandbox.ReplaceInputWindow(patch.minimumTimeMs,
                                               patch.maximumTimeMs,
                                               patch.events),
                    "replacing window");
            windowPatchNs.push_back(ElapsedNs(t0, Clock::now()));
        }

        const auto row = [tickNs](const char *name, double ns) {
            std::printf("  %-44s %9.2f us  %6.2f ticks\n",
                        name, ns / 1000.0, ns / tickNs);
        };
        std::printf("micro costs (median, optimized CPU, 1 thread)\n");
        row("tick, AdvanceTicks(1) only", Median(rawTickNs));
        row("tick, with condition + evaluator observe", tickNs);
        row("CaptureState", Median(captureNs));
        row("release captured state", Median(releaseNs));
        row("RestoreState", Median(restoreNs));
        row("ReplaceInputWindow, one D=10 segment", Median(segmentPatchNs));
        row("ReplaceInputWindow, whole window (flat)", Median(windowPatchNs));
        row("CreateSession (stand-in for session copy)", Median(sessionNs));
        row("composite Mutate, whole window", Median(mutateNs));
        const double nodeNs = Median(restoreNs) + Median(segmentPatchNs) +
                Median(captureNs) + Median(releaseNs) + Median(sessionNs);
        row("node = restore+segment patch+capture+release+session", nodeNs);

        // Tick cost along the window. A binary tree simulates segment s
        // 2^(s+1) times per 2^D leaves, so late ticks dominate its cost.
        std::vector<std::vector<double>> baselineSegmentNs(segments10.size());
        std::vector<std::vector<double>> mutatedSegmentNs(segments10.size());
        for (std::uint32_t rep = 0u; rep < scaled(100u); ++rep) {
            for (const bool mutated : {false, true}) {
                PhysicsSandboxStateView view =
                        Require(sandbox.RestoreState(branch), "restoring");
                if (mutated) {
                    const Patch &patch = pool[poolIndex++ % poolSize];
                    Require(sandbox.ReplaceInputWindow(patch.minimumTimeMs,
                                                       patch.maximumTimeMs,
                                                       patch.events),
                            "replacing window");
                }
                for (std::size_t index = 0u; index < segments10.size();
                     ++index) {
                    const auto t0 = Clock::now();
                    view = bench.SimulateObserved(
                            segments10[index].ticks, view, *session);
                    (mutated ? mutatedSegmentNs : baselineSegmentNs)[index]
                            .push_back(ElapsedNs(t0, Clock::now()) /
                                       segments10[index].ticks);
                }
            }
        }
        std::printf("\ntick cost by D=10 segment (us per tick)\n"
                    "  segment  inputs ms      baseline  mutated\n");
        double flatProfileNs = 0.0;
        double treeProfileNs = 0.0;
        for (std::size_t index = 0u; index < segments10.size(); ++index) {
            const double baselineNs = Median(baselineSegmentNs[index]);
            const double mutatedNs = Median(mutatedSegmentNs[index]);
            std::printf("  %7zu  %5lld-%5lld  %8.2f  %7.2f\n",
                        index,
                        static_cast<long long>(segments10[index].firstInputMs),
                        static_cast<long long>(segments10[index].lastInputMs),
                        baselineNs / 1000.0,
                        mutatedNs / 1000.0);
            const double segmentNs = mutatedNs * segments10[index].ticks;
            flatProfileNs += segmentNs;
            treeProfileNs += segmentNs *
                    static_cast<double>(2u << index) /
                    static_cast<double>(1u << segments10.size());
        }
        std::printf("  D=10 tick-only speedup weighted by this profile: "
                    "%.2fx\n",
                    flatProfileNs / treeProfileNs);

        // 3. End to end: flat iterations against binary trees whose nodes
        // replay a segment slice of a pooled candidate. Mutation draws are
        // excluded from both so only simulation and node operations count.
        const std::uint32_t flatIterations = scaled(2000u);
        const auto flat0 = Clock::now();
        for (std::uint32_t iteration = 0u; iteration < flatIterations;
             ++iteration) {
            const PhysicsSandboxStateView view =
                    Require(sandbox.RestoreState(branch), "restoring");
            const Patch &patch = pool[iteration % poolSize];
            Require(sandbox.ReplaceInputWindow(patch.minimumTimeMs,
                                               patch.maximumTimeMs,
                                               patch.events),
                    "replacing window");
            std::unique_ptr<IterationEvaluationSession> iterationSession =
                    evaluator->CreateSession();
            bench.SimulateObserved(
                    windowTicks + tailTicks, view, *iterationSession);
        }
        const double flatNs =
                ElapsedNs(flat0, Clock::now()) / flatIterations;
        std::printf("\nend to end (flat iteration = %u ticks)\n",
                    windowTicks + tailTicks);
        std::printf("  %-44s %9.2f us\n", "flat iteration", flatNs / 1000.0);

        for (const std::uint32_t depth : {10u, 15u}) {
            const std::vector<Segment> segments =
                    Segments(firstMs, lastMs, depth, tickMs);
            std::vector<std::vector<std::vector<PhysicsSandboxInputEvent>>>
                    slices(poolSize);
            for (std::uint32_t index = 0u; index < poolSize; ++index) {
                for (const Segment &segment : segments) {
                    slices[index].push_back(Slice(pool[index], segment));
                }
            }
            std::uint64_t leaves = 0u;
            std::uint64_t ticks = 0u;
            std::uint32_t draw = 0u;
            const std::function<void(std::uint32_t,
                                     const PhysicsSandboxState &,
                                     PhysicsSandboxStateView)>
                    expand = [&](std::uint32_t level,
                                 const PhysicsSandboxState &parent,
                                 PhysicsSandboxStateView parentView) {
                for (std::uint32_t child = 0u; child < 2u; ++child) {
                    // The first child continues from the state just
                    // captured, so only the second child restores.
                    const PhysicsSandboxStateView view = child == 0u
                            ? parentView
                            : Require(sandbox.RestoreState(parent),
                                      "restoring");
                    const Segment &segment = segments[level];
                    Require(sandbox.ReplaceInputWindow(
                                    segment.firstInputMs,
                                    segment.lastInputMs,
                                    slices[draw++ % poolSize][level]),
                            "replacing segment");
                    std::unique_ptr<IterationEvaluationSession> nodeSession =
                            evaluator->CreateSession();
                    const bool leaf = level + 1u == depth;
                    const std::uint32_t count =
                            segment.ticks + (leaf ? tailTicks : 0u);
                    const PhysicsSandboxStateView end =
                            bench.SimulateObserved(count, view, *nodeSession);
                    ticks += count;
                    if (leaf) {
                        ++leaves;
                        continue;
                    }
                    const PhysicsSandboxState state =
                            Require(sandbox.CaptureState(), "capturing");
                    expand(level + 1u, state, end);
                }
            };
            const std::uint32_t trees = depth == 10u ? scaled(4u) : 1u;
            const auto tree0 = Clock::now();
            for (std::uint32_t tree = 0u; tree < trees; ++tree) {
                const PhysicsSandboxStateView view =
                        Require(sandbox.RestoreState(branch), "restoring");
                expand(0u, branch, view);
            }
            const double leafNs = ElapsedNs(tree0, Clock::now()) / leaves;
            const double tickRatio =
                    static_cast<double>(windowTicks + tailTicks) /
                    (static_cast<double>(ticks) / leaves);
            std::printf("  D=%-2u tree leaf (%llu leaves, %.1f ticks/leaf)"
                        "      %9.2f us  -> %.2fx measured, %.2fx tick-only\n",
                        depth,
                        static_cast<unsigned long long>(leaves),
                        static_cast<double>(ticks) / leaves,
                        leafNs / 1000.0,
                        flatNs / leafNs,
                        tickRatio);
        }
        std::printf("(observed samples: %llu)\n",
                    static_cast<unsigned long long>(bench.observed_));
    } catch (const std::exception &error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
    return 0;
}
