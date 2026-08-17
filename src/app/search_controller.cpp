#include "app/search_controller.h"

#include "app/compact_number_format.h"
#include "app/packs_directory_finder.h"
#include "app/search_worker.h"
#include "app/system_file_dialog.h"
#include "mutations/input_event_formatter.h"
#include "mutations/replay_input_script.h"
#include "searches/algorithm_registry.h"

#include <forevervalidator/validation.h>

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QPalette>
#include <QRandomGenerator>
#include <QSettings>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace forevertas::app {
namespace {

constexpr char kPacksDirectoryKey[] = "paths/packsDirectory";
constexpr char kReplayPathKey[] = "paths/replayPath";
constexpr char kBaseInputScriptKey[] = "inputs/baseScript";
constexpr char kSimulationBackendKey[] = "selection/simulationBackend";
constexpr char kSimulationHorizonKey[] = "search/simulationHorizonMs";
constexpr char kConditionScriptKey[] = "search/conditionScript";
constexpr char kCpuWorkerCountKey[] = "backends/cpu/workerCount";
constexpr char kCudaParallelSampleCountKey[] =
        "backends/cuda/parallelSampleCount";
constexpr char kCudaCalibrationEnabledKey[] =
        "backends/cuda/calibrationEnabled";
constexpr char kCudaSessionSpecializationEnabledKey[] =
        "backends/cuda/sessionSpecializationEnabled";
constexpr char kRandomizeSeedsOnStartKey[] =
        "search/randomizeSeedsOnStart";
constexpr char kDrawTargetsThroughBlocksKey[] =
        "viewer/drawTargetsThroughBlocks";
constexpr char kRenderModeKey[] = "viewer/renderMode";
constexpr char kDarkModeKey[] = "appearance/darkMode";
std::atomic_bool gAutomaticPacksSearchScheduled{false};

void ApplyApplicationPalette(bool dark) {
    auto *const application =
            qobject_cast<QApplication *>(QCoreApplication::instance());
    if (application == nullptr) {
        return;
    }

    QPalette palette;
    const QColor window =
            dark ? QColor(QStringLiteral("#171a18"))
                 : QColor(QStringLiteral("#eceeeb"));
    const QColor surface =
            dark ? QColor(QStringLiteral("#292e2a")) : Qt::white;
    const QColor alternate =
            dark ? QColor(QStringLiteral("#242925"))
                 : QColor(QStringLiteral("#f3f5f1"));
    const QColor control =
            dark ? QColor(QStringLiteral("#343a35"))
                 : QColor(QStringLiteral("#e1e5df"));
    const QColor text =
            dark ? QColor(QStringLiteral("#f0f3ef"))
                 : QColor(QStringLiteral("#202421"));
    const QColor muted =
            dark ? QColor(QStringLiteral("#aeb8b0"))
                 : QColor(QStringLiteral("#667064"));
    const QColor accent =
            dark ? QColor(QStringLiteral("#45b778"))
                 : QColor(QStringLiteral("#26734d"));
    const QColor accentText =
            dark ? QColor(QStringLiteral("#101411")) : Qt::white;
    const QColor disabledSurface =
            dark ? QColor(QStringLiteral("#252925"))
                 : QColor(QStringLiteral("#ecefe9"));
    const QColor disabledText =
            dark ? QColor(QStringLiteral("#737b74"))
                 : QColor(QStringLiteral("#92988f"));
    const QColor tooltip =
            dark ? QColor(QStringLiteral("#f0f3ef"))
                 : QColor(QStringLiteral("#202421"));
    const QColor tooltipText =
            dark ? QColor(QStringLiteral("#202421")) : Qt::white;

    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, surface);
    palette.setColor(QPalette::AlternateBase, alternate);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Button, control);
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::BrightText, text);
    palette.setColor(QPalette::Highlight, accent);
    palette.setColor(QPalette::HighlightedText, accentText);
    palette.setColor(QPalette::ToolTipBase, tooltip);
    palette.setColor(QPalette::ToolTipText, tooltipText);
    palette.setColor(QPalette::PlaceholderText, muted);
    palette.setColor(QPalette::Disabled, QPalette::WindowText, disabledText);
    palette.setColor(QPalette::Disabled, QPalette::Text, disabledText);
    palette.setColor(QPalette::Disabled, QPalette::Button, disabledSurface);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, disabledText);
    palette.setColor(
            QPalette::Disabled, QPalette::HighlightedText, disabledText);
    application->setPalette(palette);
}

struct ReplayInputExtractionResult {
    QString script;
    QString error;
};

ReplayInputExtractionResult ExtractReplayInputScript(
        const QString &packsDirectory,
        const QString &replayPath) {
    ReplayInputExtractionResult result;
    try {
        result.script = QString::fromStdString(
                forevertas::ExtractReplayInputScript(
                        packsDirectory.toUtf8().toStdString(),
                        replayPath.toUtf8().toStdString()));
    } catch (const std::exception &exception) {
        result.error = QString::fromUtf8(exception.what());
    } catch (...) {
        result.error =
                QStringLiteral("Unexpected replay input extraction failure");
    }
    return result;
}

QString StoredValue(const char *key, const QString &fallback) {
    return QSettings().value(QLatin1String(key), fallback).toString();
}

QString BackendId(PhysicsBackend backend) {
    const std::string_view id = PhysicsBackendId(backend);
    return QString::fromLatin1(id.data(), static_cast<qsizetype>(id.size()));
}

}  // namespace

SearchController::SearchController(QObject *parent)
    : QObject(parent),
      cuboidTargets_(configuration_.legacyEvaluationSettings(
              QString::fromLatin1(kVolumeEntryEvaluationId))),
      customVolumeTargets_(configuration_.legacyEvaluationSettings(
              QString::fromLatin1(kCustomVolumeEntryEvaluationId))),
      poseTargets_(configuration_.legacyEvaluationSettings(
              QString::fromLatin1(kPoseTargetEvaluationId))) {
    initialize(nullptr);
}

SearchController::SearchController(const QStringList &packsSearchPatterns,
                                   QObject *parent)
    : QObject(parent),
      cuboidTargets_(configuration_.legacyEvaluationSettings(
              QString::fromLatin1(kVolumeEntryEvaluationId))),
      customVolumeTargets_(configuration_.legacyEvaluationSettings(
              QString::fromLatin1(kCustomVolumeEntryEvaluationId))),
      poseTargets_(configuration_.legacyEvaluationSettings(
              QString::fromLatin1(kPoseTargetEvaluationId))) {
    initialize(&packsSearchPatterns);
}

void SearchController::initialize(const QStringList *packsSearchPatterns) {
    qRegisterMetaType<SearchCompletionPtr>();
    qRegisterMetaType<SearchImprovementPtr>();
    connect(&configuration_, &BlockProgramModel::structureChanged, this,
            &SearchController::publishConfigurationChange);
    connect(&configuration_, &BlockProgramModel::blockChanged, this,
            [this](int blockId) {
                emit blockUpdated(blockId);
                // A failed text apply's error is stale once the user
                // edits the program structurally; programTextChanged
                // also notifies programTextError.
                if (!programTextError_.isEmpty()) {
                    programTextError_.clear();
                }
                emit programTextChanged();
                refreshValidation();
            });
    // Target collections feed the block program's mirrored fields at
    // compile time, so selection changes only need revalidation.
    connect(&cuboidTargets_,
            &CuboidTargetModel::selectedTargetChanged,
            this,
            &SearchController::refreshValidation);
    connect(&customVolumeTargets_,
            &CustomVolumeTargetModel::selectedTargetChanged,
            this,
            &SearchController::refreshValidation);
    connect(&customVolumeTargets_,
            &CustomVolumeTargetModel::drawingChanged,
            this,
            &SearchController::customVolumeDrawingChanged);
    connect(&poseTargets_,
            &PoseTargetModel::selectedTargetChanged,
            this,
            &SearchController::refreshValidation);
    packsDirectory_ = StoredValue(kPacksDirectoryKey, {});
    replayPath_ = StoredValue(kReplayPathKey, {});
    baseInputScript_ = StoredValue(kBaseInputScriptKey, {});
    InputScriptParseResult parsed =
            ParseInputScript(baseInputScript_.toStdString());
    parsedBaseInputCommands_ = std::move(parsed.commands);
    if (parsed.error) {
        baseInputScriptError_ = QString::fromStdString(*parsed.error);
    }
    inputScriptPersistTimer_ = new QTimer(this);
    inputScriptPersistTimer_->setSingleShot(true);
    inputScriptPersistTimer_->setInterval(350);
    connect(inputScriptPersistTimer_, &QTimer::timeout, this, [this]() {
        persist(kBaseInputScriptKey, baseInputScript_);
    });
    cudaParallelSampleCount_ = StoredValue(
            kCudaParallelSampleCountKey,
            QString::number(kDefaultCudaParallelSampleCount));
    simulationHorizonMs_ = StoredValue(
            kSimulationHorizonKey,
            QString::number(kDefaultSimulationHorizonMs));
    conditionScript_ = StoredValue(kConditionScriptKey, {});
    if (!QSettings().contains(QLatin1String(kSimulationHorizonKey))) {
        QSettings().setValue(
                QLatin1String(kSimulationHorizonKey),
                simulationHorizonMs_);
    }
    cpuWorkerCount_ = StoredValue(
            kCpuWorkerCountKey,
            QString::number(DefaultCpuWorkerCount()));
    cudaCalibrationEnabled_ = QSettings()
            .value(QLatin1String(kCudaCalibrationEnabledKey), false)
            .toBool();
    cudaSessionSpecializationEnabled_ = QSettings()
            .value(QLatin1String(
                           kCudaSessionSpecializationEnabledKey),
                   true)
            .toBool();
#if FOREVERVALIDATOR_HAS_CUDA
    const forevervalidator::CudaBackendDiagnostics cuda =
            forevervalidator::QueryCudaBackendDiagnostics();
    cudaAvailable_ = cuda.IsReady();
    cudaFastModeAvailable_ = cuda.SupportsSessionSpecialization();
    if (cuda.IsReady()) {
        const QString device = QString::fromStdString(cuda.deviceName);
        const QString capability =
                QStringLiteral("%1.%2")
                        .arg(cuda.computeCapabilityMajor)
                        .arg(cuda.computeCapabilityMinor);
        cudaStatusText_ = cudaFastModeAvailable_
                ? QStringLiteral(
                          "CUDA ready: %1 (compute capability %2). Fast CUDA "
                          "is available.")
                          .arg(device, capability)
                : QStringLiteral(
                          "CUDA ready: %1 (compute capability %2). Fast CUDA "
                          "requires compute capability 7.5 or newer, so "
                          "regular CUDA will be used.")
                          .arg(device, capability);
    } else {
        cudaStatusText_ = QStringLiteral("CUDA unavailable: %1")
                                  .arg(QString::fromStdString(cuda.diagnostic));
    }
#else
    cudaStatusText_ =
            QStringLiteral("CUDA support is not compiled into this build.");
#endif
    QSettings settings;
    randomizeSeedsOnStart_ = settings
            .value(QLatin1String(kRandomizeSeedsOnStartKey), true)
            .toBool();
    if (!settings.contains(QLatin1String(kRandomizeSeedsOnStartKey))) {
        settings.setValue(
                QLatin1String(kRandomizeSeedsOnStartKey), true);
    }
    drawTargetsThroughBlocks_ = settings
            .value(QLatin1String(kDrawTargetsThroughBlocksKey), false)
            .toBool();
    darkMode_ =
            QSettings().value(QLatin1String(kDarkModeKey), false).toBool();
    renderMode_ = QSettings()
            .value(QLatin1String(kRenderModeKey),
                   QStringLiteral("textured"))
            .toString();
    ApplyApplicationPalette(darkMode_);
    const QString storedBackend = StoredValue(
            kSimulationBackendKey,
            BackendId(PhysicsBackend::Reference));
    const std::optional<PhysicsBackend> parsedBackend =
            ParsePhysicsBackend(storedBackend.toStdString());
    simulationBackend_ = parsedBackend.value_or(PhysicsBackend::Reference);
    if (!parsedBackend) {
        // A stored "cuda" selection on a build without CUDA stays in
        // storage so switching binaries does not lose the preference;
        // only genuinely unknown values are rewritten.
        const bool knownButUnavailable =
                storedBackend == QLatin1String("cuda");
        if (!knownButUnavailable) {
            QSettings().setValue(
                    QLatin1String(kSimulationBackendKey),
                    BackendId(simulationBackend_));
        }
    }
    scheduleAutoDetectPacksDirectory(packsSearchPatterns);
    refreshValidation();
}

SearchController::~SearchController() {
    if (inputScriptPersistTimer_ != nullptr) {
        inputScriptPersistTimer_->stop();
    }
    persist(kBaseInputScriptKey, baseInputScript_);
    QSettings().sync();
    waitForWorker();
}

QString SearchController::packsDirectory() const {
    return packsDirectory_;
}

QString SearchController::autoDetectedPacksDirectory() const {
    return autoDetectedPacksDirectory_;
}

QString SearchController::replayPath() const {
    return replayPath_;
}

QString SearchController::baseInputScript() const {
    return baseInputScript_;
}

QString SearchController::baseInputScriptError() const {
    return baseInputScriptError_;
}

bool SearchController::canUndoBaseInputScript() const {
    return !baseInputScriptUndoHistory_.empty();
}

bool SearchController::extractingReplayInputs() const {
    return extractingReplayInputs_;
}

bool SearchController::canExtractReplayInputs() const {
    const QFileInfo packsInfo(packsDirectory_);
    const QFileInfo replayInfo(replayPath_);
    return !running_ && !extractingReplayInputs_ &&
            packsInfo.isDir() && packsInfo.isReadable() &&
            replayInfo.isFile() && replayInfo.isReadable() &&
            replayInfo.fileName().endsWith(
                    QStringLiteral(".Replay.Gbx"),
                    Qt::CaseInsensitive);
}

QString SearchController::replayInputStatusText() const {
    return replayInputStatusText_;
}

QVariantList SearchController::simulationBackendOptions() const {
    QVariantList options{
            QVariantMap{
                    {QStringLiteral("id"),
                     BackendId(PhysicsBackend::Reference)},
                    {QStringLiteral("label"), QStringLiteral("Reference")},
                    {QStringLiteral("description"),
                     QStringLiteral("Broadest compatibility")}},
            QVariantMap{
                    {QStringLiteral("id"),
                     BackendId(PhysicsBackend::OptimizedCpu)},
                    {QStringLiteral("label"),
                     QStringLiteral("CPU Optimized")},
                    {QStringLiteral("description"),
                     QStringLiteral(
                             "Faster runtime optimized for Stadium, may "
                             "break compatibility in other environments")}},
            QVariantMap{
                    {QStringLiteral("id"),
                     BackendId(PhysicsBackend::MultiThreadedCpu)},
                    {QStringLiteral("label"),
                     QStringLiteral("CPU Multi-threaded")},
                    {QStringLiteral("description"),
                     QStringLiteral(
                             "Runs independent optimized CPU simulations "
                             "across multiple worker threads")}},
    };
#if FOREVERVALIDATOR_HAS_CUDA
    options.push_back(QVariantMap{
            {QStringLiteral("id"),
             BackendId(PhysicsBackend::Cuda)},
            {QStringLiteral("label"), QStringLiteral("CUDA")},
            {QStringLiteral("description"),
             QStringLiteral(
                     "NVIDIA CUDA for Stadium; compute capability 5.0+ is "
                     "supported, with Fast CUDA on 7.5+")}});
#endif
    return options;
}

QString SearchController::simulationBackendId() const {
    return BackendId(simulationBackend_);
}

QString SearchController::simulationHorizonMs() const {
    return simulationHorizonMs_;
}

QString SearchController::conditionScript() const {
    return conditionScript_;
}

QString SearchController::cpuWorkerCount() const {
    return cpuWorkerCount_;
}

QString SearchController::cudaParallelSampleCount() const {
    return cudaParallelSampleCount_;
}

bool SearchController::cudaCalibrationEnabled() const {
    return cudaCalibrationEnabled_;
}

bool SearchController::cudaSessionSpecializationEnabled() const {
    return cudaSessionSpecializationEnabled_;
}

bool SearchController::cudaAvailable() const {
    return cudaAvailable_;
}

bool SearchController::cudaFastModeAvailable() const {
    return cudaFastModeAvailable_;
}

QString SearchController::cudaStatusText() const {
    return cudaStatusText_;
}

bool SearchController::randomizeSeedsOnStart() const {
    return randomizeSeedsOnStart_;
}

bool SearchController::drawTargetsThroughBlocks() const {
    return drawTargetsThroughBlocks_;
}

QString SearchController::renderMode() const {
    return renderMode_;
}

bool SearchController::darkMode() const {
    return darkMode_;
}

QVariantList SearchController::blockPalette() const {
    return configuration_.palette();
}

QVariantMap SearchController::blockScript() const {
    return configuration_.scriptSummary();
}

QString SearchController::programText() const {
    return configuration_.programText();
}

QString SearchController::programTextError() const {
    return programTextError_;
}

QString SearchController::evaluationTargetId() const {
    return configuration_.evaluationTargetId();
}

CuboidTargetModel *SearchController::cuboidTargets() {
    return &cuboidTargets_;
}

CustomVolumeTargetModel *SearchController::customVolumeTargets() {
    return &customVolumeTargets_;
}

bool SearchController::customVolumeDrawing() const {
    return customVolumeTargets_.drawing();
}

PoseTargetModel *SearchController::poseTargets() {
    return &poseTargets_;
}

bool SearchController::canStart() const {
    return valid_ && !running_ && !extractingReplayInputs_;
}

bool SearchController::running() const {
    return running_;
}

bool SearchController::stopping() const {
    return stopping_;
}

bool SearchController::progressIndeterminate() const {
    return progressIndeterminate_;
}

double SearchController::progressValue() const {
    return progressValue_;
}

QString SearchController::validationMessage() const {
    return validationMessage_;
}

QString SearchController::statusText() const {
    return statusText_;
}

bool SearchController::liveMetricsVisible() const {
    return liveMetricsVisible_;
}

QString SearchController::iterationCountText() const {
    return iterationCountText_;
}

QString SearchController::throughputText() const {
    return throughputText_;
}

QString SearchController::elapsedText() const {
    return elapsedText_;
}

QString SearchController::resultText() const {
    return resultText_;
}

QString SearchController::bestInputsText() const {
    return bestInputsText_;
}

void SearchController::setReplayPath(const QString &value) {
    if (replayPath_ == value) {
        return;
    }
    replayPath_ = value;
    persist(kReplayPathKey, value);
    emit replayPathChanged();
    emit replayInputStateChanged();
    refreshValidation();
}

void SearchController::setBaseInputScript(const QString &value) {
    applyBaseInputScript(value, true);
}

void SearchController::applyBaseInputScript(const QString &value,
                                            bool recordUndo) {
    if (baseInputScript_ == value) {
        return;
    }
    if (recordUndo) {
        constexpr std::size_t MaximumUndoEntries = 100u;
        if (baseInputScriptUndoHistory_.size() == MaximumUndoEntries) {
            baseInputScriptUndoHistory_.erase(
                    baseInputScriptUndoHistory_.begin());
        }
        baseInputScriptUndoHistory_.push_back(baseInputScript_);
    }
    baseInputScript_ = value;
    InputScriptParseResult parsed = ParseInputScript(value.toStdString());
    parsedBaseInputCommands_ = std::move(parsed.commands);
    baseInputScriptError_ = parsed.error
            ? QString::fromStdString(*parsed.error)
            : QString{};
    if (inputScriptPersistTimer_ != nullptr) {
        inputScriptPersistTimer_->start();
    }
    emit baseInputScriptChanged();
    refreshValidation();
}

bool SearchController::undoBaseInputScript() {
    if (baseInputScriptUndoHistory_.empty()) {
        return false;
    }
    QString previous = std::move(baseInputScriptUndoHistory_.back());
    baseInputScriptUndoHistory_.pop_back();
    applyBaseInputScript(previous, false);
    return true;
}

void SearchController::setSimulationBackendId(const QString &value) {
    const std::optional<PhysicsBackend> parsed =
            ParsePhysicsBackend(value.toStdString());
    if (!parsed || simulationBackend_ == *parsed) {
        return;
    }
    simulationBackend_ = *parsed;
    persist(kSimulationBackendKey, BackendId(simulationBackend_));
    emit simulationBackendIdChanged();
    refreshValidation();
}

void SearchController::setSimulationHorizonMs(const QString &value) {
    if (simulationHorizonMs_ == value) {
        return;
    }
    simulationHorizonMs_ = value;
    persist(kSimulationHorizonKey, value);
    emit simulationHorizonMsChanged();
    refreshValidation();
}

void SearchController::setConditionScript(const QString &value) {
    if (conditionScript_ == value) {
        return;
    }
    conditionScript_ = value;
    persist(kConditionScriptKey, value);
    emit conditionScriptChanged();
    refreshValidation();
}

void SearchController::setCpuWorkerCount(const QString &value) {
    if (cpuWorkerCount_ == value) {
        return;
    }
    cpuWorkerCount_ = value;
    persist(kCpuWorkerCountKey, value);
    emit cpuWorkerCountChanged();
    refreshValidation();
}

void SearchController::setCudaParallelSampleCount(const QString &value) {
    if (cudaParallelSampleCount_ == value) {
        return;
    }
    cudaParallelSampleCount_ = value;
    persist(kCudaParallelSampleCountKey, value);
    emit cudaParallelSampleCountChanged();
    refreshValidation();
}

void SearchController::setCudaCalibrationEnabled(bool value) {
    if (cudaCalibrationEnabled_ == value) {
        return;
    }
    cudaCalibrationEnabled_ = value;
    QSettings().setValue(
            QLatin1String(kCudaCalibrationEnabledKey), value);
    emit cudaCalibrationEnabledChanged();
    refreshValidation();
}

void SearchController::setCudaSessionSpecializationEnabled(bool value) {
    if (cudaSessionSpecializationEnabled_ == value) {
        return;
    }
    cudaSessionSpecializationEnabled_ = value;
    QSettings().setValue(
            QLatin1String(kCudaSessionSpecializationEnabledKey), value);
    emit cudaSessionSpecializationEnabledChanged();
}

void SearchController::setRandomizeSeedsOnStart(bool value) {
    if (randomizeSeedsOnStart_ == value) {
        return;
    }
    randomizeSeedsOnStart_ = value;
    QSettings().setValue(
            QLatin1String(kRandomizeSeedsOnStartKey), value);
    emit randomizeSeedsOnStartChanged();
}

void SearchController::setDrawTargetsThroughBlocks(bool value) {
    if (drawTargetsThroughBlocks_ == value) {
        return;
    }
    drawTargetsThroughBlocks_ = value;
    QSettings().setValue(
            QLatin1String(kDrawTargetsThroughBlocksKey), value);
    emit drawTargetsThroughBlocksChanged();
}

void SearchController::setDarkMode(bool value) {
    if (darkMode_ == value) {
        return;
    }
    darkMode_ = value;
    QSettings().setValue(QLatin1String(kDarkModeKey), value);
    ApplyApplicationPalette(value);
    emit darkModeChanged();
}

void SearchController::setRenderMode(const QString &value) {
    if (renderMode_ == value) {
        return;
    }
    renderMode_ = value;
    QSettings().setValue(QLatin1String(kRenderModeKey), value);
    emit renderModeChanged();
}

void SearchController::setEvaluationTargetId(const QString &value) {
    setEvaluatorBlock(QStringLiteral("evaluate/") + value);
}

QVariantMap SearchController::blockData(int blockId) const {
    return configuration_.blockData(blockId);
}

bool SearchController::addBlock(const QString &definitionId) {
    return configuration_.addBlock(definitionId);
}

bool SearchController::removeBlock(int blockId) {
    return configuration_.removeBlock(blockId);
}

bool SearchController::setBlockField(int blockId,
                                     const QString &key,
                                     const QString &value) {
    return configuration_.setBlockField(blockId, key, value);
}

int SearchController::attachReporter(int blockId,
                                     const QString &key,
                                     const QString &reporterDefinitionId) {
    return configuration_.attachReporter(
            blockId, key, reporterDefinitionId);
}

bool SearchController::detachReporter(int blockId, const QString &key) {
    return configuration_.detachReporter(blockId, key);
}

bool SearchController::moveBlock(int blockId, int toIndex) {
    return configuration_.moveBlock(blockId, toIndex);
}

bool SearchController::setBlockPosition(int blockId, double x, double y) {
    return configuration_.setBlockPosition(blockId, x, y);
}

QVariantList SearchController::blockCanvas() const {
    return configuration_.blockCanvas();
}

int SearchController::addLooseBlock(const QString &definitionId,
                                    double x,
                                    double y) {
    return configuration_.addLooseBlock(definitionId, x, y);
}

bool SearchController::attachBlock(int parentId, int index, int childId) {
    return configuration_.attachBlock(parentId, index, childId);
}

bool SearchController::detachBlockToCanvas(int blockId, double x, double y) {
    return configuration_.detachBlockToCanvas(blockId, x, y);
}

bool SearchController::graftReporterBlock(int blockId,
                                          const QString &key,
                                          int reporterId) {
    return configuration_.graftReporterBlock(blockId, key, reporterId);
}

bool SearchController::setEvaluatorBlockId(int blockId) {
    return configuration_.setEvaluatorBlockId(blockId);
}

bool SearchController::setEvaluatorBlock(const QString &definitionId) {
    return configuration_.setEvaluator(definitionId);
}

void SearchController::resetBlocks() {
    configuration_.resetToDefault();
}

bool SearchController::applyProgramText(const QString &text) {
    QString error;
    const bool applied = configuration_.setProgramText(text, &error);
    programTextError_ = applied ? QString() : error;
    emit programTextChanged();
    return applied;
}

void SearchController::focusSelectedCuboid() {
    const QVariantMap target = cuboidTargets_.selectedTarget();
    const QVariant center = target.value(QStringLiteral("center"));
    const QVariant size = target.value(QStringLiteral("size"));
    if (!center.canConvert<QVector3D>() || !size.canConvert<QVector3D>()) {
        return;
    }
    emit cuboidFocusRequested(
            center.value<QVector3D>(), size.value<QVector3D>());
}



void SearchController::focusSelectedCustomVolume() {
    const QVariantMap target = customVolumeTargets_.selectedTarget();
    const QVariant center = target.value(QStringLiteral("focusCenter"));
    const QVariant size = target.value(QStringLiteral("focusSize"));
    if (!center.canConvert<QVector3D>() || !size.canConvert<QVector3D>()) {
        return;
    }
    emit customVolumeFocusRequested(
            center.value<QVector3D>(), size.value<QVector3D>());
}

void SearchController::beginCustomVolumeDrawing() {
    if (configuration_.evaluationTargetId() ==
        QString::fromLatin1(kCustomVolumeEntryEvaluationId)) {
        customVolumeTargets_.beginDrawing();
    }
}

void SearchController::finishCustomVolumeDrawing() {
    customVolumeTargets_.finishDrawing();
}

void SearchController::cancelCustomVolumeDrawing() {
    customVolumeTargets_.cancelDrawing();
}



void SearchController::focusSelectedPoseTarget() {
    const QVariantMap target = poseTargets_.selectedTarget();
    const QVariant position = target.value(QStringLiteral("position"));
    if (!position.canConvert<QVector3D>()) {
        return;
    }
    emit poseTargetFocusRequested(position.value<QVector3D>(),
                                  QVector3D(4.0F, 2.5F, 7.0F));
}

void SearchController::setPacksDirectory(const QString &value) {
    if (packsDirectory_ == value) {
        return;
    }
    clearAutoDetectedPacksDirectory();
    packsDirectory_ = value;
    persist(kPacksDirectoryKey, value);
    emit packsDirectoryChanged();
    emit replayInputStateChanged();
    refreshValidation();
}

void SearchController::browseForPacksDirectory() {
    const QFileInfo current(packsDirectory_.isEmpty()
                                    ? autoDetectedPacksDirectory_
                                    : packsDirectory_);
    const QString initialDirectory = current.isDir()
            ? current.absoluteFilePath()
            : QDir::homePath();
    const QString selected = OpenSystemDirectoryDialog(
            QStringLiteral("Select Packs directory"), initialDirectory);
    if (!selected.isEmpty()) {
        setPacksDirectory(selected);
    }
}

void SearchController::applyAutoDetectedPacksDirectory() {
    if (autoDetectedPacksDirectory_.isEmpty()) {
        return;
    }
    const QString detected = autoDetectedPacksDirectory_;
    setPacksDirectory(detected);
}

void SearchController::browseForReplay() {
    const QFileInfo current(replayPath_);
    const QString initialPath = current.isFile()
            ? current.absoluteFilePath()
            : QDir::homePath();
    const QString selected = OpenSystemFileDialog(
            QStringLiteral("Select replay or challenge"), initialPath);
    if (!selected.isEmpty()) {
        setReplayPath(selected);
    }
}

QString SearchController::formatCompactNumber(double value) const {
    return FormatCompactNumber(value);
}

void SearchController::extractReplayInputs() {
    if (!canExtractReplayInputs() || inputExtractionThread_ != nullptr) {
        return;
    }
    const QString packsDirectory = QFileInfo(packsDirectory_)
            .absoluteFilePath();
    const QString replayPath = QFileInfo(replayPath_).absoluteFilePath();
    const QString scriptAtExtractionStart = baseInputScript_;
    setExtractingReplayInputs(true);
    setReplayInputStatusText(QStringLiteral("Extracting replay inputs..."));

    QThread *const thread = QThread::create(
            [this, packsDirectory, replayPath, scriptAtExtractionStart]() {
                ReplayInputExtractionResult result =
                        ExtractReplayInputScript(packsDirectory, replayPath);
                QMetaObject::invokeMethod(
                        this,
                        [this,
                         packsDirectory,
                         replayPath,
                         scriptAtExtractionStart,
                         result = std::move(result)]() mutable {
                            if (packsDirectory !=
                                        QFileInfo(packsDirectory_)
                                                .absoluteFilePath() ||
                                replayPath !=
                                        QFileInfo(replayPath_)
                                                .absoluteFilePath()) {
                                setReplayInputStatusText(QStringLiteral(
                                        "Replay selection changed; extracted "
                                        "inputs were discarded."),
                                                          true);
                            } else if (!result.error.isEmpty()) {
                                setReplayInputStatusText(
                                        QStringLiteral(
                                                "Input extraction failed: %1")
                                                .arg(result.error),
                                        true);
                            } else if (baseInputScript_ !=
                                       scriptAtExtractionStart) {
                                setReplayInputStatusText(QStringLiteral(
                                        "Base input script was edited while "
                                        "extracting; extracted inputs were "
                                        "discarded."),
                                                          true);
                            } else {
                                setBaseInputScript(result.script);
                                setReplayInputStatusText(
                                        QStringLiteral(
                                                "Replay inputs extracted"));
                            }
                            setExtractingReplayInputs(false);
                        },
                        Qt::QueuedConnection);
            });
    inputExtractionThread_ = thread;
    connect(thread, &QThread::finished, this, [this, thread]() {
        if (inputExtractionThread_ == thread) {
            inputExtractionThread_ = nullptr;
        }
        thread->deleteLater();
    });
    thread->start();
}

void SearchController::startSearch() {
    if (running_ || extractingReplayInputs_) {
        return;
    }

    ValidationResult validation = validate();
    if (!validation.request) {
        refreshValidation();
        return;
    }
    if (randomizeSeedsOnStart_ &&
        configuration_.randomizeSeeds(
                QRandomGenerator::system()->generate())) {
        validation = validate();
        if (!validation.request) {
            refreshValidation();
            return;
        }
    }

    setResultText({});
    setBestInputsText({});
    setLiveMetrics({}, {}, {}, false);
    lastCompletion_.reset();
    setProgress(true, 0.0);
    setStatusText(QStringLiteral("Starting search..."));
    setStopping(false);
    setRunning(true);

    stopRequested_ = std::make_shared<std::atomic_bool>(false);
    cancellationRequested_ = std::make_shared<std::atomic_bool>(false);
    iterationPhase_ = std::make_shared<std::atomic<SearchIterationPhase>>(
            SearchIterationPhase::Pending);
    QThread *const thread = new QThread(this);
    SearchWorker *const worker = new SearchWorker(
            *validation.request,
            ++searchSerial_,
            stopRequested_,
            cancellationRequested_,
            iterationPhase_);
    worker->moveToThread(thread);
    workerThread_ = thread;

    connect(thread, &QThread::started, worker, &SearchWorker::run);
    connect(worker,
            &SearchWorker::stageChanged,
            this,
            [this](const QString &status, bool indeterminate) {
                setStatusText(status);
                setProgress(indeterminate, progressValue_);
            });
    connect(worker,
            &SearchWorker::progressChanged,
            this,
            [this](double value, const QString &status) {
                setStatusText(status);
                setProgress(false, value);
            });
    connect(worker,
            &SearchWorker::metricsChanged,
            this,
            [this](const QString &iterationCountText,
                   const QString &throughputText,
                   const QString &elapsedText) {
                setLiveMetrics(iterationCountText,
                               throughputText,
                               elapsedText,
                               true);
            });
    connect(worker,
            &SearchWorker::cudaBatchSizeChanged,
            this,
            [this](std::uint32_t batchSize) {
                setCudaParallelSampleCount(
                        QString::number(batchSize));
            });
    connect(worker,
            &SearchWorker::bestChanged,
            this,
            [this](const QString &summary, const QString &inputsText) {
                setResultText(summary);
                setBestInputsText(inputsText);
            });
    connect(worker,
            &SearchWorker::improvementFound,
            this,
            [this](SearchImprovementPtr improvement) {
                emit searchImprovement(std::move(improvement));
            });
    connect(worker,
            &SearchWorker::succeeded,
            this,
            [this](SearchCompletionPtr completion) {
                lastCompletion_ = completion;
                setResultText(completion->summary);
                setBestInputsText(completion->inputsText);
                setProgress(false, 1.0);
                lastRunFailed_ = false;
                setStatusText(QStringLiteral("Search complete"));
                emit searchCompleted(std::move(completion));
            });
    connect(worker, &SearchWorker::cancelled, this, [this]() {
        lastRunFailed_ = false;
        setStatusText(QStringLiteral("Search aborted"));
        setProgress(false, progressValue_);
    });
    connect(worker,
            &SearchWorker::failed,
            this,
            [this](const QString &message) {
                setResultText(message);
                lastRunFailed_ = true;
                setStatusText(QStringLiteral("Search failed"));
                setProgress(false, progressValue_);
            });
    connect(worker, &SearchWorker::finished, thread, &QThread::quit);
    connect(worker, &SearchWorker::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, [this, thread]() {
        if (workerThread_ == thread) {
            workerThread_ = nullptr;
            stopRequested_.reset();
            cancellationRequested_.reset();
            iterationPhase_.reset();
            setStopping(false);
            setRunning(false);
        }
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);

    thread->start();
}

void SearchController::stopSearch() {
    if (!running_ || stopping_ || !stopRequested_) {
        return;
    }
    setStopping(true);
    if (iterationPhase_ != nullptr &&
        TryCancelBeforeSearchIteration(iterationPhase_) &&
        cancellationRequested_ != nullptr) {
        cancellationRequested_->store(true, std::memory_order_relaxed);
        setStatusText(QStringLiteral("Aborting search startup..."));
        return;
    }
    stopRequested_->store(true, std::memory_order_relaxed);
    setStatusText(QStringLiteral("Stopping after current iteration..."));
}

void SearchController::publishConfigurationChange() {
    const QString evaluationId = configuration_.evaluationTargetId();
    if (evaluationId != publishedEvaluationTargetId_) {
        publishedEvaluationTargetId_ = evaluationId;
        emit evaluationTargetIdChanged();
    }
    emit blockStructureChanged();
    emit programTextChanged();
    refreshValidation();
}

SearchController::ValidationResult SearchController::validate() const {
    const QFileInfo packsInfo(packsDirectory_);
    if (packsDirectory_.isEmpty()) {
        return {{}, QStringLiteral("Select a Packs directory.")};
    }
    if (!packsInfo.exists() || !packsInfo.isDir() ||
        !packsInfo.isReadable()) {
        return {{}, QStringLiteral(
                            "The Packs path must be a readable directory.")};
    }

    const QFileInfo replayInfo(replayPath_);
    if (replayPath_.isEmpty()) {
        return {{}, QStringLiteral("Select a replay or challenge file.")};
    }
    if (!replayInfo.exists() || !replayInfo.isFile() ||
        !replayInfo.isReadable()) {
        return {{}, QStringLiteral(
                            "The scenario path must be a readable file.")};
    }

    bool horizonParsed = false;
    const QString trimmedHorizon = simulationHorizonMs_.trimmed();
    const qulonglong horizonValue =
            trimmedHorizon.toULongLong(&horizonParsed);
    if (!horizonParsed || trimmedHorizon != simulationHorizonMs_ ||
        horizonValue < kSearchTickDurationMs ||
        horizonValue > kMaximumSimulationHorizonMs ||
        horizonValue % kSearchTickDurationMs != 0u) {
        return {
                {},
                QStringLiteral(
                        "Simulation horizon must be a whole number of "
                        "milliseconds between %1 and %2, aligned to %1 ms.")
                        .arg(kSearchTickDurationMs)
                        .arg(kMaximumSimulationHorizonMs)};
    }
    const std::uint32_t simulationHorizonMs =
            static_cast<std::uint32_t>(horizonValue);
    const BlockConfigurationValidation configurationValidation =
            configuration_.validate(
                    kSearchTickDurationMs,
                    simulationHorizonMs);
    if (!configurationValidation.configuration) {
        return {{}, configurationValidation.error};
    }
    const auto &configuration = *configurationValidation.configuration;
    ConditionVariables conditionVariables;
    if (configuration.evaluationTarget.id == kPointTargetEvaluationId) {
        const OptionSettings &settings =
                configuration.evaluationTarget.settings;
        try {
            conditionVariables.emplace(
                    "bf_target_point",
                    ConditionVariable{
                            std::stod(settings.at("x")),
                            std::stod(settings.at("y")),
                            std::stod(settings.at("z")),
                            true});
        } catch (...) {
            return {{}, QStringLiteral(
                                "The point target's x, y, and z values must "
                                "all be valid numbers.")};
        }
    }
    ConditionCompileResult condition = CompileConditionScript(
            conditionScript_.toStdString(), conditionVariables);
    if (condition.error) {
        return {{}, QString::fromStdString(*condition.error)};
    }
    if (!baseInputScriptError_.isEmpty()) {
        return {{}, baseInputScriptError_};
    }

    std::uint32_t parallelSampleCount = 1u;
    bool calibrateCudaParallelSampleCount = false;
    if (simulationBackend_ == PhysicsBackend::MultiThreadedCpu) {
        bool parsed = false;
        const QString trimmed = cpuWorkerCount_.trimmed();
        const uint value = trimmed.toUInt(&parsed);
        if (!parsed || trimmed != cpuWorkerCount_ || value == 0u ||
            value > kMaximumCpuWorkerCount) {
            return {
                    {},
                    QStringLiteral(
                            "CPU worker threads must be a whole number "
                            "between 1 and %1.")
                            .arg(kMaximumCpuWorkerCount)};
        }
        parallelSampleCount = value;
    }
#if FOREVERVALIDATOR_HAS_CUDA
    if (simulationBackend_ == PhysicsBackend::Cuda) {
        if (!cudaAvailable_) {
            return {{}, cudaStatusText_};
        }
        if (configuration.evaluationTarget.id ==
            kCustomVolumeEntryEvaluationId) {
            return {
                    {},
                    QStringLiteral(
                            "Custom volume targets currently require a CPU "
                            "physics backend.")};
        }
        calibrateCudaParallelSampleCount =
                cudaCalibrationEnabled_;
        if (!calibrateCudaParallelSampleCount) {
            bool parsed = false;
            const QString trimmed =
                    cudaParallelSampleCount_.trimmed();
            const uint value = trimmed.toUInt(&parsed);
            if (!parsed ||
                trimmed != cudaParallelSampleCount_ ||
                value == 0u) {
                return {
                        {},
                        QStringLiteral(
                                "CUDA parallel samples must be a positive "
                                "whole number.")};
            }
            parallelSampleCount = value;
        }
    }
#endif

    SearchRequest request{
            packsInfo.absoluteFilePath().toUtf8().toStdString(),
            replayInfo.absoluteFilePath().toUtf8().toStdString()};
    request.backend = simulationBackend_;
    request.parallelSampleCount = parallelSampleCount;
    request.calibrateCudaParallelSampleCount =
            calibrateCudaParallelSampleCount;
    request.searchAlgorithm = configuration.searchAlgorithm;
    request.modifiers = configuration.modifiers;
    request.evaluationTarget = configuration.evaluationTarget;
    request.baseInputCommands = parsedBaseInputCommands_;
    request.useCudaSessionSpecialization =
            cudaSessionSpecializationEnabled_ && cudaFastModeAvailable_;
    request.simulationHorizonMs = simulationHorizonMs;
    request.condition = std::move(condition.program);
    return {std::move(request), {}};
}

void SearchController::refreshValidation() {
    const QString newMessage = validate().error;
    const bool newValid = newMessage.isEmpty();
    const bool oldCanStart = canStart();
    const bool messageChanged = validationMessage_ != newMessage;
    valid_ = newValid;
    validationMessage_ = newMessage;
    if (messageChanged) {
        emit validationChanged();
    }
    if (oldCanStart != canStart()) {
        emit canStartChanged();
    }
}

void SearchController::setRunning(bool value) {
    if (running_ == value) {
        return;
    }
    const bool oldCanStart = canStart();
    running_ = value;
    cuboidTargets_.setEditingEnabled(!value);
    customVolumeTargets_.setEditingEnabled(!value);
    poseTargets_.setEditingEnabled(!value);
    emit runningChanged();
    emit replayInputStateChanged();
    if (oldCanStart != canStart()) {
        emit canStartChanged();
    }
}

void SearchController::setExtractingReplayInputs(bool value) {
    if (extractingReplayInputs_ == value) {
        return;
    }
    const bool oldCanStart = canStart();
    extractingReplayInputs_ = value;
    emit replayInputStateChanged();
    if (oldCanStart != canStart()) {
        emit canStartChanged();
    }
}

bool SearchController::replayInputStatusIsError() const {
    return replayInputStatusIsError_;
}

bool SearchController::lastRunFailed() const {
    return lastRunFailed_;
}

void SearchController::setReplayInputStatusText(const QString &value,
                                                bool isError) {
    if (replayInputStatusText_ == value &&
        replayInputStatusIsError_ == isError) {
        return;
    }
    replayInputStatusText_ = value;
    replayInputStatusIsError_ = isError;
    emit replayInputStateChanged();
}

void SearchController::setStopping(bool value) {
    if (stopping_ == value) {
        return;
    }
    stopping_ = value;
    emit stoppingChanged();
}

void SearchController::setStatusText(const QString &value) {
    if (statusText_ == value) {
        return;
    }
    statusText_ = value;
    emit statusChanged();
}

void SearchController::setLiveMetrics(
        const QString &iterationCountText,
        const QString &throughputText,
        const QString &elapsedText,
        bool visible) {
    if (iterationCountText_ == iterationCountText &&
        throughputText_ == throughputText && elapsedText_ == elapsedText &&
        liveMetricsVisible_ == visible) {
        return;
    }
    iterationCountText_ = iterationCountText;
    throughputText_ = throughputText;
    elapsedText_ = elapsedText;
    liveMetricsVisible_ = visible;
    emit metricsChanged();
}

void SearchController::setResultText(const QString &value) {
    if (resultText_ == value) {
        return;
    }
    resultText_ = value;
    emit resultChanged();
}

void SearchController::setBestInputsText(const QString &value) {
    if (bestInputsText_ == value) {
        return;
    }
    bestInputsText_ = value;
    emit resultChanged();
}

void SearchController::setProgress(bool indeterminate, double value) {
    value = std::clamp(value, 0.0, 1.0);
    if (progressIndeterminate_ == indeterminate &&
        progressValue_ == value) {
        return;
    }
    progressIndeterminate_ = indeterminate;
    progressValue_ = value;
    emit progressChanged();
}

void SearchController::scheduleAutoDetectPacksDirectory(
        const QStringList *packsSearchPatterns) {
    if (autoDetectionScheduled_ || !packsDirectory_.trimmed().isEmpty()) {
        return;
    }
    if (packsSearchPatterns == nullptr &&
        gAutomaticPacksSearchScheduled.exchange(
                true, std::memory_order_relaxed)) {
        return;
    }
    autoDetectionScheduled_ = true;
    const std::optional<QStringList> patterns = packsSearchPatterns == nullptr
            ? std::nullopt
            : std::optional<QStringList>(*packsSearchPatterns);

    QTimer::singleShot(0, this, [this, patterns]() {
        if (!packsDirectory_.trimmed().isEmpty() ||
            autoDetectionThread_ != nullptr) {
            return;
        }

        QThread *const thread = QThread::create([this, patterns]() {
            const QString detected = patterns
                    ? FindInstalledPacksDirectory(*patterns)
                    : FindInstalledPacksDirectory();
            QMetaObject::invokeMethod(
                    this,
                    [this, detected]() {
                        publishAutoDetectedPacksDirectory(detected);
                    },
                    Qt::QueuedConnection);
        });
        autoDetectionThread_ = thread;
        connect(
                thread,
                &QThread::finished,
                this,
                [this, thread]() {
                    if (autoDetectionThread_ == thread) {
                        autoDetectionThread_ = nullptr;
                    }
                    thread->deleteLater();
                },
                Qt::QueuedConnection);
        thread->start();
    });
}

void SearchController::publishAutoDetectedPacksDirectory(
        const QString &detected) {
    if (detected.isEmpty() || !packsDirectory_.trimmed().isEmpty()) {
        return;
    }
    autoDetectedPacksDirectory_ = detected;
    emit autoDetectedPacksDirectoryChanged();
}

void SearchController::clearAutoDetectedPacksDirectory() {
    if (autoDetectedPacksDirectory_.isEmpty()) {
        return;
    }
    autoDetectedPacksDirectory_.clear();
    emit autoDetectedPacksDirectoryChanged();
}

void SearchController::persist(const char *key, const QString &value) {
    QSettings().setValue(QLatin1String(key), value);
}

void SearchController::waitForWorker() {
    if (autoDetectionThread_ != nullptr) {
        disconnect(autoDetectionThread_, nullptr, this, nullptr);
        autoDetectionThread_->wait();
        delete autoDetectionThread_;
        autoDetectionThread_ = nullptr;
    }
    if (inputExtractionThread_ != nullptr) {
        disconnect(inputExtractionThread_, nullptr, this, nullptr);
        inputExtractionThread_->wait();
        delete inputExtractionThread_;
        inputExtractionThread_ = nullptr;
    }
    if (stopRequested_) {
        stopRequested_->store(true, std::memory_order_relaxed);
    }
    if (cancellationRequested_) {
        cancellationRequested_->store(true, std::memory_order_relaxed);
    }
    if (workerThread_ != nullptr) {
        workerThread_->quit();
        workerThread_->wait();
        workerThread_ = nullptr;
    }
}

}  // namespace forevertas::app
