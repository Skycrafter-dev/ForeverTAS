#include "app/search_worker.h"

#include "app/compact_number_format.h"
#include "app/rolling_throughput.h"
#include "mutations/input_event_formatter.h"
#include "time_format.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <optional>
#include <utility>

namespace forevertas::app {
namespace {

QString IterationLabel(
        SearchWinnerSource source,
        const std::optional<std::uint64_t> &iterationIndex) {
    if (source == SearchWinnerSource::Baseline) {
        return QStringLiteral("Baseline");
    }
    return iterationIndex
            ? QStringLiteral("Iteration #%1")
                      .arg(FormatCompactNumber(
                              static_cast<double>(*iterationIndex + 1u)))
            : QStringLiteral("Mutation");
}

QString IterationsPerSecond(double rate) {
    return FormatCompactNumber(rate);
}

QString RoundedDuration(
        std::chrono::steady_clock::duration duration) {
    return QString::fromStdString(FormatHumanDuration(
            std::chrono::round<std::chrono::seconds>(duration)));
}

QString LastImprovementText(const SearchLiveUpdate &live) {
    if (!live.lastImprovementElapsed) {
        return QStringLiteral("none");
    }
    const auto age = live.elapsed > *live.lastImprovementElapsed
            ? live.elapsed - *live.lastImprovementElapsed
            : std::chrono::steady_clock::duration::zero();
    return RoundedDuration(age) + QStringLiteral(" ago");
}

QString FormatLive(const SearchLiveUpdate &live, const QString &heading) {
    if (!live.bestAvailable) {
        return QStringLiteral(
                       "%1: none yet\n"
                       "No candidate has satisfied the selected target.\n"
                       "Improvements: %2\n"
                       "Last improvement: %3")
                .arg(heading)
                .arg(FormatCompactNumber(static_cast<double>(
                        live.mutationImprovementCount)))
                .arg(LastImprovementText(live));
    }
    return QStringLiteral(
                   "%1: %2\n"
                   "%3\n"
                   "Improvements: %4\n"
                   "Last improvement: %5")
            .arg(heading)
            .arg(IterationLabel(live.winnerSource,
                                live.winningIterationIndex))
            .arg(QString::fromStdString(live.bestEvaluationDescription))
            .arg(FormatCompactNumber(static_cast<double>(
                    live.mutationImprovementCount)))
            .arg(LastImprovementText(live));
}

QString FormatTargetProgressText(const SearchLiveUpdate &live) {
    const QString qualifying = FormatCompactNumber(
            static_cast<double>(live.qualifyingCandidateCount));
    const QString candidateLabel =
            live.qualifyingCandidateCount == 1u
            ? QStringLiteral("candidate")
            : QStringLiteral("candidates");
    if (live.closestTargetDistance) {
        const double distance = *live.closestTargetDistance;
        const QString distanceText = std::isfinite(distance)
                ? QString::number(
                          std::max(0.0, distance), 'f',
                          distance < 100.0 ? 2 : 1)
                : QStringLiteral("?");
        if (live.qualifyingCandidateCount != 0u) {
            return QStringLiteral(
                           "Target triggered: %1 qualifying %2 \u2022 nearest "
                           "sampled car center: %3 m")
                    .arg(qualifying, candidateLabel, distanceText);
        }
        if (std::isfinite(distance) && distance <= 0.0) {
            return QStringLiteral(
                    "Cuboid reached geometrically (0.00 m away), but no "
                    "qualifying entry trigger was recorded yet");
        }
        return QStringLiteral(
                       "No qualifying entry trigger yet \u2022 nearest "
                       "sampled car center: %1 m away")
                .arg(distanceText);
    }
    return live.qualifyingCandidateCount == 0u
            ? QStringLiteral("Waiting for the first target sample...")
            : QStringLiteral("Target triggered: %1 qualifying %2")
                      .arg(qualifying, candidateLabel);
}

SearchLiveUpdate ToLiveUpdate(const SearchResult &result) {
    SearchLiveUpdate live;
    live.winnerSource = result.winnerSource;
    live.winningIterationIndex = result.winningIterationIndex;
    live.winningMutationCount = result.winningMutationCount;
    live.bestScore = result.bestScore;
    live.bestEvaluationTimeMs = result.bestEvaluationTimeMs;
    live.bestEvaluationDescription = result.bestEvaluationDescription;
    live.bestState = result.bestState;
    live.bestInputs = result.bestInputs;
    live.iterations = result.iterations;
    live.evaluatorCalls = result.evaluatorCalls;
    live.mutationImprovementCount = result.mutationImprovementCount;
    live.totalMutationCount = result.totalMutationCount;
    live.elapsed = result.elapsed;
    live.lastImprovementElapsed = result.lastImprovementElapsed;
    live.bestAvailable = true;
    live.qualifyingCandidateCount = result.qualifyingCandidateCount;
    live.closestTargetDistance = result.closestTargetDistance;
    return live;
}

QString FormatResult(const SearchResult &result) {
    return FormatLive(ToLiveUpdate(result), QStringLiteral("Best"));
}

QString FilePathFromUtf8(const std::string &path) {
    return QString::fromUtf8(
            path.data(), static_cast<qsizetype>(path.size()));
}

}  // namespace

QString SearchTargetProgressText(const SearchLiveUpdate &live) {
    return FormatTargetProgressText(live);
}

QString SearchStageStatus(SearchProgressStage stage,
                          std::string_view backendId,
                          bool useCudaSessionSpecialization) {
    const bool cuda = backendId == "cuda";
    const bool multiThreadedCpu =
            backendId == "multi-threaded-cpu";
    switch (stage) {
    case SearchProgressStage::OpeningPacksDirectory:
        return QStringLiteral("Opening Packs directory...");
    case SearchProgressStage::ReadingScenario:
        return QStringLiteral("Reading scenario file...");
    case SearchProgressStage::CreatingSimulation:
        if (cuda) {
            return QStringLiteral(
                    "Initializing CUDA simulation...");
        }
        if (backendId == "optimized-cpu") {
            return QStringLiteral("Initializing optimized CPU simulation...");
        }
        return QStringLiteral("Initializing reference simulation...");
    case SearchProgressStage::LoadingScenario:
        if (cuda) {
            return useCudaSessionSpecialization
                    ? QStringLiteral(
                              "Loading the map and building the fast CUDA "
                              "kernel...")
                    : QStringLiteral("Loading the map onto CUDA...");
        }
        return QStringLiteral("Loading the map into the simulation...");
    case SearchProgressStage::RestoringSimulation:
        if (cuda) {
            return QStringLiteral(
                    "Restoring the prepared CUDA simulation...");
        }
        return QStringLiteral("Restoring the prepared simulation...");
    case SearchProgressStage::ApplyingBaselineInputs:
        if (cuda) {
            return QStringLiteral(
                    "Applying baseline inputs to CUDA...");
        }
        return QStringLiteral("Applying the baseline input sequence...");
    case SearchProgressStage::PreparingSearch:
        if (cuda) {
            return useCudaSessionSpecialization
                    ? QStringLiteral("Preparing fast CUDA search...")
                    : QStringLiteral("Preparing regular CUDA search...");
        }
        if (multiThreadedCpu) {
            return QStringLiteral(
                    "Starting independent optimized CPU workers...");
        }
        return QStringLiteral("Preparing search components...");
    case SearchProgressStage::Baseline:
        return cuda
                ? QStringLiteral("Evaluating CUDA baseline...")
                : QStringLiteral("Evaluating baseline...");
    case SearchProgressStage::Calibration:
        return QStringLiteral("Calibrating CUDA throughput...");
    case SearchProgressStage::Mutations:
        if (cuda) {
            return QStringLiteral("Searching on CUDA...");
        }
        return multiThreadedCpu
                ? QStringLiteral(
                          "Searching across optimized CPU workers...")
                : QStringLiteral("Searching...");
    case SearchProgressStage::FinalSamplingSetup:
        return cuda
                ? QStringLiteral("Preparing final best-run sampling...")
                : QStringLiteral("Preparing final best-run sampling...");
    case SearchProgressStage::FinalSampling:
        return QStringLiteral("Sampling best run...");
    }
    return QStringLiteral("Preparing search...");
}

bool TryBeginSearchIteration(
        const std::shared_ptr<std::atomic<SearchIterationPhase>> &phase) {
    SearchIterationPhase expected = SearchIterationPhase::Pending;
    if (phase->compare_exchange_strong(
                expected,
                SearchIterationPhase::Started,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
        return true;
    }
    return expected == SearchIterationPhase::Started;
}

bool TryCancelBeforeSearchIteration(
        const std::shared_ptr<std::atomic<SearchIterationPhase>> &phase) {
    SearchIterationPhase expected = SearchIterationPhase::Pending;
    return phase->compare_exchange_strong(
            expected,
            SearchIterationPhase::Cancelled,
            std::memory_order_acq_rel,
            std::memory_order_acquire);
}

SearchWorker::SearchWorker(
        SearchRequest request,
        std::uint64_t searchId,
        std::shared_ptr<std::atomic_bool> stopRequested,
        std::shared_ptr<std::atomic_bool> cancellationRequested,
        std::shared_ptr<std::atomic<SearchIterationPhase>> iterationPhase)
    : request_(std::move(request)),
      searchId_(searchId),
      stopRequested_(std::move(stopRequested)),
      cancellationRequested_(std::move(cancellationRequested)),
      iterationPhase_(std::move(iterationPhase)) {}

void SearchWorker::run() {
    emit stageChanged(QStringLiteral("Preparing search..."), true);

    SearchRunControl control;
    struct LiveMetricWindow final {
        RollingThroughput throughput;
        std::uint64_t iterations = 0u;
        std::chrono::steady_clock::duration elapsed{};
        std::optional<SearchProgressStage> stage;
        std::optional<std::uint32_t> cudaBatchSize;
        std::optional<bool> cudaCalibrationActive;

        void ResetThroughput() {
            throughput.Reset(iterations, elapsed);
        }
    };
    const auto liveMetricWindow =
            std::make_shared<LiveMetricWindow>();
    control.reuseLoadedSandbox = true;
    control.stopRequested = [flag = stopRequested_]() {
        return flag->load(std::memory_order_relaxed);
    };
    control.cancellationRequested = [flag = cancellationRequested_]() {
        return flag->load(std::memory_order_relaxed);
    };
    control.beginIteration = [phase = iterationPhase_]() {
        return TryBeginSearchIteration(phase);
    };
    control.progressChanged =
            [this, liveMetricWindow](const SearchProgress &progress) {
        if (PhysicsBackendId(request_.backend) == "cuda" &&
            liveMetricWindow->stage != progress.stage) {
            liveMetricWindow->stage = progress.stage;
            liveMetricWindow->ResetThroughput();
            emit throughputReset();
        }
        if (PhysicsBackendId(request_.backend) == "cuda") {
            const bool calibrationActive =
                    progress.stage == SearchProgressStage::Calibration;
            if (liveMetricWindow->cudaCalibrationActive !=
                calibrationActive) {
                liveMetricWindow->cudaCalibrationActive =
                        calibrationActive;
                emit cudaCalibrationActiveChanged(calibrationActive);
            }
        }
        if (PhysicsBackendId(request_.backend) == "cuda" &&
            (progress.stage == SearchProgressStage::FinalSamplingSetup ||
             progress.stage == SearchProgressStage::FinalSampling) &&
            liveMetricWindow->cudaBatchSize.value_or(0u) != 0u) {
            liveMetricWindow->cudaBatchSize = 0u;
            emit cudaActiveBatchSizeChanged(0u);
        }
        if (progress.stage == SearchProgressStage::FinalSampling) {
            const double value = progress.totalWork == 0u
                    ? 1.0
                    : static_cast<double>(progress.completedWork) /
                              static_cast<double>(progress.totalWork);
            const QString status = QStringLiteral(
                    "Sampling best run with reference physics: %1 of %2 ticks");
            emit progressChanged(
                    value,
                    status
                            .arg(static_cast<qulonglong>(
                                    progress.completedWork))
                            .arg(static_cast<qulonglong>(
                                    progress.totalWork)));
            return;
        }
        emit stageChanged(
                SearchStageStatus(
                        progress.stage,
                        PhysicsBackendId(request_.backend),
                        request_.useCudaSessionSpecialization),
                true);
    };
    control.cudaBatchSizeChanged =
            [this, liveMetricWindow](std::uint32_t batchSize) {
        if (liveMetricWindow->cudaBatchSize != batchSize) {
            liveMetricWindow->cudaBatchSize = batchSize;
            liveMetricWindow->ResetThroughput();
        }
        emit cudaActiveBatchSizeChanged(batchSize);
    };
    const auto publishedTrajectoryNumber =
            std::make_shared<std::atomic_uint64_t>(0u);
    const auto publishImprovement =
            [this, publishedTrajectoryNumber](
                    const SearchLiveUpdate &live,
                    std::string_view backendId) {
                if (!live.bestAvailable || live.bestTimeline.empty()) {
                    return;
                }
                auto improvement = std::make_shared<SearchImprovement>();
                improvement->searchId = searchId_;
                improvement->improvementNumber =
                        publishedTrajectoryNumber->fetch_add(
                                1u, std::memory_order_relaxed) +
                        1u;
                improvement->packsDirectory =
                        FilePathFromUtf8(request_.packDirectory);
                improvement->replayPath =
                        FilePathFromUtf8(request_.replayPath);
                improvement->simulationBackendId = QString::fromLatin1(
                        backendId.data(),
                        static_cast<qsizetype>(backendId.size()));
                improvement->timeline = live.bestTimeline;
                emit improvementFound(std::move(improvement));
            };
    control.liveChanged = [this,
                           latestInputsText = QString(),
                           latestSource = SearchWinnerSource::Baseline,
                           latestIteration =
                                   std::optional<std::uint64_t>{},
                           publishImprovement,
                           liveMetricWindow](
                                  const SearchLiveUpdate &live) mutable {
        if (live.bestAvailable &&
            (latestInputsText.isEmpty() ||
             latestSource != live.winnerSource ||
             latestIteration != live.winningIterationIndex)) {
            latestInputsText = QString::fromStdString(
                    FormatInputScript(live.bestInputs));
            latestSource = live.winnerSource;
            latestIteration = live.winningIterationIndex;
        }
        const double throughput = liveMetricWindow->throughput.Observe(
                live.iterations, live.elapsed);
        liveMetricWindow->iterations = live.iterations;
        liveMetricWindow->elapsed = live.elapsed;
        emit metricsChanged(
                FormatCompactNumber(static_cast<double>(live.iterations)),
                IterationsPerSecond(throughput),
                RoundedDuration(live.elapsed),
                FormatCompactNumber(
                        static_cast<double>(live.evaluatorCalls)),
                FormatCompactNumber(
                        static_cast<double>(live.totalMutationCount)),
                FormatCompactNumber(static_cast<double>(
                        live.mutationImprovementCount)),
                PhysicsBackendId(request_.backend) == "cuda" &&
                        request_.evaluationTarget.id ==
                                kVolumeEntryEvaluationId
                        ? SearchTargetProgressText(live)
                        : QString{});
        publishImprovement(live, PhysicsBackendId(request_.backend));
        emit bestChanged(
                FormatLive(live, QStringLiteral("Current best")),
                latestInputsText);
    };
#if FOREVERVALIDATOR_HAS_CUDA
    if (request_.backend == PhysicsBackend::Cuda) {
        control.improvementTimelineSampled =
                [publishImprovement](const SearchLiveUpdate &live) {
                    publishImprovement(
                            live,
                            PhysicsBackendId(
                                    PhysicsBackend::Reference));
                };
    }
#endif

    try {
        SearchResult result = RunSearch(request_, &control);
        auto completion = std::make_shared<SearchCompletion>();
        completion->summary = FormatResult(result);
        completion->inputsText = QString::fromStdString(
                FormatInputScript(result.bestInputs));
        completion->packsDirectory =
                FilePathFromUtf8(request_.packDirectory);
        completion->replayPath = FilePathFromUtf8(request_.replayPath);
        PhysicsBackend resultBackend = request_.backend;
#if FOREVERVALIDATOR_HAS_CUDA
        if (resultBackend == PhysicsBackend::Cuda) {
            resultBackend = PhysicsBackend::Reference;
        }
#endif
        const std::string_view backendId = PhysicsBackendId(resultBackend);
        completion->simulationBackendId = QString::fromLatin1(
                backendId.data(), static_cast<qsizetype>(backendId.size()));
        completion->bestInputs = std::move(result.bestInputs);
        completion->bestTimeline = std::move(result.bestTimeline);
        emit succeeded(std::move(completion));
    } catch (const SearchCancelled &) {
        emit cancelled();
    } catch (const std::exception &error) {
        emit failed(QString::fromUtf8(error.what()));
    } catch (...) {
        emit failed(QStringLiteral("Unexpected search failure"));
    }
    emit finished();
}

}  // namespace forevertas::app
