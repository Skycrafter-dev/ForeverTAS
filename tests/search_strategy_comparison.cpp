// Runs the persisted ForeverTAS search configuration through RunSearch with
// the basic bruteforce and the tree search for the same wall-clock budget.
//
// Usage: forevertas-search-strategy-comparison SECONDS WORKERS VARIANT...
// A VARIANT is a tree segment count, "basic", or "basic:" / "tree:" followed
// by comma-separated KEY=VALUE algorithm settings, for example
// "tree:segmentCount=4,leafCount=64". When every variant is a segment count,
// the basic bruteforce runs first as the reference.
// WORKERS=1 uses the optimized CPU backend, more uses multi-threaded CPU.
// FOREVERTAS_COMPARE_TARGET selects another saved evaluation target, and
// FOREVERTAS_COMPARE_TARGET_SETTINGS="KEY=VALUE,..." overrides its settings
// without persisting them.
// FOREVERTAS_COMPARE_TRIALS repeats the comparison with fresh modifier seeds;
// FOREVERTAS_COMPARE_FIRST_TRIAL skips the seeds of earlier trials.
// The summary reports each variant's final scores and its best score after a
// third and two thirds of the budget.
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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace forevertas;

struct Outcome {
    std::uint64_t iterations = 0u;
    double seconds = 0.0;
    std::uint64_t improvements = 0u;
    double changesPerAttempt = 0.0;
    double score = 0.0;
    // Best score after a third and two thirds of the budget.
    double earlyScores[2] = {0.0, 0.0};
    std::string best;
};

struct Variant {
    std::string label;
    std::string algorithmId;
    OptionSettings overrides;
};

std::vector<std::pair<std::string, std::string>> ParseAssignments(
        const std::string &text) {
    std::vector<std::pair<std::string, std::string>> assignments;
    std::size_t begin = 0u;
    while (begin < text.size()) {
        std::size_t end = text.find(',', begin);
        if (end == std::string::npos) end = text.size();
        const std::string item = text.substr(begin, end - begin);
        const std::size_t equals = item.find('=');
        if (equals == std::string::npos || equals == 0u) {
            throw std::runtime_error("expected KEY=VALUE, got: " + item);
        }
        assignments.emplace_back(item.substr(0u, equals),
                                 item.substr(equals + 1u));
        begin = end + 1u;
    }
    return assignments;
}

bool IsSegmentCount(const std::string &text) {
    return !text.empty() &&
            std::all_of(text.begin(), text.end(), [](char character) {
                return character >= '0' && character <= '9';
            });
}

Variant ParseVariant(const std::string &text) {
    if (IsSegmentCount(text)) {
        return {"tree, " + text + " segments",
                kTreeSearchId,
                {{"segmentCount", text}}};
    }
    const std::size_t colon = text.find(':');
    const std::string name = text.substr(0u, colon);
    Variant variant;
    variant.label = text;
    if (name == "basic") {
        variant.algorithmId = kBasicBruteForceSearchId;
    } else if (name == "tree") {
        variant.algorithmId = kTreeSearchId;
    } else {
        throw std::runtime_error("unknown variant: " + text);
    }
    if (colon != std::string::npos) {
        for (auto &[key, value] :
             ParseAssignments(text.substr(colon + 1u))) {
            variant.overrides[key] = value;
        }
    }
    return variant;
}

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
    std::vector<std::pair<double, double>> bestTimeline;
    control.liveChanged = [&](const SearchLiveUpdate &live) {
        const double elapsed = searching.load(std::memory_order_acquire)
                ? std::chrono::duration<double>(
                          std::chrono::steady_clock::now() - searchStarted)
                          .count()
                : 0.0;
        std::lock_guard<std::mutex> guard(mutex);
        bestTimeline.emplace_back(elapsed, live.bestScore);
    };
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
    outcome.score = result->bestScore;
    outcome.best = result->bestEvaluationDescription;
    std::lock_guard<std::mutex> guard(mutex);
    for (int index = 0; index < 2; ++index) {
        const double limit = seconds * (index + 1) / 3.0;
        outcome.earlyScores[index] = bestTimeline.empty()
                ? result->bestScore
                : bestTimeline.front().second;
        for (const auto &[elapsed, score] : bestTimeline) {
            if (elapsed <= limit) outcome.earlyScores[index] = score;
        }
    }
    if (reportedIterations > result->iterations) {
        throw std::runtime_error("reported more attempts than completed");
    }
    return outcome;
}

QString Stored(const QSettings &settings, const char *key) {
    return settings.value(QString::fromLatin1(key)).toString();
}

struct Totals {
    std::uint64_t attempts = 0u;
    double seconds = 0.0;
    std::uint64_t improvements = 0u;
    std::vector<double> scores;
    std::vector<double> earlyScores[2];
};

std::string Spread(const std::vector<double> &values) {
    if (values.empty()) return "n/a";
    double mean = 0.0;
    for (const double value : values) mean += value;
    mean /= static_cast<double>(values.size());
    double variance = 0.0;
    for (const double value : values) {
        variance += (value - mean) * (value - mean);
    }
    const double deviation = values.size() < 2u
            ? 0.0
            : std::sqrt(variance / static_cast<double>(values.size() - 1u));
    const auto [low, high] = std::minmax_element(values.begin(), values.end());
    char text[128];
    std::snprintf(text, sizeof(text), "%.4f +- %.4f [%.4f, %.4f]",
                  mean, deviation, *low, *high);
    return text;
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    std::setlocale(LC_NUMERIC, "C");
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
    try {
        std::vector<Variant> variants;
        bool onlySegmentCounts = true;
        for (int index = 3; index < argc; ++index) {
            variants.push_back(ParseVariant(argv[index]));
            onlySegmentCounts = onlySegmentCounts && IsSegmentCount(argv[index]);
        }
        if (variants.empty()) variants.push_back(ParseVariant("10"));
        if (onlySegmentCounts) {
            variants.insert(variants.begin(),
                            {"basic bruteforce", kBasicBruteForceSearchId, {}});
        }

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
        for (const auto &[key, value] : ParseAssignments(
                     qgetenv("FOREVERTAS_COMPARE_TARGET_SETTINGS")
                             .toStdString())) {
            if (!model.setEvaluationTargetSetting(
                        QString::fromStdString(key),
                        QString::fromStdString(value),
                        false) &&
                model.evaluationTargetSettingsFor(model.evaluationTargetId())
                                .value(QString::fromStdString(key))
                                .toString() != QString::fromStdString(value)) {
                throw std::runtime_error("unknown evaluation setting: " + key);
            }
        }
        const QByteArray trialsText = qgetenv("FOREVERTAS_COMPARE_TRIALS");
        const int trials = trialsText.isEmpty() ? 1 : trialsText.toInt();
        const int firstTrial =
                qgetenv("FOREVERTAS_COMPARE_FIRST_TRIAL").toInt();
        std::vector<Totals> totals(variants.size());
        for (int trial = 0; trial < trials; ++trial) {
            if (trials > 1 || firstTrial > 0) {
                // Mirrors "Randomize modifier seeds on Start".
                model.randomizeModifierSeeds(
                        0x9e3779b9u *
                        static_cast<std::uint32_t>(firstTrial + trial + 1));
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
            double basicRate = 0.0;
            for (std::size_t index = 0u; index < variants.size(); ++index) {
                const Variant &variant = variants[index];
                const SearchAlgorithmRegistration *const registration =
                        FindSearchAlgorithm(variant.algorithmId);
                if (registration == nullptr) {
                    throw std::runtime_error("unknown search algorithm");
                }
                OptionSettings settings = registration->defaultSettings;
                settings["autoPromoteBest"] = autoPromote;
                for (const auto &[key, value] : variant.overrides) {
                    settings[key] = value;
                }
                if (const auto error = registration->validateSettings(
                            settings, kSearchTickDurationMs)) {
                    throw std::runtime_error(variant.label + ": " + *error);
                }
                request.searchAlgorithm = {variant.algorithmId, settings};
                const Outcome outcome = Run(request, seconds);
                Totals &total = totals[index];
                total.attempts += outcome.iterations;
                total.seconds += outcome.seconds;
                total.improvements += outcome.improvements;
                total.scores.push_back(outcome.score);
                total.earlyScores[0].push_back(outcome.earlyScores[0]);
                total.earlyScores[1].push_back(outcome.earlyScores[1]);
                const double rate = outcome.iterations / outcome.seconds;
                if (index == 0u) basicRate = rate;
                std::printf("  %-22s %12llu attempts %10.0f /s  %5.1f "
                            "changes  %4llu improvements  %s  "
                            "(%.2fx attempts/s)\n",
                            variant.label.c_str(),
                            static_cast<unsigned long long>(
                                    outcome.iterations),
                            rate,
                            outcome.changesPerAttempt,
                            static_cast<unsigned long long>(
                                    outcome.improvements),
                            outcome.best.c_str(),
                            rate / basicRate);
            }
        }
        std::printf("totals over %d trial(s):\n", trials);
        for (std::size_t index = 0u; index < variants.size(); ++index) {
            const Totals &total = totals[index];
            std::printf("  %-22s %10.0f attempts/s  %4llu improvements "
                        "(%.2f per minute)\n"
                        "    final score %s\n"
                        "    at 1/3 %s, at 2/3 %s\n",
                        variants[index].label.c_str(),
                        total.attempts / total.seconds,
                        static_cast<unsigned long long>(total.improvements),
                        total.improvements * 60.0 / total.seconds,
                        Spread(total.scores).c_str(),
                        Spread(total.earlyScores[0]).c_str(),
                        Spread(total.earlyScores[1]).c_str());
        }
    } catch (const std::exception &error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
    return 0;
}
