#ifndef FOREVERTAS_APP_SEARCH_CONTROLLER_H
#define FOREVERTAS_APP_SEARCH_CONTROLLER_H

#include "app/search_configuration_model.h"
#include "app/search_completion.h"
#include "app/cuboid_target_model.h"
#include "app/custom_volume_target_model.h"
#include "app/pose_target_model.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>

class QThread;
class QTimer;

namespace forevertas::app {

class SearchController final : public QObject {
    Q_OBJECT

    Q_PROPERTY(QString packsDirectory READ packsDirectory WRITE
                       setPacksDirectory NOTIFY packsDirectoryChanged)
    Q_PROPERTY(QString autoDetectedPacksDirectory READ
                       autoDetectedPacksDirectory NOTIFY
                       autoDetectedPacksDirectoryChanged)
    Q_PROPERTY(QString replayPath READ replayPath WRITE setReplayPath NOTIFY
                       replayPathChanged)
    Q_PROPERTY(QString baseInputScript READ baseInputScript WRITE
                       setBaseInputScript NOTIFY baseInputScriptChanged)
    Q_PROPERTY(QString baseInputScriptError READ baseInputScriptError NOTIFY
                       baseInputScriptChanged)
    Q_PROPERTY(bool canUndoBaseInputScript READ canUndoBaseInputScript NOTIFY
                       baseInputScriptChanged)
    Q_PROPERTY(bool extractingReplayInputs READ extractingReplayInputs NOTIFY
                       replayInputStateChanged)
    Q_PROPERTY(bool canExtractReplayInputs READ canExtractReplayInputs NOTIFY
                       replayInputStateChanged)
    Q_PROPERTY(QString replayInputStatusText READ replayInputStatusText NOTIFY
                       replayInputStateChanged)
    Q_PROPERTY(QVariantList simulationBackendOptions READ
                       simulationBackendOptions CONSTANT)
    Q_PROPERTY(QString simulationBackendId READ simulationBackendId WRITE
                       setSimulationBackendId NOTIFY simulationBackendIdChanged)
    Q_PROPERTY(QString simulationHorizonMs READ simulationHorizonMs WRITE
                       setSimulationHorizonMs NOTIFY simulationHorizonMsChanged)
    Q_PROPERTY(QString conditionScript READ conditionScript WRITE
                       setConditionScript NOTIFY conditionScriptChanged)
    Q_PROPERTY(QString cpuWorkerCount READ cpuWorkerCount WRITE
                       setCpuWorkerCount NOTIFY cpuWorkerCountChanged)
    Q_PROPERTY(QString cudaParallelSampleCount READ cudaParallelSampleCount WRITE
                       setCudaParallelSampleCount NOTIFY
                       cudaParallelSampleCountChanged)
    Q_PROPERTY(bool cudaCalibrationEnabled READ cudaCalibrationEnabled WRITE
                       setCudaCalibrationEnabled NOTIFY
                       cudaCalibrationEnabledChanged)
    Q_PROPERTY(QString cudaCalibrationStartSampleCount READ
                       cudaCalibrationStartSampleCount WRITE
                       setCudaCalibrationStartSampleCount NOTIFY
                       cudaCalibrationStartSampleCountChanged)
    Q_PROPERTY(QString cudaActiveCalibrationBatchSampleCount READ
                       cudaActiveCalibrationBatchSampleCount NOTIFY
                       cudaActiveCalibrationBatchSampleCountChanged)
    Q_PROPERTY(QString cudaActiveBatchSampleCount READ
                       cudaActiveBatchSampleCount NOTIFY
                       cudaActiveBatchSampleCountChanged)
    Q_PROPERTY(bool cudaSessionSpecializationEnabled READ
                       cudaSessionSpecializationEnabled WRITE
                       setCudaSessionSpecializationEnabled NOTIFY
                       cudaSessionSpecializationEnabledChanged)
    Q_PROPERTY(bool randomizeSeedsOnStart READ randomizeSeedsOnStart WRITE
                       setRandomizeSeedsOnStart NOTIFY
                       randomizeSeedsOnStartChanged)
    Q_PROPERTY(bool drawTargetsThroughBlocks READ drawTargetsThroughBlocks WRITE
                       setDrawTargetsThroughBlocks NOTIFY
                       drawTargetsThroughBlocksChanged)
    Q_PROPERTY(bool darkMode READ darkMode WRITE setDarkMode NOTIFY
                       darkModeChanged)
    Q_PROPERTY(QVariantList searchAlgorithmOptions READ searchAlgorithmOptions
                       CONSTANT)
    Q_PROPERTY(QVariantList modifierOptions READ modifierOptions CONSTANT)
    Q_PROPERTY(QVariantList evaluationTargetOptions READ evaluationTargetOptions
                       CONSTANT)
    Q_PROPERTY(QString searchAlgorithmId READ searchAlgorithmId WRITE
                       setSearchAlgorithmId NOTIFY searchAlgorithmIdChanged)
    Q_PROPERTY(QString evaluationTargetId READ evaluationTargetId WRITE
                       setEvaluationTargetId NOTIFY evaluationTargetIdChanged)
    Q_PROPERTY(QVariantMap searchAlgorithmSettings READ searchAlgorithmSettings
                       NOTIFY searchAlgorithmSettingsChanged)
    Q_PROPERTY(QVariantList modifierPasses READ modifierPasses NOTIFY
                       modifierPassesChanged)
    Q_PROPERTY(QVariantMap evaluationTargetSettings READ
                       evaluationTargetSettings NOTIFY
                       evaluationTargetSettingsChanged)
    Q_PROPERTY(forevertas::app::CuboidTargetModel* cuboidTargets READ
                       cuboidTargets CONSTANT)
    Q_PROPERTY(forevertas::app::CustomVolumeTargetModel*
                       customVolumeTargets READ customVolumeTargets CONSTANT)
    Q_PROPERTY(bool customVolumeDrawing READ customVolumeDrawing NOTIFY
                       customVolumeDrawingChanged)
    Q_PROPERTY(forevertas::app::PoseTargetModel* poseTargets READ
                       poseTargets CONSTANT)

    Q_PROPERTY(bool canStart READ canStart NOTIFY canStartChanged)
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(bool stopping READ stopping NOTIFY stoppingChanged)
    Q_PROPERTY(bool progressIndeterminate READ progressIndeterminate NOTIFY
                       progressChanged)
    Q_PROPERTY(double progressValue READ progressValue NOTIFY progressChanged)
    Q_PROPERTY(QString validationMessage READ validationMessage NOTIFY
                       validationChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(bool liveMetricsVisible READ liveMetricsVisible NOTIFY
                       metricsChanged)
    Q_PROPERTY(QString iterationCountText READ iterationCountText NOTIFY
                       metricsChanged)
    Q_PROPERTY(QString throughputText READ throughputText NOTIFY metricsChanged)
    Q_PROPERTY(QString elapsedText READ elapsedText NOTIFY metricsChanged)
    Q_PROPERTY(QString evaluationCountText READ evaluationCountText NOTIFY
                       metricsChanged)
    Q_PROPERTY(QString mutationCountText READ mutationCountText NOTIFY
                       metricsChanged)
    Q_PROPERTY(QString improvementCountText READ improvementCountText NOTIFY
                       metricsChanged)
    Q_PROPERTY(QString resultText READ resultText NOTIFY resultChanged)
    Q_PROPERTY(QString bestInputsText READ bestInputsText NOTIFY resultChanged)

public:
    explicit SearchController(QObject *parent = nullptr);
    explicit SearchController(const QStringList &packsSearchPatterns,
                              QObject *parent = nullptr);
    ~SearchController() override;

    QString packsDirectory() const;
    QString autoDetectedPacksDirectory() const;
    QString replayPath() const;
    QString baseInputScript() const;
    QString baseInputScriptError() const;
    bool canUndoBaseInputScript() const;
    bool extractingReplayInputs() const;
    bool canExtractReplayInputs() const;
    QString replayInputStatusText() const;
    QVariantList simulationBackendOptions() const;
    QString simulationBackendId() const;
    QString simulationHorizonMs() const;
    QString conditionScript() const;
    QString cpuWorkerCount() const;
    QString cudaParallelSampleCount() const;
    bool cudaCalibrationEnabled() const;
    QString cudaCalibrationStartSampleCount() const;
    QString cudaActiveCalibrationBatchSampleCount() const;
    QString cudaActiveBatchSampleCount() const;
    bool cudaSessionSpecializationEnabled() const;
    bool randomizeSeedsOnStart() const;
    bool drawTargetsThroughBlocks() const;
    bool darkMode() const;
    QVariantList searchAlgorithmOptions() const;
    QVariantList modifierOptions() const;
    QVariantList evaluationTargetOptions() const;
    QString searchAlgorithmId() const;
    QString evaluationTargetId() const;
    QVariantMap searchAlgorithmSettings() const;
    QVariantList modifierPasses() const;
    QVariantMap evaluationTargetSettings() const;
    CuboidTargetModel *cuboidTargets();
    CustomVolumeTargetModel *customVolumeTargets();
    bool customVolumeDrawing() const;
    PoseTargetModel *poseTargets();

    bool canStart() const;
    bool running() const;
    bool stopping() const;
    bool progressIndeterminate() const;
    double progressValue() const;
    QString validationMessage() const;
    QString statusText() const;
    bool liveMetricsVisible() const;
    QString iterationCountText() const;
    QString throughputText() const;
    QString elapsedText() const;
    QString evaluationCountText() const;
    QString mutationCountText() const;
    QString improvementCountText() const;
    QString resultText() const;
    QString bestInputsText() const;

public slots:
    void setPacksDirectory(const QString &value);
    void setReplayPath(const QString &value);
    void setBaseInputScript(const QString &value);
    void setSimulationBackendId(const QString &value);
    void setSimulationHorizonMs(const QString &value);
    void setConditionScript(const QString &value);
    void setCpuWorkerCount(const QString &value);
    void setCudaParallelSampleCount(const QString &value);
    void setCudaCalibrationEnabled(bool value);
    void setCudaCalibrationStartSampleCount(const QString &value);
    void setCudaSessionSpecializationEnabled(bool value);
    void setRandomizeSeedsOnStart(bool value);
    void setDrawTargetsThroughBlocks(bool value);
    void setDarkMode(bool value);
    void setSearchAlgorithmId(const QString &value);
    void setEvaluationTargetId(const QString &value);

    Q_INVOKABLE void browseForPacksDirectory();
    Q_INVOKABLE void applyAutoDetectedPacksDirectory();
    Q_INVOKABLE void browseForReplay();
    Q_INVOKABLE QString formatCompactNumber(double value) const;
    Q_INVOKABLE void extractReplayInputs();
    Q_INVOKABLE bool undoBaseInputScript();
    Q_INVOKABLE void setSearchAlgorithmSetting(const QString &key,
                                               const QString &value);
    Q_INVOKABLE void addModifierPass(const QString &id);
    Q_INVOKABLE void removeModifierPass(int index);
    Q_INVOKABLE void moveModifierPass(int fromIndex, int toIndex);
    Q_INVOKABLE void setModifierPassId(int index, const QString &id);
    Q_INVOKABLE void setModifierPassSetting(int index,
                                            const QString &key,
                                            const QString &value);
    Q_INVOKABLE void setEvaluationTargetSetting(const QString &key,
                                                const QString &value);
    Q_INVOKABLE void focusSelectedCuboid();
    Q_INVOKABLE void focusSelectedCustomVolume();
    Q_INVOKABLE void beginCustomVolumeDrawing();
    Q_INVOKABLE void finishCustomVolumeDrawing();
    Q_INVOKABLE void cancelCustomVolumeDrawing();
    Q_INVOKABLE void focusSelectedPoseTarget();
    Q_INVOKABLE void startSearch();
    Q_INVOKABLE void stopSearch();

signals:
    void packsDirectoryChanged();
    void autoDetectedPacksDirectoryChanged();
    void replayPathChanged();
    void baseInputScriptChanged();
    void replayInputStateChanged();
    void simulationBackendIdChanged();
    void simulationHorizonMsChanged();
    void conditionScriptChanged();
    void cpuWorkerCountChanged();
    void cudaParallelSampleCountChanged();
    void cudaCalibrationEnabledChanged();
    void cudaCalibrationStartSampleCountChanged();
    void cudaActiveCalibrationBatchSampleCountChanged();
    void cudaActiveBatchSampleCountChanged();
    void cudaSessionSpecializationEnabledChanged();
    void randomizeSeedsOnStartChanged();
    void drawTargetsThroughBlocksChanged();
    void darkModeChanged();
    void searchAlgorithmIdChanged();
    void evaluationTargetIdChanged();
    void searchAlgorithmSettingsChanged();
    void modifierPassesChanged();
    void evaluationTargetSettingsChanged();
    void canStartChanged();
    void runningChanged();
    void stoppingChanged();
    void progressChanged();
    void validationChanged();
    void statusChanged();
    void metricsChanged();
    void resultChanged();
    void searchImprovement(
            forevertas::app::SearchImprovementPtr improvement);
    void searchCompleted(forevertas::app::SearchCompletionPtr completion);
    void cuboidFocusRequested(const QVector3D &center,
                              const QVector3D &size);
    void customVolumeFocusRequested(const QVector3D &center,
                                    const QVector3D &size);
    void customVolumeDrawingChanged();
    void poseTargetFocusRequested(const QVector3D &center,
                                  const QVector3D &size);

private:
    struct ValidationResult {
        std::optional<SearchRequest> request;
        QString error;
    };

    ValidationResult validate() const;
    void refreshValidation();
    void setRunning(bool value);
    void setStopping(bool value);
    void setStatusText(const QString &value);
    void setLiveMetrics(const QString &iterationCountText,
                        const QString &throughputText,
                        const QString &elapsedText,
                        const QString &evaluationCountText,
                        const QString &mutationCountText,
                        const QString &improvementCountText,
                        bool visible);
    void setCudaActiveCalibrationBatchSampleCount(const QString &value);
    void setCudaActiveBatchSampleCount(const QString &value);
    void setResultText(const QString &value);
    void setBestInputsText(const QString &value);
    void setExtractingReplayInputs(bool value);
    void setReplayInputStatusText(const QString &value);
    void setProgress(bool indeterminate, double value);
    void initialize(const QStringList *packsSearchPatterns);
    void scheduleAutoDetectPacksDirectory(
            const QStringList *packsSearchPatterns);
    void publishAutoDetectedPacksDirectory(const QString &detected);
    void clearAutoDetectedPacksDirectory();
    void persist(const char *key, const QString &value);
    void waitForWorker();
    void synchronizeSelectedCuboid();
    void synchronizeCuboidSetting(const QString &key,
                                   const QString &value);
    void synchronizeSelectedCustomVolume();
    void synchronizeCustomVolumeSetting(const QString &key,
                                        const QString &value);
    void synchronizeSelectedPoseTarget();
    void synchronizePoseTargetSetting(const QString &key,
                                      const QString &value);
    void applyBaseInputScript(const QString &value, bool recordUndo);

    QString packsDirectory_;
    QString autoDetectedPacksDirectory_;
    QString replayPath_;
    QString baseInputScript_;
    QString baseInputScriptError_;
    std::vector<QString> baseInputScriptUndoHistory_;
    QString replayInputStatusText_;
    std::vector<ParsedInputCommand> parsedBaseInputCommands_;
    PhysicsBackend simulationBackend_ = PhysicsBackend::Reference;
    QString simulationHorizonMs_ = QString::number(
            kDefaultSimulationHorizonMs);
    QString conditionScript_;
    QString cpuWorkerCount_ = QString::number(DefaultCpuWorkerCount());
    QString cudaParallelSampleCount_ = QString::number(
            kDefaultCudaParallelSampleCount);
    bool cudaCalibrationEnabled_ = false;
    QString cudaCalibrationStartSampleCount_ = QString::number(
            kDefaultCudaCalibrationStartSampleCount);
    bool cudaCalibrationActive_ = false;
    QString cudaActiveCalibrationBatchSampleCount_;
    QString cudaActiveBatchSampleCount_;
    bool cudaSessionSpecializationEnabled_ = true;
    bool randomizeSeedsOnStart_ = true;
    bool drawTargetsThroughBlocks_ = false;
    bool darkMode_ = false;
    SearchConfigurationModel configuration_;
    CuboidTargetModel cuboidTargets_;
    CustomVolumeTargetModel customVolumeTargets_;
    PoseTargetModel poseTargets_;
    QString validationMessage_;
    QString statusText_ = QStringLiteral("Ready");
    QString iterationCountText_;
    QString throughputText_;
    QString elapsedText_;
    QString evaluationCountText_;
    QString mutationCountText_;
    QString improvementCountText_;
    QString resultText_;
    QString bestInputsText_;
    bool valid_ = false;
    bool liveMetricsVisible_ = false;
    bool running_ = false;
    bool stopping_ = false;
    bool progressIndeterminate_ = false;
    bool autoDetectionScheduled_ = false;
    double progressValue_ = 0.0;
    QThread *autoDetectionThread_ = nullptr;
    QThread *inputExtractionThread_ = nullptr;
    QThread *workerThread_ = nullptr;
    QTimer *inputScriptPersistTimer_ = nullptr;
    QTimer *searchElapsedUpdateTimer_ = nullptr;
    std::optional<std::chrono::steady_clock::time_point> searchStarted_;
    bool extractingReplayInputs_ = false;
    std::shared_ptr<std::atomic_bool> stopRequested_;
    std::shared_ptr<std::atomic_bool> cancellationRequested_;
    std::shared_ptr<std::atomic<SearchIterationPhase>> iterationPhase_;
    SearchCompletionPtr lastCompletion_;
    std::uint64_t searchSerial_ = 0u;
};

}  // namespace forevertas::app

#endif
