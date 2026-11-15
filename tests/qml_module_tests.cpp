// UI checks in the app's own QML module (loadFromModule, as the app loads it;
// the QML smoke tests load the source files):
// - a panel's hidden header comes back with a click on the panel's top edge,
//   even where the panel's content reaches that edge;
// - with the 3D view drawn (not offscreen: run under a display), a box's
//   arrow dragged moves the box;
// - the pose target's position and rotation, each copied on its own from the
//   camera or the car; from the camera, the pose faces where it looks;
// - Enter in every number field of the search settings, for every target.
//   With PACKS REPLAY, after a stopped search's improvements, where users'
//   windows hung: Qt 6.9's garbage collector, marking a value QML compiled to
//   C++ held, read one that was gone and allocated without end (hence the
//   module is bytecode only). It fails on that runaway (std::bad_alloc), a
//   crash, or an Enter slower than 500 ms.
#include "app/input_preview_binding.h"
#include "app/mcp_bridge.h"
#include "app/search_controller.h"
#include "viewer/race_timeline_item.h"
#include "viewer/race_viewer_controller.h"
#include "viewer/render_module_library.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QJSValue>
#include <QUrl>
#include <QQmlComponent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QImage>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QQuaternion>
#include <QVector3D>
#include <QStandardPaths>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#ifndef FOREVERTAS_QML_IMPORT_DIR
#error "FOREVERTAS_QML_IMPORT_DIR must be defined"
#endif

namespace {

void Settle(int milliseconds = 30) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
}

// The number fields a user can type in now (ScrubNumberField).
QList<QQuickItem *> VisibleNumberFields(QObject *root) {
    QList<QQuickItem *> fields;
    for (QQuickItem *item : root->findChildren<QQuickItem *>()) {
        if (item->property("scrubbable").toBool() && item->isVisible() && item->isEnabled())
            fields.push_back(item);
    }
    return fields;
}

void Click(QQuickWindow *window, QPointF at) {
    for (const QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
        QMouseEvent event(type, at, at, window->mapToGlobal(at), Qt::LeftButton,
                          type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &event);
    }
}

void Drag(QQuickWindow *window, QPointF from, QPointF to) {
    QMouseEvent press(QEvent::MouseButtonPress, from, from, window->mapToGlobal(from), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &press);
    Settle(30);
    for (int step = 1; step <= 20; ++step) {
        const QPointF at = from + (to - from) * (step / 20.0);
        QMouseEvent move(QEvent::MouseMove, at, at, window->mapToGlobal(at), Qt::NoButton, Qt::LeftButton,
                         Qt::NoModifier);
        QCoreApplication::sendEvent(window, &move);
        Settle(10);
    }
    QMouseEvent release(QEvent::MouseButtonRelease, to, to, window->mapToGlobal(to), Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &release);
    Settle(50);
}

// A box target's X arrow dragged in the race view moves the box (needs the
// 3D view drawn: not on the offscreen platform). Empty when it does.
std::string GizmoDragProblem(QQuickWindow *window, forevertas::app::SearchController &controller,
                             forevertas::viewer::RaceViewerController &viewer) {
    QObject *const boxes = controller.property("cuboidTargets").value<QObject *>();
    const QVector3D car = viewer.carPosition();
    std::cout << "car at " << car.x() << " " << car.y() << " " << car.z() << '\n';
    int index = -1;
    QMetaObject::invokeMethod(boxes, "addTarget", Q_RETURN_ARG(int, index), Q_ARG(double, car.x()),
                              Q_ARG(double, car.y()), Q_ARG(double, car.z()));
    QMetaObject::invokeMethod(boxes, "selectTarget", Q_ARG(int, index));
    for (const char *axis : {"x", "y", "z"})
        QMetaObject::invokeMethod(boxes, "setSizeComponent", Q_ARG(int, index),
                                  Q_ARG(QString, QString::fromLatin1(axis)), Q_ARG(QString, QStringLiteral("2")));
    controller.setEvaluationTargetId(QStringLiteral("volume-entry-time"));
    Settle(800);
    // targets drawn through blocks (or ray traced) are in the overlay view, which the app picks in then
    const bool through = controller.property("drawTargetsThroughBlocks").toBool() ||
                         window->property("renderMode").toString() == QStringLiteral("textured-rt");
    QQuickItem *const view = window->findChild<QQuickItem *>(
            through ? QStringLiteral("rayTracingTrajectoryOverlay") : QStringLiteral("rasterMapView"));
    QObject *const scene = window->findChild<QObject *>(
            through ? QStringLiteral("rayTracingCuboidEditorScene") : QStringLiteral("rasterCuboidEditorScene"));
    QObject *arrow = nullptr;
    for (QObject *object : scene != nullptr ? scene->findChildren<QObject *>() : QList<QObject *>{})
        if (object->property("editorKind").toString() == QStringLiteral("move") &&
            object->property("editorAxis").toString() == QStringLiteral("x") &&
            object->property("targetIndex").toInt() == index)
            arrow = object;
    if (view == nullptr || arrow == nullptr)
        return "no race view or no X arrow on the new box";
    // a point of the arrow off its middle, away from the box (Qt's own pick,
    // without the camera's framing, took it for the resize handle)
    const qreal barLength = arrow->parent() != nullptr ? arrow->parent()->property("barLength").toReal() : 0;
    const QVector3D onArrow = arrow->property("scenePosition").value<QVector3D>() +
                              QVector3D(float(0.35 * barLength), 0, 0);
    QVector3D onScreen;
    QMetaObject::invokeMethod(view, "mapFrom3DScene", Q_RETURN_ARG(QVector3D, onScreen),
                              Q_ARG(QVector3D, onArrow));
    if (onScreen.z() < 0 || onScreen.x() < 0 || onScreen.y() < 0 || onScreen.x() > view->width() ||
        onScreen.y() > view->height())
        return "the X arrow is not in the race view";
    // where the camera's projection draws that point
    const QPointF at = view->mapToScene(QPointF(onScreen.x(), onScreen.y()));
    if (qEnvironmentVariableIsSet("FTAS_GIZMO_PROBE")) {
        // along the X arrow's row: drawn red or not, and what the pick finds
        const QImage picture = window->grabWindow();
        const qreal ratio = picture.width() / qreal(window->width());
        std::string drawnRow, pickedRow;
        for (int x = 300; x < 900; x += 4) {
            const QPointF scene(x, at.y());
            const QColor c = picture.pixelColor(int(scene.x() * ratio), int(scene.y() * ratio));
            drawnRow += c.red() > 170 && c.green() < 130 && c.blue() < 130 ? 'R' : '.';
            const QPointF local = view->mapFromScene(scene);
            QQmlExpression probe(qmlContext(view), view, QStringLiteral(
                    "(function(){ const h = viewport.editorPick(%3, %1, %2);"
                    " return h ? String(h.editorKind) + ':' + String(h.editorAxis) : '' })()")
                    .arg(local.x()).arg(local.y()).arg(view->objectName()));
            const QString hit = probe.evaluate().toString();
            pickedRow += hit == QStringLiteral("move:x") ? 'M' : hit == QStringLiteral("resize:x") ? 'S'
                       : hit.isEmpty() ? '.' : 'o';
        }
        std::cout << "x 300..900 step 4 at y " << at.y() << "\n  drawn  " << drawnRow << "\n  picked " << pickedRow
                  << '\n';
    }
    const QVariantMap before = boxes->property("selectedTarget").toMap();
    Drag(window, at, at + QPointF(80, 0));
    const QVariantMap after = boxes->property("selectedTarget").toMap();
    std::cout << "box center x " << before.value(QStringLiteral("centerX")).toDouble() << " -> "
              << after.value(QStringLiteral("centerX")).toDouble() << " (arrow at " << at.x() << ", "
              << at.y() << ")\n";
    if (before.value(QStringLiteral("centerX")) == after.value(QStringLiteral("centerX")))
        return "dragging the box's X arrow did not move the box";
    return {};
}

std::string PoseCopyProblem(QQuickWindow *window, forevertas::app::SearchController &controller,
                            forevertas::viewer::RaceViewerController &viewer) {
    QObject *const poses = controller.property("poseTargets").value<QObject *>();
    QQuickItem *const viewport = window->findChild<QQuickItem *>(QStringLiteral("raceViewport"));
    // the button a user sees (the settings may exist more than once)
    const auto button = [&](const char *name) {
        QQuickItem *found = nullptr;
        for (QQuickItem *item : window->findChildren<QQuickItem *>(QString::fromLatin1(name)))
            if (found == nullptr || (item->isVisible() && !found->isVisible()))
                found = item;
        return found != nullptr && found->property("enabled").toBool() &&
               QMetaObject::invokeMethod(found, "clicked");
    };
    const auto pose = [&]() { return poses->property("selectedTarget").toMap(); };
    const auto position = [&]() { return pose().value(QStringLiteral("position")).value<QVector3D>(); };
    const auto rotation = [&]() { return pose().value(QStringLiteral("rotation")).value<QQuaternion>(); };
    const auto same = [](const QQuaternion &a, const QQuaternion &b) {
        return std::abs(QQuaternion::dotProduct(a.normalized(), b.normalized())) > 0.99999F;
    };
    if (viewport == nullptr)
        return "no race viewport";
    const QQuaternion before = rotation();
    // the camera as the click finds it (a drawn view's camera may still be settling)
    if (!button("poseTargetPositionFromCameraButton"))
        return "no Position from Camera button";
    const QVector3D camera = viewport->property("sceneCameraPosition").value<QVector3D>();
    Settle(20);
    if (position() != camera || !same(rotation(), before))
        return "Position from Camera did not take the camera's position alone: pose at (" +
               std::to_string(position().x()) + ", " + std::to_string(position().y()) + ", " +
               std::to_string(position().z()) + "), camera at (" + std::to_string(camera.x()) + ", " +
               std::to_string(camera.y()) + ", " + std::to_string(camera.z()) + "), rotation kept " +
               std::to_string(same(rotation(), before));
    if (!button("poseTargetRotationFromCameraButton"))
        return "no Rotation from Camera button";
    const QQuaternion cameraRotation = viewport->property("sceneCameraRotation").value<QQuaternion>();
    Settle(20);
    const QQuaternion fromCamera = rotation();
    const float ahead = (fromCamera.rotatedVector(QVector3D(0, 0, 1)) -
                         cameraRotation.rotatedVector(QVector3D(0, 0, -1))).length();
    const float right = (fromCamera.rotatedVector(QVector3D(-1, 0, 0)) -
                         cameraRotation.rotatedVector(QVector3D(1, 0, 0))).length();
    if (position() != camera || ahead > 1e-3F || right > 1e-3F)
        return "Rotation from Camera: the pose does not face where the camera looks (ahead off by " +
               std::to_string(ahead) + ", right by " + std::to_string(right) + ")";
    if (!button("poseTargetPositionFromCarButton"))
        return "no Position from Car button";
    Settle(20);
    if (position() != viewer.carPosition() || !same(rotation(), fromCamera))
        return "Position from Car did not take the car's position alone";
    if (!button("poseTargetRotationFromCarButton"))
        return "no Rotation from Car button";
    Settle(20);
    if (position() != viewer.carPosition() || !same(rotation(), viewer.carRotation()))
        return "Rotation from Car did not take the car's rotation alone";
    return {};
}

void PressEnter(QQuickWindow *window) {
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
    QCoreApplication::sendEvent(window, &press);
    QCoreApplication::sendEvent(window, &release);
}

}  // namespace

int main(int argc, char **argv) {
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ForeverTASTests"));
    QCoreApplication::setApplicationName(QStringLiteral("QmlModuleTests"));
    QStandardPaths::setTestModeEnabled(true);

    forevertas::app::SearchController controller;
    forevertas::viewer::RaceViewerController viewer;
    forevertas::app::BindInputPreview(controller, viewer);
    forevertas::viewer::RegisterRaceViewerQmlTypes();
    // FOREVERTAS_RENDER_MODULE set: the In-game render module, drawing as the app has it
    const bool inGame = qEnvironmentVariableIsSet("FOREVERTAS_RENDER_MODULE");
    if (inGame) {
        if (!forevertas::viewer::LoadedRenderModule().loaded) {
            std::cerr << "the render module did not load\n";
            return 1;
        }
        QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    }
    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::warnings, [](const QList<QQmlError> &warnings) {
        for (const QQmlError &warning : warnings)
            std::cerr << warning.toString().toStdString() << '\n';
    });
    engine.addImportPath(QStringLiteral(FOREVERTAS_QML_IMPORT_DIR));
    forevertas::app::McpBridge mcp(controller, &engine, false);
    engine.setInitialProperties({
            {QStringLiteral("mcp"), QVariant::fromValue(static_cast<QObject *>(&mcp))},
            {QStringLiteral("controller"), QVariant::fromValue(static_cast<QObject *>(&controller))},
            {QStringLiteral("viewer"), QVariant::fromValue(static_cast<QObject *>(&viewer))}});
    engine.loadFromModule(QStringLiteral("ForeverTAS"), QStringLiteral("Main"));
    auto *const window = qobject_cast<QQuickWindow *>(
            engine.rootObjects().isEmpty() ? nullptr : engine.rootObjects().front());
    if (window == nullptr) {
        std::cerr << "the ForeverTAS module's Main did not load\n";
        return 1;
    }
    window->show();
    Settle(500);
    // With PACKS REPLAY: the replay loaded, as a user has it (the target
    // overlays then judge its run on every change).
    if (argc == 3) {
        controller.setPacksDirectory(QString::fromLocal8Bit(argv[1]));
        controller.setReplayPath(QString::fromLocal8Bit(argv[2]));
        viewer.loadMap(QString::fromLocal8Bit(argv[1]), QString::fromLocal8Bit(argv[2]));
        QElapsedTimer loading;
        loading.start();
        while (!(viewer.loaded() && viewer.runCount() > 0) && loading.elapsed() < 30000)
            Settle(50);
        if (!viewer.loaded()) {
            std::cerr << "the replay did not load\n";
            return 1;
        }
        // what a stopped search leaves (users froze there): its improvements,
        // each restart's latest shown, and the best run
        const QString packs = QString::fromLocal8Bit(argv[1]), replay = QString::fromLocal8Bit(argv[2]);
        const auto frames = [](float offset) {
            std::vector<forevertas::SearchTimelineFrame> result(1000);
            for (std::size_t index = 0; index < result.size(); ++index) {
                result[index].timeMs = static_cast<std::int64_t>(index) * 10;
                result[index].positionX = 500.0f + 0.3f * static_cast<float>(index);
                result[index].positionY = 10.0f;
                result[index].positionZ = 500.0f + offset;
                result[index].linearSpeedX = 30.0f;
                result[index].checkpointsTotal = 1u;
            }
            return result;
        };
        for (std::uint64_t number = 1; number <= 40; ++number)
            viewer.addSearchImprovement(packs, replay, frames(0.05f * static_cast<float>(number)),
                                        QStringLiteral("optimized-cpu"), 7u, number, number / 6u);
        viewer.addSearchRun(packs, replay, frames(3.0f));
        Settle(500);
    }

    if (inGame) {
        window->setProperty("renderMode", QStringLiteral("module"));
        Settle(3000);
    }
    // FTAS_RENDER_MODE / FTAS_THROUGH_BLOCKS: the gizmo check in that view
    if (qEnvironmentVariableIsSet("FTAS_RENDER_MODE"))
        window->setProperty("renderMode", qEnvironmentVariable("FTAS_RENDER_MODE"));
    if (qEnvironmentVariableIsSet("FTAS_THROUGH_BLOCKS"))
        controller.setProperty("drawTargetsThroughBlocks", true);
    Settle(500);
    // the 3D view drawn (not offscreen): the gizmo check, and not the Enter
    // rounds (too slow on a software renderer)
    const bool drawn = QGuiApplication::platformName() != QStringLiteral("offscreen");
    if (argc == 3 && drawn) {
        const std::string problem = GizmoDragProblem(window, controller, viewer);
        if (!problem.empty()) {
            std::cerr << problem << '\n';
            return 1;
        }
        std::cout << "a box's arrow drags the box\n";

    }
    // The scripted telemetry's edit button lies inside its panel and has an
    // icon (users saw no way to edit it).
    if (argc == 3) {
        viewer.setProperty("telemetryScript", QStringLiteral(
                "Camera pos: X {camera.x} Y {camera.y} Z {camera.z} and a long tail of text to fill the line"));
        Settle(200);
        QQuickItem *const panel = window->findChild<QQuickItem *>(QStringLiteral("scriptedTelemetry"));
        QQuickItem *const edit = window->findChild<QQuickItem *>(QStringLiteral("editTelemetryButton"));
        if (panel == nullptr || edit == nullptr)
            return std::cerr << "no scripted telemetry panel or edit button\n", 1;
        const QRectF inPanel = edit->mapRectToItem(panel, QRectF(0, 0, edit->width(), edit->height()));
        if (!edit->isVisible() || inPanel.right() > panel->width() + 0.5 || inPanel.left() < 0) {
            std::cerr << "the telemetry edit button is outside its panel (" << inPanel.left() << ".."
                      << inPanel.right() << " of " << panel->width() << ")\n";
            return 1;
        }
        // drawn with an icon, not a character a font may lack
        // (a ThemedButton's own text label drew an icon set on it as nothing)
        QQmlExpression icon(qmlContext(edit), edit, QStringLiteral("icon.source.toString()"));
        const QObject *const content = edit->property("contentItem").value<QObject *>();
        if (icon.evaluate().toString().isEmpty() || content == nullptr ||
            !QByteArray(content->metaObject()->className()).contains("IconLabel"))
            return std::cerr << "the telemetry edit button draws no icon\n", 1;
        // FTAS_PICTURE: the window, to look at (needs the 3D view drawn),
        // and Edit > Preferences > MCP beside it (-mcp)
        if (qEnvironmentVariableIsSet("FTAS_PICTURE")) {
            window->grabWindow().save(qEnvironmentVariable("FTAS_PICTURE"));
            QObject *const preferences = window->findChild<QObject *>(QStringLiteral("preferencesDialog"));
            if (preferences != nullptr) {
                preferences->setProperty("section", 2);
                QMetaObject::invokeMethod(preferences, "open");
                Settle(400);
                window->grabWindow().save(qEnvironmentVariable("FTAS_PICTURE").replace(
                        QStringLiteral(".png"), QStringLiteral("-mcp.png")));
                QMetaObject::invokeMethod(preferences, "close");
                Settle(200);
            }
        }
        std::cout << "the telemetry edit button lies inside its panel\n";
    }
    // The pose target's position and rotation, each from the camera or the
    // car on its own; from the camera, the pose faces where it looks.
    if (argc == 3) {
        controller.setEvaluationTargetId(QStringLiteral("pose-target"));
        viewer.setProperty("selectedRunId", QStringLiteral("preview"));
        viewer.setProperty("timeMs", 3000);
        Settle(300);
        const std::string problem = PoseCopyProblem(window, controller, viewer);
        if (!problem.empty()) {
            std::cerr << problem << '\n';
            return 1;
        }
        std::cout << "pose position and rotation copy on their own; from the camera it faces its way\n";
    }
    // the settings live in these panels, a tab each: one shown at a time
    QQuickItem *const space = window->findChild<QQuickItem *>(QStringLiteral("dockSpace"));
    QObject *const layout = space != nullptr ? space->property("layout").value<QObject *>() : nullptr;
    if (layout == nullptr) {
        std::cerr << "no dock layout\n";
        return 1;
    }
    const auto show = [&](const QString &panel) {
        QMetaObject::invokeMethod(layout, "setOpen", Q_ARG(QString, panel), Q_ARG(bool, true));
        QString group;
        QMetaObject::invokeMethod(layout, "groupOf", Q_RETURN_ARG(QString, group), Q_ARG(QString, panel));
        QMetaObject::invokeMethod(layout, "setCurrent", Q_ARG(QString, group), Q_ARG(QString, panel));
        Settle();
    };
    // The target panel's header hidden, then a click in the middle of its top edge.
    show(QStringLiteral("target"));
    QVariant shown;
    QMetaObject::invokeMethod(space, "setHeaderShown", Q_ARG(QVariant, QStringLiteral("target")),
                              Q_ARG(QVariant, false));
    QQuickItem *panel = nullptr;
    for (QQuickItem *item : space->findChildren<QQuickItem *>())
        if (item->property("panelId").toString() == QStringLiteral("target") &&
            item->property("isDockPanel").toBool())
            panel = item;
    QQuickItem *const frame = panel != nullptr ? panel->property("frame").value<QQuickItem *>() : nullptr;
    // the header slides away (slowly where drawing is slow)
    QElapsedTimer sliding;
    sliding.start();
    while (frame != nullptr && std::abs(panel->y() - frame->y()) > 1 && sliding.elapsed() < 4000)
        Settle(50);
    if (frame == nullptr || std::abs(panel->y() - frame->y()) > 1) {
        std::cerr << "the target panel's header did not hide\n";
        return 1;
    }
    Click(window, frame->mapToScene(QPointF(frame->width() / 2, 2)));
    Settle(400);
    QMetaObject::invokeMethod(space, "headerShown", Q_RETURN_ARG(QVariant, shown),
                              Q_ARG(QVariant, QStringLiteral("target")));
    if (!shown.toBool()) {
        std::cerr << "a click on the panel's top edge did not bring its header back\n";
        return 1;
    }
    std::cout << "a hidden header comes back with a click on the panel's top edge\n";

    const int rounds = drawn ? 0 : qEnvironmentVariableIntValue("FTAS_ENTER_ROUNDS") > 0
            ? qEnvironmentVariableIntValue("FTAS_ENTER_ROUNDS") : 1;
    int pressed = 0;
    // the slowest Enter, and where: a press that hangs the window is a bug too
    qint64 slowest = 0;
    std::string slowestField;
    for (int round = 0; round < rounds; ++round) {
        for (const QVariant &option : controller.evaluationTargetOptions()) {
            const QString target = option.toMap().value(QStringLiteral("id")).toString();
            controller.setEvaluationTargetId(target);
            int count = 0;
            for (const QString &panel : {QStringLiteral("target"), QStringLiteral("base"),
                                         QStringLiteral("passes"), QStringLiteral("search")}) {
                show(panel);
                const qsizetype fields = VisibleNumberFields(window).size();
                for (qsizetype index = 0; index < fields; ++index) {
                    // the field list changes as values do: take it again each time
                    const QList<QQuickItem *> now = VisibleNumberFields(window);
                    if (index >= now.size())
                        break;
                    QQuickItem *const field = now[index];
                    const QString original = field->property("text").toString();
                    // a different value (the setter runs), then the original back
                    for (const QString &text : {QStringLiteral("1"), original}) {
                        field->forceActiveFocus();
                        field->setProperty("text", text);
                        QElapsedTimer press;
                        press.start();
                        PressEnter(window);
                        QCoreApplication::processEvents();
                        if (press.elapsed() > slowest) {
                            slowest = press.elapsed();
                            slowestField = (target + QLatin1Char(' ') + panel + QLatin1Char(' ') +
                                            field->objectName()).toStdString();
                        }
                        Settle(2);
                        ++pressed;
                    }
                    ++count;
                }
            }
            if (round == 0)
                std::cout << target.toStdString() << ": Enter in " << count << " fields\n";
        }
    }
    if (pressed == 0 && !drawn) {
        std::cerr << "no number field was found to press Enter in\n";
        return 1;
    }
    std::cout << "Enter pressed " << pressed << " times without a crash; slowest " << slowest
              << " ms (" << slowestField << ")\n";
    const qint64 limit = 500;
    if (slowest > limit) {
        std::cerr << "an Enter took longer than " << limit << " ms\n";
        return 1;
    }

    // "Go to": a typed place is gone to, a wrong one says why, and the key's
    // box opens over the race view (needs the replay loaded).
    if (argc == 3 && !drawn) {
        QObject *const controls = window->findChild<QObject *>(QStringLiteral("raceControls"));
        QVariant error;
        QMetaObject::invokeMethod(controls, "goTo", Q_RETURN_ARG(QVariant, error),
                                  Q_ARG(QVariant, QStringLiteral("0:01.5")));
        Settle(50);
        if (!error.toString().isEmpty() || viewer.timeMs() != 1500) {
            std::cerr << "go to 0:01.5 landed at " << viewer.timeMs() << " ms: " << error.toString().toStdString()
                      << '\n';
            return 1;
        }
        QMetaObject::invokeMethod(controls, "goTo", Q_RETURN_ARG(QVariant, error),
                                  Q_ARG(QVariant, QStringLiteral("+0.5")));
        QVariant wrong;
        QMetaObject::invokeMethod(controls, "goTo", Q_RETURN_ARG(QVariant, wrong),
                                  Q_ARG(QVariant, QStringLiteral("banana")));
        if (viewer.timeMs() != 2000 || wrong.toString().isEmpty()) {
            std::cerr << "go to +0.5 or a wrong place failed\n";
            return 1;
        }
        QMetaObject::invokeMethod(controls, "goToRequested");
        Settle(100);
        QObject *const box = window->findChild<QObject *>(QStringLiteral("goToPopup"));
        QObject *const field = box ? box->findChild<QObject *>(QStringLiteral("goToField")) : nullptr;
        if (box == nullptr || !box->property("opened").toBool() || field == nullptr ||
            field->property("text").toString() != QStringLiteral("+0.5")) {
            std::cerr << "the Go to box did not open with the last place\n";
            return 1;
        }
        QMetaObject::invokeMethod(box, "close");
        std::cout << "go to: typed places, a wrong one explained, the key's box\n";
    }

    // A number typed out of range or not whole is stored as the field shows
    // it (a tester's pass showed valid numbers the search refused).
    if (!drawn) {
        // a whole-number field from 0 to 10 that stores what it is edited to
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nimport \"" +
                                  QUrl::fromLocalFile(QStringLiteral(FOREVERTAS_QML_IMPORT_DIR) +
                                                      QStringLiteral("/ForeverTAS/qml/settings")).toString().toUtf8() +
                                  "\"\nScrubNumberField { width: 120; height: 30; integer: true; minimum: 0;"
                                  " maximum: 10; value: \"1\"; onEdited: edited => value = edited }",
                          QUrl());
        auto *const whole = qobject_cast<QQuickItem *>(component.create());
        if (whole == nullptr) {
            std::cerr << "no number field: " << component.errorString().toStdString() << '\n';
            return 1;
        }
        whole->setParentItem(window->contentItem());
        whole->setZ(1e6);
        const qint64 minimum = qint64(whole->property("minimum").toDouble());
        const std::pair<QString, QString> cases[] = {
                {QStringLiteral("-5"), QString::number(minimum)},
                {QString::number(minimum + 2) + QStringLiteral(".6"), QString::number(minimum + 3)},
                {QStringLiteral("25"), QStringLiteral("10")}};
        for (const auto &[typed, stored] : cases) {
            whole->forceActiveFocus();
            whole->setProperty("text", typed);
            PressEnter(window);
            Settle(50);
            const QString value = whole->property("value").toString();
            const QString text = whole->property("text").toString();
            if (value != stored || text != stored) {
                std::cerr << "typed " << typed.toStdString() << " in " << whole->objectName().toStdString() << ": stored "
                          << value.toStdString() << ", shown " << text.toStdString() << '\n';
                return 1;
            }
        }
        std::cout << "typed numbers are stored as the field shows them\n";
    }
    return 0;
}
