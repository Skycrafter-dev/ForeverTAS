#ifndef FOREVERTAS_APP_REMOTE_REMOTE_CODEC_H
#define FOREVERTAS_APP_REMOTE_REMOTE_CODEC_H

// Remote searching's messages (docs/REMOTE.md): a search request, its live
// updates and its result as JSON. A result carries no car state or physics
// snapshot: the client simulates the winner's inputs itself.

#include "searches/search_runner.h"

#include <QJsonArray>
#include <QJsonObject>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace forevertas::app::remote {

QJsonArray EncodeEvents(const std::vector<SandboxInputEvent> &events);
std::vector<SandboxInputEvent> DecodeEvents(const QJsonArray &array);

// The request without its files: a worker runs it on its own Packs and the
// replay it is sent.
QJsonObject EncodeRequest(const SearchRequest &request);
SearchRequest DecodeRequest(const QJsonObject &object, const std::string &packDirectory,
                            const std::string &replayPath);

// A device's share of a search: its iteration limit and ids, what it samples.
QJsonObject EncodeShare(const SearchRunControl &control);
void DecodeShare(const QJsonObject &object, SearchRunControl *control);

QJsonObject EncodeLive(const SearchLiveUpdate &live);
SearchLiveUpdate DecodeLive(const QJsonObject &object);

// A device's final result as a worker reports it: its best's inputs and score
// and its counts (the client simulates the best to have the rest).
struct RemoteOutcome {
    SearchWinnerSource winnerSource = SearchWinnerSource::Baseline;
    std::optional<std::uint64_t> winningIterationIndex;
    std::size_t winningMutationCount = 0u;
    double bestScore = 0.0;
    std::vector<SandboxInputEvent> bestInputs;
    std::uint64_t iterations = 0u;
    std::uint64_t evaluatorCalls = 0u;
    std::uint64_t mutationImprovementCount = 0u;
    std::uint64_t totalMutationCount = 0u;
    std::chrono::steady_clock::duration elapsed{};
    std::optional<std::chrono::steady_clock::duration> lastImprovementElapsed;
    std::optional<ApproximateResult> approximate;
};
QJsonObject EncodeResult(const SearchResult &result);
RemoteOutcome DecodeResult(const QJsonObject &object);

}  // namespace forevertas::app::remote

#endif
