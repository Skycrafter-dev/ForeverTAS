// How long the window's thread is busy while dragging, as a fast mouse
// drags (a move every millisecond): a dock panel by its header, and the race
// view, and the pointer moved over the race view without a button. Not a test: forevertas-drag-benchmark PACKS REPLAY prints, per drag,
// the moves handled, the window thread's time per move (mean, 99th
// percentile, worst) and the frames drawn.
#include "app/input_preview_binding.h"
#include "app/search_controller.h"
#include "viewer/race_timeline_item.h"
#include "viewer/race_viewer_controller.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

void Settle(int milliseconds) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
}

void Mouse(QQuickWindow *window, QEvent::Type type, QPointF at, Qt::MouseButtons buttons) {
    QMouseEvent event(type, at, at, window->mapToGlobal(at),
                      type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &event);
}

// Presses at start, moves along a loop for durationMs (a move per ms, as it
// comes, the window thread taking what it takes), releases.
void Drag(QQuickWindow *window, const char *name, QPointF start, qreal radius, int durationMs,
          bool hover = false) {
    int frames = 0;
    const auto connection = QObject::connect(window, &QQuickWindow::afterAnimating, [&] { ++frames; });
    std::vector<double> busy;
    if (!hover)
        Mouse(window, QEvent::MouseButtonPress, start, Qt::LeftButton);
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < durationMs) {
        const double t = static_cast<double>(clock.nsecsElapsed()) / 1e9;
        const QPointF at = start + QPointF(radius * std::sin(t * 3.0), 0.5 * radius * (1.0 - std::cos(t * 3.0)));
        QElapsedTimer handling;
        handling.start();
        Mouse(window, QEvent::MouseMove, at, hover ? Qt::NoButton : Qt::LeftButton);
        QCoreApplication::processEvents();
        busy.push_back(static_cast<double>(handling.nsecsElapsed()) / 1e6);
        const qint64 next = static_cast<qint64>(busy.size());
        while (clock.elapsed() < next)
            QThread::usleep(100);
    }
    if (!hover)
        Mouse(window, QEvent::MouseButtonRelease, start, Qt::NoButton);
    Settle(200);
    QObject::disconnect(connection);
    std::vector<double> sorted = busy;
    std::sort(sorted.begin(), sorted.end());
    double total = 0;
    for (const double value : busy)
        total += value;
    std::printf("%-18s %5zu moves in %d ms; window thread per move: mean %.2f ms, p99 %.2f ms, worst %.1f ms; "
                "busy %.0f%%; %d frames\n",
                name, busy.size(), durationMs, total / static_cast<double>(busy.size()),
                sorted[sorted.size() * 99 / 100], sorted.back(), 100.0 * total / durationMs, frames);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: forevertas-drag-benchmark PACKS REPLAY\n");
        return 2;
    }
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ForeverTASTests"));
    QCoreApplication::setApplicationName(QStringLiteral("DragBenchmark"));
    QStandardPaths::setTestModeEnabled(true);

    forevertas::app::SearchController controller;
    forevertas::viewer::RaceViewerController viewer;
    forevertas::app::BindInputPreview(controller, viewer);
    forevertas::viewer::RegisterRaceViewerQmlTypes();
    QQmlApplicationEngine engine;
    engine.addImportPath(QStringLiteral(FOREVERTAS_QML_IMPORT_DIR));
    engine.setInitialProperties({
            {QStringLiteral("controller"), QVariant::fromValue(static_cast<QObject *>(&controller))},
            {QStringLiteral("viewer"), QVariant::fromValue(static_cast<QObject *>(&viewer))}});
    engine.loadFromModule(QStringLiteral("ForeverTAS"), QStringLiteral("Main"));
    auto *const window = qobject_cast<QQuickWindow *>(
            engine.rootObjects().isEmpty() ? nullptr : engine.rootObjects().front());
    if (window == nullptr)
        return 1;
    window->resize(1600, 1000);
    window->show();
    controller.setPacksDirectory(QString::fromLocal8Bit(argv[1]));
    controller.setReplayPath(QString::fromLocal8Bit(argv[2]));
    viewer.loadMap(QString::fromLocal8Bit(argv[1]), QString::fromLocal8Bit(argv[2]));
    QElapsedTimer loading;
    loading.start();
    while (!(viewer.loaded() && viewer.runCount() > 0) && loading.elapsed() < 30000)
        Settle(50);
    if (!viewer.loaded()) {
        std::fprintf(stderr, "the replay did not load\n");
        return 1;
    }
    Settle(1000);

    // a docked panel's header, and the race view's middle
    QQuickItem *header = nullptr;
    for (QQuickItem *item : window->findChildren<QQuickItem *>(QStringLiteral("dockHeaderDragArea")))
        if (item->isVisible() && item->width() > 100 && (header == nullptr || item->y() < header->y()))
            header = item;
    QQuickItem *const view = window->findChild<QQuickItem *>(QStringLiteral("rasterMapView"));
    if (header == nullptr || view == nullptr) {
        std::fprintf(stderr, "no panel header or race view\n");
        return 1;
    }
    const QPointF headerAt = header->mapToScene(QPointF(header->width() / 2, header->height() / 2));
    Drag(window, "dock panel", headerAt, 200, 3000);
    Settle(500);
    const QPointF viewAt = view->mapToScene(QPointF(view->width() / 2, view->height() / 2));
    Drag(window, "race view", viewAt, 150, 3000);
    Settle(500);
    {
        // frames drawn with no input at all, for comparison
        int idleFrames = 0;
        const auto counting = QObject::connect(window, &QQuickWindow::afterAnimating, [&] { ++idleFrames; });
        Settle(2000);
        QObject::disconnect(counting);
        std::printf("%-18s %d frames in 2000 ms\n", "idle", idleFrames);
    }
    Drag(window, "race view hover", viewAt, 150, 3000, true);
    return 0;
}
