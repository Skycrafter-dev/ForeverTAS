// The In-game module's video export as the Export dialog runs it
// (VideoExportController), for checking it on a platform by hand.
//
//   forevertas-video-export PACKS REPLAY OUT.mp4 [WIDTH HEIGHT FPS]
//
// Needs the render module next to the executable (or FOREVERTAS_RENDER_MODULE)
// and TrackMania's display settings (see the module).
#include "app/app_settings.h"
#include "app/input_preview_binding.h"
#include "app/search_controller.h"
#include "viewer/race_viewer_controller.h"
#include "viewer/video_export_controller.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#include <functional>
#include <iostream>

namespace {

bool WaitUntil(const std::function<bool()> &condition, int milliseconds) {
    QElapsedTimer timer;
    timer.start();
    while (!condition()) {
        if (timer.elapsed() > milliseconds) return false;
        QEventLoop loop;
        QTimer::singleShot(50, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 4) {
        std::cerr << "usage: forevertas-video-export PACKS REPLAY OUT.mp4 [WIDTH HEIGHT FPS]\n";
        return 2;
    }
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ForeverTASTests"));
    QCoreApplication::setApplicationName(QStringLiteral("VideoExport"));
    QStandardPaths::setTestModeEnabled(true);
    forevertas::app::AppSettings::Instance().clear();

    forevertas::app::SearchController controller;
    forevertas::viewer::RaceViewerController viewer;
    forevertas::app::BindInputPreview(controller, viewer);
    controller.setPacksDirectory(QString::fromLocal8Bit(argv[1]));
    controller.setReplayPath(QString::fromLocal8Bit(argv[2]));
    controller.extractReplayInputs();
    WaitUntil([&] { return !controller.baseInputScript().isEmpty(); }, 20000);
    controller.previewBaseInputs();
    if (!WaitUntil([&] { return viewer.loaded() && viewer.runCount() > 0; }, 60000)) {
        std::cerr << "the replay's run did not load\n";
        return 1;
    }

    forevertas::viewer::VideoExportController exporter;
    std::cout << "module available " << exporter.available() << std::endl;
    if (!exporter.available()) return 1;
    exporter.setViewer(&viewer);
    exporter.setOutputPath(QString::fromLocal8Bit(argv[3]));
    if (argc > 6) {
        exporter.setVideoWidth(std::atoi(argv[4]));
        exporter.setVideoHeight(std::atoi(argv[5]));
        exporter.setFramesPerSecond(std::atoi(argv[6]));
    }
    QObject::connect(&exporter, &forevertas::viewer::VideoExportController::stateChanged, [&] {
        std::cout << "status: " << exporter.status().toStdString() << std::endl;
    });
    if (!exporter.start()) {
        std::cerr << "export did not start: " << exporter.status().toStdString() << '\n';
        return 1;
    }
    WaitUntil([&] { return !exporter.running(); }, 50000);
    if (exporter.running()) {
        exporter.abort();
        WaitUntil([&] { return !exporter.running(); }, 10000);
        std::cout << "stopped at " << exporter.progress() * 100.0 << "%" << std::endl;
        return exporter.progress() > 0.0 ? 0 : 1;
    }
    std::cout << "finished: " << exporter.status().toStdString() << std::endl;
    return exporter.finishedPath().isEmpty() ? 1 : 0;
}
