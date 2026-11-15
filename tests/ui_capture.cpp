// Screenshots of the main window in given UI states, for checking layouts by
// eye. The replay's inputs are the previewed run.
//
//   forevertas-ui-capture PACKS REPLAY OUT_DIR [STATE...]
//
// States (each one PNG, applied in order, cumulative): vertical, horizontal
// (the Inputs timeline across), headers-off, headers-on, scrub (the timeline
// a second in).
#include "app/app_settings.h"
#include "app/diagnostics_startup.h"
#include "app/input_preview_binding.h"
#include "app/search_controller.h"
#include "app/search_session_store.h"
#include "viewer/race_timeline_item.h"
#include "viewer/race_viewer_controller.h"

#include <QApplication>
#include <QDir>
#include <QGuiApplication>
#include <QFile>
#include <QElapsedTimer>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QCursor>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <iostream>

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
        Wait(50);
    }
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 4) {
        std::cerr << "usage: forevertas-ui-capture PACKS REPLAY OUT_DIR [STATE...]\n";
        return 2;
    }
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ForeverTASTests"));
    QCoreApplication::setApplicationName(QStringLiteral("UiCapture"));
    QStandardPaths::setTestModeEnabled(true);
    forevertas::app::AppSettings::Instance().clear();

    forevertas::app::SearchController controller;
    forevertas::viewer::RaceViewerController viewer;
    forevertas::app::BindInputPreview(controller, viewer);
    forevertas::viewer::RegisterRaceViewerQmlTypes();
    // The log (Help > Check the car writes to it): next to the screenshots.
    forevertas::app::StartDiagnostics();
    forevertas::app::DiagnosticsLogs logs;
    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::warnings, [](const QList<QQmlError> &warnings) {
        for (const QQmlError &warning : warnings) std::cerr << warning.toString().toStdString() << '\n';
    });
    engine.setInitialProperties({{QStringLiteral("controller"), QVariant::fromValue(static_cast<QObject *>(&controller))},
                                 {QStringLiteral("viewer"), QVariant::fromValue(static_cast<QObject *>(&viewer))},
                                 {QStringLiteral("logs"), QVariant::fromValue(static_cast<QObject *>(&logs))}});
    engine.load(QUrl::fromLocalFile(QStringLiteral(FOREVERTAS_SOURCE_DIR "/qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        std::cerr << "failed to create Main.qml\n";
        return 1;
    }
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().front());
    if (window == nullptr) return 1;
    window->resize(1600, 900);

    controller.setPacksDirectory(QString::fromLocal8Bit(argv[1]));
    // FT_CAPTURE_SESSIONS=N: N saved sessions for this map in History
    if (const int sessions = qEnvironmentVariableIntValue("FT_CAPTURE_SESSIONS"); sessions > 0) {
        forevertas::SearchRequest request{argv[1], argv[2]};
        for (int session = 0; session < sessions; ++session) {
            // listed only with a saved restart
            const auto location = forevertas::app::SearchSessionStore::Create(request);
            forevertas::app::SearchSessionStore::SaveAbortedCycle(location, request, 0, forevertas::SearchLiveUpdate{});
        }
    }
    controller.setReplayPath(QString::fromLocal8Bit(argv[2]));
    controller.extractReplayInputs();
    WaitUntil([&] { return !controller.baseInputScript().isEmpty(); }, 20000);
    controller.previewBaseInputs();
    if (!WaitUntil([&] { return viewer.loaded() && viewer.runCount() > 0; }, 30000)) {
        std::cerr << "the replay's run did not load\n";
        return 1;
    }
    Wait(1500);

    const QString directory = QString::fromLocal8Bit(argv[3]);
    QDir().mkpath(directory);
    auto *timeline = window->findChild<QObject *>(QStringLiteral("raceTimeline"));
    auto *dockSpace = window->findChild<QObject *>(QStringLiteral("dockSpace"));
    int failures = 0;
    for (int i = 4; i < argc; ++i) {
        const QString state = QString::fromLocal8Bit(argv[i]);
        if (state == "horizontal" || state == "vertical") {
            if (timeline) timeline->setProperty("horizontal", state == "horizontal");
        } else if (state == "headers-off" || state == "headers-on") {
            if (dockSpace)
                QMetaObject::invokeMethod(dockSpace, "setAllHeadersShown",
                                          Q_ARG(QVariant, state == "headers-on"));
        } else if (state.startsWith("hide:") || state.startsWith("show:")) {
            // one panel's header; where its content and frame go while it slides
            QQuickItem *panel = nullptr;
            for (QQuickItem *item : dockSpace->findChildren<QQuickItem *>()) {
                if (item->property("isDockPanel").toBool() &&
                    item->property("panelId").toString() == state.mid(5)) panel = item;
            }
            const auto content = [&] {
                return panel ? QRectF(panel->mapToScene(QPointF(0, 0)), panel->size()) : QRectF();
            };
            const QRectF before = content();
            if (dockSpace)
                QMetaObject::invokeMethod(dockSpace, "setHeaderShown",
                                          Q_ARG(QVariant, state.mid(5)),
                                          Q_ARG(QVariant, state.startsWith("show:")));
            double topLow = before.top(), topHigh = before.top();
            double bottomLow = before.bottom(), bottomHigh = before.bottom();
            for (int step = 0; step < 30; ++step) {
                Wait(16);
                const QRectF now = content();
                topLow = std::min(topLow, now.top());
                topHigh = std::max(topHigh, now.top());
                bottomLow = std::min(bottomLow, now.bottom());
                bottomHigh = std::max(bottomHigh, now.bottom());
            }
            const QRectF after = content();
            std::cout << state.toStdString() << ": content top " << before.top() << " -> "
                      << after.top() << " (range " << topLow << ".." << topHigh << "), bottom "
                      << before.bottom() << " -> " << after.bottom() << " (range " << bottomLow
                      << ".." << bottomHigh << ")\n";
        } else if (state.startsWith("float:")) {
            // float:PANEL:x,y,w,h puts the panel's group floating there
            const QStringList parts = state.split(':');
            QObject *layout = dockSpace ? dockSpace->property("layout").value<QObject *>() : nullptr;
            QString group;
            if (layout)
                QMetaObject::invokeMethod(layout, "groupOf", Q_RETURN_ARG(QString, group),
                                          Q_ARG(QString, parts.value(1)));
            bool floating = false;
            if (layout)
                QMetaObject::invokeMethod(layout, "isFloating", Q_RETURN_ARG(bool, floating),
                                          Q_ARG(QString, group));
            if (layout && !floating)
                QMetaObject::invokeMethod(layout, "toggleFloating", Q_ARG(QString, group));
            const QStringList r = parts.value(2).split(',');
            if (layout && r.size() == 4) {
                QMetaObject::invokeMethod(layout, "groupOf", Q_RETURN_ARG(QString, group),
                                          Q_ARG(QString, parts.value(1)));
                QRectF used;
                QMetaObject::invokeMethod(layout, "placeFloating", Q_RETURN_ARG(QRectF, used),
                                          Q_ARG(QString, group),
                                          Q_ARG(QRectF, QRectF(r[0].toDouble(), r[1].toDouble(),
                                                               r[2].toDouble(), r[3].toDouble())));
            }
        } else if (state == "check-car") {
            // Help > Check the car; its answer
            QMetaObject::invokeMethod(window, "checkCar");
            QObject *dialog = window->findChild<QObject *>(QStringLiteral("carCheckDialog"));
            WaitUntil([&] { return dialog && dialog->property("opened").toBool(); }, 20000);
            std::cout << "check-car: " << (dialog ? dialog->property("message").toString().toStdString() : "?")
                      << '\n';
            if (dialog) QMetaObject::invokeMethod(dialog, "close");
        } else if (state.startsWith("car-style:")) {
            // car-style:hidden or car-style:shown for the selected car
            viewer.setVisualStyle(QStringLiteral("car:") + viewer.selectedRunStyleId(),
                                  QStringLiteral("visible"), state.endsWith("shown"));
        } else if (state == "module-car") {
            // the In-game view's car against the time shown
            QObject *module = window->findChild<QObject *>(QStringLiteral("moduleRenderView"));
            QVariantMap car;
            if (module) QMetaObject::invokeMethod(module, "carState", Q_RETURN_ARG(QVariantMap, car));
            std::cout << "module-car: frame time " << car.value("raceTimeMs").toDouble() << " ms, frames "
                      << car.value("frames").toUInt() << ", shown " << car.value("shown").toBool()
                      << "; viewer time " << viewer.property("presentationTimeMs").toDouble() << " ms\n";
            continue;
        } else if (state.startsWith("cuboid-drag")) {
            // cuboid-drag[:through|:raster]: a box ahead of the car, selected;
            // its Z resize handle (across the view) dragged 80 px with the
            // mouse; did it change?
            if (state.endsWith(":raster") || state.endsWith(":through"))
                controller.setDrawTargetsThroughBlocks(state.endsWith(":through"));
            auto *boxes = controller.cuboidTargets();
            const QVector3D car = viewer.property("carPosition").value<QVector3D>();
            if (boxes->count() == 0) boxes->addTarget(car.x(), car.y(), car.z());
            boxes->selectTarget(0);
            boxes->moveSelectedTo(car.x() + 12, car.y(), car.z());
            controller.setEvaluationTargetId(QStringLiteral("volume-entry-time"));
            Wait(1500);
            const auto before = boxes->selectedTarget();
            QQuickItem *raster = window->findChild<QQuickItem *>(QStringLiteral("rasterMapView"));
            QObject *handle = nullptr;
            for (QObject *model : window->findChildren<QObject *>(QStringLiteral("cuboidResizeHandleZ"))) {
                if (model->property("visible").toBool()) handle = model;
            }
            if (raster == nullptr || handle == nullptr) {
                std::cout << "cuboid-drag: no handle\n";
                ++failures;
                continue;
            }
            QVector3D at;
            QMetaObject::invokeMethod(raster, "mapFrom3DScene", Q_RETURN_ARG(QVector3D, at),
                                      Q_ARG(QVector3D, handle->property("scenePosition").value<QVector3D>()));
            const QPointF start = raster->mapToScene(QPointF(at.x(), at.y()));
            const auto send = [&](QEvent::Type type, QPointF point, Qt::MouseButtons buttons) {
                QMouseEvent event(type, point, point, window->mapToGlobal(point.toPoint()),
                                  type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons,
                                  Qt::NoModifier);
                QCoreApplication::sendEvent(window, &event);
            };
            send(QEvent::MouseButtonPress, start, Qt::LeftButton);
            QObject *viewportItem = window->findChild<QObject *>(QStringLiteral("raceViewport"));
            std::cout << "cuboid-drag: after the press, drag active "
                      << (viewportItem ? viewportItem->property("cuboidDragActive").toBool() : false)
                      << ", pointer captured "
                      << (viewportItem ? viewportItem->property("cuboidPointerCaptured").toBool() : false)
                      << ", handle visible " << handle->property("visible").toBool() << " pickable "
                      << handle->property("pickable").toBool() << '\n';
            for (int step = 1; step <= 8; ++step) {
                Wait(30);
                send(QEvent::MouseMove, start + QPointF(10.0 * step, 0), Qt::LeftButton);
            }
            send(QEvent::MouseButtonRelease, start + QPointF(80, 0), Qt::NoButton);
            Wait(500);
            const auto after = boxes->selectedTarget();
            std::cout << "cuboid-drag: handle at (" << start.x() << ", " << start.y() << "), size x "
                      << before.value("sizeX").toDouble() << " -> " << after.value("sizeX").toDouble()
                      << ", size z " << before.value("sizeZ").toDouble() << " -> "
                      << after.value("sizeZ").toDouble() << ", center z "
                      << before.value("centerZ").toDouble() << " -> " << after.value("centerZ").toDouble()
                      << (before == after ? " UNCHANGED" : " changed") << '\n';
        } else if (state == "timeline-wrap") {
            // the timeline's cursor dragged to the window's edge along time:
            // the pointer should come back from the other edge, time going on
            QQuickItem *line = window->findChild<QQuickItem *>(QStringLiteral("raceTimeline"));
            if (line == nullptr) { ++failures; continue; }
            const bool across = line->property("horizontal").toBool();
            const QPointF center = line->mapToScene(QPointF(line->width() / 2, line->height() / 2));
            const auto send = [&](QEvent::Type type, QPointF point, Qt::MouseButtons buttons) {
                QCursor::setPos(window->screen(), window->mapToGlobal(point.toPoint()));
                QMouseEvent event(type, point, point, window->mapToGlobal(point),
                                  type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons,
                                  Qt::NoModifier);
                QCoreApplication::sendEvent(window, &event);
            };
            send(QEvent::MouseButtonPress, center, Qt::LeftButton);
            const qint64 start = viewer.property("timeMs").toLongLong();
            // the timeline's own far edge along time
            const QPointF edge = line->mapToScene(across ? QPointF(line->width() - 0.5, line->height() / 2)
                                                         : QPointF(line->width() / 2, line->height() - 0.5));
            const QPointF near = line->mapToScene(across ? QPointF(line->width() - 40, line->height() / 2)
                                                         : QPointF(line->width() / 2, line->height() - 40));
            send(QEvent::MouseMove, near, Qt::LeftButton);
            Wait(100);
            const qint64 beforeEdge = viewer.property("timeMs").toLongLong();
            send(QEvent::MouseMove, edge, Qt::LeftButton);
            Wait(100);
            const QPoint jumped = window->mapFromGlobal(QCursor::pos(window->screen()));
            const QPointF jumpedInLine = line->mapFromScene(QPointF(jumped));
            const qint64 atEdge = viewer.property("timeMs").toLongLong();
            // carry on 30 px past the jump, as the hand would; where the
            // pointer could not jump (Wayland), on past the edge
            const bool warped = QRectF(QPointF(0, 0), line->size()).contains(jumpedInLine) &&
                                (across ? jumpedInLine.x() : jumpedInLine.y()) < 20;
            QPointF on = warped ? QPointF(jumped) : edge;
            if (across) on.setX(on.x() + 30); else on.setY(on.y() + 30);
            const auto move = [&](QPointF point) {
                QMouseEvent event(QEvent::MouseMove, point, point, window->mapToGlobal(point), Qt::NoButton,
                                  Qt::LeftButton, Qt::NoModifier);
                QCoreApplication::sendEvent(window, &event);
            };
            if (warped) {
                move(QPointF(jumped));
                Wait(100);
            }
            move(on);
            Wait(300);
            if (!warped) window->grabWindow().save(directory + "/timeline-drawn-pointer.png");
            const qint64 after = viewer.property("timeMs").toLongLong();
            send(QEvent::MouseButtonRelease, on, Qt::NoButton);
            std::cout << "timeline-wrap: " << (warped ? "pointer moved" : "pointer drawn (cannot move it)") << ", "
                      << (across ? "horizontal" : "vertical")
                      << ", pointer at the timeline's edge (" << (across ? line->width() : line->height())
                      << " px long) -> " << (across ? jumpedInLine.x() : jumpedInLine.y())
                      << "; time " << start << " -> " << beforeEdge << " -> " << atEdge << " -> " << after
                      << " ms\n";
        } else if (state.startsWith("panel-drag:") || state.startsWith("panel-resize:")) {
            // panel-drag:PANEL drags its header 200 px; panel-resize:PANEL its
            // bottom edge (floating): the time each pointer move takes
            const bool resize = state.startsWith("panel-resize:");
            const QString panelId = state.section(':', 1);
            QQuickItem *frame = nullptr;
            for (QQuickItem *item : dockSpace->findChildren<QQuickItem *>()) {
                if (item->property("groupId").isValid() && item->property("currentPanel").toString() == panelId)
                    frame = item;
            }
            if (frame == nullptr) { std::cout << state.toStdString() << ": no frame\n"; ++failures; continue; }
            const QPointF start = resize
                    ? frame->mapToScene(QPointF(frame->width() / 2, frame->height() - 1))
                    : frame->mapToScene(QPointF(frame->width() / 2, 12));
            const auto send = [&](QEvent::Type type, QPointF point, Qt::MouseButtons buttons) {
                QMouseEvent event(type, point, point, window->mapToGlobal(point),
                                  type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons,
                                  Qt::NoModifier);
                QCoreApplication::sendEvent(window, &event);
            };
            // perf record --control=fifo:PERF_CTL: only this drag is recorded
            static bool profiled = false;
            const QByteArray control = qgetenv("PERF_CTL");
            const bool profile = !profiled && !control.isEmpty();
            const auto perf = [&](const char *command) {
                if (!profile) return;
                if (FILE *fifo = std::fopen(control.constData(), "w")) {
                    std::fputs(command, fifo);
                    std::fclose(fifo);
                }
            };
            perf("enable\n");
            QElapsedTimer total;
            total.start();
            send(QEvent::MouseButtonPress, start, Qt::LeftButton);
            std::vector<double> times;
            for (int step = 1; step <= 40; ++step) {
                QElapsedTimer one;
                one.start();
                send(QEvent::MouseMove, start + QPointF(resize ? 0 : 5.0 * step, resize ? -2.0 * step : 3.0 * step),
                     Qt::LeftButton);
                QCoreApplication::processEvents();
                // PANEL_DRAG_FRAMES=1: a frame drawn after each move, as the screen would
                if (qEnvironmentVariableIsSet("PANEL_DRAG_FRAMES")) window->grabWindow();
                times.push_back(one.nsecsElapsed() / 1e6);
            }
            send(QEvent::MouseButtonRelease, start + QPointF(resize ? 0 : 200, resize ? -80 : 120), Qt::NoButton);
            perf("disable\n");
            profiled = profiled || profile;
            std::cout << state.toStdString() << ": first moves";
            for (std::size_t index = 0; index < 6 && index < times.size(); ++index)
                std::cout << ' ' << times[index];
            std::cout << " ms\n";
            std::sort(times.begin(), times.end());
            double sum = 0;
            for (double t : times) sum += t;
            std::cout << state.toStdString() << ": " << times.size() << " moves, mean " << sum / times.size()
                      << " ms, median " << times[times.size() / 2] << " ms, max " << times.back()
                      << " ms, total " << total.elapsed() << " ms\n";
        } else if (state.startsWith("hide-mid:")) {
            // a panel's header half way through hiding: a picture of it
            QMetaObject::invokeMethod(dockSpace, "setHeaderShown", Q_ARG(QVariant, state.mid(9)),
                                      Q_ARG(QVariant, false));
            Wait(70);
            const QString path = directory + "/" + QString::number(i - 3) + "-hide-mid.png";
            window->grabWindow().save(path);
            std::cout << path.toStdString() << '\n';
            Wait(600);
            continue;
        } else if (state == "timeline-grab") {
            // the yellow arrow held: pressed, then the pointer moved by the
            // system (X11: 30 moves of 10 px by events; Wayland: by whoever
            // moves the real pointer while /tmp/timeline-grab-ready exists,
            // e.g. wlrctl), then released; the time and where the pointer is
            QQuickItem *line = window->findChild<QQuickItem *>(QStringLiteral("raceTimeline"));
            if (line == nullptr) { ++failures; continue; }
            const bool across = line->property("horizontal").toBool();
            const QPointF center = line->mapToScene(QPointF(line->width() / 2, line->height() / 2));
            const QPointF global = window->mapToGlobal(center);
            const auto send = [&](QEvent::Type type, QPointF point, QPointF at, Qt::MouseButtons buttons) {
                QMouseEvent event(type, point, point, at,
                                  type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons,
                                  Qt::NoModifier);
                QCoreApplication::sendEvent(window, &event);
            };
            const qint64 start = viewer.property("timeMs").toLongLong();
            const bool wayland = QGuiApplication::platformName().startsWith(QLatin1String("wayland"));
            if (wayland) {
                // a real pointer (a virtual device in the compositor) presses
                // at the arrow, moves and lets go: its x y in the window here
                QFile ready(QStringLiteral("/tmp/timeline-grab-ready"));
                ready.open(QIODevice::WriteOnly);
                ready.write(QStringLiteral("%1 %2 %3 %4\n").arg(qRound(center.x())).arg(qRound(center.y()))
                                    .arg(window->width()).arg(window->height()).toUtf8());
                ready.close();
                WaitUntil([] { return QFile::exists(QStringLiteral("/tmp/timeline-grab-done")); }, 20000);
                QFile::remove(QStringLiteral("/tmp/timeline-grab-ready"));
                QFile::remove(QStringLiteral("/tmp/timeline-grab-done"));
                Wait(300);
                std::cout << "timeline-grab: wayland, time " << start << " -> "
                          << viewer.property("timeMs").toLongLong() << " ms\n";
                continue;
            }
            QCursor::setPos(window->screen(), global.toPoint());
            send(QEvent::MouseButtonPress, center, global, Qt::LeftButton);
            {
                for (int step = 0; step < 30; ++step) {
                    // each move from where the pointer was put back
                    const QPoint now = QCursor::pos(window->screen());
                    const QPointF next = QPointF(now) + (across ? QPointF(10, 0) : QPointF(0, 10));
                    send(QEvent::MouseMove, window->mapFromGlobal(next), next, Qt::LeftButton);
                    Wait(20);
                }
            }
            Wait(300);
            const qint64 during = viewer.property("timeMs").toLongLong();
            const QPoint before = QCursor::pos(window->screen());
            send(QEvent::MouseButtonRelease, center, global, Qt::NoButton);
            Wait(300);
            const QPointF after = line->mapFromGlobal(QPointF(QCursor::pos(window->screen())));
            std::cout << "timeline-grab: " << QGuiApplication::platformName().toStdString() << ", "
                      << (across ? "horizontal" : "vertical") << ", time " << start << " -> " << during
                      << " ms; pointer held at (" << before.x() << ", " << before.y()
                      << "), after release in the timeline at (" << after.x() << ", " << after.y() << ")\n";
        } else if (state.startsWith("tab-drag:")) {
            // tab-drag:PANEL:DX:DY: its tab pressed, moved by DX,DY in steps,
            // let go; the group's tabs after and whether it came loose
            const QStringList parts = state.split(':');
            const QString panelId = parts.value(1);
            QObject *layout = dockSpace->property("layout").value<QObject *>();
            QString group;
            QMetaObject::invokeMethod(layout, "groupOf", Q_RETURN_ARG(QString, group), Q_ARG(QString, panelId));
            QQuickItem *tab = nullptr;
            std::function<void(QQuickItem *)> look = [&](QQuickItem *item) {
                for (QQuickItem *child : item->childItems()) {
                    if (child->property("panelId").toString() == panelId &&
                        !child->property("isDockPanel").toBool() && child->property("active").isValid() &&
                        child->isVisible())
                        tab = child;
                    look(child);
                }
            };
            look(window->contentItem());
            if (tab == nullptr) { std::cout << "tab-drag: no tab\n"; ++failures; continue; }
            const QPointF start = tab->mapToScene(QPointF(tab->width() / 2, tab->height() / 2));
            const QPointF by(parts.value(2).toDouble(), parts.value(3).toDouble());
            const auto send = [&](QEvent::Type type, QPointF point, Qt::MouseButtons buttons) {
                QMouseEvent event(type, point, point, window->mapToGlobal(point),
                                  type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, Qt::NoModifier);
                QCoreApplication::sendEvent(window, &event);
            };
            send(QEvent::MouseButtonPress, start, Qt::LeftButton);
            for (int step = 1; step <= 20; ++step) {
                send(QEvent::MouseMove, start + by * (step / 20.0), Qt::LeftButton);
                Wait(15);
            }
            send(QEvent::MouseButtonRelease, start + by, Qt::NoButton);
            Wait(800);
            QString after;
            QMetaObject::invokeMethod(layout, "groupOf", Q_RETURN_ARG(QString, after), Q_ARG(QString, panelId));
            QStringList tabs;
            for (const QVariant &entry : layout->property("groups").toList()) {
                const QVariantMap map = entry.toMap();
                if (map.value("id").toString() == group) tabs = map.value("tabs").toStringList();
            }
            std::cout << state.toStdString() << ": " << (after == group ? "still in its group" : "came loose")
                      << "; that group's tabs: " << tabs.join(',').toStdString() << '\n';
            continue;
        } else if (state == "cameras") {
            // which of the car cameras 1, 2, 3 can show this run
            for (int preset = 1; preset <= 3; ++preset) {
                viewer.setProperty("cameraPreset", preset);
                Wait(200);
                std::cout << "cameras: " << preset << (viewer.property("carCameraAvailable").toBool() ? " shows" : " NOT AVAILABLE")
                          << '\n';
            }
            continue;
        } else if (state == "sessions-open" || state == "sessions-wheel") {
            // the History session list clicked open; then the wheel turned
            // up over it (to older sessions): how much of the list shows and whether it scrolls
            auto *combo = window->findChild<QQuickItem *>(QStringLiteral("sessionSelector"));
            if (combo == nullptr || !combo->isVisible()) { std::cout << state.toStdString() << ": no session list\n"; ++failures; continue; }
            auto *list = window->findChild<QQuickItem *>(QStringLiteral("sessionSelectorPopupList"));
            if (state == "sessions-open") {
                const QPointF at = combo->mapToScene(QPointF(combo->width() / 2, combo->height() / 2));
                for (const QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
                    QMouseEvent event(type, at, at, window->mapToGlobal(at), Qt::LeftButton,
                                      type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
                    QCoreApplication::sendEvent(window, &event);
                }
                Wait(600);
                list = window->findChild<QQuickItem *>(QStringLiteral("sessionSelectorPopupList"));
            } else if (list != nullptr) {
                const QPointF at = list->mapToScene(QPointF(list->width() / 2, list->height() / 2));
                const double before = list->property("contentY").toDouble();
                for (int notch = 0; notch < 5; ++notch) {
                    QWheelEvent event(at, window->mapToGlobal(at), QPoint(), QPoint(0, 120), Qt::NoButton,
                                      Qt::NoModifier, Qt::NoScrollPhase, false);
                    QCoreApplication::sendEvent(window, &event);
                    Wait(60);
                }
                Wait(1500);
                std::cout << "sessions-wheel: list scrolled " << before << " -> "
                          << list->property("contentY").toDouble() << '\n';
            }
            if (list == nullptr || !list->isVisible()) { std::cout << state.toStdString() << ": list not open\n"; ++failures; continue; }
            const QPointF top = list->mapToScene(QPointF(0, 0));
            const QPointF field = combo->mapToScene(QPointF(0, 0));
            std::cout << state.toStdString() << ": " << combo->property("count").toInt() << " sessions, list "
                      << list->height() << " px tall of " << list->property("contentHeight").toDouble()
                      << " px, opens " << (top.y() < field.y() ? "above" : "below") << " the field (list y "
                      << top.y() << ", field y " << field.y() << ", window " << window->height() << ")\n";
        } else if (state == "timeline-overshoot") {
            // the arrow held, dragged 3000 px past the run's end, then 100 px
            // back: time should come back from the end at once
            QQuickItem *line = window->findChild<QQuickItem *>(QStringLiteral("raceTimeline"));
            if (line == nullptr) { ++failures; continue; }
            const bool across = line->property("horizontal").toBool();
            const QPointF center = line->mapToScene(QPointF(line->width() / 2, line->height() / 2));
            const auto send = [&](QEvent::Type type, QPointF point, QPointF at, Qt::MouseButtons buttons) {
                QMouseEvent event(type, point, point, at,
                                  type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons,
                                  Qt::NoModifier);
                QCoreApplication::sendEvent(window, &event);
            };
            const auto moveBy = [&](qreal pixels, int steps) {
                for (int step = 0; step < steps; ++step) {
                    const QPoint now = QCursor::pos(window->screen());
                    const QPointF next = QPointF(now) + (across ? QPointF(pixels / steps, 0) : QPointF(0, pixels / steps));
                    send(QEvent::MouseMove, window->mapFromGlobal(next), next, Qt::LeftButton);
                    Wait(10);
                }
            };
            QCursor::setPos(window->screen(), window->mapToGlobal(center).toPoint());
            send(QEvent::MouseButtonPress, center, window->mapToGlobal(center), Qt::LeftButton);
            moveBy(3000, 60);
            Wait(200);
            const qint64 atEnd = viewer.property("timeMs").toLongLong();
            moveBy(-100, 10);
            Wait(200);
            const qint64 back = viewer.property("timeMs").toLongLong();
            send(QEvent::MouseButtonRelease, center, window->mapToGlobal(center), Qt::NoButton);
            Wait(200);
            std::cout << "timeline-overshoot: end " << viewer.property("durationMs").toLongLong() << " ms, at the end "
                      << atEnd << " ms, 100 px back " << back << " ms\n";
        } else if (state.startsWith("wait:")) {
            Wait(state.mid(5).toInt());
            continue;
        } else if (state == "play" || state == "pause") {
            QMetaObject::invokeMethod(&viewer, state == "play" ? "play" : "pause");
            continue;
        } else if (state.startsWith("render:")) {
            window->setProperty("renderMode", state.mid(7));
        } else if (state == "car-parts:hidden" || state == "car-parts:shown") {
            // the car drawn or not while everything about it stays: what the
            // car probe must catch
            for (QObject *model : window->findChildren<QObject *>()) {
                if (model->objectName() == QLatin1String("selectedRunCarFilledModel"))
                    model->setProperty("visible", state.endsWith("shown"));
            }
        } else if (state == "scrub") {
            viewer.setTimeMs(1000);
        } else {
            std::cerr << "unknown state " << state.toStdString() << '\n';
            ++failures;
            continue;
        }
        Wait(2000);
        const QString path = directory + "/" + QString::number(i - 3) + "-" + state + ".png";
        if (!window->grabWindow().save(path)) ++failures;
        std::cout << path.toStdString() << '\n';
    }
    return failures == 0 ? 0 : 1;
}
