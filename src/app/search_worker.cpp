#include "app/search_worker.h"

#include "app/compact_number_format.h"
#include "app/rolling_throughput.h"
#include "app/search_session_store.h"
#include "mutations/input_event_formatter.h"
#include "searches/result_input_script.h"
#include "time_format.h"

#include <chrono>
#include <exception>
#include <QRandomGenerator>
#include <random>
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

SearchLiveUpdate ToLiveUpdate(const SearchResult &result) {
    return {
            result.winnerSource,
            result.winningIterationIndex,
            result.winningMutationCount,
            result.bestScore,
            result.bestEvaluationTimeMs,
            result.bestEvaluationDescription,
            result.bestState,
            result.bestInputs,
            result.iterations,
            result.evaluatorCalls,
            result.mutationImprovementCount,
            result.totalMutationCount,
            result.elapsed,
            result.lastImprovementElapsed,
            {},
            result.objectiveScores,
            result.metricValues};
}

QString FormatResult(const SearchResult &result) {
    return FormatLive(ToLiveUpdate(result), QStringLiteral("Best"));
}

SearchLiveUpdate RetainedResult(const SearchLiveUpdate &live) {
    SearchLiveUpdate result;
    result.winnerSource = live.winnerSource;
    result.winningIterationIndex = live.winningIterationIndex;
    result.winningMutationCount = live.winningMutationCount;
    result.bestScore = live.bestScore;
    result.bestEvaluationTimeMs = live.bestEvaluationTimeMs;
    result.bestEvaluationDescription = live.bestEvaluationDescription;
    result.bestState = live.bestState;
    result.bestInputs = live.bestInputs;
    result.iterations = live.iterations;
    result.evaluatorCalls = live.evaluatorCalls;
    result.mutationImprovementCount = live.mutationImprovementCount;
    result.totalMutationCount = live.totalMutationCount;
    result.elapsed = live.elapsed;
    result.lastImprovementElapsed = live.lastImprovementElapsed;
    result.objectiveScores = live.objectiveScores;
    result.metricValues = live.metricValues;
    return result;
}

QString FilePathFromUtf8(const std::string &path) {
    return QString::fromUtf8(
            path.data(), static_cast<qsizetype>(path.size()));
}

}  // namespace

QString SearchStageStatus(SearchProgressStage stage,
                          std::string_view backendId,
                          bool useCudaSessionSpecialization) {
    if (backendId == "vulkan") {
        switch (stage) {
        case SearchProgressStage::CreatingSimulation:
            return QStringLiteral("Initializing Vulkan simulation...");
        case SearchProgressStage::LoadingScenario:
            return QStringLiteral("Loading the map onto Vulkan...");
        case SearchProgressStage::RestoringSimulation:
            return QStringLiteral("Restoring the prepared Vulkan simulation...");
        case SearchProgressStage::ApplyingBaselineInputs:
            return QStringLiteral("Applying baseline inputs to Vulkan...");
        case SearchProgressStage::PreparingSearch:
            return QStringLiteral("Preparing Vulkan search...");
        case SearchProgressStage::Baseline:
            return QStringLiteral("Evaluating Vulkan baseline...");
        case SearchProgressStage::Calibration:
            return QStringLiteral("Calibrating Vulkan throughput...");
        case SearchProgressStage::Mutations:
            return QStringLiteral("Searching on Vulkan...");
        default:
            break;
        }
    }
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
        std::shared_ptr<std::atomic<SearchIterationPhase>> iterationPhase,
        AutoRestartPolicy restartPolicy)
    : request_(std::move(request)),
      searchId_(searchId),
      stopRequested_(std::move(stopRequested)),
      cancellationRequested_(std::move(cancellationRequested)),
      iterationPhase_(std::move(iterationPhase)),
      restartPolicy_(restartPolicy) {}

void SearchWorker::run() {
    emit stageChanged(QStringLiteral("Preparing search..."), true);

    SearchRunControl control;
    control.reuseLoadedSandbox = true;
    control.stopRequested = [flag = stopRequested_]() {
        return flag->load(std::memory_order_relaxed);
    };
    control.cancellationRequested = [flag = cancellationRequested_]() {
        return flag->load(std::memory_order_relaxed);
    };
    control.progressChanged = [this](const SearchProgress &progress) {
        if (progress.stage == SearchProgressStage::FinalSampling) {
            const double value = progress.totalWork == 0u
                    ? 1.0
                    : static_cast<double>(progress.completedWork) /
                              static_cast<double>(progress.totalWork);
            const QString status = QStringLiteral(
                    "Sampling best run on optimized CPU: %1 of %2 ticks");
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
    control.cudaBatchSizeChanged = [this](std::uint32_t batchSize) {
        emit cudaBatchSizeChanged(batchSize);
    };
    control.statisticsChanged =
            [this, throughput = RollingThroughput()](
                    const SearchStatisticsUpdate &statistics) mutable {
                emit iterationCountChanged(statistics.iterations);
                emit metricsChanged(
                        FormatCompactNumber(
                                static_cast<double>(statistics.iterations)),
                        IterationsPerSecond(
                                throughput.Observe(
                                        statistics.iterations,
                                        statistics.elapsed)),
                        RoundedDuration(statistics.elapsed));
            };
    const auto publishedTrajectoryNumber =
            std::make_shared<std::atomic_uint64_t>(0u);
    const auto publishImprovement =
            [this, publishedTrajectoryNumber](
                    const SearchLiveUpdate &live,
                    std::string_view backendId) {
                if (live.bestTimeline.empty()) {
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
    std::optional<SearchLiveUpdate> retainedResult;
    control.liveChanged = [this, &retainedResult,
                           latestInputsText = QString(),
                           latestSource = SearchWinnerSource::Baseline,
                           latestIteration =
                                   std::optional<std::uint64_t>{},
                           publishImprovement](
                                  const SearchLiveUpdate &live) mutable {
        if (!retainedResult || latestInputsText.isEmpty() ||
            latestSource != live.winnerSource ||
            latestIteration != live.winningIterationIndex) {
            latestInputsText = QString::fromStdString(
                    FormatResultInputScript(live.bestInputs, request_.evaluationTarget.id,
                                            live.bestState));
            latestSource = live.winnerSource;
            latestIteration = live.winningIterationIndex;
            retainedResult = RetainedResult(live);
        } else {
            retainedResult->iterations = live.iterations;
            retainedResult->elapsed = live.elapsed;
        }
        publishImprovement(
                live, PhysicsBackendId(kAuxiliarySimulationBackend));
        emit bestChanged(
                FormatLive(live, QStringLiteral("Current best")),
                latestInputsText);
    };
#if FOREVERVALIDATOR_HAS_CUDA || FOREVERVALIDATOR_HAS_VULKAN || FOREVERVALIDATOR_HAS_HIP
    if (IsGpuBackend(request_.backend)) {
        control.improvementTimelineSampled =
                [publishImprovement](const SearchLiveUpdate &live) {
                    publishImprovement(
                            live,
                            PhysicsBackendId(kAuxiliarySimulationBackend));
                };
    }
#endif

    std::optional<SearchSessionLocation> activeSession;
    std::uint64_t restartNumber = 0;
    bool cyclePersisted = false;
    try {
        if (cancellationRequested_->load(std::memory_order_relaxed)) {
            throw SearchCancelled();
        }
        activeSession = SearchSessionStore::Create(request_);
        const SearchSessionLocation &session = *activeSession;
        emit sessionCreated(session.mapKey, session.directory);
        if (cancellationRequested_->load(std::memory_order_relaxed)) {
            throw SearchCancelled();
        }
        std::optional<SearchResult> result;
        // Each RunSearch call restores the original baseline and owns a fresh
        // best result; only modifier seeds may change between cycles.
        for (;;) {
            retainedResult.reset();
            cyclePersisted = false;
            const auto attemptStartedNs =
                    std::make_shared<std::atomic<std::int64_t>>(0);
            control.beginIteration = [phase = iterationPhase_,
                                      attemptStartedNs]() {
                if (!TryBeginSearchIteration(phase)) return false;
                const auto nowNs = std::chrono::duration_cast<
                        std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now()
                                .time_since_epoch()).count();
                std::int64_t expected = 0;
                attemptStartedNs->compare_exchange_strong(expected, nowNs);
                return true;
            };
            control.stopRequested = [flag = stopRequested_,
                                     mode = restartPolicy_.mode,
                                     duration = restartPolicy_.duration,
                                     attemptStartedNs]() {
                if (flag->load(std::memory_order_relaxed)) return true;
                if (mode != AutoRestartPolicy::Mode::Duration) return false;
                const std::int64_t started = attemptStartedNs->load();
                if (started == 0) return false;
                const auto nowNs = std::chrono::duration_cast<
                        std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now()
                                .time_since_epoch()).count();
                return nowNs - started >=
                        std::chrono::duration_cast<
                                std::chrono::nanoseconds>(duration).count();
            };
            control.iterationLimit = restartPolicy_.mode ==
                            AutoRestartPolicy::Mode::Attempts
                    ? std::optional<std::uint64_t>(restartPolicy_.attempts)
                    : std::nullopt;
            if (restartNumber > 0 && restartPolicy_.randomizeSeeds) {
                std::mt19937 random(QRandomGenerator::system()->generate());
                for (OptionConfiguration &modifier : request_.modifiers) {
                    auto seed = modifier.settings.find("seed");
                    if (seed == modifier.settings.end()) continue;
                    std::uint32_t next = random();
                    if (seed->second == std::to_string(next)) ++next;
                    seed->second = std::to_string(next);
                }
            }
            result.emplace(RunSearch(request_, &control));
            SearchSessionStore::SaveCycle(
                    session, request_, restartNumber, *result);
            cyclePersisted = true;
            emit cycleSaved(session.mapKey, session.directory,
                            restartNumber);
            if (restartPolicy_.mode == AutoRestartPolicy::Mode::Off ||
                stopRequested_->load(std::memory_order_relaxed)) break;
            ++restartNumber;
            emit stageChanged(
                    QStringLiteral("Starting restart %1...")
                            .arg(static_cast<qulonglong>(restartNumber)),
                    true);
        }
        auto completion = std::make_shared<SearchCompletion>();
        completion->summary = FormatResult(*result);
        completion->inputsText = QString::fromStdString(
                FormatResultInputScript(result->bestInputs, request_.evaluationTarget.id,
                                        result->bestState));
        completion->packsDirectory =
                FilePathFromUtf8(request_.packDirectory);
        completion->replayPath = FilePathFromUtf8(request_.replayPath);
        const std::string_view backendId =
                PhysicsBackendId(kAuxiliarySimulationBackend);
        completion->simulationBackendId = QString::fromLatin1(
                backendId.data(), static_cast<qsizetype>(backendId.size()));
        completion->bestInputs = std::move(result->bestInputs);
        completion->bestTimeline = std::move(result->bestTimeline);
        emit succeeded(std::move(completion));
    } catch (const SearchCancelled &) {
        // Only completed, published evaluations are durable. Never promote a
        // partially simulated candidate or trigger another final sampling run.
        if (activeSession && retainedResult && !cyclePersisted) {
            try {
                SearchSessionStore::SaveAbortedCycle(
                        *activeSession, request_, restartNumber, *retainedResult);
                emit this->cycleSaved(activeSession->mapKey, activeSession->directory,
                                      restartNumber);
            } catch (const std::exception &error) {
                emit failed(QStringLiteral("Search aborted; could not save the retained best: %1")
                                    .arg(QString::fromUtf8(error.what())));
                emit finished();
                return;
            }
        }
        emit cancelled();
    } catch (const std::exception &error) {
        emit failed(QString::fromUtf8(error.what()));
    } catch (...) {
        emit failed(QStringLiteral("Unexpected search failure"));
    }
    emit finished();
}

}  // namespace forevertas::app
