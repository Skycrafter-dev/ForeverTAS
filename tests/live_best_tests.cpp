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

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    QGuiApplication app(argc, argv);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QCoreApplication::setOrganizationName(QStringLiteral("live-best-test"));
    QCoreApplication::setApplicationName(QStringLiteral("live-best-test"));
    using namespace forevertas;
    viewer::RaceViewerController viewer;
    const auto check = [](bool okay, const char *message) {
        if (!okay) std::cerr << message << '\n';
        return okay;
    };
    const QString packs = QString::fromLocal8Bit(argv[1]);
    const QString replay = QString::fromLocal8Bit(argv[2]);
    viewer.setPreviewInputScript(QStringLiteral("0.00 press up"));
    viewer.loadMap(packs, replay);
    if (!WaitUntil([&] { return viewer.loaded() && !viewer.loading() && viewer.tickCount() > 1; })) return 1;
    const auto originalRuns = viewer.runCount();
    auto improvement = [&](std::uint64_t search, std::uint64_t number, float position) {
        auto result = std::make_shared<app::SearchImprovement>();
        result->searchId = search;
        result->improvementNumber = number;
        result->packsDirectory = packs;
        result->replayPath = replay;
        result->simulationBackendId = QStringLiteral("optimized-cpu");
        result->timeline.resize(101);
        for (std::size_t i = 0; i < result->timeline.size(); ++i) {
            result->timeline[i].timeMs = static_cast<std::int64_t>(i * 10);
            result->timeline[i].positionX = position + static_cast<float>(i);
            result->timeline[i].steering = 0.25f;
        }
        SandboxInputEvent steer;
        steer.action = SandboxInputAction::Steer;
        steer.value.kind = forevervalidator::experimental::PhysicsSandboxInputValueKind::Analog;
        steer.value.analog = kAnalogInputScale / 4;
        result->inputs.push_back(steer);
        return result;
    };
    viewer.beginSearchPreview(1);
    viewer.queueLiveBest(improvement(1, 1, 1));
    Drain();
    if (!check(!viewer.liveBestUpdates() && viewer.runCount() == originalRuns,
               "live update was not opt-in")) return 1;
    viewer.setLiveBestUpdates(true);
    viewer.setTimeMs(100);
    int changes = 0;
    int sceneChanges = 0;
    const auto cameraPreset = viewer.cameraPreset();
    QObject::connect(&viewer, &viewer::RaceViewerController::runsChanged, &app, [&] { ++changes; });
    QObject::connect(&viewer, &viewer::RaceViewerController::sceneChanged, &app, [&] { ++sceneChanges; });
    for (std::uint64_t i = 2; i <= 100; ++i) viewer.queueLiveBest(improvement(1, i, static_cast<float>(i)));
    Drain();
    if (!check(changes == 1 && viewer.selectedRunId() == QLatin1String("best") &&
               viewer.timeMs() == 100 && viewer.carPosition().x() == 110 && sceneChanges == 0 &&
               viewer.cameraPreset() == cameraPreset &&
               viewer.currentInputScript().contains(QStringLiteral("steer 16384")),
               "updates did not coalesce or lost pose/inputs/playhead/camera")) {
        std::cerr << "changes=" << changes << " selected=" << viewer.selectedRunId().toStdString()
                  << " time=" << viewer.timeMs() << " x=" << viewer.carPosition().x()
                  << " scene=" << sceneChanges << " inputs=" << viewer.currentInputScript().toStdString() << '\n';
        return 1;
    }
    viewer.setSelectedRunId(QStringLiteral("preview"));
    viewer.queueLiveBest(improvement(1, 101, 200));
    Drain();
    if (!check(viewer.selectedRunId() == QLatin1String("preview"), "live update stole selection")) return 1;
    auto completion = std::make_shared<app::SearchCompletion>();
    completion->searchId = 1;
    completion->packsDirectory = packs;
    completion->replayPath = replay;
    completion->simulationBackendId = QStringLiteral("optimized-cpu");
    completion->bestTimeline = improvement(1, 102, 300)->timeline;
    completion->bestInputs = improvement(1, 102, 300)->inputs;
    viewer.queueLiveBest(improvement(1, 102, 250));
    viewer.completeSearchPreview(completion);
    viewer.queueLiveBest(improvement(1, 103, 999));
    Drain();
    if (!check(viewer.selectedRunId() == QLatin1String("preview"), "completion stole selection")) return 1;
    viewer.setSelectedRunId(QStringLiteral("best"));
    viewer.setTimeMs(100);
    if (!check(viewer.carPosition().x() == 310, "pending update overwrote final result")) return 1;
    viewer.beginSearchPreview(2);
    viewer.queueLiveBest(improvement(1, 999, 999));
    viewer.queueLiveBest(improvement(2, 2, 400));
    viewer.queueLiveBest(improvement(2, 1, 999));
    Drain();
    if (!check(viewer.carPosition().x() == 410, "stale generation or sequence accepted")) return 1;
    viewer.setPreviewingHistory(true);
    viewer.queueLiveBest(improvement(2, 3, 999));
    Drain();
    if (!check(viewer.carPosition().x() == 410, "history preview overwritten")) return 1;
    viewer.setPreviewingHistory(false);
    viewer.queueLiveBest(improvement(2, 4, 999));
    viewer.endSearchPreview();
    // Give the previous completion's real CPU resampling worker time to
    // deliver its result after the newer live best has already been installed.
    Drain(2000);
    if (!check(viewer.carPosition().x() == 410, "cancelled or stale rebuilt update applied")) {
        std::cerr << "selected=" << viewer.selectedRunId().toStdString()
                  << " time=" << viewer.timeMs() << " x=" << viewer.carPosition().x() << '\n';
        return 1;
    }
    viewer.setLiveBestUpdates(false);
    return 0;
}
