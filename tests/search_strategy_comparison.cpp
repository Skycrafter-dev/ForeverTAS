// Runs the persisted ForeverTAS search configuration through RunSearch with
// the basic bruteforce and the tree search for the same wall-clock budget.
//
// Usage: forevertas-search-strategy-comparison SECONDS WORKERS SEGMENTS...
// WORKERS=1 uses the optimized CPU backend, more uses multi-threaded CPU.
// FOREVERTAS_COMPARE_TARGET selects another saved evaluation target.
// FOREVERTAS_COMPARE_TRIALS repeats the comparison with fresh modifier seeds.
// Run with XDG_CONFIG_HOME pointing at a copy of the ForeverTAS settings
// directory: SearchConfigurationModel may migrate and persist settings.

#include "app/search_configuration_model.h"
#include "conditions/condition_program.h"
#include "mutations/input_event_formatter.h"
#include "searches/algorithm_registry.h"
#include "searches/search_runner.h"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace forevertas;

struct Outcome {
    std::uint64_t iterations = 0u;
    double seconds = 0.0;
    std::uint64_t improvements = 0u;
    double changesPerAttempt = 0.0;
    std::string best;
};

Outcome Run(SearchRequest request, double seconds) {
    std::atomic_bool stop{false};
    std::mutex mutex;
    std::uint64_t reportedIterations = 0u;
    SearchRunControl control;
    control.stopRequested = [&stop]() {
        return stop.load(std::memory_order_relaxed);
    };
    control.statisticsChanged = [&](const SearchStatisticsUpdate &update) {
        std::lock_guard<std::mutex> guard(mutex);
        reportedIterations = update.iterations;
    };
    control.sampleBestTimeline = false;
    control.sampleImprovementTimelines = false;

    // The budget starts when mutations start, after loading and baseline.
    std::atomic_bool searching{false};
    std::chrono::steady_clock::time_point searchStarted;
    control.progressChanged = [&](const SearchProgress &progress) {
        if (progress.stage == SearchProgressStage::Mutations &&
            !searching.load(std::memory_order_acquire)) {
            searchStarted = std::chrono::steady_clock::now();
            searching.store(true, std::memory_order_release);
        }
    };
    std::thread timer([&]() {
        while (!searching.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
        stop.store(true, std::memory_order_relaxed);
    });
    std::optional<SearchResult> result;
    try {
        result.emplace(RunSearch(request, &control));
    } catch (...) {
        searching.store(true, std::memory_order_release);
        stop.store(true);
        timer.join();
        throw;
    }
    timer.join();
    Outcome outcome;
    outcome.iterations = result->iterations;
    outcome.seconds = std::chrono::duration<double>(
                              std::chrono::steady_clock::now() -
                              searchStarted)
                              .count();
    outcome.improvements = result->mutationImprovementCount;
    outcome.changesPerAttempt = result->iterations == 0u
            ? 0.0
            : static_cast<double>(result->totalMutationCount) /
                    static_cast<double>(result->iterations);
    outcome.best = result->bestEvaluationDescription;
    std::lock_guard<std::mutex> guard(mutex);
    if (reportedIterations > result->iterations) {
        throw std::runtime_error("reported more attempts than completed");
    }
    return outcome;
}

QString Stored(const QSettings &settings, const char *key) {
    return settings.value(QString::fromLatin1(key)).toString();
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ForeverTAS"));
    QCoreApplication::setApplicationName(QStringLiteral("ForeverTAS"));
    const QByteArray configHome = qgetenv("XDG_CONFIG_HOME");
    if (argc < 3 || configHome.isEmpty() ||
        QDir(QString::fromLocal8Bit(configHome)) ==
                QDir(QDir::homePath() + QStringLiteral("/.config"))) {
        std::fprintf(stderr,
                     "usage: XDG_CONFIG_HOME=<settings copy> %s SECONDS "
                     "WORKERS [SEGMENTS...]\n",
                     argv[0]);
        return 2;
    }
    const double seconds = std::atof(argv[1]);
    const std::uint32_t workers =
            static_cast<std::uint32_t>(std::atoi(argv[2]));
    std::vector<std::string> segmentCounts;
    for (int index = 3; index < argc; ++index) {
        segmentCounts.emplace_back(argv[index]);
    }
    if (segmentCounts.empty()) segmentCounts.emplace_back("10");

    try {
        QSettings settings;
        const std::uint32_t horizonMs =
                settings.value(QStringLiteral("search/simulationHorizonMs"),
                               kDefaultSimulationHorizonMs).toUInt();
        app::SearchConfigurationModel model;
        const QByteArray target = qgetenv("FOREVERTAS_COMPARE_TARGET");
        if (!target.isEmpty() &&
            !model.setEvaluationTargetId(QString::fromUtf8(target)) &&
            model.evaluationTargetId() != QString::fromUtf8(target)) {
            throw std::runtime_error("unknown evaluation target");
        }
        const QByteArray trialsText = qgetenv("FOREVERTAS_COMPARE_TRIALS");
        const int trials = trialsText.isEmpty() ? 1 : trialsText.toInt();
        std::vector<std::uint64_t> totalAttempts(segmentCounts.size() + 1u);
        std::vector<double> totalSeconds(segmentCounts.size() + 1u);
        std::vector<std::uint64_t> totalImprovements(
                segmentCounts.size() + 1u);
        for (int trial = 0; trial < trials; ++trial) {
            if (trials > 1) {
                // Mirrors "Randomize modifier seeds on Start".
                model.randomizeModifierSeeds(
                        0x9e3779b9u * static_cast<std::uint32_t>(trial + 1));
            }
            const app::SearchConfigurationValidation validation =
                    model.validate(kSearchTickDurationMs, horizonMs);
            if (!validation.configuration) {
                throw std::runtime_error(validation.error.toStdString());
            }
            const InputScriptParseResult parsed = ParseInputScript(
                    Stored(settings, "inputs/baseScript").toStdString());
            if (!parsed) throw std::runtime_error(*parsed.error);
            ConditionCompileResult condition = CompileConditionScript(
                    Stored(settings, "search/conditionScript").toStdString());
            if (condition.error) throw std::runtime_error(*condition.error);

            SearchRequest request(
                    Stored(settings, "paths/packsDirectory").toStdString(),
                    Stored(settings, "paths/replayPath").toStdString());
            request.backend = workers > 1u ? PhysicsBackend::MultiThreadedCpu
                                           : PhysicsBackend::OptimizedCpu;
            request.parallelSampleCount = workers > 1u ? workers : 1u;
            request.modifiers = validation.configuration->modifiers;
            request.evaluationTarget =
                    validation.configuration->evaluationTarget;
            request.baseInputCommands = parsed.commands;
            request.simulationHorizonMs = horizonMs;
            request.condition = std::move(condition.program);
            const std::string autoPromote =
                    validation.configuration->searchAlgorithm.settings.count(
                            "autoPromoteBest") != 0u
                    ? validation.configuration->searchAlgorithm.settings.at(
                              "autoPromoteBest")
                    : "false";

            std::printf("%s, %u worker(s), %.0f s each, evaluation %s, "
                        "auto-promote %s\n",
                        workers > 1u ? "multi-threaded CPU" : "optimized CPU",
                        workers, seconds,
                        request.evaluationTarget.id.c_str(),
                        autoPromote.c_str());
            request.searchAlgorithm = {kBasicBruteForceSearchId,
                                       {{"autoPromoteBest", autoPromote}}};
            const Outcome basic = Run(request, seconds);
            totalAttempts[0] += basic.iterations;
            totalSeconds[0] += basic.seconds;
            totalImprovements[0] += basic.improvements;
            std::printf("  %-22s %12llu attempts %10.0f /s  %5.1f changes  "
                        "%4llu improvements  %s\n",
                        "basic bruteforce",
                        static_cast<unsigned long long>(basic.iterations),
                        basic.iterations / basic.seconds,
                        basic.changesPerAttempt,
                        static_cast<unsigned long long>(basic.improvements),
                        basic.best.c_str());
            for (std::size_t treeIndex = 0u;
                 treeIndex < segmentCounts.size();
                 ++treeIndex) {
                const std::string &segments = segmentCounts[treeIndex];
                request.searchAlgorithm = {
                        kTreeSearchId,
                        {{"segmentCount", segments},
                         {"autoPromoteBest", autoPromote}}};
                const Outcome tree = Run(request, seconds);
                totalAttempts[treeIndex + 1u] += tree.iterations;
                totalSeconds[treeIndex + 1u] += tree.seconds;
                totalImprovements[treeIndex + 1u] += tree.improvements;
                const std::string name = "tree, " + segments + " segments";
                std::printf("  %-22s %12llu attempts %10.0f /s  %5.1f "
                            "changes  %4llu improvements  %s  "
                            "(%.2fx attempts/s)\n",
                            name.c_str(),
                            static_cast<unsigned long long>(tree.iterations),
                            tree.iterations / tree.seconds,
                            tree.changesPerAttempt,
                            static_cast<unsigned long long>(tree.improvements),
                            tree.best.c_str(),
                            (tree.iterations / tree.seconds) /
                                    (basic.iterations / basic.seconds));
            }
        }
        if (trials > 1) {
            std::printf("totals over %d trials:\n", trials);
            for (std::size_t index = 0u; index < totalAttempts.size();
                 ++index) {
                const std::string name = index == 0u
                        ? std::string("basic bruteforce")
                        : "tree, " + segmentCounts[index - 1u] + " segments";
                std::printf("  %-22s %10.0f attempts/s  %4llu improvements "
                            "(%.2f per minute)\n",
                            name.c_str(),
                            totalAttempts[index] / totalSeconds[index],
                            static_cast<unsigned long long>(
                                    totalImprovements[index]),
                            totalImprovements[index] * 60.0 /
                                    totalSeconds[index]);
            }
        }
    } catch (const std::exception &error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
    return 0;
}
