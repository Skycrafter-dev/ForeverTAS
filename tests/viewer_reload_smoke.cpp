#include "viewer/race_viewer_controller.h"

#include <QGuiApplication>
#include <QTimer>

#include <iostream>

int main(int argc, char **argv) {
    if (argc != 4) {
        std::cerr << "usage: forevertas-viewer-reload <Packs> <first> <second>\n";
        return 2;
    }
    QGuiApplication application(argc, argv);
    forevertas::viewer::RaceViewerController viewer;
    const QString packs = QString::fromLocal8Bit(argv[1]);
    const QString replays[]{
            QString::fromLocal8Bit(argv[2]),
            QString::fromLocal8Bit(argv[3]),
            QString::fromLocal8Bit(argv[2])};
    int completedLoads = 0;
    // Reloading the same map keeps search results, so the improvement
    // preview survives until a different map is loaded.
    bool improvementKept = true;
    bool loadInProgress = false;
    bool preservedSceneDuringReload = true;
    QObject *previousVisualGeometry = nullptr;
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
                                    << "scene changed while reload was pending: "
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

                const bool sceneValid =
                        viewer.ellipsoidCount() > 0 && viewer.runCount() == 1 &&
                        viewer.tickCount() > 1 &&
                        viewer.durationMs() > 0 &&
                        viewer.selectedRunId() == QStringLiteral("preview") &&
                        viewer.trajectoryCount() == (improvementKept ? 2 : 1) &&
                        viewer.improvementCarCount() == (improvementKept ? 1 : 0) &&
                        viewer.visualTriangleCount() > 0 &&
                        viewer.visualMeshCount() > 0 &&
                        !viewer.visualMaterials().isEmpty() &&
                        !viewer.visualBatches().isEmpty() &&
                        viewer.visualBatchCount() ==
                                viewer.visualBatches().size() &&
                        viewer.visualBatchCount() <
                        viewer.sourceVisualObjectCount();
                if (!sceneValid) {
                    finished = true;
                    std::cerr << "viewer scene is incomplete after load "
                              << completedLoads + 1 << '\n';
                    application.quit();
                    return;
                }
                previousVisualGeometry =
                        viewer.visualBatches()
                                .front()
                                .toMap()
                                .value(QStringLiteral("geometry"))
                                .value<QObject *>();

                ++completedLoads;
                if (completedLoads < 3) {
                    improvementKept &= replays[completedLoads] == replays[completedLoads - 1];
                    viewer.loadMap(packs, replays[completedLoads]);
                    const QObject *const publishedAfterRequest =
                            viewer.visualBatches()
                                    .front()
                                    .toMap()
                                    .value(QStringLiteral("geometry"))
                                    .value<QObject *>();
                    preservedSceneDuringReload &=
                            viewer.loaded() &&
                            publishedAfterRequest == previousVisualGeometry;
                    return;
                }
                finished = true;
                exitCode = preservedSceneDuringReload ? 0 : 1;
                if (exitCode != 0) {
                    std::cerr << "an existing scene was torn down during reload\n";
                }
                application.quit();
            });

    QTimer::singleShot(180000, &application, [&]() {
        if (finished) return;
        finished = true;
        std::cerr << "replay reload timed out\n";
        application.quit();
    });
    std::vector<forevertas::SearchTimelineFrame> improvementFrames(3u);
    for (std::size_t index = 0u;
         index < improvementFrames.size();
         ++index) {
        improvementFrames[index].timeMs =
                static_cast<std::int64_t>(index * 10u);
        improvementFrames[index].positionX =
                static_cast<float>(index);
    }
    viewer.addSearchImprovement(
            packs,
            replays[0],
            improvementFrames,
            QStringLiteral("optimized-cpu"),
            1u,
            1u);
    application.exec();
    return exitCode;
}
