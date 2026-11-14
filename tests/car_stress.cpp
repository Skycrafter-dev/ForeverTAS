// Hunts the selected car vanishing from the race view: random sequences of
// what users do (play, rewind, seek, cameras, render modes, runs, base edits,
// a live search, the viewer hidden and shown) on the real main window, and
// after each one a check that the car the view should show is drawn: the
// frame grabbed twice, with the car's style shown and hidden, must differ
// around where the car projects. The first miss is written out with every
// condition the car's visibility depends on, and both frames.
//
//   forevertas-car-stress PACKS REPLAY OUT_DIR [STEPS [SEED]]
//
// Under a real or virtual display (xvfb-run): Qt Quick 3D needs a 3D API.
#include "app/app_settings.h"
#include "app/input_preview_binding.h"
#include "app/search_controller.h"
#include "viewer/race_timeline_item.h"
#include "viewer/race_viewer_controller.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QMatrix4x4>
#include <QQmlApplicationEngine>
#include <QQuaternion>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QVector3D>

#include <cmath>
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace {

void Wait(int milliseconds) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

bool WaitUntil(const std::function<bool()> &condition, int milliseconds) {
    QElapsedTimer timer;
    timer.start();
    while (!condition()) {
        if (timer.elapsed() > milliseconds) return false;
        Wait(30);
    }
    return true;
}

// Pixels that differ between two grabs inside a rectangle.
int Differing(const QImage &a, const QImage &b, const QRect &area) {
    const QRect r = area.intersected(a.rect()).intersected(b.rect());
    int count = 0;
    for (int y = r.top(); y <= r.bottom(); ++y) {
        for (int x = r.left(); x <= r.right(); ++x) {
            const QRgb p = a.pixel(x, y), q = b.pixel(x, y);
            if (std::abs(qRed(p) - qRed(q)) + std::abs(qGreen(p) - qGreen(q)) + std::abs(qBlue(p) - qBlue(q)) > 24) {
                ++count;
            }
        }
    }
    return count;
}

QString Describe(const QVariant &value) {
    if (value.canConvert<QVector3D>() && value.typeId() == QMetaType::QVector3D) {
        const QVector3D v = value.value<QVector3D>();
        return QStringLiteral("(%1, %2, %3)").arg(v.x()).arg(v.y()).arg(v.z());
    }
    if (value.typeId() == QMetaType::QQuaternion) {
        const QQuaternion q = value.value<QQuaternion>();
        return QStringLiteral("(w %1, %2, %3, %4)").arg(q.scalar()).arg(q.x()).arg(q.y()).arg(q.z());
    }
    return value.toString();
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 4) {
        std::cerr << "usage: forevertas-car-stress PACKS REPLAY OUT_DIR [STEPS [SEED]]\n";
        return 2;
    }
    const int steps = argc > 4 ? std::atoi(argv[4]) : 200;
    const unsigned seed = argc > 5 ? static_cast<unsigned>(std::strtoul(argv[5], nullptr, 10)) : 1u;
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ForeverTASTests"));
    QCoreApplication::setApplicationName(QStringLiteral("CarStress"));
    QStandardPaths::setTestModeEnabled(true);
    forevertas::app::AppSettings::Instance().clear();

    forevertas::app::SearchController controller;
    forevertas::viewer::RaceViewerController viewer;
    forevertas::app::BindInputPreview(controller, viewer);
    forevertas::viewer::RegisterRaceViewerQmlTypes();
    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::warnings, [](const QList<QQmlError> &warnings) {
        for (const QQmlError &warning : warnings) std::cerr << "qml: " << warning.toString().toStdString() << '\n';
    });
    engine.setInitialProperties({{QStringLiteral("controller"), QVariant::fromValue(static_cast<QObject *>(&controller))},
                                 {QStringLiteral("viewer"), QVariant::fromValue(static_cast<QObject *>(&viewer))}});
    engine.load(QUrl::fromLocalFile(QStringLiteral(FOREVERTAS_SOURCE_DIR "/qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) return 1;
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().front());
    if (window == nullptr) return 1;
    window->resize(1600, 900);

    controller.setPacksDirectory(QString::fromLocal8Bit(argv[1]));
    controller.setReplayPath(QString::fromLocal8Bit(argv[2]));
    controller.extractReplayInputs();
    WaitUntil([&] { return !controller.baseInputScript().isEmpty(); }, 20000);
    const QString baseScript = controller.baseInputScript();
    controller.previewBaseInputs();
    if (!WaitUntil([&] { return viewer.loaded() && viewer.runCount() > 0; }, 60000)) {
        std::cerr << "the replay's run did not load\n";
        return 1;
    }
    Wait(1000);

    QObject *viewport = window->findChild<QObject *>(QStringLiteral("raceViewport"));
    auto *rasterView = window->findChild<QQuickItem *>(QStringLiteral("rasterMapView"));
    QObject *carRoot = window->findChild<QObject *>(QStringLiteral("selectedRunCarRoot"));
    QObject *root = window;
    if (!viewport || !rasterView || !carRoot) {
        std::cerr << "the race view's objects were not found\n";
        return 1;
    }
    const QString out = QString::fromLocal8Bit(argv[3]);
    QDir().mkpath(out);
    std::mt19937 random(seed);
    const auto pick = [&](int n) { return static_cast<int>(random() % static_cast<unsigned>(n)); };

    // The actions, with their names.
    std::vector<std::pair<std::string, std::function<void()>>> actions = {
            {"play", [&] { viewer.play(); }},
            {"pause", [&] { viewer.pause(); }},
            {"seek", [&] { viewer.setTimeMs(pick(std::max<int>(1, static_cast<int>(viewer.durationMs())))); }},
            {"start", [&] { viewer.jumpToStart(); }},
            {"end", [&] { viewer.jumpToEnd(); }},
            {"step", [&] { viewer.stepTick(pick(2) ? 1 : -1); }},
            {"rewind", [&] { viewer.beginRewind(); Wait(150 + pick(400)); viewer.endRewind(); }},
            {"restart", [&] { viewer.restartRun(); }},
            {"camera1", [&] { viewport->setProperty("freeCamera", false); viewport->setProperty("orbitalCamera", false); viewer.setProperty("cameraPreset", 1); }},
            {"camera2", [&] { viewport->setProperty("freeCamera", false); viewport->setProperty("orbitalCamera", false); viewer.setProperty("cameraPreset", 2); }},
            {"camera3", [&] { viewport->setProperty("freeCamera", false); viewport->setProperty("orbitalCamera", false); viewer.setProperty("cameraPreset", 3); }},
            {"orbit", [&] { viewport->setProperty("freeCamera", false); viewport->setProperty("orbitalCamera", true); }},
            {"render", [&] {
                 static const char *modes[] = {"textured", "neutral", "collision", "wireframe", "textured"};
                 const char *mode = modes[pick(5)];
                 std::cerr << "render " << mode << '\n';
                 root->setProperty("renderMode", QString::fromLatin1(mode));
             }},
            {"run", [&] {
                 const QVariantList runs = viewer.runOptions();
                 if (!runs.isEmpty()) viewer.setSelectedRunId(runs[pick(static_cast<int>(runs.size()))].toMap().value("id").toString());
             }},
            {"edit", [&] {
                 // a base edit: the preview run is simulated again
                 QString script = baseScript;
                 script.append(QStringLiteral("\n%1.%2 press left\n%3.%4 rel left")
                                       .arg(1 + pick(3)).arg(pick(90) + 10, 2, 10, QLatin1Char('0'))
                                       .arg(4).arg(pick(90) + 10, 2, 10, QLatin1Char('0')));
                 controller.setBaseInputScript(pick(3) == 0 ? baseScript : script);
                 controller.previewBaseInputs();
             }},
            {"viewer", [&] { controller.setViewerVisible(false); Wait(200); controller.setViewerVisible(true); }},
            {"search", [&] {
                 if (controller.running()) controller.stopSearch(); else controller.startSearch();
             }},
    };

    // STRESS_SCRIPT=a,b,render:wireframe,...: these actions in order instead of random ones
    std::vector<std::string> script;
    if (const char *text = std::getenv("STRESS_SCRIPT")) {
        std::string item;
        for (const char *c = text;; ++c) {
            if (*c == ',' || *c == 0) {
                if (!item.empty()) script.push_back(item);
                item.clear();
                if (*c == 0) break;
            } else {
                item += *c;
            }
        }
    }
    const auto named = [&](const std::string &wanted) -> std::pair<std::string, std::function<void()>> {
        if (wanted.rfind("render:", 0) == 0) {
            const QString mode = QString::fromStdString(wanted.substr(7));
            return {wanted, [&, mode] { root->setProperty("renderMode", mode); }};
        }
        for (const auto &entry : actions) {
            if (entry.first == wanted) return entry;
        }
        return {wanted, [] {}};
    };
    int loads = 0, scenes = 0;
    QObject::connect(&viewer, &forevertas::viewer::RaceViewerController::sceneChanged, [&] { ++scenes; });
    QObject::connect(&viewer, &forevertas::viewer::RaceViewerController::stateChanged, [&] {
        if (viewer.property("loading").toBool()) ++loads;
    });
    int checked = 0, misses = 0;
    std::string history;
    const int total = script.empty() ? steps : static_cast<int>(script.size());
    for (int step = 0; step < total; ++step) {
        const auto [name, action] = script.empty()
                ? actions[static_cast<std::size_t>(pick(static_cast<int>(actions.size())))]
                : named(script[static_cast<std::size_t>(step)]);
        if (std::getenv("STRESS_TRACE")) std::cerr << "step " << step << " " << name << '\n';
        history += name + " ";
        action();
        Wait(150 + pick(500));

        // What the view should show: the car's own conditions, as the QML has them.
        const bool shouldShow = carRoot->property("visible").toBool();
        const bool carCamera = viewport->property("carCameraActive").toBool();
        if (!viewer.loaded() || viewer.runCount() == 0 || !shouldShow || !rasterView->isVisible()) {
            if (viewer.loaded() && viewer.runCount() > 0 && !shouldShow && carCamera && !viewer.hideSelectedCar()) {
                // the QML hides the car though the car camera trails it and does not ask for it hidden
                ++misses;
                std::cerr << "MISS (hidden by its conditions) at step " << step << " after: " << history << '\n';
            }
            continue;
        }
        const bool wasPlaying = viewer.playing();
        viewer.pause();
        Wait(120);
        // where the car projects
        QVector3D screen;
        QMetaObject::invokeMethod(rasterView, "mapFrom3DScene", Q_RETURN_ARG(QVector3D, screen),
                                  Q_ARG(QVector3D, viewer.carPosition()));
        const QPointF onWindow = rasterView->mapToScene(QPointF(screen.x(), screen.y()));
        const bool inView = screen.z() > 0.0f && onWindow.x() >= 0 && onWindow.y() >= 0 &&
                            onWindow.x() < window->width() && onWindow.y() < window->height();
        // Only the trailing car cameras: an orbit or free camera may look at the car through the map.
        if (!inView || !carCamera || viewer.hideSelectedCar()) {
            if (wasPlaying) viewer.play();
            continue;
        }
        const QImage shown = window->grabWindow();
        const QString style = QStringLiteral("car:") + viewer.selectedRunStyleId();
        viewer.setVisualStyle(style, QStringLiteral("visible"), false);
        Wait(120);
        const QImage hidden = window->grabWindow();
        viewer.setVisualStyle(style, QStringLiteral("visible"), true);
        Wait(60);
        const qreal ratio = shown.devicePixelRatio();
        const QRect around(static_cast<int>((onWindow.x() - 80) * ratio), static_cast<int>((onWindow.y() - 80) * ratio),
                           static_cast<int>(160 * ratio), static_cast<int>(160 * ratio));
        const int differing = Differing(shown, hidden, around);
        ++checked;
        if (differing < 20) {
            ++misses;
            const QString prefix = out + QStringLiteral("/miss-%1").arg(misses);
            shown.save(prefix + "-shown.png");
            hidden.save(prefix + "-hidden.png");
            if (std::getenv("STRESS_FULL") != nullptr) {
            // the car drawn over everything: where it is, if the map hides it
            viewer.setVisualStyle(style, QStringLiteral("throughBlocks"), true);
            Wait(200);
            const QImage xray = window->grabWindow();
            xray.save(prefix + "-xray.png");
            viewer.setVisualStyle(style, QStringLiteral("throughBlocks"), false);
            std::cerr << "  x-ray differs from shown in " << Differing(shown, xray, around) << " pixels\n";
            // what covers it: the views above the race view, and their visible models
            for (const char *name : {"rayTracingTrajectoryOverlay", "gpuRayTracingView", "moduleRenderView"}) {
                auto *layer = window->findChild<QQuickItem *>(QString::fromLatin1(name));
                if (layer == nullptr) continue;
                std::cerr << "  layer " << name << " visible " << layer->isVisible() << " z " << layer->z() << '\n';
                if (!layer->isVisible()) continue;
                for (QObject *model : layer->findChildren<QObject *>()) {
                    if (!model->inherits("QQuick3DModel") || !model->property("visible").toBool()) continue;
                    std::cerr << "    model " << model->objectName().toStdString() << " ("
                              << model->metaObject()->className() << ") parent "
                              << (model->parent() ? model->parent()->objectName().toStdString() : std::string("-")) << '\n';
                }
                layer->setVisible(false);
                Wait(200);
                const QImage without = window->grabWindow();
                layer->setVisible(true);
                without.save(prefix + "-without-" + name + ".png");
                std::cerr << "    without it, " << Differing(without, hidden, around) << " pixels differ from the hidden car\n";
            }
            std::cerr << "MISS (not drawn) at step " << step << ", " << differing << " pixels differ, after: " << history << '\n';
            std::cerr << "  carPosition " << Describe(viewer.carPosition()).toStdString()
                      << " carRotation " << Describe(viewer.carRotation()).toStdString()
                      << " screen " << screen.x() << "," << screen.y() << "," << screen.z()
                      << " carCamera " << carCamera << " preset " << viewer.property("cameraPreset").toInt()
                      << " render " << root->property("renderMode").toString().toStdString()
                      << " run " << viewer.selectedRunId().toStdString()
                      << " ellipsoids " << viewer.carEllipsoids().size()
                      << " time " << viewer.timeMs() << '\n';
            // below the root: the ellipsoid nodes and their models, as the scene has them
            const auto nodes = window->findChildren<QObject *>(QStringLiteral("selectedRunCarEllipsoidNode"));
            std::cerr << "  ellipsoid nodes " << nodes.size() << '\n';
            for (QObject *node : nodes) {
                std::cerr << "    node visible " << node->property("visible").toBool()
                          << " scale " << Describe(node->property("scale")).toStdString()
                          << " position " << Describe(node->property("position")).toStdString()
                          << " scenePosition " << Describe(node->property("scenePosition")).toStdString() << '\n';
            }
            for (QObject *model : window->findChildren<QObject *>(QStringLiteral("selectedRunCarFilledModel"))) {
                const QObject *geometry = model->property("geometry").value<QObject *>();
                std::cerr << "    filled model visible " << model->property("visible").toBool() << " geometry " << geometry
                          << " opacity " << model->property("opacity").toDouble()
                          << " scenePosition " << Describe(model->property("scenePosition")).toStdString()
                          << " sceneScale " << Describe(model->property("sceneScale")).toStdString() << '\n';
            }
            std::cerr << "  root visible " << carRoot->property("visible").toBool()
                      << " opacity " << carRoot->property("opacity").toDouble()
                      << " scenePosition " << Describe(carRoot->property("scenePosition")).toStdString()
                      << " sceneScale " << Describe(carRoot->property("sceneScale")).toStdString() << '\n';
            std::cerr << "  viewer loading " << viewer.property("loading").toBool() << " status '"
                      << viewer.property("statusText").toString().toStdString() << "' triangles "
                      << viewer.property("triangleCount").toLongLong() << " visual batches "
                      << viewer.property("visualBatches").toList().size() << " runs";
            for (const QVariant &run : viewer.runOptions()) std::cerr << ' ' << run.toMap().value("id").toString().toStdString();
            std::cerr << " search running " << controller.running() << " loads " << loads << " scenes " << scenes << '\n';
            // does it last: the same view a while later
            for (int later : {500, 2000}) {
                Wait(later);
                viewer.setVisualStyle(style, QStringLiteral("visible"), false);
                Wait(150);
                const QImage hiddenLater = window->grabWindow();
                viewer.setVisualStyle(style, QStringLiteral("visible"), true);
                Wait(150);
                const QImage shownLater = window->grabWindow();
                std::cerr << "  +" << later << " ms: " << Differing(shownLater, hiddenLater, around)
                          << " pixels differ (shown vs hidden), time " << viewer.timeMs() << '\n';
                shownLater.save(prefix + QStringLiteral("-later-%1.png").arg(later));
            }
            // the camera the race view renders with
            if (QObject *camera = window->findChild<QObject *>(QStringLiteral("viewCamera"))) {
                std::cerr << "  viewCamera scenePosition " << Describe(camera->property("scenePosition")).toStdString()
                          << " sceneRotation " << Describe(camera->property("sceneRotation")).toStdString()
                          << " clipNear " << camera->property("clipNear").toDouble()
                          << " clipFar " << camera->property("clipFar").toDouble()
                          << " fieldOfView " << camera->property("fieldOfView").toDouble()
                          << " clipDistance " << camera->property("clipDistance").toDouble() << '\n';
                const QMatrix4x4 projection = camera->property("projection").value<QMatrix4x4>();
                std::cerr << "  projection";
                for (int i = 0; i < 16; ++i) std::cerr << ' ' << projection.constData()[i];
                std::cerr << '\n';
            }
            std::cerr << "  carCamera position " << Describe(viewer.property("carCameraPosition")).toStdString()
                      << " rotation " << Describe(viewer.property("carCameraRotation")).toStdString()
                      << " target " << Describe(viewer.property("carCameraTarget")).toStdString()
                      << " fov " << viewer.property("carCameraFieldOfView").toDouble() << '\n';
            {
                // every visible model of the race view off at once: what is left is the background
                std::vector<QObject *> off;
                for (QObject *model : rasterView->findChildren<QObject *>()) {
                    if (model->inherits("QQuick3DModel") && model->property("visible").toBool()) {
                        model->setProperty("visible", false);
                        off.push_back(model);
                    }
                }
                Wait(200);
                const QImage bare = window->grabWindow();
                bare.save(prefix + "-bare.png");
                for (QObject *model : off) model->setProperty("visible", true);
                const QRgb centre = bare.pixel(bare.width() / 2, bare.height() / 2);
                const QRgb shownCentre = shown.pixel(shown.width() / 2, shown.height() / 2);
                std::cerr << "  all " << off.size() << " models off: centre rgb " << qRed(centre) << "," << qGreen(centre) << ","
                          << qBlue(centre) << " (shown " << qRed(shownCentre) << "," << qGreen(shownCentre) << ","
                          << qBlue(shownCentre) << "), " << Differing(bare, hidden, around) << " pixels differ from shown\n";
                std::cerr << "  raster view size " << rasterView->width() << "x" << rasterView->height()
                          << " opacity " << rasterView->opacity() << " visible " << rasterView->isVisible() << '\n';
            }
            }
            {
                // the visible models by name; each name's group off at once
                std::map<std::string, std::vector<QObject *>> groups;
                for (QObject *model : rasterView->findChildren<QObject *>()) {
                    if (model->inherits("QQuick3DModel") && model->property("visible").toBool()) {
                        std::string name = model->objectName().toStdString();
                        if (name.empty()) name = std::string("<") + model->metaObject()->className() + "> under " +
                                (model->parent() ? model->parent()->objectName().toStdString() : "-");
                        groups[name].push_back(model);
                    }
                }
                for (auto &[name, list] : groups) {
                    for (QObject *model : list) model->setProperty("visible", false);
                    Wait(150);
                    const QImage without = window->grabWindow();
                    for (QObject *model : list) model->setProperty("visible", true);
                    const QRgb centre = without.pixel(without.width() / 2, without.height() / 2);
                    std::cerr << "  group " << name << " x" << list.size() << " off: centre " << qRed(centre) << ","
                              << qGreen(centre) << "," << qBlue(centre) << '\n';
                }
            }
            break;
            // in the race view: which visible model, switched off, lets the car be seen
            std::vector<QObject *> culprits;
            for (QObject *model : rasterView->findChildren<QObject *>()) {
                if (!model->inherits("QQuick3DModel") || !model->property("visible").toBool()) continue;
                if (model->objectName().startsWith(QStringLiteral("selectedRunCar"))) continue;
                model->setProperty("visible", false);
                Wait(120);
                const QImage without = window->grabWindow();
                model->setProperty("visible", true);
                const int seen = Differing(without, hidden, around);
                if (seen > 200) culprits.push_back(model);
                std::cerr << "    " << (seen > 200 ? "COVERS " : "        ") << model->objectName().toStdString() << " ("
                          << model->metaObject()->className() << ") parent "
                          << (model->parent() ? model->parent()->objectName().toStdString() : std::string("-"))
                          << " differing " << seen << '\n';
            }
            for (QObject *model : culprits) {
                std::cerr << "  covering model " << model->objectName().toStdString() << " position "
                          << Describe(model->property("scenePosition")).toStdString() << " scale "
                          << Describe(model->property("sceneScale")).toStdString() << " geometry "
                          << model->property("geometry").value<QObject *>() << '\n';
                for (const QByteArray &name : {QByteArray("source"), QByteArray("objectName")})
                    std::cerr << "    " << name.constData() << " " << model->property(name.constData()).toString().toStdString() << '\n';
            }
            break;
        }
        if (wasPlaying) viewer.play();
    }
    if (controller.running()) controller.stopSearch();
    WaitUntil([&] { return !controller.running(); }, 20000);
    std::cout << "steps " << steps << ", checked " << checked << ", misses " << misses << '\n';
    return misses == 0 ? 0 : 1;
}
