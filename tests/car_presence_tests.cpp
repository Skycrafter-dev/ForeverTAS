#include "app/search_completion.h"
#include "mutations/modifier_utils.h"
#include "viewer/race_viewer_controller.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QGuiApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>

#include <iostream>

namespace {
template <typename Predicate>
bool WaitUntil(Predicate predicate, int timeout = 120000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    return predicate();
}
}

// Each check covers one way a car used to drop out of the scene while it
// should still have been shown.
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    QGuiApplication app(argc, argv);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QCoreApplication::setOrganizationName(QStringLiteral("car-presence-test"));
    QCoreApplication::setApplicationName(QStringLiteral("car-presence-test"));
    using namespace forevertas;
    viewer::RaceViewerController viewer;
    const auto check = [](bool okay, const char *message) {
        if (!okay) std::cerr << message << '\n';
        return okay;
    };
    const QString packs = QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath();
    const QString replay = QFileInfo(QString::fromLocal8Bit(argv[2])).absoluteFilePath();
    // The same map spelled differently, as a typed path or another platform's
    // file dialog would.
    const QFileInfo replayInfo(replay);
    const QString oddReplay = replayInfo.absolutePath() + QStringLiteral("/./") + replayInfo.fileName();
    const QString oddPacks = packs + QStringLiteral("/.");
    std::vector<SearchTimelineFrame> frames(101);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        frames[i].timeMs = static_cast<std::int64_t>(i * 10);
        frames[i].positionX = static_cast<float>(i);
        frames[i].rotationW = 1.0f;
    }
    auto completion = std::make_shared<app::SearchCompletion>();
    completion->searchId = 1;
    completion->packsDirectory = packs;
    completion->replayPath = replay;
    completion->simulationBackendId = QStringLiteral("optimized-cpu");
    completion->bestTimeline = frames;
    completion->bestInputs = {SwitchEvent(0, SandboxInputAction::Accelerate, true)};
    viewer.setPreviewInputScript(QStringLiteral("0.00 press up"));

    // A finished search loads its map; the app's own load request for the
    // same map (spelled differently) must not throw the Best run away.
    viewer.beginSearchPreview(1);
    viewer.completeSearchPreview(completion);
    viewer.loadMap(oddPacks, oddReplay);
    if (!check(WaitUntil([&] { return viewer.loaded() && !viewer.loading() &&
                                     viewer.hasTrajectoryForRun(QStringLiteral("best")) &&
                                     viewer.hasTrajectoryForRun(QStringLiteral("preview")); }),
               "a load request for the same map discarded the finished Best run")) return 1;
    if (!check(viewer.isMapLoaded(packs, replay) && viewer.isMapLoaded(oddPacks, oddReplay) &&
               viewer.loadedReplayPath() == replay,
               "the loaded map was not recognized under another spelling")) return 1;
    viewer.endSearchPreview();

    // Search results name the map in their own way; they must not reload it.
    viewer.addSearchImprovement(oddPacks, oddReplay, frames, QStringLiteral("optimized-cpu"), 2, 1, 0);
    if (!check(!viewer.loading() && viewer.improvementCarCount() == 1,
               "an improvement for the loaded map reloaded it")) return 1;

    // Reloading the same map keeps the Best car, its selection and the
    // improvement preview; a preview edit made during the load is applied
    // once the load is done.
    viewer.setSelectedRunId(QStringLiteral("best"));
    viewer.loadMap(oddPacks, oddReplay);
    viewer.setPreviewInputScript(QStringLiteral("0.00 press up\n0.50 rel up"));
    if (!check(viewer.loading(), "reload did not start")) return 1;
    if (!check(WaitUntil([&] { return !viewer.loading() &&
                                     viewer.hasTrajectoryForRun(QStringLiteral("preview")); }) &&
               viewer.hasTrajectoryForRun(QStringLiteral("best")) &&
               viewer.selectedRunId() == QLatin1String("best") &&
               viewer.improvementCarCount() == 1,
               "reloading the same map dropped the Best car, its selection or the preview")) {
        std::cerr << "best=" << viewer.hasTrajectoryForRun(QStringLiteral("best"))
                  << " selected=" << viewer.selectedRunId().toStdString()
                  << " improvementCars=" << viewer.improvementCarCount() << '\n';
        return 1;
    }
    viewer.setSelectedRunId(QStringLiteral("preview"));
    if (!check(WaitUntil([&] { return viewer.inputSample(10).accelerate > 0.99f &&
                                     viewer.inputSample(70).accelerate < 0.01f; }),
               "the preview edit made during the load was not applied")) return 1;
    viewer.requestShutdown();
    WaitUntil([&] { return viewer.shutdownReady(); }, 60000);
    return 0;
}
