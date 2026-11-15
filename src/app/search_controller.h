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
    // Plain explanation of the last extraction failure; empty otherwise.
    Q_PROPERTY(QVariantMap replayInputFailure READ replayInputFailure NOTIFY
                       replayInputStateChanged)
    Q_PROPERTY(QVariantList simulationBackendOptions READ
                       simulationBackendOptions NOTIFY
                       computeBackendsChanged)
    Q_PROPERTY(QString simulationBackendId READ simulationBackendId WRITE
                       setSimulationBackendId NOTIFY simulationBackendIdChanged)
    Q_PROPERTY(QString simulationHorizonMs READ simulationHorizonMs WRITE
                       setSimulationHorizonMs NOTIFY simulationHorizonMsChanged)
    Q_PROPERTY(QString previewExtentMs READ previewExtentMs WRITE
                       setPreviewExtentMs NOTIFY previewExtentMsChanged)
    Q_PROPERTY(qint64 requiredSimulationHorizonMs READ requiredSimulationHorizonMs
                       NOTIFY requiredSimulationHorizonMsChanged)
    Q_PROPERTY(QString conditionScript READ conditionScript WRITE
                       setConditionScript NOTIFY conditionScriptChanged)
    Q_PROPERTY(QVariantList conditionReference READ conditionReference CONSTANT)
    Q_PROPERTY(QString cpuWorkerCount READ cpuWorkerCount WRITE
                       setCpuWorkerCount NOTIFY cpuWorkerCountChanged)
    Q_PROPERTY(QString cudaParallelSampleCount READ cudaParallelSampleCount WRITE
                       setCudaParallelSampleCount NOTIFY
                       cudaParallelSampleCountChanged)
    Q_PROPERTY(bool cudaCalibrationEnabled READ cudaCalibrationEnabled WRITE
                       setCudaCalibrationEnabled NOTIFY
                       cudaCalibrationEnabledChanged)
    Q_PROPERTY(QString hipParallelSampleCount READ hipParallelSampleCount WRITE
                       setHipParallelSampleCount NOTIFY
                       hipParallelSampleCountChanged)
    Q_PROPERTY(bool hipCalibrationEnabled READ hipCalibrationEnabled WRITE
                       setHipCalibrationEnabled NOTIFY
                       hipCalibrationEnabledChanged)
    // CUDA: "Exact" (false) or "Approximate" (true) physics for the search.
    Q_PROPERTY(bool approximatePhysics READ approximatePhysics WRITE
                       setApproximatePhysics NOTIFY approximatePhysicsChanged)
    Q_PROPERTY(bool approximatePhysicsAvailable READ
                       approximatePhysicsAvailable NOTIFY
                       computeBackendsChanged)
    // The chosen GPU (or every ticked graphics card) can run Approximate physics.
    Q_PROPERTY(bool approximatePhysicsOnGpu READ approximatePhysicsOnGpu NOTIFY gpuCardsChanged)
    // An NVIDIA and an AMD graphics card: the "GPU" backend, a box per card.
    Q_PROPERTY(bool multipleGpus READ multipleGpus NOTIFY computeBackendsChanged)
    Q_PROPERTY(QString cudaCardName READ cudaCardName NOTIFY computeBackendsChanged)
    Q_PROPERTY(QString hipCardName READ hipCardName NOTIFY computeBackendsChanged)
    Q_PROPERTY(bool cudaCardEnabled READ cudaCardEnabled WRITE setCudaCardEnabled NOTIFY gpuCardsChanged)
    Q_PROPERTY(bool hipCardEnabled READ hipCardEnabled WRITE setHipCardEnabled NOTIFY gpuCardsChanged)
    // The twin on CPU threads (the multi-threaded CPU backend, two workers or more).
    Q_PROPERTY(bool approximatePhysicsCpuAvailable READ
                       approximatePhysicsCpuAvailable CONSTANT)
    Q_PROPERTY(bool cudaAvailable READ cudaAvailable NOTIFY computeBackendsChanged)
    Q_PROPERTY(QString cudaStatusText READ cudaStatusText NOTIFY computeBackendsChanged)
    Q_PROPERTY(bool hipAvailable READ hipAvailable NOTIFY computeBackendsChanged)
    Q_PROPERTY(QString hipStatusText READ hipStatusText NOTIFY computeBackendsChanged)
    Q_PROPERTY(bool randomizeSeedsOnStart READ randomizeSeedsOnStart WRITE
                       setRandomizeSeedsOnStart NOTIFY
                       randomizeSeedsOnStartChanged)
    Q_PROPERTY(bool drawTargetsThroughBlocks READ drawTargetsThroughBlocks WRITE
                       setDrawTargetsThroughBlocks NOTIFY
                       drawTargetsThroughBlocksChanged)
    Q_PROPERTY(bool viewerVisible READ viewerVisible WRITE setViewerVisible NOTIFY viewerVisibleChanged)
    Q_PROPERTY(bool targetMouseEditingLocked READ targetMouseEditingLocked WRITE
                       setTargetMouseEditingLocked NOTIFY targetMouseEditingLockedChanged)
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
    // Bumped whenever a window chain changes, for bindings on windowChain().
    Q_PROPERTY(int windowChainRevision READ windowChainRevision NOTIFY
                       windowChainsChanged)
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
    Q_PROPERTY(QString iterationCountExactText READ iterationCountExactText NOTIFY metricsChanged)
    Q_PROPERTY(QString iterationCountRawText READ iterationCountRawText NOTIFY metricsChanged)
    Q_PROPERTY(QString throughputText READ throughputText NOTIFY metricsChanged)
    Q_PROPERTY(QString elapsedText READ elapsedText NOTIFY metricsChanged)
    Q_PROPERTY(QString resultText READ resultText NOTIFY resultChanged)
    Q_PROPERTY(QVariantMap failureDiagnostic READ failureDiagnostic NOTIFY resultChanged)
    Q_PROPERTY(QString bestInputsText READ bestInputsText NOTIFY resultChanged)
    Q_PROPERTY(QString autoRestartMode READ autoRestartMode WRITE
                       setAutoRestartMode NOTIFY autoRestartChanged)
    Q_PROPERTY(QString autoRestartDuration READ autoRestartDuration WRITE
                       setAutoRestartDuration NOTIFY autoRestartChanged)
    Q_PROPERTY(QString autoRestartAttempts READ autoRestartAttempts WRITE
                       setAutoRestartAttempts NOTIFY autoRestartChanged)
    Q_PROPERTY(QVariantList sessionOptions READ sessionOptions NOTIFY
                       historyChanged)
    Q_PROPERTY(QString selectedSessionDirectory READ
                       selectedSessionDirectory NOTIFY historyChanged)
    Q_PROPERTY(QVariantList cycleRows READ cycleRows NOTIFY historyChanged)
    // The current search's best as a table row (SearchSessionStore::LiveRow);
    // "running" turns false when the search ends. Empty before a search.
    Q_PROPERTY(QVariantMap liveRow READ liveRow NOTIFY liveRowChanged)
    Q_PROPERTY(bool evaluatingBase READ evaluatingBase NOTIFY baseEvaluationChanged)
    Q_PROPERTY(bool canEvaluateBase READ canEvaluateBase NOTIFY baseEvaluationChanged)
    Q_PROPERTY(QString baseEvaluationText READ baseEvaluationText NOTIFY baseEvaluationChanged)
    Q_PROPERTY(QVariantMap baseEvaluationResult READ baseEvaluationResult NOTIFY baseEvaluationChanged)
    Q_PROPERTY(bool historyHasOlder READ historyHasOlder NOTIFY historyChanged)
    Q_PROPERTY(bool historyHasNewer READ historyHasNewer NOTIFY historyChanged)
    Q_PROPERTY(qint64 selectedCycleRestart READ selectedCycleRestart NOTIFY historyChanged)
    Q_PROPERTY(QString selectedInputsText READ selectedInputsText NOTIFY
                       historyChanged)

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
    QVariantMap replayInputFailure() const { return replayInputFailure_; }
    QVariantList simulationBackendOptions() const;
    QString simulationBackendId() const;
    QString simulationHorizonMs() const;
    QString previewExtentMs() const { return previewExtentMs_; }
    qint64 requiredSimulationHorizonMs() const;
    QString conditionScript() const;
    QString cpuWorkerCount() const;
    QString cudaParallelSampleCount() const;
    QString hipParallelSampleCount() const;
    bool cudaCalibrationEnabled() const;
    bool hipCalibrationEnabled() const;
    bool approximatePhysics() const;
    bool approximatePhysicsAvailable() const;
    bool approximatePhysicsCpuAvailable() const;
    bool cudaAvailable() const;
    bool hipAvailable() const;
    bool approximatePhysicsOnGpu() const;
    bool multipleGpus() const;
    QString cudaCardName() const;
    QString hipCardName() const;
    bool cudaCardEnabled() const;
    bool hipCardEnabled() const;
    QString cudaStatusText() const;
    QString hipStatusText() const;
    bool randomizeSeedsOnStart() const;
    bool drawTargetsThroughBlocks() const;
    bool viewerVisible() const;
    bool targetMouseEditingLocked() const;
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
    void requestShutdown();
    bool shutdownReady() const;
    void flushSettings();
    bool evaluatingBase() const { return baselineEvaluationThread_ != nullptr; }
    bool canEvaluateBase() const;
    QString baseEvaluationText() const { return baseEvaluationText_; }
    QVariantMap baseEvaluationResult() const { return baseEvaluationResult_; }
    bool running() const;
    bool stopping() const;
    bool progressIndeterminate() const;
    double progressValue() const;
    QString validationMessage() const;
    QVariantList conditionReference() const;
    QString statusText() const;
    QVariantMap failureDiagnostic() const { return failureDiagnostic_; }
    bool liveMetricsVisible() const;
    QString iterationCountText() const;
    QString iterationCountExactText() const;
    QString iterationCountRawText() const;
    QString throughputText() const;
    QString elapsedText() const;
    QString resultText() const;
    QString bestInputsText() const;
    QString autoRestartMode() const;
    QString autoRestartDuration() const;
    QString autoRestartAttempts() const;
    QVariantList sessionOptions() const;
    QString selectedSessionDirectory() const;
    QVariantList cycleRows() const;
    QVariantMap liveRow() const;
    bool historyHasOlder() const { return historyHasOlder_; }
    bool historyHasNewer() const { return historyHasNewer_; }
    qint64 selectedCycleRestart() const { return selectedCycleRestart_; }
    QString selectedInputsText() const;

public slots:
    void setPacksDirectory(const QString &value);
    void setReplayPath(const QString &value);
    void setBaseInputScript(const QString &value);
    void setSimulationBackendId(const QString &value);
    void setSimulationHorizonMs(const QString &value);
    void setPreviewExtentMs(const QString &value);
    void setConditionScript(const QString &value);
    void setCpuWorkerCount(const QString &value);
    void setCudaParallelSampleCount(const QString &value);
    void setHipParallelSampleCount(const QString &value);
    void setCudaCalibrationEnabled(bool value);
    void setHipCalibrationEnabled(bool value);
    void setCudaCardEnabled(bool value);
    void setHipCardEnabled(bool value);
    void setApproximatePhysics(bool value);
    void setRandomizeSeedsOnStart(bool value);
    void setAutoRestartMode(const QString &value);
    void setAutoRestartDuration(const QString &value);
    void setAutoRestartAttempts(const QString &value);
    void setDrawTargetsThroughBlocks(bool value);
    void setViewerVisible(bool value);
    void setTargetMouseEditingLocked(bool value);
    void setSearchAlgorithmId(const QString &value);
    void setEvaluationTargetId(const QString &value);

    // Both return whether a path was picked.
    Q_INVOKABLE bool browseForPacksDirectory();
    Q_INVOKABLE void applyAutoDetectedPacksDirectory();
    Q_INVOKABLE bool browseForReplay();
    // Saved window and panel preferences (panel layout, zoom, glass look).
    Q_INVOKABLE QVariant uiSetting(const QString &key,
                                   const QVariant &fallback) const;
    Q_INVOKABLE void setUiSetting(const QString &key, const QVariant &value);
    // Whether the keyboard's letters sit like AZERTY's (for default keys).
    Q_INVOKABLE bool keyboardIsAzerty() const;
    Q_INVOKABLE QString formatCompactNumber(double value) const;
    Q_INVOKABLE bool extendSimulationHorizon();
    Q_INVOKABLE void previewBaseInputs();
    Q_INVOKABLE QString formatSpeed(double metersPerSecond) const;
    // Where the search starts judging its target (ms); -1 when unknown.
    Q_INVOKABLE double evaluationStartMs() const;
    Q_INVOKABLE void extractReplayInputs();
    Q_INVOKABLE bool undoBaseInputScript();
    Q_INVOKABLE void setSearchAlgorithmSetting(const QString &key,
                                               const QString &value);
    Q_INVOKABLE void addModifierPass(const QString &id);
    Q_INVOKABLE void removeModifierPass(int index);
    Q_INVOKABLE void moveModifierPass(int fromIndex, int toIndex);
    Q_INVOKABLE void setModifierPassId(int index, const QString &id);
    Q_INVOKABLE void setModifierPassEnabled(int index, bool enabled);
    Q_INVOKABLE void setModifierPassSetting(int index,
                                            const QString &key,
                                            const QString &value);
    // Window chains (as in TICK): windows linked in the same color change
    // together. Keys: "pass:<index>", "target", "searchEnd",
    // "previewExtent". A chain is {color, linked}.
    Q_INVOKABLE QVariantMap windowChain(const QString &key) const;
    int windowChainRevision() const { return windowChainRevision_; }
    Q_INVOKABLE void setWindowChain(const QString &key, const QString &color,
                                    bool linked);
    Q_INVOKABLE void setEvaluationTargetSetting(const QString &key,
                                                const QString &value);
    Q_INVOKABLE void focusSelectedCuboid();
    Q_INVOKABLE void focusSelectedCustomVolume();
    Q_INVOKABLE void beginCustomVolumeDrawing();
    Q_INVOKABLE void finishCustomVolumeDrawing();
    Q_INVOKABLE void cancelCustomVolumeDrawing();
    Q_INVOKABLE void focusSelectedPoseTarget();
    // GPU backends again (after a GPU module was installed)
    Q_INVOKABLE void refreshComputeBackends();
    Q_INVOKABLE void startSearch();
    Q_INVOKABLE void evaluateBase();
    Q_INVOKABLE void cancelBaseEvaluation();
    Q_INVOKABLE void stopSearch();
    Q_INVOKABLE void selectSession(int index);
    Q_INVOKABLE void selectCycle(int index);
    Q_INVOKABLE void changeCyclePage(bool older);

signals:
    // a GPU backend came or went (a GPU module was installed)
    void computeBackendsChanged();
    void gpuCardsChanged();
    void baseEvaluationChanged();
    void packsDirectoryChanged();
    void autoDetectedPacksDirectoryChanged();
    void replayPathChanged();
    void baseInputScriptChanged();
    void replayInputStateChanged();
    void simulationBackendIdChanged();
    void simulationHorizonMsChanged();
    void previewExtentMsChanged();
    void conditionScriptChanged();
    void requiredSimulationHorizonMsChanged();
    void cpuWorkerCountChanged();
    void cudaParallelSampleCountChanged();
    void hipParallelSampleCountChanged();
    void cudaCalibrationEnabledChanged();
    void hipCalibrationEnabledChanged();
    void approximatePhysicsChanged();
    void randomizeSeedsOnStartChanged();
    void drawTargetsThroughBlocksChanged();
    void viewerVisibleChanged();
    void targetMouseEditingLockedChanged();
    void searchAlgorithmIdChanged();
    void evaluationTargetIdChanged();
    void searchAlgorithmSettingsChanged();
    void modifierPassesChanged();
    void windowChainsChanged();
    void evaluationTargetSettingsChanged();
    void canStartChanged();
    void runningChanged();
    void stoppingChanged();
    void progressChanged();
    void validationChanged();
    void statusChanged();
    void metricsChanged();
    void resultChanged();
    void autoRestartChanged();
    void historyChanged();
    void liveRowChanged();
    void searchSessionReset();
    void searchStarted(std::uint64_t searchId);
    void basePreviewRequested();
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
    struct ChainWindow {
        bool hasStart = false;
        QString start;
        QString end;
        QString endMode;
    };
    QStringList windowChainKeys() const;
    std::optional<ChainWindow> chainWindow(const QString &key) const;
    void applyChainWindow(const QString &key, const ChainWindow &window);
    void propagateWindowChain(const QString &key);
    bool propagatingWindowChain_ = false;
    int windowChainRevision_ = 0;

    struct ValidationResult {
        std::optional<SearchRequest> request;
        QString error;
        // the "GPU" backend with two cards ticked: both (the request is the first's)
        std::vector<SearchDevice> devices = {};
    };
    // The "GPU" backend (two cards) is chosen: simulationBackend_ is then the
    // first ticked card's.
    bool gpusChosen() const;
    void setGpusChosen(bool chosen);
    QString deviceSettings(PhysicsBackend backend, SearchDevice *device) const;

    ValidationResult validate(bool baselineOnly = false) const;
    void refreshValidation();
    void setRunning(bool value);
    void setStopping(bool value);
    void setStatusText(const QString &value);
    void setLiveMetrics(const QString &iterationCountText,
                        const QString &throughputText,
                        const QString &elapsedText,
                        bool visible);
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
    void refreshSessions();
    void readComputeBackends();

    QString packsDirectory_;
    QString autoDetectedPacksDirectory_;
    QString replayPath_;
    QString baseInputScript_;
    QString baseInputScriptError_;
    std::vector<QString> baseInputScriptUndoHistory_;
    QString replayInputStatusText_;
    QVariantMap replayInputFailure_;
    std::vector<ParsedInputCommand> parsedBaseInputCommands_;
    PhysicsBackend simulationBackend_ = PhysicsBackend::Reference;
    QString simulationHorizonMs_ = QString::number(
            kDefaultSimulationHorizonMs);
    // How far the viewer previews inputs; independent of the search end.
    QString previewExtentMs_;
    QString conditionScript_;
    QString cpuWorkerCount_ = QString::number(DefaultCpuWorkerCount());
    QString cudaParallelSampleCount_ = QString::number(
            kDefaultCudaParallelSampleCount);
    QString hipParallelSampleCount_ = QString::number(
            kDefaultCudaParallelSampleCount);
    bool cudaCalibrationEnabled_ = false;
    bool hipCalibrationEnabled_ = false;
    bool approximatePhysics_ = false;
    bool cudaAvailable_ = false;
    QString cudaStatusText_;
    bool hipAvailable_ = false;
    bool cudaApproximate_ = false;
    bool gpusMode_ = false;
    bool cudaCardEnabled_ = true;
    bool hipCardEnabled_ = true;
    QString cudaCardName_;
    QString hipCardName_;
    bool hipApproximate_ = false;
    QString hipStatusText_;
    bool randomizeSeedsOnStart_ = true;
    bool drawTargetsThroughBlocks_ = false;
    bool viewerVisible_ = true;
    bool targetMouseEditingLocked_ = false;
    SearchConfigurationModel configuration_;
    CuboidTargetModel cuboidTargets_;
    CustomVolumeTargetModel customVolumeTargets_;
    PoseTargetModel poseTargets_;
    QString validationMessage_;
    QString statusText_ = QStringLiteral("Ready");
    QVariantMap failureDiagnostic_;
    QString iterationCountText_;
    std::uint64_t iterationCount_ = 0u;
    QString throughputText_;
    QString elapsedText_;
    QString resultText_;
    QString bestInputsText_;
    QString autoRestartMode_ = QStringLiteral("off");
    QString autoRestartDuration_ = QStringLiteral("00:05:00");
    QString autoRestartAttempts_ = QStringLiteral("1000");
    QVariantList sessionOptions_;
    QVariantList cycleRows_;
    QVariantMap liveRow_;
    bool historyHasOlder_ = false;
    bool historyHasNewer_ = false;
    bool historySelectionExplicit_ = false;
    qint64 selectedCycleRestart_ = -1;
    QString selectedSessionDirectory_;
    QString selectedInputsText_;
    bool valid_ = false;
    bool liveMetricsVisible_ = false;
    bool running_ = false;
    bool shuttingDown_ = false;
    bool stopping_ = false;
    bool progressIndeterminate_ = false;
    bool autoDetectionScheduled_ = false;
    double progressValue_ = 0.0;
    QThread *autoDetectionThread_ = nullptr;
    QThread *baselineEvaluationThread_ = nullptr;
    std::shared_ptr<std::atomic_bool> baselineEvaluationCancelled_;
    quint64 baselineEvaluationGeneration_ = 0;
    QString baseEvaluationText_;
    QVariantMap baseEvaluationResult_;
    QThread *inputExtractionThread_ = nullptr;
    QThread *workerThread_ = nullptr;
    QTimer *inputScriptPersistTimer_ = nullptr;
    bool extractingReplayInputs_ = false;
    std::shared_ptr<std::atomic_bool> stopRequested_;
    std::shared_ptr<std::atomic_bool> cancellationRequested_;
    std::shared_ptr<std::atomic<SearchIterationPhase>> iterationPhase_;
    SearchCompletionPtr lastCompletion_;
    std::uint64_t searchSerial_ = 0u;
};

}  // namespace forevertas::app

#endif
