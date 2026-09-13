#ifndef FOREVERTAS_BENCHMARK_NATIVE_ONLY
#include "app/block_editor_bridge.h"
#include "app/search_controller.h"
#endif
#include "mutations/replay_input_script.h"
#include "searches/search_runner.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>

#include <chrono>
#include <iomanip>
#include <iostream>
#include <stdexcept>

// Same map, script, horizon, backend and candidate limit for both paths. The
// native comparison is an existing-event pass and maximum speed over the final
// tick. Supply a matching visual program rather than benchmarking a no-op.
int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setOrganizationName("ForeverTAS-tests");
    QCoreApplication::setApplicationName("block-program-benchmark");
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    try {
        if (argc != 8) throw std::runtime_error(
            "usage: benchmark packs replay workspace.json|native backend workers candidates horizon-ms");
#ifndef FOREVERTAS_BENCHMARK_NATIVE_ONLY
        forevertas::app::SearchController controller;
        controller.setSimulationBackendId(QString::fromUtf8(argv[4]));
        forevertas::app::BlockEditorBridge bridge(&controller);
#endif
        forevertas::SearchRequest request{argv[1], argv[2]};
        request.parallelSampleCount = static_cast<std::uint32_t>(std::stoul(argv[5]));
        request.useCudaSessionSpecialization = !qEnvironmentVariableIsSet("FOREVERTAS_BENCH_CUDA_REGULAR");
        request.simulationHorizonMs = static_cast<std::uint32_t>(std::stoul(argv[7]));
        bool knownBackend=false;
        for (auto backend : {forevertas::PhysicsBackend::Reference, forevertas::PhysicsBackend::OptimizedCpu,
                            forevertas::PhysicsBackend::MultiThreadedCpu
#if FOREVERVALIDATOR_HAS_CUDA
                            , forevertas::PhysicsBackend::Cuda
#endif
                            })
            if (std::string(argv[4]) == forevertas::PhysicsBackendId(backend)) { request.backend = backend; knownBackend=true; }
        if (!knownBackend) throw std::runtime_error("unknown or unavailable backend");
        const auto script = forevertas::ParseInputScript(forevertas::ExtractReplayInputScript(argv[1], argv[2]));
        if (!script) throw std::runtime_error(*script.error);
        request.baseInputCommands = script.commands;
        const bool blocks=std::string(argv[3])!="native";
        if (blocks) {
#ifdef FOREVERTAS_BENCHMARK_NATIVE_ONLY
            throw std::runtime_error("the baseline executable only benchmarks native search");
#else
            QFile file(QString::fromUtf8(argv[3]));
            if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("cannot open workspace");
            if (!bridge.applyWorkspace(QString::fromUtf8(file.readAll()), bridge.workspaceRevision()+1))
                throw std::runtime_error(bridge.diagnosticsJson().toStdString());
            request.executable = std::make_shared<const forevertas::blocks::VisualProgram>(*controller.executableBlockProgram());
#endif
        } else {
            const auto *modifier = forevertas::FindModifier(forevertas::kExistingEventPerturbationModifierId);
            request.modifiers = {{modifier->id, modifier->defaultSettings}};
            request.modifiers.front().settings["minTimeMs"] = "1000";
            request.modifiers.front().settings["maxTimeMs"] = std::to_string(request.simulationHorizonMs - 10);
            const auto *target = forevertas::FindEvaluationTarget(forevertas::kVelocityEvaluationId);
            request.evaluationTarget = {target->id, target->defaultSettings};
            request.evaluationTarget.settings["minTimeMs"] = qEnvironmentVariableIsSet("FOREVERTAS_BENCH_TARGET_START")
                ? qEnvironmentVariable("FOREVERTAS_BENCH_TARGET_START").toStdString() : argv[7];
            request.evaluationTarget.settings["maxTimeMs"] = argv[7];
            request.searchAlgorithm.settings["autoPromoteBest"] = "false";
        }
        forevertas::SearchRunControl control;
        control.iterationLimit = std::stoull(argv[6]);
        control.sampleBestTimeline = false;
        control.sampleImprovementTimelines = false;
#ifndef FOREVERTAS_BENCHMARK_NATIVE_ONLY
        control.compileVisualPrograms = !qEnvironmentVariableIsSet("FOREVERTAS_BENCH_INTERPRETER");
#endif
        std::optional<forevertas::SearchStatisticsUpdate> firstSample,lastSample;
        control.statisticsChanged = [&](const forevertas::SearchStatisticsUpdate &sample) {
            if (sample.iterations<128) return;
            if (!firstSample) firstSample=sample;
            lastSample=sample;
        };
        const auto started = std::chrono::steady_clock::now();
        auto executeStarted = started;
        bool executionReported = false;
        std::string executionMode;
        QJsonArray executionModes;
        QJsonArray executionModeTimeline;
#ifndef FOREVERTAS_BENCHMARK_NATIVE_ONLY
        control.visualExecutionModeChanged = [&](const std::string &mode) {
            if (mode!=executionMode) {
                executionModes.append(QString::fromStdString(mode));
                executionModeTimeline.append(QJsonObject{{"mode",QString::fromStdString(mode)},
                    {"executionSeconds",std::chrono::duration<double>(std::chrono::steady_clock::now()-executeStarted).count()}});
            }
            executionMode=mode;
        };
#endif
        control.progressChanged = [&](const forevertas::SearchProgress &progress) {
            // Native CPU's Mutations event is emitted by the first aggregated
            // worker update, after candidate work has already begun.
            if (!executionReported && progress.stage == forevertas::SearchProgressStage::ApplyingBaselineInputs) {
                executeStarted = std::chrono::steady_clock::now();
                executionReported = true;
            }
        };
        const auto result = forevertas::RunSearch(request, &control);
        const auto ended = std::chrono::steady_clock::now();
        if (!executionReported) throw std::runtime_error("missing common benchmark timing boundary");
        const double total = std::chrono::duration<double>(ended-started).count();
        const double execution = std::chrono::duration<double>(ended-executeStarted).count();
        const auto measured = firstSample && lastSample ? lastSample->iterations-firstSample->iterations : 0;
        const double steady = firstSample && lastSample ?
            std::chrono::duration<double>(lastSample->elapsed-firstSample->elapsed).count() : 0;
        QByteArray inputs;
        for (const auto &event : result.bestInputs) {
            for (const auto value : {static_cast<qint64>(event.timeMs),static_cast<qint64>(event.action),
                    static_cast<qint64>(event.value.kind),static_cast<qint64>(event.value.analog),
                    static_cast<qint64>(event.value.switchState)}) {
                inputs.append(QByteArray::number(value)); inputs.append(',');
            }
            inputs.append('\n');
        }
        const QJsonObject report{
            {"path",blocks ? "blocks" : "native"}, {"backend",QString::fromUtf8(argv[4])},
            {"workers",static_cast<qint64>(request.parallelSampleCount)},
            {"horizonMs",static_cast<qint64>(request.simulationHorizonMs)},
            {"candidates",static_cast<qint64>(result.iterations)}, {"totalSeconds",total},
            {"evaluatorCalls",static_cast<qint64>(result.evaluatorCalls)},
            {"mutations",static_cast<qint64>(result.totalMutationCount)},
            {"executionSeconds",execution}, {"candidatesPerSecond",result.iterations/execution},
            {"reportedSearchSeconds",std::chrono::duration<double>(result.elapsed).count()},
            {"timingContract","baseline-inputs-to-completion-v1"},
            {"score",result.bestScore}, {"evaluationTimeMs",result.bestEvaluationTimeMs},
            {"inputsSha256",QString::fromLatin1(QCryptographicHash::hash(inputs,QCryptographicHash::Sha256).toHex())},
            {"executionMode",QString::fromStdString(executionMode)},
            {"executionModes",executionModes},
            {"executionModeTimeline",executionModeTimeline},
            {"steadyCandidates",static_cast<qint64>(measured)}, {"steadySeconds",steady},
            {"steadyCandidatesPerSecond",steady>0 ? QJsonValue(measured/steady) : QJsonValue{}}
        };
        std::cout << QJsonDocument(report).toJson(QJsonDocument::Compact).constData() << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
