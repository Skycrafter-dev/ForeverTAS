#include "viewer/video_export_controller.h"
#include "app/app_settings.h"
#include "diagnostics/diagnostics_log.h"

#include "app/system_file_dialog.h"
#include "viewer/race_viewer_controller.h"
#include "viewer/render_module_library.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>

#include <algorithm>
#include <array>
#include <vector>

namespace forevertas::viewer {
namespace {

constexpr char kSettingsGroup[] = "videoExport/";

}  // namespace

VideoExportController::VideoExportController(QObject *parent)
    : QObject(parent) {
    app::AppSettings &settings = app::AppSettings::Instance();
    const QString group = QString::fromLatin1(kSettingsGroup);
    camera_ = std::clamp(settings.value(group + QStringLiteral("camera"), 1).toInt(), 1, 3);
    videoWidth_ = settings.value(group + QStringLiteral("width"), 1920).toInt();
    videoHeight_ = settings.value(group + QStringLiteral("height"), 1080).toInt();
    framesPerSecond_ = settings.value(group + QStringLiteral("fps"), 60).toInt();
    progressTimer_.setInterval(100);
    connect(&progressTimer_, &QTimer::timeout, this,
            &VideoExportController::pollProgress);
}

VideoExportController::~VideoExportController() {
    if (worker_ != nullptr) {
        cancel_ = 1;
        worker_->wait();
        delete worker_;
    }
}

bool VideoExportController::available() const {
    return LoadedRenderModule().exportVideo != nullptr;
}

QObject *VideoExportController::viewer() const { return viewer_; }

void VideoExportController::setViewer(QObject *viewer) {
    auto *const controller = qobject_cast<RaceViewerController *>(viewer);
    if (viewer_ == controller) {
        return;
    }
    viewer_ = controller;
    emit viewerChanged();
}

QString VideoExportController::outputPath() const { return outputPath_; }

void VideoExportController::setOutputPath(const QString &path) {
    if (outputPath_ == path) {
        return;
    }
    outputPath_ = path;
    emit settingsChanged();
}

int VideoExportController::camera() const { return camera_; }

void VideoExportController::setCamera(int camera) {
    camera = std::clamp(camera, 1, 3);
    if (camera_ == camera) {
        return;
    }
    camera_ = camera;
    saveSettings();
    emit settingsChanged();
}

int VideoExportController::videoWidth() const { return videoWidth_; }

void VideoExportController::setVideoWidth(int width) {
    width = std::clamp(width & ~1, 320, 7680);
    if (videoWidth_ == width) {
        return;
    }
    videoWidth_ = width;
    saveSettings();
    emit settingsChanged();
}

int VideoExportController::videoHeight() const { return videoHeight_; }

void VideoExportController::setVideoHeight(int height) {
    height = std::clamp(height & ~1, 240, 4320);
    if (videoHeight_ == height) {
        return;
    }
    videoHeight_ = height;
    saveSettings();
    emit settingsChanged();
}

int VideoExportController::framesPerSecond() const { return framesPerSecond_; }

void VideoExportController::setFramesPerSecond(int framesPerSecond) {
    framesPerSecond = std::clamp(framesPerSecond, 10, 240);
    if (framesPerSecond_ == framesPerSecond) {
        return;
    }
    framesPerSecond_ = framesPerSecond;
    saveSettings();
    emit settingsChanged();
}

bool VideoExportController::running() const { return running_; }

double VideoExportController::progress() const { return progress_; }

QString VideoExportController::status() const { return status_; }

QString VideoExportController::finishedPath() const { return finishedPath_; }

bool VideoExportController::chooseOutput() {
    const QString initial =
            outputPath_.isEmpty() ? defaultOutputPath() : outputPath_;
    QString selected = app::OpenSystemSaveDialog(
            QStringLiteral("Export to MP4"), initial,
            QStringLiteral("MP4 video"), QStringLiteral("mp4"));
    if (selected.isEmpty()) {
        return false;
    }
    if (QFileInfo(selected).suffix().isEmpty()) {
        selected += QStringLiteral(".mp4");
    }
    setOutputPath(selected);
    return true;
}

bool VideoExportController::start() {
    if (running_ || !available()) {
        return false;
    }
    if (viewer_ == nullptr || !viewer_->loaded()) {
        status_ = tr("Load a replay first.");
        emit stateChanged();
        return false;
    }
    if (outputPath_.isEmpty()) {
        setOutputPath(defaultOutputPath());
    }
    QDir().mkpath(QFileInfo(outputPath_).absolutePath());

    const std::vector<FtrmControl> controls =
            RenderModuleControls(viewer_->selectedRunInputs());
    const QByteArray packs = viewer_->loadedPacksDirectory().toUtf8();
    const QByteArray cache = RenderModuleCacheDirectory().toUtf8();
    const QByteArray map = viewer_->loadedReplayPath().toUtf8();
    const QByteArray output = outputPath_.toUtf8();
    FtrmVideoSettings settings{};
    settings.controlCount = static_cast<int>(controls.size());
    settings.durationMs = static_cast<int>(viewer_->durationMs());
    settings.camera = camera_;
    settings.width = videoWidth_;
    settings.height = videoHeight_;
    settings.framesPerSecond = framesPerSecond_;

    cancel_ = 0;
    framesDone_ = 0;
    frameCount_ = 0;
    progress_ = 0.0;
    running_ = true;
    runningPath_ = outputPath_;
    status_ = tr("Loading the map…");
    diagnostics::Log("video export: %s, %dx%d at %d fps, camera %d, %d ms, %zu controls, map %s",
                     qUtf8Printable(outputPath_), videoWidth_, videoHeight_, framesPerSecond_,
                     static_cast<int>(camera_), settings.durationMs, controls.size(), map.constData());
    emit progressChanged();
    emit stateChanged();

    auto context = std::make_shared<RenderModuleWorkerContext>();
    worker_ = QThread::create([this, controls, packs, cache, map, output,
                               settings, context]() mutable {
        settings.useCurrentContext = context->makeCurrent() ? 1 : 0;
        diagnostics::Log("video export: OpenGL context %s; the module starts",
                         settings.useCurrentContext != 0 ? "shared" : "unavailable (module's own)");
        settings.packsDirectory = packs.constData();
        settings.cacheDirectory = cache.constData();
        settings.mapPath = map.constData();
        settings.outputPath = output.constData();
        settings.controls = controls.data();
        std::array<char, 1024> error{};
        const int succeeded = LoadedRenderModule().exportVideo(
                &settings,
                [](void *user, int done, int count) {
                    auto *const self = static_cast<VideoExportController *>(user);
                    self->framesDone_.store(done, std::memory_order_relaxed);
                    self->frameCount_.store(count, std::memory_order_relaxed);
                },
                this, &cancel_, error.data(), static_cast<int>(error.size()));
        context->release();
        diagnostics::Log("video export: the module returned %d%s%s", succeeded,
                         error[0] != '\0' ? ": " : "", error.data());
        const QString message = QString::fromUtf8(error.data());
        QMetaObject::invokeMethod(
                this,
                [this, succeeded, message, context] {
                    finish(succeeded != 0, message);
                },
                Qt::QueuedConnection);
    });
    worker_->setObjectName(QStringLiteral("VideoExport"));
    worker_->start();
    progressTimer_.start();
    return true;
}

void VideoExportController::abort() {
    if (running_) {
        cancel_ = 1;
        status_ = tr("Stopping…");
        emit stateChanged();
    }
}

void VideoExportController::showFinished() const {
    if (!finishedPath_.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(
                QFileInfo(finishedPath_).absolutePath()));
    }
}

void VideoExportController::pollProgress() {
    const int count = frameCount_.load(std::memory_order_relaxed);
    const int done = framesDone_.load(std::memory_order_relaxed);
    if (count <= 0) {
        return;
    }
    const double progress = static_cast<double>(done) / count;
    if (static_cast<int>(progress * 10.0) != static_cast<int>(progress_ * 10.0)) {
        diagnostics::Log("video export: frame %d of %d", done, count);
    }
    if (progress != progress_) {
        progress_ = progress;
        emit progressChanged();
    }
    if (cancel_ == 0) {
        const QString status = tr("Frame %1 of %2").arg(done).arg(count);
        if (status != status_) {
            status_ = status;
            emit stateChanged();
        }
    }
}

void VideoExportController::finish(bool succeeded, const QString &error) {
    progressTimer_.stop();
    if (worker_ != nullptr) {
        worker_->wait();
        delete worker_;
        worker_ = nullptr;
    }
    running_ = false;
    if (succeeded) {
        progress_ = 1.0;
        finishedPath_ = runningPath_;
        status_ = tr("Saved %1").arg(QDir::toNativeSeparators(runningPath_));
    } else if (cancel_ != 0) {
        status_ = tr("Stopped.");
    } else {
        status_ = error.isEmpty() ? tr("The video could not be made.") : error;
    }
    diagnostics::Log("video export: %s", qUtf8Printable(status_));
    emit progressChanged();
    emit stateChanged();
}

void VideoExportController::saveSettings() const {
    app::AppSettings &settings = app::AppSettings::Instance();
    const QString group = QString::fromLatin1(kSettingsGroup);
    settings.setValue(group + QStringLiteral("camera"), camera_);
    settings.setValue(group + QStringLiteral("width"), videoWidth_);
    settings.setValue(group + QStringLiteral("height"), videoHeight_);
    settings.setValue(group + QStringLiteral("fps"), framesPerSecond_);
}

QString VideoExportController::defaultOutputPath() const {
    QString folder =
            QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    if (folder.isEmpty()) {
        folder = QDir::homePath();
    }
    QString name = QStringLiteral("run");
    if (viewer_ != nullptr && !viewer_->loadedReplayPath().isEmpty()) {
        name = QFileInfo(viewer_->loadedReplayPath()).fileName();
        for (const QString &suffix :
             {QStringLiteral(".Replay.Gbx"), QStringLiteral(".Challenge.Gbx"),
              QStringLiteral(".Gbx")}) {
            if (name.endsWith(suffix, Qt::CaseInsensitive)) {
                name.chop(suffix.size());
                break;
            }
        }
    }
    return folder + QLatin1Char('/') + name + QStringLiteral(".mp4");
}

}  // namespace forevertas::viewer
