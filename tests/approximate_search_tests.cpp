// Approximate physics: a CUDA search on the twin's GPU engine whose new bests are
// rebuilt on exact physics. The reported result must be exact: its input script,
// pasted back as base inputs, evaluates on optimized CPU to the same score.
//
//   forevertas-approximate-search PACKS REPLAY BRANCH_MS WINDOW_MS [ATTEMPTS [exact|cpu]]
//
// Steer insertions in [BRANCH_MS, BRANCH_MS + WINDOW_MS], best speed over the
// half second after the window. "exact": the same search on exact physics (FV
// CUDA), for the timings only. "cpu": the twin on the multi-threaded CPU
// backend (FTAS_CPU_WORKERS workers, default 4: three run the twin, one
// rebuilds).
//
// FTAS_TARGET picks the target (default velocity): checkpoint (FTAS_CP: the
// lap's checkpoint ordinal, default 1), distance, distance-walls, time
// (earliest tick with FTAS_CONDITION), scripted (FTAS_SCRIPT, default
// "max car.speed"), condition (velocity under FTAS_CONDITION), entry-speed and
// custom-volume (a box / prism around FTAS_POINT "x,y,z", 8 m), each judged
// after the window until FTAS_HORIZON_MS (default the window's end + 1 s).
#include "conditions/condition_program.h"
#include "mutations/input_event_formatter.h"
#include "mutations/replay_input_script.h"
#include "physics_backend.h"

#include <cstdlib>
#include "searches/algorithm_registry.h"
#include "searches/result_input_script.h"
#include "searches/search_runner.h"

#include <chrono>
#include <cstdio>
#include <optional>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>

namespace {

forevertas::OptionConfiguration Defaults(const std::string &id, bool evaluator) {
    if (evaluator) {
        const auto *registration = forevertas::FindEvaluationTarget(id);
        if (registration == nullptr) throw std::runtime_error("missing target " + id);
        return {registration->id, registration->defaultSettings};
    }
    const auto *registration = forevertas::FindModifier(id);
    if (registration == nullptr) throw std::runtime_error("missing modifier " + id);
    return {registration->id, registration->defaultSettings};
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 5) {
        std::cerr << "usage: forevertas-approximate-search PACKS REPLAY BRANCH_MS WINDOW_MS [ATTEMPTS]\n";
        return 2;
    }
    try {
        const char *packs = argv[1];
        const char *replay = argv[2];
        const std::int64_t branchMs = std::stoll(argv[3]);
        const std::int64_t windowMs = std::stoll(argv[4]);
        const std::uint64_t attempts = argc > 5 ? std::stoull(argv[5]) : 200000u;
        const bool exactOnly = argc > 6 && std::string(argv[6]) == "exact";
        bool onCpu = argc > 6 && std::string(argv[6]) == "cpu";
        forevertas::InputScriptParseResult base =
                forevertas::ParseInputScript(forevertas::ExtractReplayInputScript(packs, replay));
        if (!base) throw std::runtime_error(*base.error);

        forevertas::OptionConfiguration insertion = Defaults(forevertas::kInputInsertionModifierId, false);
        insertion.settings["minTimeMs"] = std::to_string(branchMs);
        insertion.settings["maxTimeMs"] = std::to_string(branchMs + windowMs);
        const auto env = [](const char *name, const char *otherwise) {
            const char *value = std::getenv(name);
            return std::string(value != nullptr && *value != '\0' ? value : otherwise);
        };
        const std::string targetName = env("FTAS_TARGET", "velocity");
        const std::int64_t horizonMs = std::stoll(env("FTAS_HORIZON_MS", std::to_string(branchMs + windowMs + 1000).c_str()));
        const std::string from = std::to_string(branchMs + windowMs);
        const std::string until = std::to_string(horizonMs);
        forevertas::OptionConfiguration speed = Defaults(forevertas::kVelocityEvaluationId, true);
        speed.settings["minTimeMs"] = from;
        speed.settings["maxTimeMs"] = std::to_string(branchMs + windowMs + 500);
        std::optional<forevertas::ConditionProgram> condition;
        const auto compile = [&]() {
            forevertas::ConditionCompileResult compiled =
                    forevertas::CompileConditionScript(env("FTAS_CONDITION", "car.speed > 20"));
            if (!compiled.program) throw std::runtime_error("condition: " + compiled.error.value_or("?"));
            condition = compiled.program;
        };
        double point[3] = {0.0, 0.0, 0.0};
        std::sscanf(env("FTAS_POINT", "0,0,0").c_str(), "%lf,%lf,%lf", &point[0], &point[1], &point[2]);
        if (targetName == "checkpoint") {
            speed = Defaults(forevertas::kCheckpointTimeEvaluationId, true);
            speed.settings["checkpointIndex"] = env("FTAS_CP", "1");
        } else if (targetName == "distance" || targetName == "distance-walls") {
            speed = Defaults(forevertas::kDistanceTargetEvaluationId, true);
            speed.settings["minTimeMs"] = from;
            speed.settings["maxTimeMs"] = until;
            speed.settings["checkpointIndex"] = env("FTAS_CP", "1");
            if (targetName == "distance-walls") speed.settings["route"] = "around";
        } else if (targetName == "time") {
            speed = Defaults(forevertas::kTimeEvaluationId, true);
            speed.settings["minTimeMs"] = from;
            speed.settings["maxTimeMs"] = until;
            compile();
        } else if (targetName == "scripted") {
            speed = Defaults(forevertas::kScriptedTargetEvaluationId, true);
            speed.settings["minTimeMs"] = from;
            speed.settings["maxTimeMs"] = until;
            speed.settings["script"] = env("FTAS_SCRIPT", "max car.speed");
        } else if (targetName == "condition") {
            compile();
        } else if (targetName == "entry-speed") {
            speed = Defaults(forevertas::kVolumeEntryEvaluationId, true);
            speed.settings["centerX"] = std::to_string(point[0]);
            speed.settings["centerY"] = std::to_string(point[1]);
            speed.settings["centerZ"] = std::to_string(point[2]);
            speed.settings["sizeX"] = speed.settings["sizeY"] = speed.settings["sizeZ"] = "8";
            speed.settings["measure"] = "speed";
        } else if (targetName == "custom-volume") {
            speed = Defaults(forevertas::kCustomVolumeEntryEvaluationId, true);
            speed.settings["originX"] = std::to_string(point[0]);
            speed.settings["originY"] = std::to_string(point[1] - 4.0);
            speed.settings["originZ"] = std::to_string(point[2]);
            speed.settings["depth"] = "8";
            speed.settings["polygon"] = "-4,-4;4,-4;0,4";
        } else if (targetName != "velocity") {
            throw std::runtime_error("unknown FTAS_TARGET " + targetName);
        }

        forevertas::SearchRequest request{packs, replay};
        request.baseInputCommands = base.commands;
#if FOREVERVALIDATOR_HAS_CUDA
        request.backend = forevertas::PhysicsBackend::Cuda;
#elif FOREVERVALIDATOR_HAS_HIP
        request.backend = forevertas::PhysicsBackend::Hip;
#else
        onCpu = true;   // a build without a GPU: the twin on CPU threads
#endif
        request.approximatePhysics = !exactOnly;
        // exact: batches of 4096 (calibration starts from one candidate a batch, too noisy for short searches)
        request.parallelSampleCount = static_cast<std::uint32_t>(std::stoul(env("FTAS_BATCH", "4096")));
        request.calibrateCudaParallelSampleCount = !exactOnly || env("FTAS_CALIBRATE", "0") == "1";
        if (onCpu) {
            const char *workers = std::getenv("FTAS_CPU_WORKERS");
            request.backend = forevertas::PhysicsBackend::MultiThreadedCpu;
            request.parallelSampleCount = workers != nullptr ? static_cast<std::uint32_t>(std::stoul(workers)) : 4u;
            request.calibrateCudaParallelSampleCount = false;
        }
        request.simulationHorizonMs = static_cast<std::uint32_t>(horizonMs);
        request.condition = condition;
        request.modifiers = {insertion};
        request.evaluationTarget = speed;
        forevertas::SearchRunControl control;
        control.iterationLimit = attempts;
        std::uint64_t reports = 0u;
        const auto started = std::chrono::steady_clock::now();
        const auto at = [started]() {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        };
        control.liveChanged = [&reports, &at](const forevertas::SearchLiveUpdate &live) {
            if (live.winnerSource != forevertas::SearchWinnerSource::Mutation) return;
            ++reports;
            std::cerr << "  " << at() << " s: best " << live.bestScore << " after " << live.iterations
                      << " attempts\n";
        };
        int stage = -1;
        control.progressChanged = [&stage, &at](const forevertas::SearchProgress &progress) {
            if (static_cast<int>(progress.stage) == stage) return;
            stage = static_cast<int>(progress.stage);
            std::cerr << "  " << at() << " s: stage " << stage << " (" << progress.completedWork << " attempts)\n";
        };
        const forevertas::SearchResult result = forevertas::RunSearch(request, &control);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

        if (exactOnly) {
            std::cout << "exact search: " << result.iterations << " attempts in " << seconds << " s; result "
                      << result.bestEvaluationDescription << '\n';
            return 0;
        }
        bool ok = true;
        if (!result.approximate) {
            std::cerr << "FAIL: the result has no approximate score\n";
            ok = false;
        } else if (result.winnerSource == forevertas::SearchWinnerSource::Mutation &&
                   !result.approximate->driftMeters) {
            std::cerr << "FAIL: a rebuilt best has no drift\n";
            ok = false;
        }
        // the result's inputs as the user copies them, evaluated on exact physics
        const std::string script = forevertas::FormatResultInputScript(
                result.bestInputs, request.evaluationTarget.id, result.bestState);
        forevertas::InputScriptParseResult pasted = forevertas::ParseInputScript(script);
        if (!pasted) throw std::runtime_error("result script: " + *pasted.error);
        forevertas::SearchRequest exact = request;
        exact.approximatePhysics = false;
        exact.baseInputCommands = pasted.commands;
        const std::optional<forevertas::SearchResult> replayed = forevertas::EvaluateBaseline(exact);
        if (!replayed) {
            std::cerr << "FAIL: the result's inputs do not reach the target on exact physics\n";
            ok = false;
        } else if (replayed->bestScore != result.bestScore ||
                   replayed->bestEvaluationTimeMs != result.bestEvaluationTimeMs) {
            std::cerr.precision(17);
            std::cerr << "FAIL: exact physics scores the result's inputs " << replayed->bestScore << " at "
                      << replayed->bestEvaluationTimeMs << " ms, the result says " << result.bestScore << " at "
                      << result.bestEvaluationTimeMs << " ms\n";
            ok = false;
        }
        std::cout.precision(9);
        std::cout << "approximate search: " << result.iterations << " attempts in " << seconds << " s, "
                  << result.mutationImprovementCount << " improvements (" << reports
                  << " reported); result " << result.bestEvaluationDescription;
        if (result.approximate) {
            std::cout << "; approximate " << result.approximate->description;
            if (result.approximate->driftMeters)
                std::cout << ", drift " << *result.approximate->driftMeters * 1000.0 << " mm";
        }
        std::cout << "; exact replay of the pasted script: "
                  << (replayed ? std::to_string(replayed->bestScore) : std::string("none")) << '\n'
                  << (ok ? "PASS" : "FAIL") << '\n';
        return ok ? 0 : 1;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
