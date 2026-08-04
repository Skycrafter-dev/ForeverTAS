#include "viewer/race_timeline_item.h"
#include "viewer/race_viewer_controller.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QFileInfo>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <iostream>

namespace {

using forevertas::viewer::RaceTimelineItem;
using forevertas::viewer::RaceViewerController;
using forevervalidator::experimental::PhysicsSandboxInputAction;
using forevervalidator::experimental::PhysicsSandboxInputEvent;
using forevervalidator::experimental::PhysicsSandboxInputValueKind;
using forevervalidator::experimental::PhysicsSandboxSwitchState;

std::vector<forevertas::SearchTimelineFrame> SyntheticSearchTimeline() {
    std::vector<forevertas::SearchTimelineFrame> frames;
    frames.reserve(1001u);
    const QQuaternion rotation =
            (QQuaternion::fromAxisAndAngle(
                     0.0f, 1.0f, 0.0f, 35.0f) *
             QQuaternion::fromAxisAndAngle(
                     1.0f, 0.0f, 0.0f, 20.0f))
                    .normalized();
    for (std::int64_t tick = 0; tick <= 1000; ++tick) {
        frames.push_back({
                tick * 10,
                static_cast<float>(tick) * 0.01f,
                0.0f,
                0.0f,
                rotation.x(),
                rotation.y(),
                rotation.z(),
                rotation.scalar(),
                tick >= 10 && tick < 700 ? 1.0f : 0.0f,
                tick >= 700 && tick < 800 ? 1.0f : 0.0f,
                tick >= 100 && tick < 300
                        ? -0.5f
                        : tick >= 300 && tick < 500 ? 0.5f : 0.0f,
                tick >= 741 ? 2u : tick >= 145 ? 1u : 0u,
                2u,
                0u,
                1u,
                tick >= 1000,
                tick >= 1000
                        ? std::optional<std::uint32_t>(9995u)
                        : std::nullopt});
    }
    return frames;
}

PhysicsSandboxInputEvent SwitchInput(
        std::int32_t timeMs,
        PhysicsSandboxInputAction action,
        bool pressed) {
    PhysicsSandboxInputEvent event;
    event.timeMs = timeMs;
    event.action = action;
    event.value.kind = PhysicsSandboxInputValueKind::Switch;
    event.value.switchState = pressed
            ? PhysicsSandboxSwitchState::Pressed
            : PhysicsSandboxSwitchState::Released;
    return event;
}

PhysicsSandboxInputEvent AnalogInput(
        std::int32_t timeMs,
        PhysicsSandboxInputAction action,
        forevervalidator::AnalogInputState value) {
    PhysicsSandboxInputEvent event;
    event.timeMs = timeMs;
    event.action = action;
    event.value.kind = PhysicsSandboxInputValueKind::Analog;
    event.value.analog = value;
    return event;
}

std::vector<PhysicsSandboxInputEvent> SyntheticSearchInputs() {
    return {
            SwitchInput(0, PhysicsSandboxInputAction::RaceRunning, true),
            SwitchInput(0, PhysicsSandboxInputAction::Accelerate, true),
            AnalogInput(500, PhysicsSandboxInputAction::Steer, -32768),
            SwitchInput(1000, PhysicsSandboxInputAction::Brake, true),
            SwitchInput(1500, PhysicsSandboxInputAction::SteerRight, true)};
}

template <typename Predicate>
bool WaitUntil(Predicate predicate, int timeoutMs = 60000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    return predicate();
}

qint64 FindActivityTick(const RaceViewerController &viewer, char channel) {
    for (qint64 tick = 0; tick < viewer.tickCount(); ++tick) {
        const auto sample = viewer.inputSample(tick);
        const bool active = channel == 'l'
                ? sample.steering < -0.01f
                : channel == 'r'
                ? sample.steering > 0.01f
                : channel == 'a'
                ? sample.accelerate > 0.0f
                : sample.brake > 0.0f;
        if (active) {
            return tick;
        }
    }
    return -1;
}

bool TimelinePaintsColor(RaceTimelineItem &timeline,
                         RaceViewerController &viewer,
                         qint64 tick,
                         const QColor &color) {
    if (tick < 0) {
        return false;
    }
    viewer.setCurrentTick(tick);
    QImage image(252, 600, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    timeline.paint(&painter);
    painter.end();
    const QRgb expected = color.rgba();
    for (int y = 0; y < image.height(); ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(
                image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (line[x] == expected) {
                return true;
            }
        }
    }
    return false;
}

QImage RenderTimeline(RaceTimelineItem &timeline) {
    QImage image(252, 600, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    timeline.paint(&painter);
    painter.end();
    return image;
}

bool PixelIs(const QImage &image, int x, int y, const QColor &color) {
    return image.pixelColor(x, y) == color;
}

bool PixelIsAnyRulerMark(const QImage &image, int x, int y) {
    const QColor pixel = image.pixelColor(x, y);
    return pixel == QColor(QStringLiteral("#9aa69e")) ||
            pixel == QColor(QStringLiteral("#59635d")) ||
            pixel == QColor(QStringLiteral("#343b37"));
}

int RulerMarkLengthNear(const QImage &image, int expectedY) {
    const QColor rulerBackground(QStringLiteral("#0c100e"));
    int longest = 0;
    for (int y = expectedY - 1; y <= expectedY + 1; ++y) {
        if (y < 0 || y >= image.height()) {
            continue;
        }
        int leftmost = 50;
        for (int x = 35; x < 50; ++x) {
            if (image.pixelColor(x, y) != rulerBackground) {
                leftmost = x;
                break;
            }
        }
        longest = std::max(longest, 50 - leftmost);
    }
    return longest;
}

void SendTimelineMouseEvent(RaceTimelineItem &timeline,
                            QEvent::Type type,
                            Qt::MouseButton button,
                            Qt::MouseButtons buttons,
                            const QPointF &position) {
    QMouseEvent event(
            type,
            position,
            position,
            button,
            buttons,
            Qt::NoModifier);
    QCoreApplication::sendEvent(&timeline, &event);
}

void DragTimeline(RaceTimelineItem &timeline,
                  Qt::MouseButton button,
                  const QPointF &start,
                  const QPointF &end) {
    SendTimelineMouseEvent(
            timeline, QEvent::MouseButtonPress, button, button, start);
    SendTimelineMouseEvent(
            timeline, QEvent::MouseMove, Qt::NoButton, button, end);
    SendTimelineMouseEvent(
            timeline, QEvent::MouseButtonRelease, button, Qt::NoButton, end);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "usage: forevertas-viewer-smoke <Packs> <replay>\n";
        return 2;
    }

    QGuiApplication application(argc, argv);
    QCoreApplication::setOrganizationName(
            QStringLiteral("ForeverTASTests"));
    QCoreApplication::setApplicationName(
            QStringLiteral("ViewerSmoke"));
    QStandardPaths::setTestModeEnabled(true);
    QSettings().clear();
    {
        RaceViewerController settingsProbe;
        if (settingsProbe.cameraPreset() != 1) {
            std::cerr << "camera preset did not default to far\n";
            return 1;
        }
        if (settingsProbe.telemetryScript() !=
                    settingsProbe.defaultTelemetryScript() ||
            settingsProbe.renderTelemetry(
                    settingsProbe.telemetryScript(),
                    QVector3D(1.25f, -0.001f, 3.5f)) !=
                    QStringLiteral(
                            "Camera pos: X 1.25   Y 0.00   Z 3.50") ||
            settingsProbe.telemetryScriptError(
                    QStringLiteral("{missing.field}")) !=
                    QStringLiteral(
                            "Unknown telemetry field {missing.field}")) {
            std::cerr << "scripted telemetry defaults are invalid\n";
            return 1;
        }
        settingsProbe.setCameraPreset(2);
        settingsProbe.setTelemetryScript(
                QStringLiteral("Time {time.ms:0} ms"));
    }
    {
        RaceViewerController settingsProbe;
        if (settingsProbe.cameraPreset() != 2 ||
            settingsProbe.telemetryScript() !=
                    QStringLiteral("Time {time.ms:0} ms")) {
            std::cerr << "viewer settings were not persisted\n";
            return 1;
        }
        settingsProbe.setCameraPreset(1);
        settingsProbe.setTelemetryScript(
                settingsProbe.defaultTelemetryScript());
    }
    RaceViewerController viewer;
    viewer.startManualDrive();
    if (viewer.manualDriving() ||
        viewer.statusText() != QStringLiteral(
                "Load a map before starting manual drive.")) {
        std::cerr << "manual drive started without a loaded map\n";
        return 1;
    }
    const QString packsDirectory = QString::fromLocal8Bit(argv[1]);
    const QString replayPath = QString::fromLocal8Bit(argv[2]);
    const std::vector<forevertas::SearchTimelineFrame> searchTimeline =
            SyntheticSearchTimeline();
    const QString trajectoryScript = QStringLiteral(
            "0.00 press up\n"
            "0.10 rel up");
    viewer.setPreviewInputScript(trajectoryScript);
    int exitCode = 1;
    bool completed = false;
    bool verificationStarted = false;
    bool mapOnlyStateObserved = false;
    bool manualVerificationStarted = false;
    bool manualDriveValid = false;
    bool takeoverVerificationStarted = false;
    bool manualTakeoverValid = false;
    bool manualInitialNeutral = false;
    bool trajectoryPreviewValid = false;
    bool improvementTrajectoriesValid = false;
    bool cameraPresetsValid = false;
    QVector3D manualInitialPosition;
    QVector3D manualInitialCameraPosition;
    QObject::connect(
            &viewer,
            &forevertas::viewer::RaceViewerController::stateChanged,
            &application,
            [&]() {
                if (completed || verificationStarted) {
                    return;
                }
                if (viewer.loading()) {
                    return;
                }
                if (viewer.loaded()) {
                    if (viewer.runCount() == 1 &&
                        viewer.selectedRunId() == QStringLiteral("preview")) {
                        if (manualVerificationStarted) {
                            return;
                        }
                        mapOnlyStateObserved = viewer.durationMs() > 0 &&
                                viewer.tickCount() > 1 &&
                                viewer.statusText() ==
                                        QStringLiteral("Map loaded");
                        if (!mapOnlyStateObserved) {
                            completed = true;
                            std::cerr << "map load published a replay run\n";
                            application.quit();
                            return;
                        }
                        manualVerificationStarted = true;
                        const QVector3D farPosition =
                                viewer.carCameraPosition();
                        const QVector3D farLook =
                                viewer.carCameraTarget() - farPosition;
                        const QVector3D farToCar =
                                viewer.carPosition() - farPosition;
                        const QVector3D carForward =
                                viewer.carRotation().rotatedVector(
                                        QVector3D(0.0f, 0.0f, 1.0f));
                        const QVector3D carToFarCamera =
                                farPosition - viewer.carPosition();
                        const float farLookAlignment = QVector3D::dotProduct(
                                farLook.normalized(), farToCar.normalized());
                        const bool farLooksTowardCar =
                                farLook.lengthSquared() > 0.000001f &&
                                farToCar.lengthSquared() > 0.000001f &&
                                farLookAlignment > 0.0f;
                        const float farBehindAlignment = QVector3D::dotProduct(
                                carForward.normalized(),
                                carToFarCamera.normalized());
                        const bool farStartsBehindCar =
                                carForward.lengthSquared() > 0.000001f &&
                                carToFarCamera.lengthSquared() > 0.000001f &&
                                farBehindAlignment < -0.25f;
                        const double farFov =
                                viewer.carCameraFieldOfView();
                        viewer.setCameraPreset(2);
                        const QVector3D nearPosition =
                                viewer.carCameraPosition();
                        const QVector3D nearLook =
                                viewer.carCameraTarget() - nearPosition;
                        const QVector3D nearToCar =
                                viewer.carPosition() - nearPosition;
                        const QVector3D carToNearCamera =
                                nearPosition - viewer.carPosition();
                        const float nearLookAlignment = QVector3D::dotProduct(
                                nearLook.normalized(), nearToCar.normalized());
                        const float nearBehindAlignment =
                                QVector3D::dotProduct(
                                        carForward.normalized(),
                                        carToNearCamera.normalized());
                        const bool nearValid =
                                viewer.carCameraAvailable() &&
                                !viewer.hideSelectedCar() &&
                                (nearPosition - farPosition)
                                                .lengthSquared() >
                                        0.000001f &&
                                nearLook.lengthSquared() > 0.000001f &&
                                nearToCar.lengthSquared() > 0.000001f &&
                                nearLookAlignment > 0.0f &&
                                nearBehindAlignment < -0.25f;
                        viewer.setCameraPreset(3);
                        const QVector3D internalPosition =
                                viewer.carCameraPosition();
                        const bool internalValid =
                                viewer.carCameraAvailable() &&
                                viewer.hideSelectedCar() &&
                                (internalPosition - viewer.carPosition())
                                                .lengthSquared() <
                                        16.0f;
                        viewer.setCameraPreset(1);
                        cameraPresetsValid =
                                viewer.carCameraAvailable() &&
                                viewer.cameraPreset() == 1 &&
                                !viewer.hideSelectedCar() &&
                                std::isfinite(farFov) && farFov >= 20.0 &&
                                farFov <= 150.0 && farLooksTowardCar &&
                                farStartsBehindCar &&
                                nearValid && internalValid;
                        if (!cameraPresetsValid) {
                            completed = true;
                            std::cerr
                                    << "camera preset checks failed: "
                                    << "available="
                                    << viewer.carCameraAvailable()
                                    << ", farFov=" << farFov
                                    << ", farLookAlignment="
                                    << farLookAlignment
                                    << ", farBehindAlignment="
                                    << farBehindAlignment
                                    << ", nearLookAlignment="
                                    << nearLookAlignment
                                    << ", nearBehindAlignment="
                                    << nearBehindAlignment
                                    << ", nearDelta="
                                    << (nearPosition - farPosition)
                                               .lengthSquared()
                                    << ", internalDistance="
                                    << (internalPosition -
                                        viewer.carPosition())
                                               .lengthSquared()
                                    << '\n';
                            application.quit();
                            return;
                        }
                        const QVariantList trajectoryPaths =
                                viewer.trajectoryPaths();
                        const bool trajectoryGeometryValid =
                                trajectoryPaths.size() == 1 &&
                                trajectoryPaths.front()
                                        .toMap()
                                        .value(QStringLiteral("geometry"))
                                        .value<QObject *>() != nullptr &&
                                trajectoryPaths.front()
                                        .toMap()
                                        .value(QStringLiteral("name"))
                                        .toString() ==
                                        QStringLiteral("Inputs") &&
                                trajectoryPaths.front()
                                                .toMap()
                                                .value(QStringLiteral("kind"))
                                                .toString() ==
                                        QStringLiteral("preview") &&
                                trajectoryPaths.front()
                                        .toMap()
                                        .value(QStringLiteral("color"))
                                        .toString() ==
                                        QStringLiteral("#41c979");
                        QObject *const previewGeometry =
                                trajectoryPaths.front()
                                        .toMap()
                                        .value(QStringLiteral("geometry"))
                                        .value<QObject *>();
                        const bool previewToggleInitiallyVisible =
                                viewer.hasTrajectoryForRun(
                                        QStringLiteral("preview")) &&
                                viewer.trajectoryVisibleForRun(
                                        QStringLiteral("preview"));
                        viewer.setTrajectoryVisibleForRun(
                                QStringLiteral("preview"), false);
                        const bool previewToggleHidden =
                                !viewer.trajectoryVisibleForRun(
                                        QStringLiteral("preview")) &&
                                !viewer.trajectoryPaths()
                                         .front()
                                         .toMap()
                                         .value(QStringLiteral("visible"))
                                         .toBool();
                        viewer.setTrajectoryVisibleForRun(
                                QStringLiteral("preview"), true);
                        viewer.jumpToEnd();
                        const QVector3D shortAccelerationPosition =
                                viewer.carPosition();
                        viewer.setPreviewInputScript(
                                QStringLiteral(
                                        "0.00 press up\n"
                                        "0.20 rel up"));
                        const bool valuePreviewReady = WaitUntil([&]() {
                            return viewer.runCount() == 1 &&
                                    viewer.currentInputScript().contains(
                                            QStringLiteral("0.20 rel up"));
                        });
                        viewer.jumpToEnd();
                        const QVector3D valueEditedPosition =
                                viewer.carPosition();
                        const bool valueEditApplied =
                                valuePreviewReady &&
                                viewer.trajectoryCount() == 1 &&
                                viewer.runCount() == 1 &&
                                viewer.currentInputScript().contains(
                                        QStringLiteral("0.20 rel up")) &&
                                (valueEditedPosition -
                                 shortAccelerationPosition)
                                                .lengthSquared() >
                                        0.000001f;
                        viewer.setPreviewInputScript(
                                QStringLiteral(
                                        "0.00 press up\n"
                                        "0.00 press left\n"
                                        "0.20 rel left\n"
                                        "0.20 rel up"));
                        const bool eventPreviewReady = WaitUntil([&]() {
                            return viewer.runCount() == 1 &&
                                    FindActivityTick(viewer, 'l') >= 0;
                        });
                        viewer.jumpToEnd();
                        const bool eventEditApplied =
                                eventPreviewReady &&
                                viewer.trajectoryCount() == 1 &&
                                viewer.runCount() == 1 &&
                                viewer.trajectoryPaths()
                                                .front()
                                                .toMap()
                                                .value(QStringLiteral(
                                                        "geometry"))
                                                .value<QObject *>() ==
                                        previewGeometry &&
                                viewer.previewInputScript().contains(
                                        QStringLiteral("press left")) &&
                                FindActivityTick(viewer, 'l') >= 0;
                        const QString snapshotScript = QStringLiteral(
                                "0.00 press up\n"
                                "1.00 press left\n"
                                "1.20 rel left\n"
                                "2.00 rel up");
                        viewer.setPreviewInputScript(snapshotScript);
                        const bool snapshotBaselineReady = WaitUntil([&]() {
                            return FindActivityTick(viewer, 'l') >= 100 &&
                                    FindActivityTick(viewer, 'r') < 0;
                        });
                        viewer.jumpToEnd();
                        const QVector3D snapshotBaselinePosition =
                                viewer.carPosition();
                        viewer.setPreviewInputScript(QStringLiteral(
                                "0.00 press up\n"
                                "1.00 press right\n"
                                "1.20 rel right\n"
                                "2.00 rel up"));
                        const bool snapshotEditReady = WaitUntil([&]() {
                            return FindActivityTick(viewer, 'r') >= 100 &&
                                    FindActivityTick(viewer, 'l') < 0;
                        });
                        viewer.setPreviewInputScript(snapshotScript);
                        const bool snapshotRestoreReady = WaitUntil([&]() {
                            return FindActivityTick(viewer, 'l') >= 100 &&
                                    FindActivityTick(viewer, 'r') < 0;
                        });
                        viewer.jumpToEnd();
                        const bool snapshotRoundTripExact =
                                snapshotBaselineReady && snapshotEditReady &&
                                snapshotRestoreReady &&
                                (viewer.carPosition() -
                                 snapshotBaselinePosition)
                                                .lengthSquared() <
                                        0.00000001f;
                        viewer.jumpToStart();
                        viewer.play();
                        viewer.setPreviewInputScript(
                                QStringLiteral(
                                        "0.00 press up\n"
                                        "0.00 press left\n"
                                        "0.30 rel left\n"
                                        "0.30 rel up"));
                        const bool playbackPreviewReady = WaitUntil([&]() {
                            return viewer.currentInputScript().contains(
                                    QStringLiteral("0.30 rel up"));
                        });
                        const bool playbackContinuedAfterEdit =
                                playbackPreviewReady &&
                                viewer.playing() &&
                                viewer.selectedRunId() ==
                                        QStringLiteral("preview");
                        viewer.pause();
                        viewer.setPreviewInputScript(
                                QStringLiteral("not a command"));
                        const bool invalidEditCleared = WaitUntil([&]() {
                            return viewer.trajectoryCount() == 0 &&
                                    viewer.runCount() == 0 &&
                                    viewer.selectedRunId().isEmpty();
                        }) &&
                                viewer.trajectoryCount() == 0 &&
                                viewer.runCount() == 0 &&
                                viewer.selectedRunId().isEmpty();
                        viewer.setPreviewInputScript(
                                QStringLiteral(
                                        "0.00 press up\n"
                                        "0.00 press left\n"
                                        "0.20 rel left\n"
                                        "0.20 rel up"));
                        const bool finalPreviewReady = WaitUntil([&]() {
                            return viewer.runCount() == 1 &&
                                    FindActivityTick(viewer, 'l') >= 0;
                        });
                        trajectoryPreviewValid =
                                trajectoryGeometryValid &&
                                previewToggleInitiallyVisible &&
                                previewToggleHidden &&
                                valueEditApplied &&
                                eventEditApplied &&
                                snapshotRoundTripExact &&
                                playbackContinuedAfterEdit &&
                                invalidEditCleared &&
                                finalPreviewReady &&
                                viewer.previewInputScript().contains(
                                        QStringLiteral("press left")) &&
                                viewer.trajectoryCount() == 1 &&
                                viewer.runCount() == 1 &&
                                viewer.tickCount() > 1 &&
                                viewer.durationMs() > 0 &&
                                viewer.selectedRunId() ==
                                        QStringLiteral("preview");
                        if (!trajectoryPreviewValid) {
                            completed = true;
                            std::cerr
                                    << "automatic trajectory preview checks failed: "
                                    << "geometry="
                                    << trajectoryGeometryValid
                                    << ", valueEdit=" << valueEditApplied
                                    << ", eventEdit=" << eventEditApplied
                                    << ", snapshotRoundTrip="
                                    << snapshotRoundTripExact
                                    << ", playbackEdit="
                                    << playbackContinuedAfterEdit
                                    << ", invalidClear="
                                    << invalidEditCleared
                                    << ", count="
                                    << viewer.trajectoryCount()
                                    << ", runs=" << viewer.runCount()
                                    << '\n';
                            application.quit();
                            return;
                        }
                        std::vector<forevertas::SearchTimelineFrame>
                                firstImprovement = searchTimeline;
                        std::vector<forevertas::SearchTimelineFrame>
                                secondImprovement = searchTimeline;
                        for (forevertas::SearchTimelineFrame &frame :
                             firstImprovement) {
                            frame.positionX += 3.0f;
                        }
                        for (forevertas::SearchTimelineFrame &frame :
                             secondImprovement) {
                            frame.positionX += 6.0f;
                        }
                        viewer.addSearchImprovement(
                                packsDirectory,
                                replayPath,
                                firstImprovement,
                                QStringLiteral("optimized-cpu"),
                                42u,
                                1u);
                        viewer.addSearchImprovement(
                                packsDirectory,
                                replayPath,
                                secondImprovement,
                                QStringLiteral("optimized-cpu"),
                                42u,
                                2u);
                        viewer.addSearchImprovement(
                                packsDirectory,
                                replayPath,
                                firstImprovement,
                                QStringLiteral("optimized-cpu"),
                                42u,
                                1u);
                        std::vector<forevertas::SearchTimelineFrame>
                                invalidImprovement = firstImprovement;
                        invalidImprovement.front().timeMs = 10;
                        viewer.addSearchImprovement(
                                packsDirectory,
                                replayPath,
                                invalidImprovement,
                                QStringLiteral("optimized-cpu"),
                                42u,
                                3u);
                        const QVariantList improvedPaths =
                                viewer.trajectoryPaths();
                        improvementTrajectoriesValid =
                                viewer.trajectoryCount() == 3 &&
                                improvedPaths.size() == 3 &&
                                improvedPaths.at(1)
                                                .toMap()
                                                .value(QStringLiteral("name"))
                                                .toString() ==
                                        QStringLiteral("Improvement 1") &&
                                improvedPaths.at(1)
                                                .toMap()
                                                .value(QStringLiteral("kind"))
                                                .toString() ==
                                        QStringLiteral("improvement") &&
                                std::fabs(
                                        improvedPaths.at(1)
                                                        .toMap()
                                                        .value(QStringLiteral(
                                                                "opacity"))
                                                        .toDouble() -
                                        0.3) < 0.001 &&
                                improvedPaths.at(2)
                                                .toMap()
                                                .value(QStringLiteral("name"))
                                                .toString() ==
                                        QStringLiteral("Improvement 2") &&
                                std::fabs(
                                        improvedPaths.at(2)
                                                        .toMap()
                                                        .value(QStringLiteral(
                                                                "opacity"))
                                                        .toDouble() -
                                        0.96) < 0.001 &&
                                improvedPaths.at(1)
                                                .toMap()
                                                .value(QStringLiteral(
                                                        "geometry"))
                                                .value<QObject *>() !=
                                        nullptr &&
                                improvedPaths.at(2)
                                                .toMap()
                                                .value(QStringLiteral(
                                                        "geometry"))
                                                .value<QObject *>() !=
                                        nullptr;
                        if (!improvementTrajectoriesValid) {
                            completed = true;
                            std::cerr
                                    << "improvement trajectory checks failed: "
                                    << viewer.trajectoryCount() << '\n';
                            application.quit();
                            return;
                        }
                        viewer.startManualDrive();
                        if (!viewer.manualDriving() ||
                            viewer.selectedRunId() !=
                                    QStringLiteral("manual") ||
                            viewer.tickCount() != 1) {
                            completed = true;
                            std::cerr << "manual drive did not start\n";
                            application.quit();
                            return;
                        }
                        const auto initialManualInput =
                                viewer.inputSample(0);
                        manualInitialNeutral =
                                initialManualInput.steering == 0.0f &&
                                initialManualInput.accelerate == 0.0f &&
                                initialManualInput.brake == 0.0f;
                        manualInitialPosition = viewer.carPosition();
                        manualInitialCameraPosition =
                                viewer.carCameraPosition();
                        cameraPresetsValid &=
                                viewer.carCameraAvailable() &&
                                viewer.cameraPreset() == 1;
                        viewer.setManualInput(
                                QStringLiteral("left"), true);
                        viewer.setManualInput(
                                QStringLiteral("right"), true);
                        viewer.setManualInput(
                                QStringLiteral("accelerate"), true);
                        QTimer::singleShot(
                                200,
                                &application,
                                [&]() {
                                    const auto bothPressed =
                                            viewer.inputSample(
                                                    viewer.currentTick());
                                    const bool leftPriority =
                                            viewer.manualDriving() &&
                                            viewer.currentTick() >= 10 &&
                                            bothPressed.steering == -1.0f &&
                                            bothPressed.accelerate == 1.0f &&
                                            bothPressed.brake == 0.0f;
                                    const bool physicsAdvanced =
                                            (viewer.carPosition() -
                                             manualInitialPosition)
                                                    .lengthSquared() >
                                            0.000001f;
                                    const bool cameraFollowedManualDrive =
                                            viewer.carCameraAvailable() &&
                                            (viewer.carCameraPosition() -
                                             manualInitialCameraPosition)
                                                    .lengthSquared() >
                                            0.000001f;
                                    viewer.setManualInput(
                                            QStringLiteral("left"), false);
                                    viewer.setManualInput(
                                            QStringLiteral("accelerate"),
                                            false);
                                    viewer.setManualInput(
                                            QStringLiteral("brake"), true);
                                    QTimer::singleShot(
                                            60,
                                            &application,
                                            [&, leftPriority,
                                             physicsAdvanced,
                                             cameraFollowedManualDrive]() {
                                                const auto rightPressed =
                                                        viewer.inputSample(
                                                                viewer.currentTick());
                                                const bool rightAfterRelease =
                                                        rightPressed.steering ==
                                                                1.0f &&
                                                        rightPressed.accelerate ==
                                                                0.0f &&
                                                        rightPressed.brake ==
                                                                1.0f;
                                                viewer.releaseManualInputs();
                                                viewer.stopManualDrive();
                                                const QString manualScript =
                                                        viewer.currentInputScript();
                                                const bool manualCopyValid =
                                                        viewer.canCopyCurrentInputs() &&
                                                        manualScript.contains(
                                                                QStringLiteral(
                                                                        " press left")) &&
                                                        manualScript.contains(
                                                                QStringLiteral(
                                                                        " press right")) &&
                                                        manualScript.contains(
                                                                QStringLiteral(
                                                                        " press up")) &&
                                                        manualScript.contains(
                                                                QStringLiteral(
                                                                        " press down")) &&
                                                        manualScript.contains(
                                                                QStringLiteral(
                                                                        " rel right")) &&
                                                        manualScript.contains(
                                                                QStringLiteral(
                                                                        " rel down"));
                                                const bool stoppedCleanly =
                                                        !viewer.manualDriving() &&
                                                        !viewer.manualLeft() &&
                                                        !viewer.manualRight() &&
                                                        !viewer.manualAccelerate() &&
                                                        !viewer.manualBrake() &&
                                                        viewer.tickCount() >=
                                                                15;
                                                viewer.startManualDrive();
                                                const bool restartedCleanly =
                                                        viewer.manualDriving() &&
                                                        viewer.tickCount() ==
                                                                1 &&
                                                        viewer.timeMs() == 0;
                                                viewer.setManualInput(
                                                        QStringLiteral(
                                                                "accelerate"),
                                                        true);
                                                QEventLoop accelerationLoop;
                                                QTimer::singleShot(
                                                        160,
                                                        &accelerationLoop,
                                                        &QEventLoop::quit);
                                                accelerationLoop.exec();
                                                const float distanceBeforeRespawn =
                                                        (viewer.carPosition() -
                                                         manualInitialPosition)
                                                                .lengthSquared();
                                                const bool respawnQueued =
                                                        viewer.respawnManualDrive();
                                                const qint64 tickBeforeRespawn =
                                                        viewer.currentTick();
                                                QEventLoop respawnLoop;
                                                QTimer::singleShot(
                                                        80,
                                                        &respawnLoop,
                                                        &QEventLoop::quit);
                                                respawnLoop.exec();
                                                const bool respawnExecuted =
                                                        viewer.manualDriving() &&
                                                        viewer.currentTick() >
                                                                tickBeforeRespawn &&
                                                        viewer.manualAccelerate() &&
                                                        viewer.inputSample(
                                                                      viewer.currentTick())
                                                                        .accelerate >
                                                                0.99f &&
                                                        viewer.currentInputScript()
                                                                .contains(
                                                                        QStringLiteral(
                                                                                "press enter"));
                                                const float distanceAfterRespawn =
                                                        (viewer.carPosition() -
                                                         manualInitialPosition)
                                                                .lengthSquared();
                                                const bool respawnResetVehicle =
                                                        distanceBeforeRespawn >
                                                                0.000001f &&
                                                        distanceAfterRespawn <
                                                                distanceBeforeRespawn *
                                                                        0.25f;
                                                const bool giveUpRestarted =
                                                        viewer.giveUpManualDrive() &&
                                                        viewer.manualDriving() &&
                                                        viewer.tickCount() ==
                                                                1 &&
                                                        viewer.timeMs() == 0 &&
                                                        viewer.manualAccelerate() &&
                                                        !viewer.currentInputScript()
                                                                 .contains(
                                                                         QStringLiteral(
                                                                                 "press enter"));
                                                const bool respawnAfterGiveUp =
                                                        viewer.respawnManualDrive();
                                                QEventLoop respawnAfterGiveUpLoop;
                                                QTimer::singleShot(
                                                        30,
                                                        &respawnAfterGiveUpLoop,
                                                        &QEventLoop::quit);
                                                respawnAfterGiveUpLoop.exec();
                                                const bool respawnAfterGiveUpExecuted =
                                                        viewer.manualAccelerate() &&
                                                        viewer.inputSample(
                                                                      viewer.currentTick())
                                                                        .accelerate >
                                                                0.99f &&
                                                        viewer.currentInputScript()
                                                                .contains(
                                                                        QStringLiteral(
                                                                                "press enter"));
                                                viewer.stopManualDrive();
                                                const bool actionsRejectedWhenStopped =
                                                        !viewer.respawnManualDrive() &&
                                                        !viewer.giveUpManualDrive();
                                                manualDriveValid =
                                                        manualInitialNeutral &&
                                                        leftPriority &&
                                                        physicsAdvanced &&
                                                        cameraFollowedManualDrive &&
                                                        rightAfterRelease &&
                                                        manualCopyValid &&
                                                        stoppedCleanly &&
                                                        restartedCleanly &&
                                                        respawnQueued &&
                                                        respawnExecuted &&
                                                        respawnResetVehicle &&
                                                        giveUpRestarted &&
                                                        respawnAfterGiveUp &&
                                                        respawnAfterGiveUpExecuted &&
                                                        actionsRejectedWhenStopped;
                                                if (!manualDriveValid) {
                                                    std::cerr
                                                            << "manual drive checks failed: leftPriority="
                                                            << leftPriority
                                                            << ", initialNeutral="
                                                            << manualInitialNeutral
                                                            << ", physicsAdvanced="
                                                            << physicsAdvanced
                                                            << ", cameraFollowed="
                                                            << cameraFollowedManualDrive
                                                            << ", rightAfterRelease="
                                                            << rightAfterRelease
                                                            << ", manualCopy="
                                                            << manualCopyValid
                                                            << ", stoppedCleanly="
                                                            << stoppedCleanly
                                                            << ", restartedCleanly="
                                                            << restartedCleanly
                                                            << ", respawnQueued="
                                                            << respawnQueued
                                                            << ", respawnExecuted="
                                                            << respawnExecuted
                                                            << ", respawnReset="
                                                            << respawnResetVehicle
                                                            << " (distance "
                                                            << distanceBeforeRespawn
                                                            << " -> "
                                                            << distanceAfterRespawn
                                                            << ")"
                                                            << ", giveUpRestarted="
                                                            << giveUpRestarted
                                                            << ", respawnAfterGiveUp="
                                                            << respawnAfterGiveUp
                                                            << "/"
                                                            << respawnAfterGiveUpExecuted
                                                            << ", stoppedActionsRejected="
                                                            << actionsRejectedWhenStopped
                                                            << '\n';
                                                }
                                                viewer.addSearchRun(
                                                        packsDirectory,
                                                        replayPath,
                                                        searchTimeline,
                                                        SyntheticSearchInputs(),
                                                        QStringLiteral(
                                                                "optimized-cpu"));
                                            });
                                });
                        return;
                    }
                    if (!manualDriveValid) {
                        return;
                    }
                    if (!takeoverVerificationStarted) {
                        viewer.setCurrentTick(500);
                        viewer.setCameraPreset(3);
                        const QVector3D carForward =
                                viewer.carRotation().rotatedVector(
                                        QVector3D(0.0f, 0.0f, 1.0f));
                        const QVector3D internalForward =
                                viewer.carCameraTarget() -
                                viewer.carCameraPosition();
                        const float internalAlignment =
                                QVector3D::dotProduct(
                                        carForward.normalized(),
                                        internalForward.normalized());
                        const bool internalOrientationValid =
                                viewer.carCameraAvailable() &&
                                viewer.hideSelectedCar() &&
                                carForward.x() > 0.1f &&
                                carForward.y() < -0.05f &&
                                internalForward.lengthSquared() >
                                        0.000001f &&
                                internalAlignment > 0.999f;
                        cameraPresetsValid &= internalOrientationValid;
                        viewer.setCameraPreset(1);
                        if (!internalOrientationValid) {
                            completed = true;
                            std::cerr
                                    << "internal camera did not follow the "
                                       "vehicle orientation: carForward="
                                    << carForward.x() << ','
                                    << carForward.y() << ','
                                    << carForward.z()
                                    << " cameraForward="
                                    << internalForward.x() << ','
                                    << internalForward.y() << ','
                                    << internalForward.z()
                                    << " alignment="
                                    << internalAlignment << '\n';
                            application.quit();
                            return;
                        }
                        takeoverVerificationStarted = true;
                        bool takeoverPreviewPublished = false;
                        const QMetaObject::Connection previewConnection =
                                QObject::connect(
                                        &viewer,
                                        &forevertas::viewer::
                                                RaceViewerController::
                                                        stateChanged,
                                        &application,
                                        [&]() {
                                            takeoverPreviewPublished = true;
                                        });
                        viewer.setPreviewInputScript(
                                QStringLiteral(
                                        "0.00 press up\n"
                                        "0.00 gas 32768\n"
                                        "0.00 steer 65536\n"
                                        "0.01 steer 32768\n"
                                        "1.00 steer 0\n"
                                        "2.00 rel up"));
                        const bool takeoverPreviewReady = WaitUntil([&]() {
                            return takeoverPreviewPublished;
                        });
                        QObject::disconnect(previewConnection);
                        viewer.setSelectedRunId(
                                QStringLiteral("preview"));
                        viewer.setCurrentTick(1);
                        viewer.setTakeOverOnInput(false);
                        viewer.play();
                        viewer.setManualInput(
                                QStringLiteral("left"), false);
                        const bool disabledTakeoverIgnored =
                                takeoverPreviewReady &&
                                viewer.playing() &&
                                !viewer.manualDriving() &&
                                !viewer.manualSteeringTakenOver() &&
                                !viewer.manualLongitudinalTakenOver() &&
                                viewer.selectedRunId() ==
                                        QStringLiteral("preview");
                        viewer.pause();
                        viewer.setTakeOverOnInput(true);
                        viewer.play();
                        viewer.setManualInput(
                                QStringLiteral("left"), false);
                        const bool releaseStartedSteeringTakeover =
                                viewer.manualDriving() &&
                                !viewer.playing() &&
                                viewer.manualSteeringTakenOver() &&
                                !viewer.manualLongitudinalTakenOver() &&
                                !viewer.manualLeft() &&
                                !viewer.manualRight() &&
                                viewer.selectedRunId() ==
                                        QStringLiteral("manual");
                        QTimer::singleShot(
                                80,
                                &application,
                                [&, disabledTakeoverIgnored,
                                 releaseStartedSteeringTakeover]() {
                                    const auto steeringOnlySample =
                                            viewer.inputSample(
                                                    viewer.currentTick());
                                    const bool longitudinalStayedAutomatic =
                                            viewer.manualDriving() &&
                                            steeringOnlySample.accelerate >
                                                    0.99f &&
                                            steeringOnlySample.brake <
                                                    0.01f &&
                                            std::fabs(
                                                    steeringOnlySample.steering) <
                                                    0.01f;
                                    viewer.releaseManualInputs();
                                    const bool focusReleasePreservedUnclaimed =
                                            viewer.manualDriving() &&
                                            viewer.manualSteeringTakenOver() &&
                                            !viewer.manualLongitudinalTakenOver();
                                    viewer.setManualInput(
                                            QStringLiteral("brake"), false);
                                    const bool releaseStartedLongitudinalTakeover =
                                            viewer.manualDriving() &&
                                            viewer.manualSteeringTakenOver() &&
                                            viewer.manualLongitudinalTakenOver();
                                    QTimer::singleShot(
                                            80,
                                            &application,
                                            [&, disabledTakeoverIgnored,
                                             releaseStartedSteeringTakeover,
                                             longitudinalStayedAutomatic,
                                             focusReleasePreservedUnclaimed,
                                             releaseStartedLongitudinalTakeover]() {
                                                const auto bothTakenOverSample =
                                                        viewer.inputSample(
                                                                viewer.currentTick());
                                                const bool automaticLongitudinalStopped =
                                                        bothTakenOverSample.accelerate <
                                                                0.01f &&
                                                        bothTakenOverSample.brake <
                                                                0.01f;
                                                viewer.setManualInput(
                                                        QStringLiteral("left"),
                                                        true);
                                                viewer.setManualInput(
                                                        QStringLiteral("brake"),
                                                        true);
                                                QTimer::singleShot(
                                                        80,
                                                        &application,
                                                        [&, disabledTakeoverIgnored,
                                                         releaseStartedSteeringTakeover,
                                                         longitudinalStayedAutomatic,
                                                         focusReleasePreservedUnclaimed,
                                                         releaseStartedLongitudinalTakeover,
                                                         automaticLongitudinalStopped]() {
                                                            const auto manualSample =
                                                                    viewer.inputSample(
                                                                            viewer.currentTick());
                                                            const bool manualChannelsApplied =
                                                                    manualSample.steering <
                                                                            -0.99f &&
                                                                    manualSample.brake >
                                                                            0.99f &&
                                                                    manualSample.accelerate <
                                                                            0.01f;
                                                            const bool takeoverRespawnQueued =
                                                                    viewer.respawnManualDrive();
                                                            QEventLoop takeoverRespawnLoop;
                                                            QTimer::singleShot(
                                                                    30,
                                                                    &takeoverRespawnLoop,
                                                                    &QEventLoop::quit);
                                                            takeoverRespawnLoop.exec();
                                                            const bool takeoverRespawnExecuted =
                                                                    viewer.currentInputScript()
                                                                            .contains(
                                                                                    QStringLiteral(
                                                                                            "press enter"));
                                                            viewer.releaseManualInputs();
                                                            viewer.stopManualDrive();
                                                            const QString
                                                                    takeoverScript =
                                                                            viewer.currentInputScript();
                                                            const bool mixedHistoryCopied =
                                                                    viewer.canCopyCurrentInputs() &&
                                                                    takeoverScript.contains(
                                                                            QStringLiteral(
                                                                                    " press up")) &&
                                                                    takeoverScript.contains(
                                                                            QStringLiteral(
                                                                                    " gas 32768")) &&
                                                                    takeoverScript.contains(
                                                                            QStringLiteral(
                                                                                    "0.01 steer 32768\n"
                                                                                    "0.01 steer 0")) &&
                                                                    takeoverScript.contains(
                                                                            QStringLiteral(
                                                                                    " gas 0")) &&
                                                                    takeoverScript.contains(
                                                                            QStringLiteral(
                                                                                    " press left")) &&
                                                                    takeoverScript.contains(
                                                                            QStringLiteral(
                                                                                    " press down")) &&
                                                                    takeoverScript.contains(
                                                                            QStringLiteral(
                                                                                    " rel left")) &&
                                                                    takeoverScript.contains(
                                                                            QStringLiteral(
                                                                                    " rel down")) &&
                                                                    !takeoverScript.contains(
                                                                            QStringLiteral(
                                                                                    "1.00 steer 0")) &&
                                                                    !takeoverScript.contains(
                                                                            QStringLiteral(
                                                                                    "2.00 rel up"));
                                                            const qint64 takeoverEndTick =
                                                                    viewer.currentTick();
                                                            const QVector3D takeoverEndPosition =
                                                                    viewer.carPosition();
                                                            const QQuaternion takeoverEndRotation =
                                                                    viewer.carRotation();
                                                            bool copiedPreviewPublished = false;
                                                            const QMetaObject::Connection copiedPreviewConnection =
                                                                    QObject::connect(
                                                                            &viewer,
                                                                            &forevertas::viewer::
                                                                                    RaceViewerController::
                                                                                            stateChanged,
                                                                            &application,
                                                                            [&]() {
                                                                                copiedPreviewPublished = true;
                                                                            });
                                                            viewer.setPreviewInputScript(
                                                                    takeoverScript);
                                                            const bool copiedPreviewReady =
                                                                    WaitUntil([&]() {
                                                                        return copiedPreviewPublished;
                                                                    });
                                                            QObject::disconnect(
                                                                    copiedPreviewConnection);
                                                            viewer.setSelectedRunId(
                                                                    QStringLiteral("preview"));
                                                            viewer.setCurrentTick(
                                                                    takeoverEndTick);
                                                            const bool copiedRaceMatches =
                                                                    copiedPreviewReady &&
                                                                    (viewer.carPosition() -
                                                                     takeoverEndPosition)
                                                                                    .lengthSquared() <
                                                                            0.0000000001f &&
                                                                    std::abs(QQuaternion::dotProduct(
                                                                            viewer.carRotation(),
                                                                            takeoverEndRotation)) >
                                                                            0.999999f;

                                                            viewer.setCurrentTick(
                                                                    std::min<qint64>(
                                                                            3,
                                                                            viewer.tickCount() - 1));
                                                            viewer.play();
                                                            viewer.setManualInput(
                                                                    QStringLiteral("right"),
                                                                    true);
                                                            const bool restartTakeoverStarted =
                                                                    viewer.manualDriving() &&
                                                                    viewer.selectedRunId() ==
                                                                            QStringLiteral("manual");
                                                            const bool takeoverRestoredSource =
                                                                    viewer.giveUpManualDrive() &&
                                                                    !viewer.manualDriving() &&
                                                                    viewer.selectedRunId() ==
                                                                            QStringLiteral("preview") &&
                                                                    viewer.currentTick() == 0 &&
                                                                    viewer.playing();
                                                            viewer.pause();
                                                            viewer.setSelectedRunId(
                                                                    QStringLiteral("best"));
                                                            viewer.setCurrentTick(1);
                                                            viewer.play();
                                                            viewer.setManualInput(
                                                                    QStringLiteral("left"),
                                                                    false);
                                                            const bool bestRestartTakeoverStarted =
                                                                    viewer.manualDriving() &&
                                                                    viewer.selectedRunId() ==
                                                                            QStringLiteral("manual");
                                                            const bool bestTakeoverRestoredSource =
                                                                    viewer.giveUpManualDrive() &&
                                                                    !viewer.manualDriving() &&
                                                                    viewer.selectedRunId() ==
                                                                            QStringLiteral("best") &&
                                                                    viewer.currentTick() == 0 &&
                                                                    viewer.playing();
                                                            viewer.pause();
                                                            viewer.addSearchRun(
                                                                    packsDirectory,
                                                                    replayPath,
                                                                    searchTimeline,
                                                                    {AnalogInput(
                                                                            0,
                                                                            PhysicsSandboxInputAction::Accelerate,
                                                                            0)},
                                                                    QStringLiteral(
                                                                            "optimized-cpu"));
                                                            viewer.setCurrentTick(1);
                                                            viewer.setTakeOverOnInput(
                                                                    true);
                                                            viewer.play();
                                                            viewer.setManualInput(
                                                                    QStringLiteral(
                                                                            "left"),
                                                                    true);
                                                            const bool failedTakeoverRecovered =
                                                                    viewer.playing() &&
                                                                    !viewer.manualDriving() &&
                                                                    viewer.selectedRunId() ==
                                                                            QStringLiteral(
                                                                                    "best") &&
                                                                    viewer.statusText().contains(
                                                                            QStringLiteral(
                                                                                    "Manual takeover failed"));
                                                            viewer.pause();
                                                            manualTakeoverValid =
                                                                    disabledTakeoverIgnored &&
                                                                    releaseStartedSteeringTakeover &&
                                                                    longitudinalStayedAutomatic &&
                                                                    focusReleasePreservedUnclaimed &&
                                                                    releaseStartedLongitudinalTakeover &&
                                                                    automaticLongitudinalStopped &&
                                                                    manualChannelsApplied &&
                                                                    takeoverRespawnQueued &&
                                                                    takeoverRespawnExecuted &&
                                                                    mixedHistoryCopied &&
                                                                    copiedRaceMatches &&
                                                                    restartTakeoverStarted &&
                                                                    takeoverRestoredSource &&
                                                                    bestRestartTakeoverStarted &&
                                                                    bestTakeoverRestoredSource &&
                                                                    failedTakeoverRecovered;
                                                            if (!manualTakeoverValid) {
                                                                std::cerr
                                                                        << "manual takeover checks failed: releaseSteering="
                                                                        << releaseStartedSteeringTakeover
                                                                        << ", disabledIgnored="
                                                                        << disabledTakeoverIgnored
                                                                        << ", automaticLongitudinal="
                                                                        << longitudinalStayedAutomatic
                                                                        << ", focusReleasePreserved="
                                                                        << focusReleasePreservedUnclaimed
                                                                        << ", releaseLongitudinal="
                                                                        << releaseStartedLongitudinalTakeover
                                                                        << ", stoppedLongitudinal="
                                                                        << automaticLongitudinalStopped
                                                                        << ", manualChannels="
                                                                        << manualChannelsApplied
                                                                        << ", takeoverRespawn="
                                                                        << takeoverRespawnQueued
                                                                        << "/"
                                                                        << takeoverRespawnExecuted
                                                                        << ", mixedHistory="
                                                                        << mixedHistoryCopied
                                                                        << ", copiedRaceMatches="
                                                                        << copiedRaceMatches
                                                                        << ", restoredSource="
                                                                        << restartTakeoverStarted
                                                                        << "/"
                                                                        << takeoverRestoredSource
                                                                        << ", restoredBest="
                                                                        << bestRestartTakeoverStarted
                                                                        << "/"
                                                                        << bestTakeoverRestoredSource
                                                                        << ", failureRecovered="
                                                                        << failedTakeoverRecovered
                                                                        << " (playing="
                                                                        << viewer.playing()
                                                                        << ", manual="
                                                                        << viewer.manualDriving()
                                                                        << ", selected="
                                                                        << viewer.selectedRunId().toStdString()
                                                                        << ", status="
                                                                        << viewer.statusText().toStdString()
                                                                        << ")"
                                                                        << ", script="
                                                                        << takeoverScript.toStdString()
                                                                        << '\n';
                                                            }
                                                            viewer.setTakeOverOnInput(
                                                                    false);
                                                            viewer.addSearchRun(
                                                                    packsDirectory,
                                                                    replayPath,
                                                                    searchTimeline,
                                                                    SyntheticSearchInputs(),
                                                                    QStringLiteral(
                                                                            "optimized-cpu"));
                                                        });
                                            });
                                });
                        return;
                    }
                    if (!manualTakeoverValid) {
                        return;
                    }
                    verificationStarted = true;
                    const QVector2D clipPlanes = viewer.cameraClipPlanes(
                            viewer.carPosition() + QVector3D(0.0f, 0.0f, 38.0f),
                            38.0);
                    std::cout << viewer.triangleCount() << " triangles, "
                              << viewer.visualTriangleCount()
                              << " visual triangles, "
                              << viewer.visualMeshCount() << " visual meshes, "
                              << viewer.sourceVisualObjectCount()
                              << " visible source objects, "
                              << viewer.visualBatchCount() << " batches, "
                              << viewer.materialCount() << " materials, "
                              << viewer.shadowCount()
                              << " shadows, clip=" << clipPlanes.x() << ".."
                              << clipPlanes.y() << ", "
                              << viewer.ellipsoidCount() << " ellipsoids, "
                              << viewer.durationMs() << " ms, "
                              << viewer.tickCount() << " ticks\n";

                    RaceTimelineItem timeline;
                    timeline.setWidth(252);
                    timeline.setHeight(600);
                    const QImage lightTimelineImage =
                            RenderTimeline(timeline);
                    timeline.setDarkMode(true);
                    const QImage darkTimelineImage =
                            RenderTimeline(timeline);
                    const bool timelineThemePalette =
                            PixelIs(lightTimelineImage,
                                    0,
                                    0,
                                    QColor(QStringLiteral("#f4f5f2"))) &&
                            PixelIs(darkTimelineImage,
                                    0,
                                    0,
                                    QColor(QStringLiteral("#101412")));
                    timeline.setViewer(&viewer);
                    const qint64 leftSteeringTick =
                            FindActivityTick(viewer, 'l');
                    const qint64 rightSteeringTick =
                            FindActivityTick(viewer, 'r');
                    const qint64 accelerationTick =
                            FindActivityTick(viewer, 'a');
                    const qint64 brakeTick = FindActivityTick(viewer, 'b');
                    const bool leftSteeringPainted = TimelinePaintsColor(
                            timeline,
                            viewer,
                            leftSteeringTick,
                            QColor(QStringLiteral("#4f9ddd")));
                    const bool rightSteeringPainted = TimelinePaintsColor(
                            timeline,
                            viewer,
                            rightSteeringTick,
                            QColor(QStringLiteral("#4f9ddd")));
                    const bool accelerationPainted = TimelinePaintsColor(
                                    timeline,
                                    viewer,
                                    accelerationTick,
                                    QColor(QStringLiteral("#3dbd73")));
                    const bool brakePainted = TimelinePaintsColor(
                                    timeline,
                                    viewer,
                                    brakeTick,
                                    QColor(QStringLiteral("#df5555")));
                    const bool timelineInputs = leftSteeringPainted &&
                            rightSteeringPainted &&
                            accelerationPainted && brakePainted;

                    viewer.setCurrentTick(
                            std::min<qint64>(500, viewer.tickCount() - 1));
                    timeline.setPixelsPerTick(3.0);
                    const qint64 dragStartTimeMs = viewer.timeMs();
                    SendTimelineMouseEvent(
                            timeline,
                            QEvent::MouseButtonPress,
                            Qt::LeftButton,
                            Qt::LeftButton,
                            QPointF(126.0, 70.0));
                    const bool leftPressDoesNotSnap =
                            viewer.timeMs() == dragStartTimeMs;
                    SendTimelineMouseEvent(
                            timeline,
                            QEvent::MouseMove,
                            Qt::NoButton,
                            Qt::LeftButton,
                            QPointF(126.0, 100.0));
                    SendTimelineMouseEvent(
                            timeline,
                            QEvent::MouseButtonRelease,
                            Qt::LeftButton,
                            Qt::NoButton,
                            QPointF(126.0, 100.0));
                    const bool naturalScrubDirection =
                            viewer.timeMs() < dragStartTimeMs;

                    viewer.setTimeMs(5000);
                    int scrubTimeSignals = 0;
                    int scrubPoseSignals = 0;
                    const QMetaObject::Connection scrubTimeConnection =
                            QObject::connect(
                                    &viewer,
                                    &RaceViewerController::timeChanged,
                                    &application,
                                    [&]() { ++scrubTimeSignals; });
                    const QMetaObject::Connection scrubPoseConnection =
                            QObject::connect(
                                    &viewer,
                                    &RaceViewerController::poseChanged,
                                    &application,
                                    [&]() { ++scrubPoseSignals; });
                    QElapsedTimer scrubClock;
                    scrubClock.start();
                    SendTimelineMouseEvent(
                            timeline,
                            QEvent::MouseButtonPress,
                            Qt::LeftButton,
                            Qt::LeftButton,
                            QPointF(126.0, 300.0));
                    constexpr int kSyntheticMoveCount = 4701;
                    for (int move = 0; move < kSyntheticMoveCount; ++move) {
                        SendTimelineMouseEvent(
                                timeline,
                                QEvent::MouseMove,
                                Qt::NoButton,
                                Qt::LeftButton,
                                QPointF(126.0,
                                        static_cast<qreal>(move % 600)));
                    }
                    const qreal finalScrubY =
                            static_cast<qreal>(
                                    (kSyntheticMoveCount - 1) % 600);
                    SendTimelineMouseEvent(
                            timeline,
                            QEvent::MouseButtonRelease,
                            Qt::LeftButton,
                            Qt::NoButton,
                            QPointF(126.0, finalScrubY));
                    const qint64 scrubElapsedMs = scrubClock.elapsed();
                    QObject::disconnect(scrubTimeConnection);
                    QObject::disconnect(scrubPoseConnection);
                    const qint64 exactScrubTime = std::clamp<qint64>(
                            static_cast<qint64>(std::llround(
                                    5000.0 -
                                    (finalScrubY - 300.0) / 3.0 *
                                            viewer.tickDurationMs())),
                            0,
                            viewer.timelineSeekLimitMs());
                    const bool continuousScrubCoalesced =
                            scrubElapsedMs < 250 &&
                            scrubTimeSignals <= 2 &&
                            scrubPoseSignals <= 2 &&
                            viewer.timeMs() == exactScrubTime;

                    viewer.setTimeMs(5000);
                    timeline.setPixelsPerTick(1.0);
                    const QImage baseScaleImage = RenderTimeline(timeline);
                    const bool baseScaleReadable =
                            PixelIs(baseScaleImage,
                                    49,
                                    400,
                                    QColor(QStringLiteral("#9aa69e"))) &&
                            PixelIs(baseScaleImage,
                                    49,
                                    310,
                                    QColor(QStringLiteral("#59635d"))) &&
                            !PixelIsAnyRulerMark(baseScaleImage, 49, 301) &&
                            !PixelIs(baseScaleImage,
                                     60,
                                     310,
                                     QColor(QStringLiteral("#59635d")));

                    timeline.setPixelsPerTick(3.0);
                    const QImage mediumScaleImage = RenderTimeline(timeline);
                    const bool mediumScaleReadable =
                            PixelIs(mediumScaleImage,
                                    49,
                                    450,
                                    QColor(QStringLiteral("#9aa69e"))) &&
                            PixelIs(mediumScaleImage,
                                    49,
                                    330,
                                    QColor(QStringLiteral("#59635d"))) &&
                            !PixelIsAnyRulerMark(mediumScaleImage, 49, 303);

                    timeline.setPixelsPerTick(12.0);
                    const QImage fineScaleImage = RenderTimeline(timeline);
                    const bool fineScaleReadable =
                            PixelIs(fineScaleImage,
                                    49,
                                    312,
                                    QColor(QStringLiteral("#59635d"))) &&
                            PixelIs(fineScaleImage,
                                    49,
                                    420,
                                    QColor(QStringLiteral("#9aa69e"))) &&
                            !PixelIs(fineScaleImage,
                                     60,
                                     312,
                                     QColor(QStringLiteral("#59635d")));
                    const bool dynamicRulerScale = baseScaleReadable &&
                            mediumScaleReadable && fineScaleReadable;

                    timeline.setPixelsPerTick(5.0);
                    const int fineLengthAt5 = RulerMarkLengthNear(
                            RenderTimeline(timeline), 305);
                    timeline.setPixelsPerTick(6.0);
                    const int fineLengthAt6 = RulerMarkLengthNear(
                            RenderTimeline(timeline), 306);
                    timeline.setPixelsPerTick(7.0);
                    const int fineLengthAt7 = RulerMarkLengthNear(
                            RenderTimeline(timeline), 307);
                    timeline.setPixelsPerTick(9.5);
                    const int fineLengthAt95 = RulerMarkLengthNear(
                            RenderTimeline(timeline), 309);
                    const int fineLengthAt12 = RulerMarkLengthNear(
                            fineScaleImage, 312);
                    const bool fineMarksGrowSmoothly =
                            fineLengthAt5 == 0 &&
                            fineLengthAt6 > fineLengthAt5 &&
                            fineLengthAt7 > fineLengthAt6 &&
                            fineLengthAt95 > fineLengthAt7 &&
                            fineLengthAt12 > fineLengthAt95;

                    timeline.setPixelsPerTick(3.0);
                    DragTimeline(
                            timeline,
                            Qt::RightButton,
                            QPointF(126.0, 300.0),
                            QPointF(126.0, 240.0));
                    const bool rightDragZoomsIn =
                            timeline.pixelsPerTick() > 3.0;

                    viewer.jumpToStart();
                    const bool noPrematureSplits =
                            viewer.checkpointSplits().isEmpty();
                    viewer.setCurrentTick(145);
                    const QVariantList firstSplits =
                            viewer.checkpointSplits();
                    viewer.setCurrentTick(741);
                    const QVariantList checkpointSplits =
                            viewer.checkpointSplits();
                    viewer.jumpToEnd();
                    const QVariantList finishedSplits =
                            viewer.checkpointSplits();
                    viewer.setCurrentTick(145);
                    const QVariantList rewoundSplits =
                            viewer.checkpointSplits();
                    const bool checkpointSplitHistory =
                            noPrematureSplits &&
                            firstSplits.size() == 1 &&
                            firstSplits.front()
                                            .toMap()
                                            .value(QStringLiteral("label"))
                                            .toString() ==
                                    QStringLiteral("CP 1") &&
                            firstSplits.front()
                                            .toMap()
                                            .value(QStringLiteral("time"))
                                            .toString() ==
                                    QStringLiteral("1.450") &&
                            checkpointSplits.size() == 2 &&
                            checkpointSplits.back()
                                            .toMap()
                                            .value(QStringLiteral("label"))
                                            .toString() ==
                                    QStringLiteral("CP 2") &&
                            checkpointSplits.back()
                                            .toMap()
                                            .value(QStringLiteral("time"))
                                            .toString() ==
                                    QStringLiteral("7.410") &&
                            finishedSplits.size() == 3 &&
                            finishedSplits.back()
                                            .toMap()
                                            .value(QStringLiteral("label"))
                                            .toString() ==
                                    QStringLiteral("Finish") &&
                            finishedSplits.back()
                                            .toMap()
                                            .value(QStringLiteral("time"))
                                            .toString() ==
                                    QStringLiteral("9.995") &&
                            finishedSplits.back()
                                    .toMap()
                                    .value(QStringLiteral("isFinish"))
                                    .toBool() &&
                            rewoundSplits.size() == 1;
                    viewer.setCurrentTick(100);
                    const bool timeLabelUnambiguous =
                            viewer.timeText().startsWith(
                                    QStringLiteral("00:00:01.000 / "));
                    const QString copiedSearchScript =
                            viewer.currentInputScript();
                    const bool searchCopyStopsAtCurrentTime =
                            viewer.canCopyCurrentInputs() &&
                            copiedSearchScript.contains(
                                    QStringLiteral("0.00 press up")) &&
                            copiedSearchScript.contains(
                                    QStringLiteral(
                                            "0.49 steer -32768")) &&
                            copiedSearchScript.contains(
                                    QStringLiteral("0.99 press down")) &&
                            !copiedSearchScript.contains(
                                    QStringLiteral("1.49 press right"));
                    QSet<QString> visibleMaterialClasses;
                    for (const QVariant &entry : viewer.visualBatches()) {
                        const QVariantMap batch = entry.toMap();
                        if (batch.value(QStringLiteral("defaultVisible"))
                                    .toBool()) {
                            const qint64 bindingIndex =
                                    batch.value(QStringLiteral(
                                                        "materialBindingIndex"))
                                            .toLongLong();
                            if (bindingIndex >= 0 &&
                                bindingIndex <
                                        viewer.visualMaterials().size()) {
                                visibleMaterialClasses.insert(
                                        viewer.visualMaterials()
                                                .at(bindingIndex)
                                                .toMap()
                                                .value(QStringLiteral(
                                                        "materialClass"))
                                                .toString());
                            }
                        }
                    }
                    const bool sceneValid = mapOnlyStateObserved &&
                            manualDriveValid &&
                            cameraPresetsValid &&
                            viewer.whiteboard()->mapKey().startsWith(
                                    QStringLiteral("collision-sha256:")) &&
                            viewer.whiteboard()->mapKey().size() == 81 &&
                            !viewer.whiteboard()->mapName().isEmpty() &&
                            viewer.whiteboard()->mapName() !=
                                    QFileInfo(replayPath)
                                            .completeBaseName() &&
                            viewer.triangleCount() > 0 &&
                            viewer.visualTriangleCount() > 0 &&
                            viewer.visualMeshCount() > 0 &&
                            viewer.materialCount() > 0 &&
                            !viewer.visualMaterials().isEmpty() &&
                            viewer.visualMaterials().size() <
                                    viewer.visualBatches().size() &&
                            !viewer.visualBatches().isEmpty() &&
                            viewer.visualBatchCount() ==
                                    viewer.visualBatches().size() &&
                            viewer.visualBatchCount() <
                                    viewer.sourceVisualObjectCount() &&
                            viewer.shadowCount() == 0 &&
                            viewer.diagnosticCount() > 0 &&
                            visibleMaterialClasses.size() >= 3 &&
                            viewer.ellipsoidCount() > 0 &&
                            viewer.durationMs() > 0 &&
                            viewer.tickCount() ==
                                    viewer.durationMs() /
                                                    viewer.tickDurationMs() +
                                            1 &&
                            timelineThemePalette &&
                            timelineInputs && naturalScrubDirection &&
                            leftPressDoesNotSnap &&
                            continuousScrubCoalesced && dynamicRulerScale &&
                            fineMarksGrowSmoothly && rightDragZoomsIn &&
                            timeLabelUnambiguous &&
                            checkpointSplitHistory &&
                            searchCopyStopsAtCurrentTime &&
                            trajectoryPreviewValid &&
                            improvementTrajectoriesValid &&
                            manualTakeoverValid;
                    if (!sceneValid) {
                        std::cerr
                                << "viewer scene checks failed: "
                                   "leftSteeringTick="
                                << leftSteeringTick
                                << ", rightSteeringTick=" << rightSteeringTick
                                << ", accelerationTick=" << accelerationTick
                                << ", brakeTick=" << brakeTick
                                << ", leftSteeringPainted="
                                << leftSteeringPainted
                                << ", rightSteeringPainted="
                                << rightSteeringPainted
                                << ", accelerationPainted="
                                << accelerationPainted
                                << ", brakePainted=" << brakePainted
                                << ", timelineThemePalette="
                                << timelineThemePalette
                                << ", naturalScrubDirection="
                                << naturalScrubDirection
                                << ", leftPressDoesNotSnap="
                                << leftPressDoesNotSnap
                                << ", continuousScrubCoalesced="
                                << continuousScrubCoalesced
                                << ", scrubElapsedMs=" << scrubElapsedMs
                                << ", scrubTimeSignals=" << scrubTimeSignals
                                << ", scrubPoseSignals=" << scrubPoseSignals
                                << ", baseScaleReadable=" << baseScaleReadable
                                << ", mediumScaleReadable="
                                << mediumScaleReadable
                                << ", fineScaleReadable=" << fineScaleReadable
                                << ", fineLengthAt5=" << fineLengthAt5
                                << ", fineLengthAt6=" << fineLengthAt6
                                << ", fineLengthAt7=" << fineLengthAt7
                                << ", fineLengthAt95=" << fineLengthAt95
                                << ", fineLengthAt12=" << fineLengthAt12
                                << ", rightDragZoomsIn=" << rightDragZoomsIn
                                << ", timeLabelUnambiguous="
                                << timeLabelUnambiguous
                                << ", checkpointSplits="
                                << checkpointSplitHistory
                                << ", searchCopy="
                                << searchCopyStopsAtCurrentTime
                                << ", copiedScript='"
                                << copiedSearchScript.toStdString() << "'"
                                << ", visualTriangles="
                                << viewer.visualTriangleCount()
                                << ", visualMeshes=" << viewer.visualMeshCount()
                                << ", materials=" << viewer.materialCount()
                                << ", visualInstances="
                                << viewer.visualBatches().size()
                                << ", sourceVisualObjects="
                                << viewer.sourceVisualObjectCount()
                                << ", diagnostics=" << viewer.diagnosticCount()
                                << ", visibleMaterialClasses="
                                << visibleMaterialClasses.size() << '\n';
                    }

                    viewer.jumpToStart();
                    viewer.play();
                    QTimer::singleShot(90, &application, [&, sceneValid]() {
                        const bool playbackAdvanced = viewer.playing() &&
                                viewer.currentTick() >= 4;
                        viewer.pause();
                        viewer.jumpToEnd();
                        const bool endPaused = !viewer.playing() &&
                                viewer.timeMs() == viewer.durationMs();
                        viewer.play();
                        QTimer::singleShot(
                                70,
                                &application,
                                [&, sceneValid, playbackAdvanced,
                                 endPaused]() {
                                    const bool restartedFromEnd =
                                            viewer.playing() &&
                                            viewer.currentTick() > 0 &&
                                            viewer.currentTick() <
                                                    viewer.tickCount() - 1;
                                    viewer.jumpToStart();
                                    const bool startPaused =
                                            !viewer.playing() &&
                                            viewer.currentTick() == 0;
                                    completed = true;
                                    exitCode = sceneValid &&
                                                    playbackAdvanced &&
                                                    endPaused &&
                                                    restartedFromEnd &&
                                                    startPaused
                                            ? 0
                                            : 1;
                                    if (exitCode != 0) {
                                        std::cerr
                                                << "viewer playback failed: sceneValid="
                                                << sceneValid
                                                << ", playbackAdvanced="
                                                << playbackAdvanced
                                                << ", endPaused="
                                                << endPaused
                                                << ", restartedFromEnd="
                                                << restartedFromEnd
                                                << ", startPaused="
                                                << startPaused << '\n';
                                    }
                                    application.quit();
                                });
                    });
                    return;
                }
                if (viewer.statusText() != QStringLiteral("No map loaded")) {
                    completed = true;
                    std::cerr << viewer.statusText().toStdString() << '\n';
                    application.quit();
                }
            });
    QTimer::singleShot(170000, &application, [&]() {
        if (completed) {
            return;
        }
        completed = true;
        std::cerr << "viewer loading timed out\n";
        application.quit();
    });
    viewer.loadMap(packsDirectory, replayPath, QStringLiteral("reference"));
    application.exec();
    return exitCode;
}
