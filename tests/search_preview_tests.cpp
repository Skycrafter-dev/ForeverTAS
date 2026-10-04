#include "app/search_completion.h"
#include "viewer/race_viewer_controller.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>

#include <iostream>

namespace {
template <typename Predicate>
bool WaitUntil(Predicate predicate, int timeout = 60000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    return predicate();
}
void Drain(int milliseconds = 150) { WaitUntil([] { return false; }, milliseconds); }
}

// Search improvements only add preview trajectories; the Best car changes
// once, when the search completes, and only takes the selection when the
// user has not picked another run in the meantime.
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    QGuiApplication app(argc, argv);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QCoreApplication::setOrganizationName(QStringLiteral("search-preview-test"));
    QCoreApplication::setApplicationName(QStringLiteral("search-preview-test"));
    QSettings().setValue(QStringLiteral("viewer/liveBestUpdates"), true);
    using namespace forevertas;
    viewer::RaceViewerController viewer;
    const auto check = [](bool okay, const char *message) {
        if (!okay) std::cerr << message << '\n';
        return okay;
    };
    if (!check(!QSettings().contains(QStringLiteral("viewer/liveBestUpdates")) &&
               viewer.metaObject()->indexOfProperty("liveBestUpdates") < 0,
               "the removed live best preference survived")) return 1;
    const QString packs = QString::fromLocal8Bit(argv[1]);
    const QString replay = QString::fromLocal8Bit(argv[2]);
    viewer.setPreviewInputScript(QStringLiteral("0.00 press up"));
    viewer.loadMap(packs, replay);
    if (!WaitUntil([&] { return viewer.loaded() && !viewer.loading() && viewer.tickCount() > 1; })) return 1;
    const auto originalRuns = viewer.runCount();
    const auto timeline = [](float position) {
        std::vector<SearchTimelineFrame> frames(101);
        for (std::size_t i = 0; i < frames.size(); ++i) {
            frames[i].timeMs = static_cast<std::int64_t>(i * 10);
            frames[i].positionX = position + static_cast<float>(i);
        }
        return frames;
    };
    // Completions carry no inputs so the stored-run rebuild keeps these frames.
    const auto completion = [&](std::uint64_t search, float position) {
        auto result = std::make_shared<app::SearchCompletion>();
        result->searchId = search;
        result->packsDirectory = packs;
        result->replayPath = replay;
        result->simulationBackendId = QStringLiteral("optimized-cpu");
        result->bestTimeline = timeline(position);
        return result;
    };
    const auto bestX = [&] {
        const QString selected = viewer.selectedRunId();
        viewer.setSelectedRunId(QStringLiteral("best"));
        viewer.setTimeMs(100);
        const float x = viewer.carPosition().x();
        viewer.setSelectedRunId(selected);
        return x;
    };

    viewer.beginSearchPreview(1);
    for (std::uint64_t i = 1; i <= 20; ++i)
        viewer.addSearchImprovement(packs, replay, timeline(static_cast<float>(i)),
                                    QStringLiteral("optimized-cpu"), 1, i);
    Drain();
    if (!check(viewer.runCount() == originalRuns && !viewer.hasTrajectoryForRun(QStringLiteral("best")) &&
               viewer.hasPreviewTrajectories(),
               "a search improvement created or moved a live best car")) return 1;
    viewer.completeSearchPreview(completion(1, 300));
    if (!check(WaitUntil([&] { return viewer.runCount() == originalRuns + 1; }) &&
               viewer.selectedRunId() == QLatin1String("best") && bestX() == 310,
               "the finished best was not shown and selected")) return 1;

    viewer.beginSearchPreview(2);
    viewer.setSelectedRunId(QStringLiteral("preview"));
    viewer.completeSearchPreview(completion(2, 400));
    Drain();
    if (!check(viewer.selectedRunId() == QLatin1String("preview") && bestX() == 410,
               "the finished best stole the user's selection or was not stored")) return 1;

    viewer.beginSearchPreview(3);
    viewer.completeSearchPreview(completion(2, 999));
    Drain();
    if (!check(bestX() == 410, "a stale completion replaced the best")) return 1;
    viewer.endSearchPreview();
    viewer.completeSearchPreview(completion(3, 999));
    Drain();
    if (!check(bestX() == 410, "a completion after the search ended was applied")) return 1;

    viewer.beginSearchPreview(4);
    viewer.setPreviewingHistory(true);
    viewer.completeSearchPreview(completion(4, 500));
    Drain();
    if (!check(viewer.selectedRunId() == QLatin1String("preview") && bestX() == 510,
               "a completion during a history preview took the selection")) return 1;
    viewer.setPreviewingHistory(false);
    return 0;
}
