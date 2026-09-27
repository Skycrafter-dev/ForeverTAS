#ifndef FOREVERTAS_APP_SEARCH_WORKER_H
#define FOREVERTAS_APP_SEARCH_WORKER_H

#include "app/search_completion.h"
#include "searches/search_runner.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <chrono>
#include <memory>
#include <string_view>

namespace forevertas::app {

struct AutoRestartPolicy {
    enum class Mode { Off, Duration, Attempts } mode = Mode::Off;
    std::chrono::seconds duration{0};
    std::uint64_t attempts = 0;
    bool randomizeSeeds = false;
};

QString SearchStageStatus(SearchProgressStage stage,
                          std::string_view backendId,
                          bool useCudaSessionSpecialization = false);
bool TryBeginSearchIteration(
        const std::shared_ptr<std::atomic<SearchIterationPhase>> &phase);
bool TryCancelBeforeSearchIteration(
        const std::shared_ptr<std::atomic<SearchIterationPhase>> &phase);

class SearchWorker final : public QObject {
    Q_OBJECT

public:
    SearchWorker(SearchRequest request,
                 std::uint64_t searchId,
                 std::shared_ptr<std::atomic_bool> stopRequested,
                 std::shared_ptr<std::atomic_bool> cancellationRequested,
                 std::shared_ptr<std::atomic<SearchIterationPhase>>
                         iterationPhase,
                 AutoRestartPolicy restartPolicy = {});

public slots:
    void run();

signals:
    void stageChanged(const QString &status, bool indeterminate);
    void progressChanged(double value, const QString &status);
    void metricsChanged(const QString &iterationCountText,
                        const QString &throughputText,
                        const QString &elapsedText);
    void cudaBatchSizeChanged(std::uint32_t batchSize);
    void bestChanged(const QString &summary, const QString &inputsText);
    void improvementFound(
            forevertas::app::SearchImprovementPtr improvement);
    void cycleSaved(const QString &mapKey, const QString &directory,
                    std::uint64_t restartNumber);
    void sessionCreated(const QString &mapKey, const QString &directory);
    void succeeded(forevertas::app::SearchCompletionPtr completion);
    void cancelled();
    void failed(const QString &message);
    void finished();

private:
    SearchRequest request_;
    std::uint64_t searchId_ = 0u;
    std::shared_ptr<std::atomic_bool> stopRequested_;
    std::shared_ptr<std::atomic_bool> cancellationRequested_;
    std::shared_ptr<std::atomic<SearchIterationPhase>> iterationPhase_;
    AutoRestartPolicy restartPolicy_;
};

}  // namespace forevertas::app

#endif
