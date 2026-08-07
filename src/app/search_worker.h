#ifndef FOREVERTAS_APP_SEARCH_WORKER_H
#define FOREVERTAS_APP_SEARCH_WORKER_H

#include "app/search_completion.h"
#include "searches/search_runner.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <memory>
#include <string_view>

namespace forevertas::app {

QString SearchStageStatus(SearchProgressStage stage,
                          std::string_view backendId,
                          bool useCudaSessionSpecialization = false);
QString SearchTargetProgressText(const SearchLiveUpdate &live);
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
                         iterationPhase);

public slots:
    void run();

signals:
    void stageChanged(const QString &status, bool indeterminate);
    void progressChanged(double value, const QString &status);
    void metricsChanged(const QString &iterationCountText,
                        const QString &throughputText,
                        const QString &elapsedText,
                        const QString &evaluationCountText,
                        const QString &mutationCountText,
                        const QString &improvementCountText,
                        const QString &targetProgressText);
    void throughputReset();
    void cudaCalibrationActiveChanged(bool active);
    void cudaActiveBatchSizeChanged(std::uint32_t batchSize);
    void bestChanged(const QString &summary, const QString &inputsText);
    void improvementFound(
            forevertas::app::SearchImprovementPtr improvement);
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
};

}  // namespace forevertas::app

#endif
