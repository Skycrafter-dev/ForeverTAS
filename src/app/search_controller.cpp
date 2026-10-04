#include "app/search_controller.h"

#include "app/compact_number_format.h"
#include "app/packs_directory_finder.h"
#include "app/search_worker.h"
#include "app/search_session_store.h"
#include "app/system_file_dialog.h"
#include "conditions/condition_catalog.h"
#include "mutations/input_event_formatter.h"
#include "mutations/replay_input_script.h"
#include "searches/algorithm_registry.h"
#include "time_format.h"
#include "speed_format.h"

#include <forevervalidator/validation.h>

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QPalette>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSettings>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <limits>
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
constexpr char kHipParallelSampleCountKey[] =
        "backends/hip/parallelSampleCount";
constexpr char kVulkanParallelSampleCountKey[] =
        "backends/vulkan/parallelSampleCount";
constexpr char kCudaCalibrationEnabledKey[] =
        "backends/cuda/calibrationEnabled";
constexpr char kHipCalibrationEnabledKey[] =
        "backends/hip/calibrationEnabled";
constexpr char kVulkanCalibrationEnabledKey[] =
        "backends/vulkan/calibrationEnabled";
constexpr char kCudaSessionSpecializationEnabledKey[] =
        "backends/cuda/sessionSpecializationEnabled";
constexpr char kRandomizeSeedsOnStartKey[] =
        "search/randomizeSeedsOnStart";
constexpr char kAutoRestartModeKey[] = "search/autoRestartMode";
constexpr char kAutoRestartDurationKey[] = "search/autoRestartDuration";
constexpr char kAutoRestartAttemptsKey[] = "search/autoRestartAttempts";
constexpr char kDrawTargetsThroughBlocksKey[] =
        "viewer/drawTargetsThroughBlocks";
constexpr char kTargetMouseEditingLockedKey[] = "viewer/targetMouseEditingLocked";
constexpr char kDarkModeKey[] = "appearance/darkMode";
constexpr char kViewerVisibleKey[] = "viewer/visible";
std::atomic_bool gAutomaticPacksSearchScheduled{false};

std::optional<std::chrono::seconds> ParseRestartDuration(
        const QString &value) {
    static const QRegularExpression format(
            QStringLiteral("^([0-9]{2,}):([0-5][0-9]):([0-5][0-9])$"));
    const auto match = format.match(value);
    if (!match.hasMatch()) return std::nullopt;
    bool valid = false;
    const qulonglong hours = match.captured(1).toULongLong(&valid);
    const int remainingSeconds =
            match.captured(2).toInt() * 60 + match.captured(3).toInt();
    if (!valid || hours > static_cast<qulonglong>(
                          (std::numeric_limits<std::int64_t>::max() -
                           remainingSeconds) / 3600)) {
        return std::nullopt;
    }
    const auto seconds = static_cast<std::int64_t>(
            hours * 3600 + remainingSeconds);
    const auto maximum = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::duration::max()).count();
    return seconds > 0 && seconds <= maximum
            ? std::optional(std::chrono::seconds(seconds))
            : std::nullopt;
}

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
      cuboidTargets_(configuration_.evaluationTargetSettingsFor(
              QString::fromLatin1(kVolumeEntryEvaluationId))),
      customVolumeTargets_(configuration_.evaluationTargetSettingsFor(
              QString::fromLatin1(kCustomVolumeEntryEvaluationId))),
      poseTargets_(configuration_.evaluationTargetSettingsFor(
              QString::fromLatin1(kPoseTargetEvaluationId))) {
    initialize(nullptr);
}

SearchController::SearchController(const QStringList &packsSearchPatterns,
                                   QObject *parent)
    : QObject(parent),
      cuboidTargets_(configuration_.evaluationTargetSettingsFor(
              QString::fromLatin1(kVolumeEntryEvaluationId))),
      customVolumeTargets_(configuration_.evaluationTargetSettingsFor(
              QString::fromLatin1(kCustomVolumeEntryEvaluationId))),
      poseTargets_(configuration_.evaluationTargetSettingsFor(
              QString::fromLatin1(kPoseTargetEvaluationId))) {
    initialize(&packsSearchPatterns);
}

void SearchController::initialize(const QStringList *packsSearchPatterns) {
    connect(this, &SearchController::modifierPassesChanged,
            this, &SearchController::requiredSimulationHorizonMsChanged);
    connect(this, &SearchController::evaluationTargetSettingsChanged,
            this, &SearchController::requiredSimulationHorizonMsChanged);
    qRegisterMetaType<SearchCompletionPtr>();
    qRegisterMetaType<SearchImprovementPtr>();
    connect(&cuboidTargets_,
            &CuboidTargetModel::selectedTargetChanged,
            this,
            &SearchController::synchronizeSelectedCuboid);
    connect(&customVolumeTargets_,
            &CustomVolumeTargetModel::selectedTargetChanged,
            this,
            &SearchController::synchronizeSelectedCustomVolume);
    connect(&customVolumeTargets_,
            &CustomVolumeTargetModel::drawingChanged,
            this,
            &SearchController::customVolumeDrawingChanged);
    connect(&poseTargets_,
            &PoseTargetModel::selectedTargetChanged,
            this,
            &SearchController::synchronizeSelectedPoseTarget);
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
    hipParallelSampleCount_ = StoredValue(
            kHipParallelSampleCountKey,
            QString::number(kDefaultCudaParallelSampleCount));
    hipCalibrationEnabled_ = QSettings()
            .value(QLatin1String(kHipCalibrationEnabledKey), false)
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
#if FOREVERVALIDATOR_HAS_HIP
    const auto hip = forevervalidator::QueryHipBackendDiagnostics();
    hipAvailable_ = hip.IsReady();
    hipStatusText_ = hipAvailable_
            ? QStringLiteral("HIP ready: %1")
                      .arg(QString::fromStdString(hip.deviceName))
            : QStringLiteral("HIP unavailable: %1")
                      .arg(QString::fromStdString(hip.diagnostic));
#else
    hipStatusText_ =
            QStringLiteral("HIP support is not compiled into this build.");
#endif
    vulkanParallelSampleCount_ = StoredValue(
            kVulkanParallelSampleCountKey,
            QString::number(kDefaultCudaParallelSampleCount));
    vulkanCalibrationEnabled_ = QSettings()
            .value(QLatin1String(kVulkanCalibrationEnabledKey), false).toBool();
    const auto vulkan = forevervalidator::QueryVulkanBackendDiagnostics();
    vulkanAvailable_ = vulkan.IsSearchReady();
    if (vulkanAvailable_) {
        vulkanStatusText_ = QStringLiteral("Vulkan ready: %1")
                                    .arg(QString::fromStdString(vulkan.deviceName));
    } else if (vulkan.IsReady()) {
        vulkanStatusText_ = QStringLiteral(
                "Vulkan search unavailable: %1 lacks exact FP32 controls "
                "(VK_KHR_shader_float_controls2).")
                                    .arg(QString::fromStdString(vulkan.deviceName));
    } else {
        vulkanStatusText_ = QStringLiteral("Vulkan unavailable: %1")
                                    .arg(QString::fromStdString(vulkan.diagnostic));
    }
    QSettings settings;
    randomizeSeedsOnStart_ = settings
            .value(QLatin1String(kRandomizeSeedsOnStartKey), true)
            .toBool();
    autoRestartMode_ = settings.value(
            QLatin1String(kAutoRestartModeKey), QStringLiteral("off"))
            .toString();
    autoRestartDuration_ = settings.value(
            QLatin1String(kAutoRestartDurationKey),
            QStringLiteral("00:05:00")).toString();
    autoRestartAttempts_ = settings.value(
            QLatin1String(kAutoRestartAttemptsKey),
            QStringLiteral("1000")).toString();
    if (!settings.contains(QLatin1String(kRandomizeSeedsOnStartKey))) {
        settings.setValue(
                QLatin1String(kRandomizeSeedsOnStartKey), true);
    }
    drawTargetsThroughBlocks_ = settings
            .value(QLatin1String(kDrawTargetsThroughBlocksKey), false)
            .toBool();
    targetMouseEditingLocked_ = settings.value(QLatin1String(kTargetMouseEditingLockedKey), false).toBool();
    darkMode_ =
            QSettings().value(QLatin1String(kDarkModeKey), false).toBool();
    ApplyApplicationPalette(darkMode_);
    viewerVisible_ = settings.value(QLatin1String(kViewerVisibleKey), true).toBool();
    const QString storedBackend = StoredValue(
            kSimulationBackendKey,
            BackendId(PhysicsBackend::Reference));
    const std::optional<PhysicsBackend> parsedBackend =
            ParsePhysicsBackend(storedBackend.toStdString());
    simulationBackend_ = parsedBackend.value_or(PhysicsBackend::Reference);
    if (!parsedBackend) {
        QSettings().setValue(
                QLatin1String(kSimulationBackendKey),
                BackendId(simulationBackend_));
    }
    scheduleAutoDetectPacksDirectory(packsSearchPatterns);
    synchronizeSelectedCuboid();
    synchronizeSelectedCustomVolume();
    synchronizeSelectedPoseTarget();
    refreshValidation();
    refreshSessions();
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
    return !shuttingDown_ && !running_ && !extractingReplayInputs_ && !evaluatingBase() &&
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
#if FOREVERVALIDATOR_HAS_HIP
    options.push_back(QVariantMap{
            {QStringLiteral("id"), BackendId(PhysicsBackend::Hip)},
            {QStringLiteral("label"), QStringLiteral("HIP")},
            {QStringLiteral("description"),
             QStringLiteral("HIP Compute for Stadium on compatible GPUs")}});
#endif
#if FOREVERVALIDATOR_HAS_VULKAN
    options.push_back(QVariantMap{
            {QStringLiteral("id"), BackendId(PhysicsBackend::Vulkan)},
            {QStringLiteral("label"), QStringLiteral("Vulkan")},
            {QStringLiteral("description"),
             QStringLiteral("Vulkan Compute for Stadium on compatible GPUs")}});
#endif
    return options;
}

QString SearchController::simulationBackendId() const {
    return BackendId(simulationBackend_);
}

QString SearchController::simulationHorizonMs() const {
    return simulationHorizonMs_;
}

qint64 SearchController::requiredSimulationHorizonMs() const {
    constexpr qint64 tick = 10;
    qint64 required = tick;
    bool representable = true;
    const auto include = [&](const QVariantMap &settings, bool inputTime) {
        for (const char *key : {"minTimeMs", "maxTimeMs", "targetTimeMs"}) {
            if (QLatin1String(key) == QLatin1String("maxTimeMs") &&
                settings.value("maxTimeMode").toString() == QLatin1String("horizon")) continue;
            bool valid = false;
            qint64 time = settings.value(QString::fromLatin1(key)).toLongLong(&valid);
            if (!valid || time < 0) continue;
            const qint64 offset = inputTime ? tick : 0;
            if (time > kMaximumSimulationHorizonMs - offset) {
                representable = false;
                continue;
            }
            time += offset;
            required = std::max(required, ((time + tick - 1) / tick) * tick);
        }
    };
    for (const QVariant &pass : modifierPasses()) {
        if (!pass.toMap().value(QStringLiteral("enabled"), true).toBool()) continue;
        include(pass.toMap().value(QStringLiteral("settings")).toMap(), true);
    }
    include(evaluationTargetSettings(), false);
    return representable && required <= kMaximumSimulationHorizonMs ? required : -1;
}

bool SearchController::extendSimulationHorizon() {
    if (running_) return false;
    bool valid = false;
    const qint64 current = simulationHorizonMs_.toLongLong(&valid);
    const qint64 required = requiredSimulationHorizonMs();
    if (!valid || required <= current || required < 10) return false;
    setSimulationHorizonMs(QString::number(required));
    return true;
}

void SearchController::previewBaseInputs() {
    emit basePreviewRequested();
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

QString SearchController::hipParallelSampleCount() const {
    return hipParallelSampleCount_;
}

QString SearchController::vulkanParallelSampleCount() const {
    return vulkanParallelSampleCount_;
}

bool SearchController::cudaCalibrationEnabled() const {
    return cudaCalibrationEnabled_;
}

bool SearchController::hipCalibrationEnabled() const {
    return hipCalibrationEnabled_;
}

bool SearchController::vulkanCalibrationEnabled() const {
    return vulkanCalibrationEnabled_;
}

bool SearchController::cudaSessionSpecializationEnabled() const {
    return cudaSessionSpecializationEnabled_;
}

bool SearchController::cudaAvailable() const {
    return cudaAvailable_;
}

bool SearchController::hipAvailable() const {
    return hipAvailable_;
}

bool SearchController::vulkanAvailable() const {
    return vulkanAvailable_;
}

bool SearchController::cudaFastModeAvailable() const {
    return cudaFastModeAvailable_;
}

QString SearchController::cudaStatusText() const {
    return cudaStatusText_;
}

QString SearchController::hipStatusText() const {
    return hipStatusText_;
}

QString SearchController::vulkanStatusText() const {
    return vulkanStatusText_;
}

bool SearchController::randomizeSeedsOnStart() const {
    return randomizeSeedsOnStart_;
}

bool SearchController::drawTargetsThroughBlocks() const {
    return drawTargetsThroughBlocks_;
}

bool SearchController::darkMode() const {
    return darkMode_;
}

bool SearchController::viewerVisible() const { return viewerVisible_; }

void SearchController::setViewerVisible(bool value) {
    if (viewerVisible_ == value) return;
    viewerVisible_ = value;
    QSettings().setValue(QLatin1String(kViewerVisibleKey), value);
    emit viewerVisibleChanged();
}

bool SearchController::targetMouseEditingLocked() const {
    return targetMouseEditingLocked_;
}

QVariantList SearchController::searchAlgorithmOptions() const {
    return configuration_.searchAlgorithmOptions();
}

QVariantList SearchController::modifierOptions() const {
    return configuration_.modifierOptions();
}

QVariantList SearchController::evaluationTargetOptions() const {
    return configuration_.evaluationTargetOptions();
}

QString SearchController::searchAlgorithmId() const {
    return configuration_.searchAlgorithmId();
}

QString SearchController::evaluationTargetId() const {
    return configuration_.evaluationTargetId();
}

QVariantMap SearchController::searchAlgorithmSettings() const {
    return configuration_.searchAlgorithmSettings();
}

QVariantList SearchController::modifierPasses() const {
    return configuration_.modifierPasses();
}

QVariantMap SearchController::evaluationTargetSettings() const {
    return configuration_.evaluationTargetSettings();
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
    return !shuttingDown_ && valid_ && !running_ && !extractingReplayInputs_ && !evaluatingBase();
}

bool SearchController::canEvaluateBase() const {
    return !shuttingDown_ && !running_ && !extractingReplayInputs_ && !evaluatingBase() && validate(true).request.has_value();
}

void SearchController::cancelBaseEvaluation() {
    if (baselineEvaluationCancelled_) baselineEvaluationCancelled_->store(true, std::memory_order_relaxed);
}

void SearchController::evaluateBase() {
    if (!canEvaluateBase()) return;
    auto validation = validate(true);
    if (!validation.request) return;
    baselineEvaluationCancelled_ = std::make_shared<std::atomic_bool>(false);
    const auto cancelled = baselineEvaluationCancelled_;
    const auto generation = baselineEvaluationGeneration_;
    baseEvaluationResult_.clear();
    baseEvaluationText_ = QStringLiteral("Evaluating base inputs (Optimized CPU)...");
    auto thread = QThread::create([this, request = *validation.request, cancelled, generation] {
        QVariantMap data{{QStringLiteral("backend"), QStringLiteral("optimized-cpu")},
                         {QStringLiteral("targetId"), QString::fromStdString(request.evaluationTarget.id)},
                         {QStringLiteral("horizonMs"), request.simulationHorizonMs}};
        QString text;
        try {
            SearchRunControl control;
            control.cancellationRequested = [cancelled] { return cancelled->load(std::memory_order_relaxed); };
            const auto result = EvaluateBaseline(request, &control);
            data.insert(QStringLiteral("eligible"), result.has_value());
            if (result) {
                data.insert(QStringLiteral("score"), result->bestScore);
                data.insert(QStringLiteral("evaluationTimeMs"), result->bestEvaluationTimeMs);
                data.insert(QStringLiteral("description"), QString::fromStdString(result->bestEvaluationDescription));
                QVariantList objectives, metrics;
                for (const auto value : result->objectiveScores) objectives.append(value);
                for (const auto value : result->metricValues) metrics.append(value);
                data.insert(QStringLiteral("objectiveScores"), objectives);
                data.insert(QStringLiteral("metricValues"), metrics);
                text = QStringLiteral("Base evaluation (Optimized CPU)\n%1\nScore: %2 at %3 ms")
                        .arg(QString::fromStdString(result->bestEvaluationDescription),
                             QString::number(result->bestScore, 'g', 17),
                             QString::number(result->bestEvaluationTimeMs, 'g', 17));
            } else {
                text = QStringLiteral("Base evaluation (Optimized CPU)\nNo eligible observation: the target or conditions were not satisfied.");
            }
        } catch (const SearchCancelled &) {
            text = QStringLiteral("Base evaluation cancelled.");
            data.insert(QStringLiteral("cancelled"), true);
        } catch (const std::exception &error) {
            text = QStringLiteral("Base evaluation failed: %1").arg(QString::fromUtf8(error.what()));
            data.insert(QStringLiteral("error"), QString::fromUtf8(error.what()));
        }
        QMetaObject::invokeMethod(this, [this, generation, data = std::move(data), text = std::move(text)] {
            if (generation != baselineEvaluationGeneration_) {
                baseEvaluationResult_.clear();
                baseEvaluationText_ = QStringLiteral("Settings changed; evaluate base inputs again.");
            } else {
                baseEvaluationResult_ = data;
                baseEvaluationText_ = text;
            }
            emit baseEvaluationChanged();
        }, Qt::QueuedConnection);
    });
    baselineEvaluationThread_ = thread;
    connect(thread, &QThread::finished, this, [this, thread] {
        if (baselineEvaluationThread_ == thread) {
            baselineEvaluationThread_ = nullptr;
            baselineEvaluationCancelled_.reset();
        }
        thread->deleteLater();
        emit baseEvaluationChanged();
        emit canStartChanged();
        emit replayInputStateChanged();
    });
    emit baseEvaluationChanged();
    emit canStartChanged();
    emit replayInputStateChanged();
    thread->start();
}

bool SearchController::running() const {
    return running_;
}

bool SearchController::stopping() const {
    return stopping_;
}

bool SearchController::aborting() const {
    return cancellationRequested_ && cancellationRequested_->load(std::memory_order_relaxed);
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

QVariantList SearchController::conditionReference() const {
    QVariantList result;
    const auto append = [&](const auto &entry, const QString &insertion, bool needsPointTarget) {
        QStringList aliases;
        for (const auto &alias : entry.aliases) aliases.push_back(QString::fromStdString(alias));
        result.push_back(QVariantMap{
            {QStringLiteral("name"), QString::fromStdString(entry.name)},
            {QStringLiteral("aliases"), aliases},
            {QStringLiteral("type"), QString::fromStdString(entry.type)},
            {QStringLiteral("units"), QString::fromStdString(entry.units)},
            {QStringLiteral("description"), QString::fromStdString(entry.description)},
            {QStringLiteral("insertion"), insertion},
            {QStringLiteral("conditionsOnly"), entry.conditionsOnly},
            {QStringLiteral("needsPointTarget"), needsPointTarget}});
    };
    for (const auto &entry : ConditionSymbols()) append(entry, QString::fromStdString(entry.name), false);
    for (const auto &entry : ConditionFunctions()) append(entry, QString::fromStdString(entry.example), entry.needsPointTarget);
    return result;
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

QString SearchController::iterationCountExactText() const {
    return FormatExactCount(iterationCount_);
}

QString SearchController::iterationCountRawText() const {
    return QString::number(static_cast<qulonglong>(iterationCount_));
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

QString SearchController::autoRestartMode() const {
    return autoRestartMode_;
}

QString SearchController::autoRestartDuration() const {
    return autoRestartDuration_;
}

QString SearchController::autoRestartAttempts() const {
    return autoRestartAttempts_;
}

QVariantList SearchController::sessionOptions() const {
    return sessionOptions_;
}

QString SearchController::selectedSessionDirectory() const {
    return selectedSessionDirectory_;
}

QVariantList SearchController::cycleRows() const {
    return cycleRows_;
}

QString SearchController::selectedInputsText() const {
    return selectedInputsText_;
}

void SearchController::setAutoRestartMode(const QString &value) {
    if (autoRestartMode_ == value) return;
    autoRestartMode_ = value;
    persist(kAutoRestartModeKey, value);
    emit autoRestartChanged();
    refreshValidation();
}

void SearchController::setAutoRestartDuration(const QString &value) {
    if (autoRestartDuration_ == value) return;
    autoRestartDuration_ = value;
    persist(kAutoRestartDurationKey, value);
    emit autoRestartChanged();
    refreshValidation();
}

void SearchController::setAutoRestartAttempts(const QString &value) {
    if (autoRestartAttempts_ == value) return;
    autoRestartAttempts_ = value;
    persist(kAutoRestartAttemptsKey, value);
    emit autoRestartChanged();
    refreshValidation();
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
    refreshSessions();
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

void SearchController::setSearchAlgorithmId(const QString &value) {
    if (!configuration_.setSearchAlgorithmId(value)) return;
    emit searchAlgorithmIdChanged();
    emit searchAlgorithmSettingsChanged();
    refreshValidation();
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

void SearchController::setHipParallelSampleCount(const QString &value) {
    if (hipParallelSampleCount_ == value) {
        return;
    }
    hipParallelSampleCount_ = value;
    persist(kHipParallelSampleCountKey, value);
    emit hipParallelSampleCountChanged();
    refreshValidation();
}

void SearchController::setVulkanParallelSampleCount(const QString &value) {
    if (vulkanParallelSampleCount_ == value) {
        return;
    }
    vulkanParallelSampleCount_ = value;
    persist(kVulkanParallelSampleCountKey, value);
    emit vulkanParallelSampleCountChanged();
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

void SearchController::setHipCalibrationEnabled(bool value) {
    if (hipCalibrationEnabled_ == value) {
        return;
    }
    hipCalibrationEnabled_ = value;
    QSettings().setValue(
            QLatin1String(kHipCalibrationEnabledKey), value);
    emit hipCalibrationEnabledChanged();
    refreshValidation();
}

void SearchController::setVulkanCalibrationEnabled(bool value) {
    if (vulkanCalibrationEnabled_ == value) {
        return;
    }
    vulkanCalibrationEnabled_ = value;
    QSettings().setValue(
            QLatin1String(kVulkanCalibrationEnabledKey), value);
    emit vulkanCalibrationEnabledChanged();
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

void SearchController::setTargetMouseEditingLocked(bool value) {
    if (targetMouseEditingLocked_ == value) return;
    targetMouseEditingLocked_ = value;
    QSettings().setValue(QLatin1String(kTargetMouseEditingLockedKey), value);
    emit targetMouseEditingLockedChanged();
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

void SearchController::setEvaluationTargetId(const QString &value) {
    if (!configuration_.setEvaluationTargetId(value)) return;
    emit evaluationTargetIdChanged();
    emit evaluationTargetSettingsChanged();
    synchronizeSelectedCuboid();
    synchronizeSelectedCustomVolume();
    synchronizeSelectedPoseTarget();
    refreshValidation();
}

void SearchController::setSearchAlgorithmSetting(const QString &key,
                                                 const QString &value) {
    if (!configuration_.setSearchAlgorithmSetting(key, value)) return;
    emit searchAlgorithmSettingsChanged();
    refreshValidation();
}

void SearchController::addModifierPass(const QString &id) {
    if (!configuration_.addModifierPass(id)) return;
    emit modifierPassesChanged();
    refreshValidation();
}

void SearchController::removeModifierPass(int index) {
    if (!configuration_.removeModifierPass(index)) return;
    emit modifierPassesChanged();
    refreshValidation();
}

void SearchController::moveModifierPass(int fromIndex, int toIndex) {
    if (!configuration_.moveModifierPass(fromIndex, toIndex)) return;
    emit modifierPassesChanged();
    refreshValidation();
}

void SearchController::setModifierPassId(int index, const QString &id) {
    if (!configuration_.setModifierPassId(index, id)) return;
    emit modifierPassesChanged();
    refreshValidation();
}

void SearchController::setModifierPassEnabled(int index, bool enabled) {
    if (running_ || !configuration_.setModifierPassEnabled(index, enabled)) return;
    emit modifierPassesChanged();
    refreshValidation();
}

void SearchController::setModifierPassSetting(int index,
                                              const QString &key,
                                              const QString &value) {
    if (!configuration_.setModifierPassSetting(index, key, value)) return;
    emit modifierPassesChanged();
    refreshValidation();
}

void SearchController::setEvaluationTargetSetting(const QString &key,
                                                  const QString &value) {
    const bool isCuboid = configuration_.evaluationTargetId() ==
            QString::fromLatin1(kVolumeEntryEvaluationId);
    if (!configuration_.setEvaluationTargetSetting(
                key, value, !isCuboid)) {
        return;
    }
    if (isCuboid) {
        synchronizeCuboidSetting(key, value);
    } else if (configuration_.evaluationTargetId() ==
               QString::fromLatin1(kCustomVolumeEntryEvaluationId)) {
        synchronizeCustomVolumeSetting(key, value);
    } else if (configuration_.evaluationTargetId() ==
               QString::fromLatin1(kPoseTargetEvaluationId)) {
        synchronizePoseTargetSetting(key, value);
    }
    emit evaluationTargetSettingsChanged();
    refreshValidation();
}

void SearchController::synchronizeSelectedCuboid() {
    if (configuration_.evaluationTargetId() !=
        QString::fromLatin1(kVolumeEntryEvaluationId)) {
        return;
    }
    const QVariantMap target = cuboidTargets_.selectedTarget();
    bool changed = false;
    constexpr const char *keys[] = {
            "centerX", "centerY", "centerZ", "sizeX", "sizeY", "sizeZ"};
    for (const char *const key : keys) {
        const QString qKey = QString::fromLatin1(key);
        changed |= configuration_.setEvaluationTargetSetting(
                qKey, target.value(qKey).toString(), false);
    }
    if (changed) {
        emit evaluationTargetSettingsChanged();
        refreshValidation();
    }
}

void SearchController::synchronizeCuboidSetting(const QString &key,
                                                const QString &value) {
    const int index = cuboidTargets_.selectedIndex();
    if (key == QStringLiteral("centerX")) {
        cuboidTargets_.setCenterComponent(index, QStringLiteral("x"), value);
    } else if (key == QStringLiteral("centerY")) {
        cuboidTargets_.setCenterComponent(index, QStringLiteral("y"), value);
    } else if (key == QStringLiteral("centerZ")) {
        cuboidTargets_.setCenterComponent(index, QStringLiteral("z"), value);
    } else if (key == QStringLiteral("sizeX")) {
        cuboidTargets_.setSizeComponent(index, QStringLiteral("x"), value);
    } else if (key == QStringLiteral("sizeY")) {
        cuboidTargets_.setSizeComponent(index, QStringLiteral("y"), value);
    } else if (key == QStringLiteral("sizeZ")) {
        cuboidTargets_.setSizeComponent(index, QStringLiteral("z"), value);
    }
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

void SearchController::synchronizeSelectedCustomVolume() {
    if (configuration_.evaluationTargetId() !=
        QString::fromLatin1(kCustomVolumeEntryEvaluationId)) {
        return;
    }
    const QVariantMap target = customVolumeTargets_.selectedTarget();
    bool changed = false;
    constexpr const char *keys[] = {
            "plane", "originX", "originY", "originZ", "depth", "polygon"};
    for (const char *const key : keys) {
        const QString qKey = QString::fromLatin1(key);
        changed |= configuration_.setEvaluationTargetSetting(
                qKey, target.value(qKey).toString());
    }
    if (changed) {
        emit evaluationTargetSettingsChanged();
        refreshValidation();
    }
}

void SearchController::synchronizeCustomVolumeSetting(
        const QString &key,
        const QString &value) {
    const int index = customVolumeTargets_.selectedIndex();
    if (key == QStringLiteral("plane")) {
        customVolumeTargets_.setPlane(index, value);
    } else if (key == QStringLiteral("originX")) {
        customVolumeTargets_.setOriginComponent(
                index, QStringLiteral("x"), value);
    } else if (key == QStringLiteral("originY")) {
        customVolumeTargets_.setOriginComponent(
                index, QStringLiteral("y"), value);
    } else if (key == QStringLiteral("originZ")) {
        customVolumeTargets_.setOriginComponent(
                index, QStringLiteral("z"), value);
    } else if (key == QStringLiteral("depth")) {
        customVolumeTargets_.setDepth(index, value);
    } else if (key == QStringLiteral("polygon")) {
        customVolumeTargets_.setPolygon(index, value);
    }
}

void SearchController::focusSelectedCustomVolume() {
    const QVariantMap target = customVolumeTargets_.selectedTarget();
    emit customVolumeFocusRequested(
            target.value(QStringLiteral("focusCenter")).value<QVector3D>(),
            target.value(QStringLiteral("focusSize")).value<QVector3D>());
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

void SearchController::synchronizeSelectedPoseTarget() {
    if (configuration_.evaluationTargetId() !=
        QString::fromLatin1(kPoseTargetEvaluationId)) {
        return;
    }
    const QVariantMap target = poseTargets_.selectedTarget();
    bool changed = false;
    constexpr const char *keys[] = {
            "x", "y", "z", "yawDegrees", "pitchDegrees", "rollDegrees"};
    for (const char *const key : keys) {
        const QString qKey = QString::fromLatin1(key);
        changed |= configuration_.setEvaluationTargetSetting(
                qKey, target.value(qKey).toString());
    }
    if (changed) {
        emit evaluationTargetSettingsChanged();
        refreshValidation();
    }
}

void SearchController::synchronizePoseTargetSetting(
        const QString &key,
        const QString &value) {
    const int index = poseTargets_.selectedIndex();
    if (key == QStringLiteral("x") ||
        key == QStringLiteral("y") ||
        key == QStringLiteral("z")) {
        poseTargets_.setPositionComponent(index, key, value);
    } else if (key == QStringLiteral("yawDegrees")) {
        poseTargets_.setRotationComponent(
                index, QStringLiteral("yaw"), value);
    } else if (key == QStringLiteral("pitchDegrees")) {
        poseTargets_.setRotationComponent(
                index, QStringLiteral("pitch"), value);
    } else if (key == QStringLiteral("rollDegrees")) {
        poseTargets_.setRotationComponent(
                index, QStringLiteral("roll"), value);
    }
}

void SearchController::focusSelectedPoseTarget() {
    const QVariantMap target = poseTargets_.selectedTarget();
    emit poseTargetFocusRequested(
            target.value(QStringLiteral("position")).value<QVector3D>(),
            QVector3D(4.0F, 2.5F, 7.0F));
}

void SearchController::setPacksDirectory(const QString &value) {
    clearAutoDetectedPacksDirectory();
    if (packsDirectory_ == value) {
        return;
    }
    packsDirectory_ = value;
    persist(kPacksDirectoryKey, value);
    emit packsDirectoryChanged();
    emit replayInputStateChanged();
    refreshValidation();
    refreshSessions();
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

QString SearchController::formatInterpolatedTime(double milliseconds) const {
    return QString::fromStdString(FormatInterpolatedTimeMilliseconds(milliseconds));
}

QString SearchController::formatSpeed(double metersPerSecond) const {
    return QString::fromStdString(FormatDisplaySpeed(metersPerSecond));
}

void SearchController::extractReplayInputs() {
    if (!canExtractReplayInputs() || inputExtractionThread_ != nullptr) {
        return;
    }
    const QString packsDirectory = QFileInfo(packsDirectory_)
            .absoluteFilePath();
    const QString replayPath = QFileInfo(replayPath_).absoluteFilePath();
    setExtractingReplayInputs(true);
    setReplayInputStatusText(QStringLiteral("Extracting replay inputs..."));

    QThread *const thread = QThread::create(
            [this, packsDirectory, replayPath]() {
                ReplayInputExtractionResult result =
                        ExtractReplayInputScript(packsDirectory, replayPath);
                QMetaObject::invokeMethod(
                        this,
                        [this,
                         packsDirectory,
                         replayPath,
                         result = std::move(result)]() mutable {
                            if (packsDirectory !=
                                        QFileInfo(packsDirectory_)
                                                .absoluteFilePath() ||
                                replayPath !=
                                        QFileInfo(replayPath_)
                                                .absoluteFilePath()) {
                                setReplayInputStatusText(QStringLiteral(
                                        "Replay selection changed; extracted "
                                        "inputs were discarded."));
                            } else if (!result.error.isEmpty()) {
                                setReplayInputStatusText(
                                        QStringLiteral(
                                                "Input extraction failed: %1")
                                                .arg(result.error));
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
    if (shuttingDown_ || running_ || extractingReplayInputs_ || evaluatingBase()) {
        return;
    }

    ValidationResult validation = validate();
    if (!validation.request) {
        refreshValidation();
        return;
    }
    if (randomizeSeedsOnStart_ &&
        configuration_.randomizeModifierSeeds(
                QRandomGenerator::system()->generate())) {
        emit modifierPassesChanged();
        validation = validate();
        if (!validation.request) {
            refreshValidation();
            return;
        }
    }

    emit searchSessionReset();
    selectedSessionDirectory_.clear();
    cycleRows_.clear();
    selectedInputsText_.clear();
    emit historyChanged();
    setResultText({});
    setBestInputsText({});
    iterationCount_ = 0u;
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
    AutoRestartPolicy restartPolicy;
    if (autoRestartMode_ == QLatin1String("duration")) {
        restartPolicy.mode = AutoRestartPolicy::Mode::Duration;
        restartPolicy.duration = *ParseRestartDuration(autoRestartDuration_);
    } else if (autoRestartMode_ == QLatin1String("attempts")) {
        restartPolicy.mode = AutoRestartPolicy::Mode::Attempts;
        restartPolicy.attempts = autoRestartAttempts_.toULongLong();
    }
    restartPolicy.randomizeSeeds = randomizeSeedsOnStart_;
    QThread *const thread = new QThread(this);
    SearchWorker *const worker = new SearchWorker(
            *validation.request,
            ++searchSerial_,
            stopRequested_,
            cancellationRequested_,
            iterationPhase_,
            restartPolicy);
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
    connect(worker, &SearchWorker::iterationCountChanged, this,
            [this](qulonglong count) {
                if (iterationCount_ == count) return;
                iterationCount_ = count;
                emit metricsChanged();
            });
    connect(worker,
            &SearchWorker::cudaBatchSizeChanged,
            this,
            [this](std::uint32_t batchSize) {
                if (PhysicsBackendId(simulationBackend_) == "vulkan") {
                    setVulkanParallelSampleCount(QString::number(batchSize));
                } else if (PhysicsBackendId(simulationBackend_) == "hip") {
                    setHipParallelSampleCount(QString::number(batchSize));
                } else {
                    setCudaParallelSampleCount(QString::number(batchSize));
                }
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
    connect(worker, &SearchWorker::sessionCreated, this,
            [this](const QString &, const QString &directory) {
                sessionOptions_.append(SearchSessionStore::Session(directory));
                selectedSessionDirectory_ = directory;
                cycleRows_.clear();
                selectedInputsText_.clear();
                selectedCycleRestart_ = -1;
                historySelectionExplicit_ = false;
                historyHasOlder_ = historyHasNewer_ = false;
                emit historyChanged();
            });
    connect(worker, &SearchWorker::cycleSaved, this,
            [this](const QString &, const QString &directory, std::uint64_t restart) {
                if (directory != selectedSessionDirectory_) return;
                if (historyHasNewer_) {
                    emit historyChanged();
                    return;
                }
                const auto row = SearchSessionStore::Cycle(directory, restart);
                if (row.isEmpty()) return;
                cycleRows_.append(row);
                if (cycleRows_.size() > static_cast<qsizetype>(SearchSessionStore::kCyclePageSize)) {
                    cycleRows_.removeFirst();
                    historyHasOlder_ = true;
                }
                if (!historySelectionExplicit_) {
                    selectCycle(static_cast<int>(cycleRows_.size()) - 1);
                    historySelectionExplicit_ = false;
                } else emit historyChanged();
            });
    connect(worker,
            &SearchWorker::succeeded,
            this,
            [this](SearchCompletionPtr completion) {
                lastCompletion_ = completion;
                setResultText(completion->summary);
                setBestInputsText(completion->inputsText);
                setProgress(false, 1.0);
                setStatusText(QStringLiteral("Search complete"));
                emit searchCompleted(std::move(completion));
            });
    connect(worker, &SearchWorker::cancelled, this, [this]() {
        setStatusText(QStringLiteral("Search aborted"));
        setProgress(false, progressValue_);
    });
    connect(worker,
            &SearchWorker::failed,
            this,
            [this](const QString &message) {
                setResultText(message);
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

    emit searchStarted(searchSerial_);
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
        emit stoppingChanged();
        setStatusText(QStringLiteral("Aborting search startup..."));
        return;
    }
    stopRequested_->store(true, std::memory_order_relaxed);
    setStatusText(QStringLiteral("Stopping after current iteration..."));
}

void SearchController::abortSearch() {
    if (!running_ || !cancellationRequested_ || aborting()) return;
    if (stopRequested_) stopRequested_->store(true, std::memory_order_relaxed);
    cancellationRequested_->store(true, std::memory_order_relaxed);
    setStopping(true);
    emit stoppingChanged();
    setStatusText(QStringLiteral("Aborting at the next simulation checkpoint..."));
}

SearchController::ValidationResult SearchController::validate(bool baselineOnly) const {
    if (!baselineOnly && autoRestartMode_ != QLatin1String("off") &&
        autoRestartMode_ != QLatin1String("duration") &&
        autoRestartMode_ != QLatin1String("attempts")) {
        return {{}, QStringLiteral("Choose an autorestart mode.")};
    }
    if (!baselineOnly && autoRestartMode_ == QLatin1String("duration") &&
        !ParseRestartDuration(autoRestartDuration_)) {
        return {{}, QStringLiteral(
                "Autorestart duration must be HH:MM:SS and greater than zero.")};
    }
    if (!baselineOnly && autoRestartMode_ == QLatin1String("attempts")) {
        bool valid = false;
        const qulonglong attempts =
                autoRestartAttempts_.toULongLong(&valid);
        if (!valid || attempts == 0 || attempts > 9007199254740991ULL ||
            !QRegularExpression(QStringLiteral("^[0-9]+$"))
                     .match(autoRestartAttempts_).hasMatch()) {
            return {{}, QStringLiteral(
                    "Autorestart attempts must be a positive whole number "
                    "no greater than 9,007,199,254,740,991.")};
        }
    }
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
                        "milliseconds between 10 and %1, aligned to 10 ms.")
                        .arg(kMaximumSimulationHorizonMs)};
    }
    const std::uint32_t simulationHorizonMs =
            static_cast<std::uint32_t>(horizonValue);
    const SearchConfigurationValidation configurationValidation =
            configuration_.validate(
                    kSearchTickDurationMs,
                    simulationHorizonMs, baselineOnly);
    if (!configurationValidation.configuration) {
        return {{}, configurationValidation.error};
    }
    const SearchComponentConfiguration &configuration =
            *configurationValidation.configuration;
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
                                "Point target cannot be exposed to the condition script.")};
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
    if (!baselineOnly && simulationBackend_ == PhysicsBackend::MultiThreadedCpu) {
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
    if (!baselineOnly && simulationBackend_ == PhysicsBackend::Cuda) {
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
#if FOREVERVALIDATOR_HAS_HIP
    if (!baselineOnly && simulationBackend_ == PhysicsBackend::Hip) {
        if (!hipAvailable_) {
            return {{}, hipStatusText_};
        }
        if (configuration.evaluationTarget.id ==
            kCustomVolumeEntryEvaluationId) {
            return {
                    {},
                    QStringLiteral(
                            "Custom volume targets currently require a CPU "
                            "physics backend.")};
        }
        calibrateCudaParallelSampleCount = hipCalibrationEnabled_;
        if (!calibrateCudaParallelSampleCount) {
            bool parsed = false;
            const QString trimmed = hipParallelSampleCount_.trimmed();
            const uint value = trimmed.toUInt(&parsed);
            if (!parsed || trimmed != hipParallelSampleCount_ || value == 0u) {
                return {{}, QStringLiteral(
                        "HIP parallel samples must be a positive whole number.")};
            }
            parallelSampleCount = value;
        }
    }
#endif
#if FOREVERVALIDATOR_HAS_VULKAN
    if (!baselineOnly && simulationBackend_ == PhysicsBackend::Vulkan) {
        if (!vulkanAvailable_) {
            return {{}, vulkanStatusText_};
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
                vulkanCalibrationEnabled_;
        if (!calibrateCudaParallelSampleCount) {
            bool parsed = false;
            const QString trimmed =
                    vulkanParallelSampleCount_.trimmed();
            const uint value = trimmed.toUInt(&parsed);
            if (!parsed ||
                trimmed != vulkanParallelSampleCount_ ||
                value == 0u) {
                return {
                        {},
                        QStringLiteral(
                                "Vulkan parallel samples must be a positive "
                                "whole number.")};
            }
            parallelSampleCount = value;
        }
    }
#endif

    SearchRequest request{
            packsInfo.absoluteFilePath().toUtf8().toStdString(),
            replayInfo.absoluteFilePath().toUtf8().toStdString()};
    request.backend = baselineOnly ? kAuxiliarySimulationBackend : simulationBackend_;
    request.parallelSampleCount = parallelSampleCount;
    request.calibrateCudaParallelSampleCount =
            calibrateCudaParallelSampleCount;
    request.searchAlgorithm = configuration.searchAlgorithm;
    request.modifiers = configuration.modifiers;
    request.evaluationTarget = configuration.evaluationTarget;
    request.baseInputCommands = parsedBaseInputCommands_;
    request.useCudaSessionSpecialization =
            PhysicsBackendId(simulationBackend_) == "cuda" &&
            cudaSessionSpecializationEnabled_ && cudaFastModeAvailable_;
    request.simulationHorizonMs = simulationHorizonMs;
    request.condition = std::move(condition.program);
    return {std::move(request), {}};
}

void SearchController::refreshSessions() {
    historyHasOlder_ = historyHasNewer_ = false;
    historySelectionExplicit_ = false;
    selectedCycleRestart_ = -1;
    emit searchSessionReset();
    if (!QFileInfo(packsDirectory_).isDir() ||
        !QFileInfo(replayPath_).isFile()) {
        sessionOptions_.clear();
        cycleRows_.clear();
        selectedSessionDirectory_.clear();
        selectedInputsText_.clear();
        emit historyChanged();
        return;
    }
    try {
        SearchRequest request{
                packsDirectory_.toUtf8().toStdString(),
                replayPath_.toUtf8().toStdString()};
        const QString mapKey = SearchSessionStore::Identify(request).mapKey;
        sessionOptions_ = SearchSessionStore::SessionsForMap(mapKey);
        if (sessionOptions_.isEmpty()) {
            selectedSessionDirectory_.clear();
            cycleRows_.clear();
            selectedInputsText_.clear();
            emit historyChanged();
            return;
        }
        selectSession(static_cast<int>(sessionOptions_.size()) - 1);
    } catch (const std::exception &) {
        sessionOptions_.clear();
        cycleRows_.clear();
        selectedSessionDirectory_.clear();
        selectedInputsText_.clear();
        emit historyChanged();
    }
}

void SearchController::selectSession(int index) {
    if (running_ || index < 0 || index >= sessionOptions_.size()) return;
    const QString directory = sessionOptions_[index].toMap()
                                        .value(QStringLiteral("directory"))
                                        .toString();
    if (directory != selectedSessionDirectory_) emit searchSessionReset();
    selectedSessionDirectory_ = directory;
    auto page = SearchSessionStore::ReadCyclePage(selectedSessionDirectory_);
    cycleRows_ = std::move(page.rows);
    historyHasOlder_ = page.hasOlder;
    historyHasNewer_ = page.hasNewer;
    historySelectionExplicit_ = false;
    selectedCycleRestart_ = -1;
    selectedInputsText_.clear();
    emit historyChanged();
    if (!cycleRows_.isEmpty()) selectCycle(
            static_cast<int>(cycleRows_.size()) - 1);
    historySelectionExplicit_ = false;
}

void SearchController::selectCycle(int index) {
    if (index < 0 || index >= cycleRows_.size()) return;
    historySelectionExplicit_ = true;
    selectedCycleRestart_ = cycleRows_[index].toMap().value(QStringLiteral("restart")).toLongLong();
    const QString fileName = cycleRows_[index].toMap()
                                     .value(QStringLiteral("inputFile"))
                                     .toString();
    selectedInputsText_ = SearchSessionStore::Inputs(
            selectedSessionDirectory_, fileName);
    emit historyChanged();
}

void SearchController::changeCyclePage(bool older) {
    if (cycleRows_.isEmpty() || (older ? !historyHasOlder_ : !historyHasNewer_)) return;
    const auto anchor = (older ? cycleRows_.front() : cycleRows_.back()).toMap()
                                .value(QStringLiteral("restart")).toULongLong();
    auto page = SearchSessionStore::ReadCyclePage(selectedSessionDirectory_, anchor, older);
    if (page.rows.isEmpty()) return;
    cycleRows_ = std::move(page.rows);
    historyHasOlder_ = page.hasOlder;
    historyHasNewer_ = page.hasNewer;
    historySelectionExplicit_ = true;
    emit historyChanged();
}

void SearchController::refreshValidation() {
    ++baselineEvaluationGeneration_;
    cancelBaseEvaluation();
    if (!evaluatingBase() && !baseEvaluationResult_.isEmpty()) {
        baseEvaluationResult_.clear();
        baseEvaluationText_ = QStringLiteral("Settings changed; evaluate base inputs again.");
    }
    emit baseEvaluationChanged();
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
    emit baseEvaluationChanged();
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
    emit baseEvaluationChanged();
    emit replayInputStateChanged();
    if (oldCanStart != canStart()) {
        emit canStartChanged();
    }
}

void SearchController::setReplayInputStatusText(const QString &value) {
    if (replayInputStatusText_ == value) {
        return;
    }
    replayInputStatusText_ = value;
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
        if (shuttingDown_ || !packsDirectory_.trimmed().isEmpty() ||
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

void SearchController::flushSettings() {
    if (inputScriptPersistTimer_ != nullptr) inputScriptPersistTimer_->stop();
    persist(kBaseInputScriptKey, baseInputScript_);
    QSettings().sync();
}

void SearchController::requestShutdown() {
    if (shuttingDown_) return;
    shuttingDown_ = true;
    cancelBaseEvaluation();
    if (stopRequested_) stopRequested_->store(true, std::memory_order_relaxed);
    if (cancellationRequested_) cancellationRequested_->store(true, std::memory_order_relaxed);
    for (auto thread : {workerThread_, autoDetectionThread_, inputExtractionThread_, baselineEvaluationThread_})
        if (thread != nullptr) thread->requestInterruption();
    emit canStartChanged();
    emit baseEvaluationChanged();
    emit replayInputStateChanged();
}

bool SearchController::shutdownReady() const {
    for (const auto thread : {workerThread_, autoDetectionThread_, inputExtractionThread_, baselineEvaluationThread_})
        if (thread != nullptr && thread->isRunning()) return false;
    return true;
}

void SearchController::waitForWorker() {
    cancelBaseEvaluation();
    if (baselineEvaluationThread_ != nullptr) {
        disconnect(baselineEvaluationThread_, nullptr, this, nullptr);
        baselineEvaluationThread_->wait();
        delete baselineEvaluationThread_;
        baselineEvaluationThread_ = nullptr;
    }
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
