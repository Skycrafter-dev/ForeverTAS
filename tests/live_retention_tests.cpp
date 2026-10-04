#include "app/search_session_store.h"
#include "viewer/race_viewer_controller.h"

#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QThread>

#include <iostream>
#include <set>

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    QGuiApplication app(argc, argv);
    using namespace forevertas;
    using app::SearchSessionStore;
    using viewer::RaceViewerController;
    const auto check = [](bool okay, const char *message) {
        if (!okay) std::cerr << message << '\n';
        return okay;
    };
    QTemporaryDir directory;
    SearchRequest request{argv[1], argv[2]};
    request.backend = PhysicsBackend::OptimizedCpu;
    request.modifiers[0].settings["minTimeMs"] = "0";
    request.modifiers[0].settings["maxTimeMs"] = "20";
    request.evaluationTarget.settings["minTimeMs"] = "0";
    request.evaluationTarget.settings["maxTimeMs"] = "20";
    SearchRunControl control;
    control.iterationLimit = 0;
    control.sampleBestTimeline = false;
    auto result = RunSearch(request, &control);
    result.iterations = 123;
    const app::SearchSessionLocation location{{}, {}, directory.path()};
    for (std::uint64_t i = 0; i < 1000; ++i) SearchSessionStore::SaveCycle(location, request, i, result);
    auto page = SearchSessionStore::ReadCyclePage(directory.path());
    if (!check(page.rows.size() == 256 && page.hasOlder && !page.hasNewer &&
               page.rows.front().toMap().value("restart").toInt() == 744,
               "latest cycle page is not bounded")) return 1;
    std::set<std::uint64_t> visited;
    for (;;) {
        for (const auto &row : page.rows) visited.insert(row.toMap().value("restart").toULongLong());
        if (!page.hasOlder) break;
        page = SearchSessionStore::ReadCyclePage(directory.path(),
                page.rows.front().toMap().value("restart").toULongLong(), true);
        if (!check(page.rows.size() <= 256 && page.hasNewer, "older page lost navigation")) return 1;
    }
    const auto newer = SearchSessionStore::ReadCyclePage(directory.path(),
            page.rows.back().toMap().value("restart").toULongLong(), false);
    if (!check(visited.size() == 1000 && newer.rows.front().toMap().value("restart").toInt() == 232 &&
               QDir(directory.path()).entryList({"restart-*.txt"}, QDir::Files).size() == 1000 &&
               SearchSessionStore::Cycle(directory.path(), 999).value("attemptsExact") == "123",
               "paging lost saved cycles or exact metadata")) return 1;

    RaceViewerController viewer;
    viewer.setPreviewInputScript(QStringLiteral("0.00 press up"));
    const QString packs = QString::fromLocal8Bit(argv[1]);
    const QString replay = QString::fromLocal8Bit(argv[2]);
    std::vector<SearchTimelineFrame> frames(101);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        frames[i].timeMs = static_cast<std::int64_t>(i * 10);
        frames[i].positionX = static_cast<float>(i);
    }
    for (std::uint64_t i = 1; i <= 1200; ++i)
        viewer.addSearchImprovement(packs, replay, frames, "optimized-cpu", 1, i);
    if (!check(viewer.pendingImprovementBytes() == 64 * frames.size() * sizeof(viewer::RaceViewerFrame),
               "pending trajectories grew beyond their count budget")) return 1;
    QElapsedTimer timer;
    timer.start();
    while ((!viewer.loaded() || viewer.loading() || !viewer.hasTrajectoryForRun("preview")) && timer.elapsed() < 60000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    if (!check(viewer.loaded() && !viewer.loading(), "map did not load")) return 1;
    viewer.addSearchRun(packs, replay, frames);
    for (std::uint64_t i = 1201; i <= 2400; ++i)
        viewer.addSearchImprovement(packs, replay, frames, "optimized-cpu", 1, i);
    int retained = 0;
    for (const auto &path : viewer.trajectoryPaths()) {
        const auto row = path.toMap();
        if (row.value("kind") == "improvement") {
            ++retained;
            if (!check(row.value("improvementNumber").toULongLong() >= 2337, "old path not evicted")) return 1;
        }
    }
    if (!check(retained == 64 && viewer.retainedImprovementBytes() <= RaceViewerController::kMaximumLiveTrajectoryBytes &&
               viewer.pendingImprovementBytes() == 0 && viewer.hasTrajectoryForRun("best") &&
               viewer.hasTrajectoryForRun("preview"), "eviction lost best/base or exceeded storage budget")) {
        std::cerr << "retained=" << retained << " bytes=" << viewer.retainedImprovementBytes()
                  << " pending=" << viewer.pendingImprovementBytes() << " best=" << viewer.hasTrajectoryForRun("best")
                  << " base=" << viewer.hasTrajectoryForRun("preview") << '\n';
        return 1;
    }
    // Exercise the byte budget independently of the count limit.
    frames.resize(200001);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        frames[i].timeMs = static_cast<std::int64_t>(i * 10);
        frames[i].positionX = static_cast<float>(i);
    }
    for (std::uint64_t i = 2401; i <= 2420; ++i)
        viewer.addSearchImprovement(packs, replay, frames, "optimized-cpu", 1, i);
    if (!check(viewer.retainedImprovementBytes() <= RaceViewerController::kMaximumLiveTrajectoryBytes &&
               viewer.retainedImprovementBytes() > RaceViewerController::kMaximumLiveTrajectoryBytes / 2,
               "vertex byte budget exceeded")) return 1;
    std::cout << "Retained vertex bytes after 2420 improvements: " << viewer.retainedImprovementBytes() << '\n';
    viewer.clearPreviewTrajectories();
    return check(viewer.retainedImprovementBytes() == 0 && viewer.pendingImprovementBytes() == 0 &&
                 viewer.hasTrajectoryForRun("best"), "clear leaked improvement geometry") ? 0 : 1;
}
