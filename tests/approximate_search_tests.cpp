// Approximate physics: a CUDA search on the twin's GPU engine whose new bests are
// rebuilt on exact physics. The reported result must be exact: its input script,
// pasted back as base inputs, evaluates on optimized CPU to the same score.
//
//   forevertas-approximate-search PACKS REPLAY BRANCH_MS WINDOW_MS [ATTEMPTS [exact]]
//
// Steer insertions in [BRANCH_MS, BRANCH_MS + WINDOW_MS], best speed over the
// half second after the window. "exact": the same search on exact physics (FV
// CUDA), for the timings only.
#include "mutations/input_event_formatter.h"
#include "mutations/replay_input_script.h"
#include "physics_backend.h"
#include "searches/algorithm_registry.h"
#include "searches/result_input_script.h"
#include "searches/search_runner.h"

#include <chrono>
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
        forevertas::InputScriptParseResult base =
                forevertas::ParseInputScript(forevertas::ExtractReplayInputScript(packs, replay));
        if (!base) throw std::runtime_error(*base.error);

        forevertas::OptionConfiguration insertion = Defaults(forevertas::kInputInsertionModifierId, false);
        insertion.settings["minTimeMs"] = std::to_string(branchMs);
        insertion.settings["maxTimeMs"] = std::to_string(branchMs + windowMs);
        forevertas::OptionConfiguration speed = Defaults(forevertas::kVelocityEvaluationId, true);
        speed.settings["minTimeMs"] = std::to_string(branchMs + windowMs);
        speed.settings["maxTimeMs"] = std::to_string(branchMs + windowMs + 500);

        forevertas::SearchRequest request{packs, replay};
        request.baseInputCommands = base.commands;
        request.backend = forevertas::PhysicsBackend::Cuda;
        request.approximatePhysics = !exactOnly;
        request.calibrateCudaParallelSampleCount = true;
        request.simulationHorizonMs = static_cast<std::uint32_t>(branchMs + windowMs + 1000);
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
