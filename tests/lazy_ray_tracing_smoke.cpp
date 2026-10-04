#include "viewer/race_viewer_controller.h"
#include "viewer/gpu_ray_tracing_view.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QThread>

#include <iostream>

namespace {
template <typename Predicate>
bool WaitUntil(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 60000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    return predicate();
}
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    QGuiApplication app(argc, argv);
    using namespace forevertas::viewer;
    RaceViewerController viewer;
    GpuRayTracingView view;
    view.setViewer(&viewer);
    const QString packs = QString::fromLocal8Bit(argv[1]);
    const QString replay = QString::fromLocal8Bit(argv[2]);
    int sceneChanges = 0;
    int readyScenes = 0;
    QObject::connect(&viewer, &RaceViewerController::sceneChanged,
                     &app, [&]() { ++sceneChanges; });
    QObject::connect(&viewer, &RaceViewerController::rayTracingSceneChanged,
                     &app, [&]() { if (viewer.rayTracingScene()) ++readyScenes; });
    const auto loaded = [&]() { return viewer.loaded() && !viewer.loading(); };
    const auto check = [](bool okay, const char *message) {
        if (!okay) std::cerr << message << '\n';
        return okay;
    };
    viewer.loadMap(packs, replay);
    if (!check(WaitUntil(loaded) && viewer.visualTriangleCount() > 0 &&
                       !viewer.rayTracingScene(), "raster load allocated RT scene")) return 1;
    const auto rasterScene = sceneChanges;
    view.setActive(true);
    if (!view.supported()) viewer.requestRayTracingScene();
    if (!check(WaitUntil([&]() { return viewer.rayTracingScene() != nullptr; }) &&
                       sceneChanges == rasterScene && readyScenes == 1,
               "RT activation failed or reset the raster scene")) return 1;
    auto scene = viewer.rayTracingScene();
    if (!check(scene->triangleCount > 0 && !scene->bvhNodes.isEmpty(),
               "lazy RT scene is empty")) return 1;
    std::cout << "Deferred RT storage: " << scene->vertices.size() + scene->triangles.size()
            + scene->bvhNodes.size() + scene->materials.size() << " bytes\n";
    view.setActive(false);
    view.setActive(true);
    viewer.requestRayTracingScene();
    if (!check(viewer.rayTracingScene() == scene && readyScenes == 1,
               "repeated activation rebuilt an unchanged RT scene")) return 1;
    view.setActive(false);
    std::weak_ptr<const RayTracingSceneData> previous = scene;
    scene.reset();
    viewer.loadMap(packs, replay);
    if (!check(WaitUntil(loaded) && !viewer.rayTracingScene() && previous.expired(),
               "map replacement retained the old RT scene")) return 1;

    // Queue an RT build then replace the map before its completion can publish.
    viewer.requestRayTracingScene();
    viewer.loadMap(packs, replay);
    if (!check(WaitUntil(loaded) && !viewer.rayTracingScene() && readyScenes == 1,
               "stale RT scene was published after a reload")) return 1;
    view.setActive(true);
    if (!view.supported()) viewer.requestRayTracingScene();
    if (!check(WaitUntil([&]() { return viewer.rayTracingScene() != nullptr; }) && readyScenes == 2,
               "RT scene could not rebuild after cancellation")) return 1;
    return 0;
}
