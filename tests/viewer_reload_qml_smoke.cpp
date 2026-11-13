#include "app/input_preview_binding.h"
#include "app/search_controller.h"
#include "viewer/race_timeline_item.h"
#include "viewer/race_viewer_controller.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFileInfo>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVector3D>

#include <iostream>

namespace {

bool PreviewSceneIsVisible(
        QObject *root,
        const forevertas::viewer::RaceViewerController &viewer,
        int loadNumber) {
    const QList<QObject *> roots = root->findChildren<QObject *>(
            QStringLiteral("runCarRoot"));
    const QList<QObject *> nodes = root->findChildren<QObject *>(
            QStringLiteral("runCarEllipsoidNode"));
    const QList<QObject *> models = root->findChildren<QObject *>(
            QStringLiteral("runCarFilledModel"));
    const QList<QObject *> visualModels = root->findChildren<QObject *>(
            QStringLiteral("trackVisualModel"));
    const QVector3D cameraPosition = viewer.carCameraPosition();
    const QVector3D cameraTarget = viewer.carCameraTarget();
    const QVector3D carPosition = viewer.carPosition();
    const QVector3D cameraLook = cameraTarget - cameraPosition;
    const QVector3D cameraToCar = carPosition - cameraPosition;
    const bool cameraFollowsPreview = viewer.carCameraAvailable() &&
            cameraLook.lengthSquared() > 0.000001f &&
            cameraToCar.lengthSquared() > 0.000001f &&
            QVector3D::dotProduct(cameraLook.normalized(),
                                  cameraToCar.normalized()) > 0.0f;
    const bool okay = viewer.loaded() && viewer.runCount() == 1 &&
            viewer.tickCount() > 1 && viewer.durationMs() > 0 &&
            viewer.selectedRunId() == QStringLiteral("preview") &&
            viewer.trajectoryCount() == 1 &&
            cameraFollowsPreview &&
            viewer.ellipsoidCount() > 0 &&
            viewer.visualBatchCount() > 0 &&
            roots.size() == 1 &&
            nodes.size() == viewer.ellipsoidCount() &&
            models.size() == viewer.ellipsoidCount() &&
            visualModels.size() == viewer.visualBatchCount();

    if (!okay) {
        std::cerr << "preview scene failed after button load " << loadNumber
                  << " roots=" << roots.size()
                  << " nodes=" << nodes.size()
                  << " models=" << models.size()
                  << " visualModels=" << visualModels.size()
                  << " visualBatches=" << viewer.visualBatchCount() << '\n';
    }
    return okay;
}

bool TriggerMenuItem(QObject *item) {
    return item != nullptr && item->property("enabled").toBool() &&
            QMetaObject::invokeMethod(item, "triggered", Qt::DirectConnection);
}

// File > Open map or replay picks a file, then loads it the same way.
bool LoadMap(QObject *root) {
    return root->property("canOpenMap").toBool() &&
            QMetaObject::invokeMethod(root, "loadMapIfReady",
                                      Qt::DirectConnection);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 4) {
        std::cerr << "usage: forevertas-viewer-reload-qml "
                     "<Packs> <first> <second>\n";
        return 2;
    }
    if (QFileInfo(QString::fromLocal8Bit(argv[2])).canonicalFilePath() ==
        QFileInfo(QString::fromLocal8Bit(argv[3])).canonicalFilePath()) {
        std::cerr << "reload regression requires two distinct replay files\n";
        return 2;
    }

    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ForeverTASTests"));
    QCoreApplication::setApplicationName(
            QStringLiteral("ViewerReloadQmlSmoke"));
    QStandardPaths::setTestModeEnabled(true);
    QSettings().clear();

    const QString packs = QString::fromLocal8Bit(argv[1]);
    const QString replays[]{
            QString::fromLocal8Bit(argv[2]),
            QString::fromLocal8Bit(argv[3]),
            QString::fromLocal8Bit(argv[2])};
    const QString protectedDraft = QStringLiteral("0.00 press up");
    {
        forevertas::app::SearchController previousSession;
        previousSession.setPacksDirectory(packs);
        previousSession.setReplayPath(replays[0]);
        previousSession.setBaseInputScript(protectedDraft);
    }
    forevertas::app::SearchController controller;
    forevertas::viewer::RaceViewerController viewer;
    forevertas::app::BindInputPreview(controller, viewer);
    if (controller.baseInputScript() != protectedDraft ||
        viewer.previewInputScript() != protectedDraft) {
        std::cerr << "persisted input script was not installed in the viewer "
                     "before QML startup\n";
        return 1;
    }
    forevertas::viewer::RegisterRaceViewerQmlTypes();
    QQmlApplicationEngine engine;
    engine.setInitialProperties({
            {QStringLiteral("controller"),
             QVariant::fromValue(static_cast<QObject *>(&controller))},
            {QStringLiteral("viewer"),
             QVariant::fromValue(static_cast<QObject *>(&viewer))}});
    engine.load(QUrl::fromLocalFile(
            QStringLiteral(FOREVERTAS_SOURCE_DIR "/qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        std::cerr << "Main.qml did not load\n";
        return 1;
    }
    QObject *const root = engine.rootObjects().front();
    QObject *const extractMenuItem = root->findChild<QObject *>(
            QStringLiteral("extractInputsMenuItem"));
    QObject *const replaceDialog = root->findChild<QObject *>(
            QStringLiteral("replaceBaseInputScriptDialog"));
    if (!TriggerMenuItem(extractMenuItem)) {
        std::cerr << "File > Extract inputs from replay failed\n";
        return 1;
    }
    QCoreApplication::processEvents();
    const bool confirmationProtectedDraft =
            replaceDialog != nullptr &&
            replaceDialog->property("visible").toBool() &&
            !controller.extractingReplayInputs() &&
            controller.baseInputScript() == protectedDraft &&
            QMetaObject::invokeMethod(
                    replaceDialog, "reject", Qt::DirectConnection);
    QCoreApplication::processEvents();
    if (!confirmationProtectedDraft ||
        controller.baseInputScript() != protectedDraft) {
        std::cerr << "non-empty input script was not protected by confirmation\n";
        return 1;
    }

    int completedLoads = 0;
    bool loadInProgress = false;
    bool preservedSceneDuringReload = true;
    bool finished = false;
    int exitCode = 1;
    QObject::connect(
            &viewer,
            &forevertas::viewer::RaceViewerController::stateChanged,
            &application,
            [&]() {
                if (finished) return;
                if (viewer.loading()) {
                    loadInProgress = true;
                    if (completedLoads > 0) {
                        const bool preserved = viewer.loaded() &&
                                viewer.ellipsoidCount() > 0 &&
                                viewer.visualBatchCount() > 0;
                        preservedSceneDuringReload &= preserved;
                        if (!preserved) {
                            std::cerr
                                    << "button reload changed the scene early: "
                                    << "loaded=" << viewer.loaded()
                                    << ", ellipsoids="
                                    << viewer.ellipsoidCount()
                                    << ", runs=" << viewer.runCount() << '\n';
                        }
                    }
                    return;
                }
                if (!loadInProgress || !viewer.loaded() ||
                    viewer.runCount() == 0) {
                    return;
                }
                loadInProgress = false;
                ++completedLoads;
                const int currentLoad = completedLoads;
                QTimer::singleShot(500, &application, [&, currentLoad]() {
                    if (finished || currentLoad != completedLoads) return;
                    if (!PreviewSceneIsVisible(root, viewer, currentLoad)) {
                        finished = true;
                        application.quit();
                        return;
                    }
                    if (currentLoad < 3) {
                        controller.setReplayPath(replays[currentLoad]);
                        QCoreApplication::processEvents();
                        if (!LoadMap(root)) {
                            finished = true;
                            std::cerr << "map load "
                                      << currentLoad + 1 << " failed\n";
                            application.quit();
                        }
                        return;
                    }
                    finished = true;
                    exitCode = preservedSceneDuringReload ? 0 : 1;
                    if (exitCode != 0) {
                        std::cerr << "the visible scene was torn down during "
                                     "a button reload\n";
                    }
                    application.quit();
                });
            });
    if (!LoadMap(root)) {
        std::cerr << "first map load failed (can open="
                  << root->property("canOpenMap").toBool()
                  << ", loading=" << viewer.loading() << ")\n";
        return 1;
    }

    QTimer::singleShot(180000, &application, [&]() {
        if (finished) return;
        finished = true;
        std::cerr << "QML distinct replay reload timed out\n";
        application.quit();
    });
    application.exec();
    return exitCode;
}
