// ForeverTAS --check-compute-module ARCHIVE [PACKS REPLAY]
//
// A GPU module (.tar.zst) as the app gets it: unpacked the app's way, loaded
// through ComputeModules (its interface key must be this app's), its status
// read. With a map and its replay, and a GPU this module runs on, two short
// searches on it must match the same searches on Optimized CPU exactly: a
// light one, and one as users run them (a large batch, inputs inserted and
// deleted along the run: AMD users' sessions failed to allocate there). A
// Challenge (no replay inputs) is searched from no inputs.
// Exit 0: good; 1: broken; 3: good, but no GPU here to search on.
#include "app/compute_check.h"
#include "app/compute_archive.h"
#include "app/compute_modules.h"
#include "compute/compute_local.h"
#include "mutations/input_event_formatter.h"
#include "mutations/replay_input_script.h"
#include "searches/algorithm_registry.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cstdio>
#include <iostream>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

using namespace forevertas;

// The replay's inputs, or none for a Challenge.
std::vector<ParsedInputCommand> BaseInputs(const char *packs, const char *replay) {
    std::string script;
    try {
        script = ExtractReplayInputScript(packs, replay);
    } catch (const std::exception &) {
        if (!QString::fromLocal8Bit(replay).endsWith(QStringLiteral(".Challenge.Gbx"), Qt::CaseInsensitive))
            throw;
    }
    const InputScriptParseResult parsed = ParseInputScript(script);
    if (!parsed) throw std::runtime_error(*parsed.error);
    return parsed.commands;
}

SearchResult Search(PhysicsBackend backend, const char *packs, const char *replay, bool heavy) {
    SearchRequest request{packs, replay};
    request.baseInputCommands = BaseInputs(packs, replay);
    request.backend = backend;
    request.parallelSampleCount = heavy ? 4096u : 16u;
    request.simulationHorizonMs = 2000u;
    if (heavy) {
        for (const char *id : {kInputInsertionModifierId, kInputDeletionModifierId}) {
            const ModifierRegistration *modifier = FindModifier(id);
            request.modifiers.push_back({modifier->id, modifier->defaultSettings});
            request.modifiers.back().settings["minTimeMs"] = "0";
            request.modifiers.back().settings["maxTimeMs"] = "1990";
        }
    } else {
        const ModifierRegistration *modifier = FindModifier(kRandomSteeringModifierId);
        request.modifiers = {{modifier->id, modifier->defaultSettings}};
        request.modifiers[0].settings["minTimeMs"] = "1000";
        request.modifiers[0].settings["maxTimeMs"] = "1000";
    }
    const EvaluationTargetRegistration *target = FindEvaluationTarget(kVelocityEvaluationId);
    request.evaluationTarget = {target->id, target->defaultSettings};
    request.evaluationTarget.settings["minTimeMs"] = "1020";
    request.evaluationTarget.settings["maxTimeMs"] = "1020";
    SearchRunControl control;
    control.iterationLimit = 32u;
    return app::ComputeModules::Instance().RunSearch(request, &control);
}

}  // namespace

namespace forevertas::app {

int CheckComputeModule(int argc, char **argv) {
#if defined(_WIN32)
    // a GUI program: its messages go to the console it was started from
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        std::FILE *stream = nullptr;
        freopen_s(&stream, "CONOUT$", "w", stdout);
        freopen_s(&stream, "CONOUT$", "w", stderr);
    }
#endif
    if (argc != 3 && argc != 5) {
        std::cerr << "usage: ForeverTAS --check-compute-module ARCHIVE [PACKS REPLAY]\n";
        return 2;
    }
    ++argv;
    --argc;
    QTemporaryDir root;
    const QString folder = root.filePath(QStringLiteral("module"));
    QString error;
    if (!ExtractComputeArchive(QString::fromLocal8Bit(argv[1]), folder, &error)) {
        std::cerr << "unpacking failed: " << error.toStdString() << '\n';
        return 1;
    }
    QFile manifestFile(folder + QStringLiteral("/module.json"));
    if (!manifestFile.open(QIODevice::ReadOnly)) {
        std::cerr << "no module.json\n";
        return 1;
    }
    const QJsonObject manifest = QJsonDocument::fromJson(manifestFile.readAll()).object();
    const PhysicsBackend backend = manifest.value(QStringLiteral("backend")).toString() == QStringLiteral("hip")
            ? PhysicsBackend::Hip : PhysicsBackend::Cuda;
    if (compute::LocalHas(backend)) {
        std::cerr << "this checker has the backend built in; build it without CUDA/HIP\n";
        return 2;
    }
    qputenv("FOREVERTAS_COMPUTE_DIR", root.path().toLocal8Bit());
    ComputeModules &modules = ComputeModules::Instance();
    if (!modules.Has(backend)) {
        std::cerr << "the module did not load (see the log: interface key or library)\n";
        return 1;
    }
    const compute::GpuStatus status = modules.Status(backend);
    std::cout << manifest.value(QStringLiteral("id")).toString().toStdString() << ": loaded; "
              << (status.ready ? "GPU ready: " + status.deviceName : "no GPU for it: " + status.diagnostic)
              << (status.approximate ? " (approximate physics too)" : "") << '\n';
    if (argc == 2) return 0;
    // a search only with the module for this GPU: CUDA runs a generation's
    // code on its later GPUs (8.6 on 8.9), PTX on any later one
    const QString id = manifest.value(QStringLiteral("id")).toString();
    const int sm = id.startsWith(QStringLiteral("cuda-sm")) ? id.mid(7).toInt() : 0;
    const int device = status.computeMajor * 10 + status.computeMinor;
    const bool forThisGpu = status.ready &&
            (backend == PhysicsBackend::Hip ||
             (sm <= device && (sm / 10 == device / 10 || manifest.value(QStringLiteral("ptx")).toBool())));
    if (!forThisGpu) return 3;

    bool allSame = true;
    for (const bool heavy : {false, true}) {
        const char *const name = heavy ? "search (batch 4096, inputs inserted and deleted)" : "search";
        try {
            const SearchResult gpu = Search(backend, argv[2], argv[3], heavy);
            const SearchResult cpu = Search(PhysicsBackend::OptimizedCpu, argv[2], argv[3], heavy);
            const bool same = gpu.winnerSource == cpu.winnerSource &&
                    gpu.winningIterationIndex == cpu.winningIterationIndex &&
                    gpu.bestScore == cpu.bestScore && gpu.iterations == cpu.iterations &&
                    FormatInputScript(gpu.bestInputs) == FormatInputScript(cpu.bestInputs);
            std::cout << name << ": " << gpu.iterations << " attempts, score " << gpu.bestScore
                      << (same ? ", the same as Optimized CPU\n" : ", NOT the same as Optimized CPU\n");
            allSame &= same;
        } catch (const std::exception &failure) {
            std::cerr << name << " failed: " << failure.what() << '\n';
            allSame = false;
        }
    }
    return allSame ? 0 : 1;
}

}  // namespace forevertas::app
