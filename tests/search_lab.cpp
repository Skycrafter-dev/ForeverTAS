// Strategy laboratory: runs search variants on scenarios described in JSON,
// without reading or writing any persisted settings.
//
// forevertas-search-lab --probe SCENARIO.json
//     Prints the baseline finish time and the car state every 250 ms.
// forevertas-search-lab SCENARIO.json SECONDS WORKERS TRIALS FIRST_TRIAL
//                       VARIANT...
//     Prints one CSV row per run. A VARIANT is LABEL=ALGORITHM[:KEY=VALUE,...]
//     where ALGORITHM is a registered search algorithm ID. Trials reseed every
//     modifier pass; variants run interleaved within each trial.
//
// Scenario keys: name, packs, replay, baseScript ("@replay" for the replay's
// own inputs, otherwise script text), horizonMs, condition, autoPromoteBest,
// modifiers [{id, settings}], evaluation {id, settings}. Settings are merged
// over the registered defaults and use the UI's user-timeline values.

#include "conditions/condition_program.h"
#include "mutations/input_event_formatter.h"
#include "mutations/input_event_utils.h"
#include "mutations/replay_input_script.h"
#include "physics_backend.h"
#include "replay_file_io.h"
#include "searches/algorithm_registry.h"
#include "searches/search_runner.h"

#include <forevervalidator/experimental/physics_sandbox.h>
#include <forevervalidator/native.h>

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <array>
#include <atomic>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace forevertas;

struct Scenario {
    std::string name;
    std::string packs;
    std::string replay;
    std::vector<ParsedInputCommand> baseCommands;
    std::uint32_t horizonMs = kDefaultSimulationHorizonMs;
    std::string condition;
    bool autoPromoteBest = true;
    std::vector<OptionConfiguration> modifiers;
    OptionConfiguration evaluation;
};

std::string Text(const QJsonValue &value) {
    if (value.isString()) return value.toString().toStdString();
    if (value.isBool()) return value.toBool() ? "true" : "false";
    if (value.isDouble()) {
        const double number = value.toDouble();
        if (number == static_cast<double>(static_cast<long long>(number))) {
            return std::to_string(static_cast<long long>(number));
        }
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.9g", number);
        return buffer;
    }
    throw std::runtime_error("unsupported JSON setting value");
}

OptionSettings Merge(const OptionSettings &defaults, const QJsonObject &over) {
    OptionSettings settings = defaults;
    for (auto it = over.begin(); it != over.end(); ++it) {
        const std::string key = it.key().toStdString();
        if (settings.find(key) == settings.end()) {
            throw std::runtime_error("unknown setting: " + key);
        }
        settings[key] = Text(it.value());
    }
    return settings;
}

Scenario LoadScenario(const char *path) {
    QFile file(QString::fromLocal8Bit(path));
    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(std::string("cannot open ") + path);
    }
    QJsonParseError parseError;
    const QJsonDocument document =
            QJsonDocument::fromJson(file.readAll(), &parseError);
    if (!document.isObject()) {
        throw std::runtime_error("scenario JSON: " +
                                 parseError.errorString().toStdString());
    }
    const QJsonObject json = document.object();
    Scenario scenario;
    scenario.name = json.value("name").toString().toStdString();
    scenario.packs = json.value("packs").toString().toStdString();
    scenario.replay = json.value("replay").toString().toStdString();
    scenario.horizonMs = static_cast<std::uint32_t>(
            json.value("horizonMs").toInt(kDefaultSimulationHorizonMs));
    scenario.condition = json.value("condition").toString().toStdString();
    scenario.autoPromoteBest = json.value("autoPromoteBest").toBool(true);
    const std::string base = json.value("baseScript").toString().toStdString();
    const InputScriptParseResult parsed = ParseInputScript(
            base == "@replay"
                    ? ExtractReplayInputScript(scenario.packs, scenario.replay)
                    : base);
    if (!parsed) throw std::runtime_error("base script: " + *parsed.error);
    scenario.baseCommands = parsed.commands;
    for (const QJsonValue &entry : json.value("modifiers").toArray()) {
        const QJsonObject pass = entry.toObject();
        const std::string id = pass.value("id").toString().toStdString();
        const ModifierRegistration *registration = FindModifier(id);
        if (registration == nullptr) {
            throw std::runtime_error("unknown modifier: " + id);
        }
        OptionSettings settings = Merge(registration->defaultSettings,
                                        pass.value("settings").toObject());
        if (const auto error =
                    registration->validateSettings(settings,
                                                   kSearchTickDurationMs)) {
            throw std::runtime_error(id + ": " + *error);
        }
        scenario.modifiers.push_back({id, settings});
    }
    const QJsonObject evaluation = json.value("evaluation").toObject();
    const std::string id = evaluation.value("id").toString().toStdString();
    const EvaluationTargetRegistration *registration =
            FindEvaluationTarget(id);
    if (registration == nullptr) {
        throw std::runtime_error("unknown evaluation target: " + id);
    }
    scenario.evaluation = {id,
                           Merge(registration->defaultSettings,
                                 evaluation.value("settings").toObject())};
    if (const auto error = registration->validateSettings(
                scenario.evaluation.settings, kSearchTickDurationMs)) {
        throw std::runtime_error(id + ": " + *error);
    }
    return scenario;
}

template <typename T, typename Error>
T Require(forevervalidator::DiscriminatedResult<T, Error> result,
          const char *operation) {
    if (!result) {
        throw std::runtime_error(std::string(operation) + " failed: " +
                                 result.Error().diagnostic);
    }
    return std::move(result).Value();
}

int Probe(const Scenario &scenario) {
    using namespace forevervalidator::experimental;
    PhysicsSandboxOptions options;
    options.backend = ToForeverValidatorBackend(PhysicsBackend::OptimizedCpu);
    options.tickDurationMs = kSearchTickDurationMs;
    options.timelineMode = PhysicsSandboxTimelineMode::Canonical;
    options.simulationHorizonMs = scenario.horizonMs;
    const forevervalidator::ReplayIdentity identity{scenario.replay};
    const forevervalidator::AssetBytes replay = Require(
            ReadReplayFileUtf8(scenario.replay, identity), "reading replay");
    PhysicsSandbox sandbox = Require(
            CreatePhysicsSandbox(
                    Require(forevervalidator::OpenInstalledPackDirectory(
                                    scenario.packs),
                            "opening packs"),
                    options),
            "creating sandbox");
    Require(sandbox.LoadScenario({replay.data(), replay.size()}, identity),
            "loading scenario");
    InputScriptBaselineResult baseline = BuildInputScriptBaseline(
            Require(sandbox.ReadInputs(), "reading inputs"),
            scenario.baseCommands,
            kSearchTickDurationMs);
    if (!baseline) throw std::runtime_error(*baseline.error);
    ConvertKeyboardSteeringToAnalog(baseline.events);
    Require(sandbox.ReplaceInputs(std::move(baseline.events)),
            "applying base script");
    std::printf("time_ms,x,y,z,speed_kmh,checkpoints,completed,finish_ms\n");
    PhysicsSandboxStateView state =
            Require(sandbox.ReadState(), "reading state");
    while (state.timeMs < scenario.horizonMs) {
        state = Require(sandbox.AdvanceTicks(1u), "advancing");
        const bool report = state.timeMs % 250u == 0u ||
                (state.raceCompleted && state.finishTimeMs &&
                 state.timeMs == *state.finishTimeMs + 10u);
        if (!report) continue;
        const auto &speed = state.car.linearSpeed;
        std::printf("%llu,%.3f,%.3f,%.3f,%.1f,%u/%u,%d,%d\n",
                    static_cast<unsigned long long>(state.timeMs),
                    state.car.position.x, state.car.position.y,
                    state.car.position.z,
                    3.6 * std::sqrt(speed.x * speed.x + speed.y * speed.y +
                                    speed.z * speed.z),
                    state.checkpointsCollected, state.checkpointsTotal,
                    state.raceCompleted ? 1 : 0,
                    state.finishTimeMs
                            ? static_cast<int>(*state.finishTimeMs)
                            : -1);
    }
    return 0;
}

struct Variant {
    std::string label;
    std::string algorithmId;
    OptionSettings overrides;
};

Variant ParseVariant(const std::string &text) {
    const std::size_t equals = text.find('=');
    const std::size_t colon = text.find(':');
    if (equals == std::string::npos ||
        (colon != std::string::npos && colon < equals)) {
        throw std::runtime_error("variant must be LABEL=ALGORITHM[:...]");
    }
    Variant variant;
    variant.label = text.substr(0, equals);
    const std::string rest = text.substr(equals + 1u);
    const std::size_t split = rest.find(':');
    variant.algorithmId = rest.substr(0, split);
    if (split != std::string::npos) {
        std::size_t start = split + 1u;
        while (start < rest.size()) {
            const std::size_t end = rest.find(',', start);
            const std::string assignment = rest.substr(
                    start, end == std::string::npos ? std::string::npos
                                                    : end - start);
            const std::size_t at = assignment.find('=');
            if (at == std::string::npos) {
                throw std::runtime_error("bad assignment: " + assignment);
            }
            variant.overrides[assignment.substr(0, at)] =
                    assignment.substr(at + 1u);
            if (end == std::string::npos) break;
            start = end + 1u;
        }
    }
    return variant;
}

constexpr int kCheckpoints = 10;

struct Outcome {
    std::uint64_t attempts = 0u;
    double seconds = 0.0;
    std::uint64_t improvements = 0u;
    double changes = 0.0;
    double baselineScore = 0.0;
    double finalScore = 0.0;
    std::array<double, kCheckpoints> checkpoints{};
    std::string description;
};

// The baseline alone: zero attempts on the optimized CPU backend.
double BaselineScore(SearchRequest request) {
    request.backend = PhysicsBackend::OptimizedCpu;
    request.parallelSampleCount = 1u;
    request.searchAlgorithm = DefaultSearchAlgorithmConfiguration();
    SearchRunControl control;
    control.iterationLimit = 0u;
    control.sampleBestTimeline = false;
    control.sampleImprovementTimelines = false;
    return RunSearch(request, &control).bestScore;
}

Outcome Run(const SearchRequest &request, double seconds,
            double baselineScore,
            std::optional<std::uint64_t> attemptLimit) {
    std::atomic_bool stop{false};
    std::atomic_bool searching{false};
    std::chrono::steady_clock::time_point started;
    std::mutex mutex;
    std::vector<std::pair<double, double>> timeline;
    SearchRunControl control;
    control.stopRequested = [&]() { return stop.load(); };
    control.sampleBestTimeline = false;
    control.sampleImprovementTimelines = false;
    control.iterationLimit = attemptLimit;
    control.progressChanged = [&](const SearchProgress &progress) {
        if (progress.stage == SearchProgressStage::Mutations &&
            !searching.load()) {
            started = std::chrono::steady_clock::now();
            searching.store(true);
        }
    };
    control.liveChanged = [&](const SearchLiveUpdate &live) {
        std::lock_guard<std::mutex> guard(mutex);
        const double elapsed = searching.load()
                ? std::chrono::duration<double>(
                          std::chrono::steady_clock::now() - started)
                          .count()
                : 0.0;
        timeline.emplace_back(elapsed, live.bestScore);
    };
    std::thread timer([&]() {
        while (!searching.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        // A fixed attempt count replaces the time budget when given.
        const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::duration<double>(seconds);
        while (!stop.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        stop.store(true);
    });
    std::optional<SearchResult> result;
    try {
        result.emplace(RunSearch(request, &control));
    } catch (...) {
        searching.store(true);
        stop.store(true);
        timer.join();
        throw;
    }
    stop.store(true);
    timer.join();
    Outcome outcome;
    outcome.attempts = result->iterations;
    outcome.seconds = std::chrono::duration<double>(
                              std::chrono::steady_clock::now() - started)
                              .count();
    outcome.improvements = result->mutationImprovementCount;
    outcome.changes = result->iterations == 0u
            ? 0.0
            : static_cast<double>(result->totalMutationCount) /
                    static_cast<double>(result->iterations);
    outcome.finalScore = result->bestScore;
    outcome.description = result->bestEvaluationDescription;
    std::lock_guard<std::mutex> guard(mutex);
    outcome.baselineScore = baselineScore;
    for (int index = 0; index < kCheckpoints; ++index) {
        const double limit = seconds * (index + 1) / kCheckpoints;
        double score = outcome.baselineScore;
        for (const auto &[elapsed, value] : timeline) {
            if (elapsed <= limit) score = value;
        }
        outcome.checkpoints[static_cast<std::size_t>(index)] =
                index + 1 == kCheckpoints ? outcome.finalScore : score;
    }
    return outcome;
}

std::uint32_t TrialSeed(int trial, std::size_t pass) {
    std::uint64_t value = 0x9e3779b97f4a7c15ull *
            (static_cast<std::uint64_t>(trial) * 131u + pass + 1u);
    value ^= value >> 31u;
    value *= 0xbf58476d1ce4e5b9ull;
    value ^= value >> 27u;
    return static_cast<std::uint32_t>(value >> 16u);
}

std::string Csv(std::string text) {
    for (char &character : text) {
        if (character == ',' || character == '\n') character = ';';
    }
    return text;
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    std::setlocale(LC_NUMERIC, "C");
    try {
        if (argc == 3 && std::string(argv[1]) == "--probe") {
            return Probe(LoadScenario(argv[2]));
        }
        if (argc < 7) {
            std::fprintf(stderr,
                         "usage: %s SCENARIO.json SECONDS WORKERS TRIALS "
                         "FIRST_TRIAL VARIANT...\n       %s --probe "
                         "SCENARIO.json\n",
                         argv[0], argv[0]);
            return 2;
        }
        const Scenario scenario = LoadScenario(argv[1]);
        const double seconds = std::atof(argv[2]);
        const std::uint32_t workers =
                static_cast<std::uint32_t>(std::atoi(argv[3]));
        const int trials = std::atoi(argv[4]);
        const int firstTrial = std::atoi(argv[5]);
        std::vector<Variant> variants;
        for (int index = 6; index < argc; ++index) {
            variants.push_back(ParseVariant(argv[index]));
        }
        std::optional<ConditionProgram> condition;
        if (!scenario.condition.empty()) {
            ConditionCompileResult compiled =
                    CompileConditionScript(scenario.condition);
            if (compiled.error) throw std::runtime_error(*compiled.error);
            condition = std::move(compiled.program);
        }
        std::printf("scenario,variant,trial,attempts,seconds,"
                    "attempts_per_s,improvements,changes,baseline,final");
        for (int index = 1; index <= kCheckpoints; ++index) {
            std::printf(",at%d", index * 10);
        }
        std::printf(",description\n");
        std::fflush(stdout);
        double baselineScore = 0.0;
        for (int trial = firstTrial; trial < firstTrial + trials; ++trial) {
            SearchRequest request(scenario.packs, scenario.replay);
            request.backend = workers > 1u ? PhysicsBackend::MultiThreadedCpu
                                           : PhysicsBackend::OptimizedCpu;
            request.parallelSampleCount = workers > 1u ? workers : 1u;
            request.baseInputCommands = scenario.baseCommands;
            request.simulationHorizonMs = scenario.horizonMs;
            request.condition = condition;
            request.evaluationTarget = scenario.evaluation;
            request.modifiers = scenario.modifiers;
            for (std::size_t pass = 0u; pass < request.modifiers.size();
                 ++pass) {
                request.modifiers[pass].settings["seed"] =
                        std::to_string(TrialSeed(trial, pass));
            }
            if (trial == firstTrial) {
                baselineScore = BaselineScore(request);
            }
            for (const Variant &variant : variants) {
                const SearchAlgorithmRegistration *registration =
                        FindSearchAlgorithm(variant.algorithmId);
                if (registration == nullptr) {
                    throw std::runtime_error("unknown algorithm: " +
                                             variant.algorithmId);
                }
                OptionSettings settings = registration->defaultSettings;
                settings["autoPromoteBest"] =
                        scenario.autoPromoteBest ? "true" : "false";
                SearchRequest variantRequest = request;
                std::optional<std::uint64_t> attemptLimit;
                for (const auto &[key, value] : variant.overrides) {
                    if (key == "@limit") {
                        attemptLimit = std::stoull(value);
                        continue;
                    }
                    // "m<pass>.<key>" overrides a modifier setting.
                    if (key.size() > 2u && key[0] == 'm' &&
                        key.find('.') != std::string::npos) {
                        const std::size_t dot = key.find('.');
                        const std::size_t pass = static_cast<std::size_t>(
                                std::stoul(key.substr(1u, dot - 1u)));
                        variantRequest.modifiers.at(pass)
                                .settings[key.substr(dot + 1u)] = value;
                        continue;
                    }
                    settings[key] = value;
                }
                if (const auto error = registration->validateSettings(
                            settings, kSearchTickDurationMs)) {
                    throw std::runtime_error(variant.label + ": " + *error);
                }
                variantRequest.searchAlgorithm = {variant.algorithmId,
                                                  settings};
                const Outcome outcome = Run(variantRequest, seconds,
                                            baselineScore, attemptLimit);
                std::printf("%s,%s,%d,%llu,%.3f,%.1f,%llu,%.2f,%.9g,%.9g",
                            scenario.name.c_str(), variant.label.c_str(),
                            trial,
                            static_cast<unsigned long long>(outcome.attempts),
                            outcome.seconds,
                            outcome.attempts / outcome.seconds,
                            static_cast<unsigned long long>(
                                    outcome.improvements),
                            outcome.changes, outcome.baselineScore,
                            outcome.finalScore);
                for (const double checkpoint : outcome.checkpoints) {
                    std::printf(",%.9g", checkpoint);
                }
                std::printf(",%s\n", Csv(outcome.description).c_str());
                std::fflush(stdout);
            }
        }
    } catch (const std::exception &error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
    return 0;
}
