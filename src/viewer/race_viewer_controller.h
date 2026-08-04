#ifndef FOREVERTAS_VIEWER_RACE_VIEWER_CONTROLLER_H
#define FOREVERTAS_VIEWER_RACE_VIEWER_CONTROLLER_H

#include "physics_backend.h"
#include "searches/search_algorithm.h"
#include "viewer/race_geometry.h"
#include "viewer/simulation_debugger_model.h"
#include "viewer/whiteboard_model.h"
#include "viewer/ray_tracing_scene.h"
#include "viewer/visual_scene_pipeline.h"

#include <QElapsedTimer>
#include <QObject>
#include <QQuaternion>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVector2D>
#include <QVector3D>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

class QThread;

namespace forevertas::viewer {

class ManualDriveRuntime;
class RaceCameraResources;
class RaceCameraRuntime;

struct RaceViewerFrame {
    std::int64_t timeMs = 0;
    QVector3D position{};
    QQuaternion rotation{};
    float accelerate = 0.0f;
    float brake = 0.0f;
    float steering = 0.0f;
    std::uint32_t checkpointsCollected = 0u;
    std::uint32_t checkpointsTotal = 0u;
    std::uint32_t completedLaps = 0u;
    std::uint32_t totalLaps = 1u;
    bool raceCompleted = false;
    std::optional<std::uint32_t> finishTimeMs;
    QVector3D linearSpeed{};
    float signedSpeed = 0.0f;
    float turbo = 0.0f;
    float cameraFlightTransition = 0.0f;
    bool burning = false;
    bool gearChanged = false;
    std::array<bool, 4> wheelContact{{true, true, true, true}};
    std::array<bool, 4> wheelHasSurface{{true, true, true, true}};
    QVector3D cameraSupportUp{0.0f, 1.0f, 0.0f};
};

struct RaceViewerSplit {
    std::uint32_t index = 0u;
    std::int64_t timeMs = 0;
    bool isFinish = false;
};

struct RaceViewerInputSample {
    float accelerate = 0.0f;
    float brake = 0.0f;
    float steering = 0.0f;
};

struct RaceViewerRun {
    QString id;
    QString name;
    std::vector<RaceViewerFrame> frames;
    std::vector<SandboxInputEvent> inputs;
    QVector3D position{};
    QQuaternion rotation{};
    std::vector<RaceViewerSplit> checkpointSplits;
    std::shared_ptr<ManualDriveRuntime> runtime;
};

struct RaceViewerMeshBuffers {
    QByteArray filled;
    QByteArray wire;
    QVector3D boundsMin{};
    QVector3D boundsMax{};
};

struct RaceViewerLoadResult {
    QString error;
    QString packsDirectory;
    QString replayPath;
    QString mapKey;
    QString mapName;
    RaceViewerMeshBuffers track;
    std::vector<StaticVisualBatch> visualBatches;
    std::shared_ptr<const RayTracingSceneData> rayTracingScene;
    QVariantList visualMaterials;
    QVariantList visualBatchItems;
    QVector3D visualBoundsMin{};
    QVector3D visualBoundsMax{};
    QVariantList carEllipsoids;
    std::int64_t triangleCount = 0;
    std::int64_t visualTriangleCount = 0;
    std::int64_t sourceVisualObjectCount = 0;
    std::int64_t sourceVisualMeshCount = 0;
    std::int64_t duplicateVisualObjectCount = 0;
    std::int64_t materialCount = 0;
    std::int64_t diagnosticCount = 0;
    std::shared_ptr<ManualDriveRuntime> manualRuntime;
    std::shared_ptr<const RaceCameraResources> cameraResources;
    PhysicsBackend backend = PhysicsBackend::OptimizedCpu;
};

struct RaceViewerInputPreviewResult {
    QString error;
    std::shared_ptr<ManualDriveRuntime> runtime;
    std::vector<RaceViewerFrame> frames;
    std::vector<SandboxInputEvent> inputs;
    RaceViewerMeshBuffers mesh;
    bool canceled = false;
};

class RaceViewerController final : public QObject {
    Q_OBJECT

    Q_PROPERTY(QQuick3DGeometry *trackFilledGeometry READ
                       trackFilledGeometry CONSTANT)
    Q_PROPERTY(QQuick3DGeometry *trackWireGeometry READ
                       trackWireGeometry CONSTANT)
    Q_PROPERTY(QQuick3DGeometry *ellipsoidFilledGeometry READ
                       ellipsoidFilledGeometry CONSTANT)
    Q_PROPERTY(QQuick3DGeometry *selectedEllipsoidFilledGeometry READ
                       selectedEllipsoidFilledGeometry NOTIFY selectedRunChanged)
    Q_PROPERTY(QVariantList ellipsoidFilledGeometries READ
                       ellipsoidFilledGeometries CONSTANT)
    Q_PROPERTY(QQuick3DGeometry *ellipsoidWireGeometry READ
                       ellipsoidWireGeometry CONSTANT)
    Q_PROPERTY(QVariantList carEllipsoids READ carEllipsoids NOTIFY
                       sceneChanged)
    Q_PROPERTY(QVariantList visualInstances READ visualInstances NOTIFY
                       sceneChanged)
    Q_PROPERTY(
            QVariantList visualBatches READ visualBatches NOTIFY sceneChanged)
    Q_PROPERTY(QVariantList visualMaterials READ visualMaterials NOTIFY
                       sceneChanged)
    Q_PROPERTY(QVariantList trajectoryPaths READ trajectoryPaths NOTIFY
                       trajectoriesChanged)
    Q_PROPERTY(qint64 trajectoryCount READ trajectoryCount NOTIFY
                       trajectoriesChanged)
    Q_PROPERTY(QString previewInputScript READ previewInputScript WRITE
                       setPreviewInputScript NOTIFY previewInputScriptChanged)
    Q_PROPERTY(qint64 simulationHorizonMs READ simulationHorizonMs WRITE
                       setSimulationHorizonMs NOTIFY simulationHorizonMsChanged)
    Q_PROPERTY(QVariantList runOptions READ runOptions NOTIFY runsChanged)
    Q_PROPERTY(QVariantList runPoses READ runPoses NOTIFY poseChanged)
    Q_PROPERTY(qint64 runCount READ runCount NOTIFY runsChanged)
    Q_PROPERTY(QString selectedRunId READ selectedRunId WRITE setSelectedRunId
                       NOTIFY selectedRunChanged)
    Q_PROPERTY(int selectedRunIndex READ selectedRunIndex NOTIFY
                       selectedRunChanged)
    Q_PROPERTY(QVector3D carPosition READ carPosition NOTIFY poseChanged)
    Q_PROPERTY(QQuaternion carRotation READ carRotation NOTIFY poseChanged)
    Q_PROPERTY(int cameraPreset READ cameraPreset WRITE setCameraPreset NOTIFY
                       cameraPresetChanged)
    Q_PROPERTY(bool carCameraAvailable READ carCameraAvailable NOTIFY
                       cameraChanged)
    Q_PROPERTY(QVector3D carCameraPosition READ carCameraPosition NOTIFY
                       cameraChanged)
    Q_PROPERTY(QQuaternion carCameraRotation READ carCameraRotation NOTIFY
                       cameraChanged)
    Q_PROPERTY(QVector3D carCameraTarget READ carCameraTarget NOTIFY
                       cameraChanged)
    Q_PROPERTY(double carCameraFieldOfView READ carCameraFieldOfView NOTIFY
                       cameraChanged)
    Q_PROPERTY(bool hideSelectedCar READ hideSelectedCar NOTIFY cameraChanged)
    Q_PROPERTY(QString telemetryScript READ telemetryScript WRITE
                       setTelemetryScript NOTIFY telemetryScriptChanged)
    Q_PROPERTY(QString defaultTelemetryScript READ defaultTelemetryScript
                       CONSTANT)
    Q_PROPERTY(qint64 durationMs READ durationMs NOTIFY timelineChanged)
    Q_PROPERTY(qint64 timeMs READ timeMs WRITE setTimeMs NOTIFY timeChanged)
    Q_PROPERTY(qint64 currentTick READ currentTick WRITE setCurrentTick NOTIFY
                       timeChanged)
    Q_PROPERTY(qint64 tickCount READ tickCount NOTIFY timelineChanged)
    Q_PROPERTY(int tickDurationMs READ tickDurationMs CONSTANT)
    Q_PROPERTY(QString timeText READ timeText NOTIFY timeChanged)
    Q_PROPERTY(QVariantList checkpointSplits READ checkpointSplits NOTIFY
                       timeChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playbackChanged)
    Q_PROPERTY(bool takeOverOnInput READ takeOverOnInput WRITE
                       setTakeOverOnInput NOTIFY takeOverOnInputChanged)
    Q_PROPERTY(bool manualDriving READ manualDriving NOTIFY
                       manualDrivingChanged)
    Q_PROPERTY(bool manualSteeringTakenOver READ manualSteeringTakenOver NOTIFY
                       manualInputChanged)
    Q_PROPERTY(bool manualLongitudinalTakenOver READ
                       manualLongitudinalTakenOver NOTIFY manualInputChanged)
    Q_PROPERTY(bool manualLeft READ manualLeft NOTIFY manualInputChanged)
    Q_PROPERTY(bool manualRight READ manualRight NOTIFY manualInputChanged)
    Q_PROPERTY(bool manualAccelerate READ manualAccelerate NOTIFY
                       manualInputChanged)
    Q_PROPERTY(bool manualBrake READ manualBrake NOTIFY manualInputChanged)
    Q_PROPERTY(bool canCopyCurrentInputs READ canCopyCurrentInputs NOTIFY
                       timelineChanged)
    Q_PROPERTY(bool loaded READ loaded NOTIFY stateChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(qint64 triangleCount READ triangleCount NOTIFY sceneChanged)
    Q_PROPERTY(qint64 visualTriangleCount READ visualTriangleCount NOTIFY
                       sceneChanged)
    Q_PROPERTY(qint64 visualMeshCount READ visualMeshCount NOTIFY
                       sceneChanged)
    Q_PROPERTY(
            qint64 visualBatchCount READ visualBatchCount NOTIFY sceneChanged)
    Q_PROPERTY(qint64 sourceVisualObjectCount READ sourceVisualObjectCount
                       NOTIFY sceneChanged)
    Q_PROPERTY(qint64 shadowCount READ shadowCount NOTIFY sceneChanged)
    Q_PROPERTY(qint64 materialCount READ materialCount NOTIFY sceneChanged)
    Q_PROPERTY(qint64 diagnosticCount READ diagnosticCount NOTIFY
                       sceneChanged)
    Q_PROPERTY(qint64 ellipsoidCount READ ellipsoidCount NOTIFY sceneChanged)
    Q_PROPERTY(double sceneRadius READ sceneRadius NOTIFY sceneChanged)
    Q_PROPERTY(QVector3D sceneBoundsMin READ sceneBoundsMin NOTIFY sceneChanged)
    Q_PROPERTY(QVector3D sceneBoundsMax READ sceneBoundsMax NOTIFY sceneChanged)
    Q_PROPERTY(forevertas::viewer::WhiteboardModel *whiteboard READ
                       whiteboard CONSTANT)
    Q_PROPERTY(forevertas::viewer::SimulationDebuggerModel *simulationDebugger READ
                       simulationDebugger CONSTANT)

public:
    explicit RaceViewerController(QObject *parent = nullptr);
    ~RaceViewerController() override;

    QQuick3DGeometry *trackFilledGeometry();
    QQuick3DGeometry *trackWireGeometry();
    QQuick3DGeometry *ellipsoidFilledGeometry();
    QQuick3DGeometry *selectedEllipsoidFilledGeometry();
    QVariantList ellipsoidFilledGeometries() const;
    QQuick3DGeometry *ellipsoidWireGeometry();
    WhiteboardModel *whiteboard();
    SimulationDebuggerModel *simulationDebugger();
    QVariantList carEllipsoids() const;
    QVariantList visualInstances() const;
    QVariantList visualBatches() const;
    QVariantList visualMaterials() const;
    QVariantList trajectoryPaths() const;
    qint64 trajectoryCount() const;
    QString previewInputScript() const;
    qint64 simulationHorizonMs() const;
    QVariantList runOptions() const;
    QVariantList runPoses() const;
    qint64 runCount() const;
    QString selectedRunId() const;
    int selectedRunIndex() const;
    QVector3D carPosition() const;
    QQuaternion carRotation() const;
    int cameraPreset() const;
    bool carCameraAvailable() const;
    QVector3D carCameraPosition() const;
    QQuaternion carCameraRotation() const;
    QVector3D carCameraTarget() const;
    double carCameraFieldOfView() const;
    bool hideSelectedCar() const;
    QString telemetryScript() const;
    QString defaultTelemetryScript() const;
    qint64 durationMs() const;
    qint64 timelineSeekLimitMs() const;
    qint64 timeMs() const;
    qint64 currentTick() const;
    qint64 tickCount() const;
    int tickDurationMs() const;
    QString timeText() const;
    QVariantList checkpointSplits() const;
    bool playing() const;
    bool takeOverOnInput() const;
    bool manualDriving() const;
    bool manualSteeringTakenOver() const;
    bool manualLongitudinalTakenOver() const;
    bool manualLeft() const;
    bool manualRight() const;
    bool manualAccelerate() const;
    bool manualBrake() const;
    bool canCopyCurrentInputs() const;
    bool loaded() const;
    bool loading() const;
    QString statusText() const;
    qint64 triangleCount() const;
    qint64 visualTriangleCount() const;
    qint64 visualMeshCount() const;
    qint64 visualBatchCount() const;
    qint64 sourceVisualObjectCount() const;
    qint64 shadowCount() const;
    qint64 materialCount() const;
    qint64 diagnosticCount() const;
    qint64 ellipsoidCount() const;
    double sceneRadius() const;
    QVector3D sceneBoundsMin() const;
    QVector3D sceneBoundsMax() const;
    std::shared_ptr<const RayTracingSceneData> rayTracingScene() const;
    RaceViewerInputSample inputSample(qint64 tick) const noexcept;
    void addSearchRun(
            const QString &packsDirectory,
            const QString &replayPath,
            const std::vector<SearchTimelineFrame> &frames);
    void addSearchRun(
            const QString &packsDirectory,
            const QString &replayPath,
            const std::vector<SearchTimelineFrame> &frames,
            const QString &backendId);
    void addSearchRun(
            const QString &packsDirectory,
            const QString &replayPath,
            const std::vector<SearchTimelineFrame> &frames,
            const std::vector<SandboxInputEvent> &inputs);
    void addSearchRun(
            const QString &packsDirectory,
            const QString &replayPath,
            const std::vector<SearchTimelineFrame> &frames,
            const std::vector<SandboxInputEvent> &inputs,
            const QString &backendId);
    void addSearchImprovement(
            const QString &packsDirectory,
            const QString &replayPath,
            const std::vector<SearchTimelineFrame> &frames,
            const QString &backendId,
            std::uint64_t searchId,
            std::uint64_t improvementNumber);

public slots:
    void setTimeMs(qint64 value);
    void setCurrentTick(qint64 tick);
    void setSelectedRunId(const QString &value);
    void setPreviewInputScript(const QString &value);
    void setSimulationHorizonMs(qint64 value);
    void setTakeOverOnInput(bool value);
    void setCameraPreset(int value);
    void setTelemetryScript(const QString &value);
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void togglePlayback();
    Q_INVOKABLE void jumpToStart();
    Q_INVOKABLE void jumpToEnd();
    Q_INVOKABLE void startManualDrive();
    Q_INVOKABLE void stopManualDrive();
    Q_INVOKABLE bool giveUpManualDrive();
    Q_INVOKABLE bool respawnManualDrive();
    Q_INVOKABLE bool startSimulationDebugger();
    Q_INVOKABLE void stopSimulationDebugger();
    Q_INVOKABLE void setManualInput(const QString &input, bool active);
    Q_INVOKABLE void releaseManualInputs();
    Q_INVOKABLE QString currentInputScript() const;
    Q_INVOKABLE bool hasTrajectoryForRun(const QString &runId) const;
    Q_INVOKABLE bool trajectoryVisibleForRun(const QString &runId) const;
    Q_INVOKABLE void setTrajectoryVisibleForRun(const QString &runId,
                                                bool visible);
    Q_INVOKABLE bool hasPreviewTrajectories() const;
    Q_INVOKABLE void clearPreviewTrajectories();
    Q_INVOKABLE void loadMap(const QString &packsDirectory,
                            const QString &replayPath);
    Q_INVOKABLE void loadMap(const QString &packsDirectory,
                            const QString &replayPath,
                            const QString &backendId);
    Q_INVOKABLE QVector2D cameraClipPlanes(const QVector3D &cameraPosition,
                                           double cameraDistance) const;
    Q_INVOKABLE QString renderTelemetry(
            const QString &script,
            const QVector3D &cameraPosition) const;
    Q_INVOKABLE QString telemetryScriptError(const QString &script) const;

signals:
    void sceneChanged();
    void poseChanged();
    void timelineChanged();
    void timeChanged();
    void playbackChanged();
    void takeOverOnInputChanged();
    void manualDrivingChanged();
    void manualInputChanged();
    void stateChanged();
    void runsChanged();
    void selectedRunChanged();
    void trajectoriesChanged();
    void previewInputScriptChanged();
    void simulationHorizonMsChanged();
    void cameraPresetChanged();
    void cameraChanged();
    void telemetryScriptChanged();

private:
    void applyLoadResult(std::uint64_t loadSerial,
                         RaceViewerLoadResult result);
    void beginMapLoad(const QString &packsDirectory,
                      const QString &replayPath,
                      PhysicsBackend backend);
    void applyPendingRunIfReady();
    bool applyPendingImprovementsIfReady();
    bool appendImprovementTrajectory(
            std::uint64_t searchId,
            std::uint64_t improvementNumber,
            const std::vector<RaceViewerFrame> &frames);
    void updateBestTrajectory(
            const QString &name,
            const std::vector<RaceViewerFrame> &frames);
    void scheduleInputPreviewRebuild();
    void scheduleStoredRunRebuilds();
    void startStoredRunRebuilds();
    void cancelStoredRunRebuilds();
    void waitForStoredRunWorker();
    void startInputPreviewBuild();
    void applyInputPreviewResult(
            std::uint64_t previewSerial,
            std::uint64_t loadSerial,
            RaceViewerInputPreviewResult result);
    void cancelInputPreviewBuild();
    void waitForInputPreviewWorker();
    void clearInputPreview();
    bool beginManualTakeover(const QString &input, bool active);
    bool applyManualInput(const QString &input, bool active);
    std::vector<SandboxInputEvent> inputHistoryForRun(
            const RaceViewerRun &run) const;
    std::vector<SandboxInputEvent> effectiveManualInputs() const;
    void resetManualTakeoverState();
    void upsertRun(QString id,
                   QString name,
                   std::vector<RaceViewerFrame> frames,
                   std::vector<SandboxInputEvent> inputs,
                   bool select,
                   std::shared_ptr<ManualDriveRuntime> runtime = nullptr);
    const RaceViewerRun *selectedRun() const noexcept;
    RaceViewerRun *selectedRun() noexcept;
    QQuick3DGeometry *ellipsoidFilledGeometryForRun(int runIndex);
    void refreshSelectedRun();
    void setLoading(bool value);
    void setStatusText(const QString &value);
    void waitForWorker();
    void updatePose();
    void updateCarCamera();
    void advancePlayback();
    void advanceManualDrive();
    void appendSimulationDebuggerFrame(const QVariantMap &frame);
    void setPlaying(bool value);
    bool resetManualDriveSession(const QString &status,
                                 bool preserveHeldInputs = false);
    void finishManualDrive(const QString &status, bool releaseInputs);
    bool replaceManualInputs();
    void appendHeldManualInputs(std::int32_t timeMs);
    void resetManualInputState();

    RaceGeometry trackFilledGeometry_;
    RaceGeometry trackWireGeometry_;
    WhiteboardModel whiteboard_;
    std::vector<std::unique_ptr<RaceGeometry>> visualGeometries_;
    std::shared_ptr<const RayTracingSceneData> rayTracingScene_;
    std::vector<std::unique_ptr<RaceGeometry>>
            ellipsoidFilledGeometries_;
    RaceGeometry ellipsoidWireGeometry_;
    struct MapLoadRequest {
        QString packsDirectory;
        QString replayPath;
        PhysicsBackend backend = PhysicsBackend::OptimizedCpu;
    };
    struct PendingRun {
        QString packsDirectory;
        QString replayPath;
        PhysicsBackend backend = PhysicsBackend::OptimizedCpu;
        std::vector<RaceViewerFrame> frames;
        std::vector<SandboxInputEvent> inputs;
    };
    struct PendingImprovement {
        QString packsDirectory;
        QString replayPath;
        PhysicsBackend backend = PhysicsBackend::OptimizedCpu;
        std::uint64_t searchId = 0u;
        std::uint64_t improvementNumber = 0u;
        std::vector<RaceViewerFrame> frames;
    };

    std::vector<RaceViewerRun> runs_;
    std::optional<MapLoadRequest> queuedMapLoad_;
    std::optional<PendingRun> pendingRun_;
    std::vector<PendingImprovement> pendingImprovements_;
    QVariantList carEllipsoids_;
    QVariantList visualBatches_;
    QVariantList visualMaterials_;
    QVariantList trajectoryPaths_;
    RaceGeometry inputPreviewGeometry_;
    RaceGeometry bestTrajectoryGeometry_;
    std::vector<std::unique_ptr<RaceGeometry>> trajectoryGeometries_;
    std::vector<QString> trajectoryKeys_;
    QVector3D carPosition_{};
    QQuaternion carRotation_{};
    QVector3D carCameraPosition_{};
    QQuaternion carCameraRotation_{};
    QVector3D carCameraTarget_{};
    double carCameraFieldOfView_ = 75.0;
    int cameraPreset_ = 1;
    bool carCameraAvailable_ = false;
    QString telemetryScript_;
    QString statusText_ = QStringLiteral("No map loaded");
    QString selectedRunId_;
    QString loadedPacksDirectory_;
    QString loadedReplayPath_;
    QString previewInputScript_;
    qint64 simulationHorizonMs_ = 6000;
    std::vector<SandboxInputEvent> takeoverSourceInputs_;
    QString takeoverSourceRunId_;
    std::optional<std::int32_t> steeringTakeoverTimeMs_;
    std::optional<std::int32_t> longitudinalTakeoverTimeMs_;
    qint64 durationMs_ = 0;
    qint64 timeMs_ = 0;
    qint64 triangleCount_ = 0;
    qint64 visualTriangleCount_ = 0;
    qint64 sourceVisualObjectCount_ = 0;
    qint64 sourceVisualMeshCount_ = 0;
    qint64 duplicateVisualObjectCount_ = 0;
    qint64 materialCount_ = 0;
    qint64 diagnosticCount_ = 0;
    double sceneRadius_ = 1.0;
    QVector3D sceneBoundsMin_{};
    QVector3D sceneBoundsMax_{};
    qint64 playbackStartTick_ = 0;
    qint64 manualDriveStartTick_ = 0;
    bool loaded_ = false;
    bool loading_ = false;
    bool playing_ = false;
    bool takeOverOnInput_ = false;
    bool manualDriving_ = false;
    bool manualTakeover_ = false;
    bool inputPreviewVisible_ = false;
    bool manualLeft_ = false;
    bool manualRight_ = false;
    bool manualAccelerate_ = false;
    bool manualBrake_ = false;
    QTimer playbackTimer_;
    QElapsedTimer playbackClock_;
    QTimer manualDriveTimer_;
    QElapsedTimer manualDriveClock_;
    SimulationDebuggerModel simulationDebugger_;
    std::shared_ptr<ManualDriveRuntime> manualRuntime_;
    std::shared_ptr<const RaceCameraResources> cameraResources_;
    std::unique_ptr<RaceCameraRuntime> cameraRuntime_;
    std::shared_ptr<ManualDriveRuntime> inputPreviewRuntime_;
    QThread *workerThread_ = nullptr;
    QThread *inputPreviewThread_ = nullptr;
    QThread *storedRunThread_ = nullptr;
    std::uint64_t loadSerial_ = 0u;
    std::uint64_t inputPreviewSerial_ = 0u;
    std::uint64_t storedRunSerial_ = 0u;
    PhysicsBackend loadedBackend_ = PhysicsBackend::OptimizedCpu;
    bool inputPreviewBuildPending_ = false;
    bool storedRunBuildPending_ = false;
};

}  // namespace forevertas::viewer

#endif
