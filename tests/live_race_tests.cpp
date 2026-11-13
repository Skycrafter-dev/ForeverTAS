// The race view's live race end to end on a replay's map, its inputs as the
// base inputs: takeover, pause, rewind on both sides of the takeover, speed,
// stepping, savestates, restart and attempts.
#include "app/input_preview_binding.h"
#include "app/search_controller.h"
#include "mutations/replay_input_script.h"
#include "viewer/race_viewer_controller.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QSettings>
#include <QTimer>

#include <cstdio>
#include <functional>
#include <utility>

using forevertas::viewer::RaceViewerController;

static bool WaitFor(const std::function<bool()> &done, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (!done()) {
        if (timer.elapsed() > timeoutMs) return false;
        QEventLoop loop;
        QTimer::singleShot(5, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return true;
}
static void Run(int ms) { WaitFor([] { return false; }, ms); }

static int failures = 0;
static void Check(bool ok, const char *what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

static void Show(RaceViewerController &v, const char *label) {
    std::printf("  [%s] run=%s t=%lld dur=%lld playing=%d driving=%d attempts=%lld speed=%.2f\n", label,
                qPrintable(v.selectedRunId()), static_cast<long long>(v.timeMs()),
                static_cast<long long>(v.durationMs()), v.playing(), v.manualDriving(),
                static_cast<long long>(v.attempts().size()), v.playbackSpeed());
}

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: forevertas-live-race-tests <Packs> <replay>\n");
        return 2;
    }
    QGuiApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ForeverTASTests"));
    QCoreApplication::setApplicationName(QStringLiteral("LiveRaceTests"));
    QSettings().clear();
    forevertas::app::SearchController controller;
    RaceViewerController v;
    forevertas::app::BindInputPreview(controller, v);
    controller.setPreviewExtentMs(QStringLiteral("12000"));
    controller.setBaseInputScript(QString::fromStdString(
            forevertas::ExtractReplayInputScript(argv[1], argv[2])));
    v.loadMap(QString::fromLocal8Bit(argv[1]), QString::fromLocal8Bit(argv[2]));
    if (!WaitFor([&] { return v.loaded() && !v.loading() &&
                              v.hasTrajectoryForRun(QStringLiteral("preview")) &&
                              v.durationMs() >= 12000; }, 40000)) {
        std::fprintf(stderr, "map did not load\n");
        return 1;
    }
    v.setSelectedRunId(QStringLiteral("preview"));

    // Play, then take over with a steering key.
    v.setCurrentTick(0);
    v.play();
    Run(1000);
    Check(v.playing() && !v.manualDriving() && v.timeMs() > 800, "plays the inputs");
    const int lineColor = v.selectedRunColorIndex();
    v.setManualInput(QStringLiteral("left"), true);
    Check(v.manualDriving() && v.playing(), "a driving key takes over and keeps running");
    Check(v.selectedRunColorIndex() == lineColor &&
                  v.selectedRunStyleId() == QStringLiteral("preview"),
          "the car driven keeps the color of the line taken over");
    const qint64 takeoverAt = v.attempts().value(0).toMap().value(QStringLiteral("takeoverMs")).toLongLong();
    Show(v, "taken over");
    Run(500);
    v.setManualInput(QStringLiteral("left"), false);
    Run(300);

    // Pause: the race stands still and keys do nothing.
    v.togglePlayback();
    const qint64 pausedAt = v.durationMs();
    v.setManualInput(QStringLiteral("right"), true);
    Run(300);
    Check(!v.playing() && v.durationMs() == pausedAt && !v.manualRight(), "paused: stands still, keys do nothing");
    v.setManualInput(QStringLiteral("right"), false);

    // Frame advance.
    v.stepTick(1);
    Check(v.durationMs() == pausedAt + 10 && v.timeMs() == pausedAt + 10, "one tick forward steps the race driven");

    // Rewind while paused, after the takeover: driven on as a new attempt.
    v.beginRewind();
    Run(200);
    v.endRewind();
    Check(!v.playing() && v.timeMs() < pausedAt && v.timeMs() > takeoverAt, "rewind while paused moves back and stays paused");
    const qint64 rewoundTo = v.timeMs();
    v.play();
    Show(v, "played after rewind past takeover");
    Check(v.manualDriving() && v.playing() && v.attempts().size() == 2 &&
                  v.selectedRunId() == QStringLiteral("manual"),
          "past the takeover: driven on, the earlier race kept as an attempt");
    Check(v.durationMs() >= rewoundTo / 10 * 10 && v.durationMs() < rewoundTo + 200, "driven on from the moment rewound to");
    Check(v.selectedRunColorIndex() == lineColor, "a rewind keeps the car's color");
    Run(300);

    // Rewind before the takeover: the inputs play again.
    v.pause();
    v.setTimeMs(std::max<qint64>(0, takeoverAt - 400));
    v.play();
    Show(v, "played after seek before takeover");
    Check(!v.manualDriving() && v.playing() && v.selectedRunId() == QStringLiteral("preview") &&
                  v.attempts().size() == 2,
          "before the takeover: the inputs play, both attempts kept");
    Run(200);

    // Speed.
    v.pause();
    v.setPlaybackSpeed(0.5);
    const qint64 before = v.timeMs();
    v.play();
    Run(1000);
    v.pause();
    const qint64 moved = v.timeMs() - before;
    std::printf("  half speed: %lld ms in 1 s\n", static_cast<long long>(moved));
    Check(moved > 300 && moved < 700, "half speed plays half as fast");
    v.setPlaybackSpeed(1.0);
    v.slower();
    Check(v.playbackSpeed() == 0.5, "slower steps the speed list");
    v.faster();
    v.faster();
    Check(v.playbackSpeed() == 2.0, "faster steps the speed list");
    v.setPlaybackSpeed(1.0);

    // Savestate on the inputs, then one while driving.
    v.setTimeMs(1500);
    v.saveState(0);
    v.play();
    Run(200);
    v.setManualInput(QStringLiteral("accelerate"), false);
    v.setManualInput(QStringLiteral("brake"), true);
    Check(v.manualDriving(), "brake takes over");
    Run(400);
    v.setManualInput(QStringLiteral("brake"), false);
    // Driving shows the car a tick behind the newest: the moment shown is
    // the one saved.
    const qint64 savedDriving = v.timeMs() / 10 * 10;
    v.saveState(1);
    Run(300);
    v.loadState(0);
    Show(v, "loaded savestate 1 (inputs)");
    Check(!v.manualDriving() && v.playing() && v.timeMs() >= 1500 && v.timeMs() < 1600 &&
                  v.selectedRunId() == QStringLiteral("preview"),
          "a savestate on the inputs plays them from there");
    Run(200);
    v.loadState(1);
    Show(v, "loaded savestate 2 (driving)");
    Check(v.manualDriving() && v.playing() && v.durationMs() >= savedDriving && v.durationMs() < savedDriving + 150,
          "a savestate taken while driving is driven on from there");
    Run(200);

    // Slow motion is smooth: the car moves on nearly every frame, while
    // playing and while driving, not once per tick (10 ms of race take a
    // second at 1/100).
    const auto distinctPositions = [&](int ms) {
        int distinct = 0;
        QVector3D last = v.carPosition();
        QVector3D lastCamera = v.carCameraPosition();
        int cameraMoves = 0;
        for (int elapsed = 0; elapsed < ms; elapsed += 20) {
            Run(20);
            if (v.carPosition() != last) ++distinct;
            if (v.carCameraPosition() != lastCamera) ++cameraMoves;
            last = v.carPosition();
            lastCamera = v.carCameraPosition();
        }
        return std::pair<int, int>{distinct, cameraMoves};
    };
    v.pause();
    v.setSelectedRunId(QStringLiteral("preview"));
    v.setTimeMs(2500);
    v.setPlaybackSpeed(0.01);
    v.play();
    const auto playing = distinctPositions(1000);
    v.setManualInput(QStringLiteral("left"), true);
    const auto driving = distinctPositions(1000);
    v.setManualInput(QStringLiteral("left"), false);
    v.pause();
    v.setPlaybackSpeed(1.0);
    std::printf("  at 1/100: playing %d car / %d camera moves, driving %d / %d in 50 frames\n",
                playing.first, playing.second, driving.first, driving.second);
    Check(playing.first >= 35 && playing.second >= 35, "slow motion playback is smooth");
    Check(driving.first >= 35 && driving.second >= 35, "slow motion driving is smooth");

    // Restart.
    v.restartRun();
    Show(v, "restarted");
    Check(!v.manualDriving() && v.playing() && v.selectedRunId() == QStringLiteral("preview") && v.timeMs() < 200,
          "restart plays the inputs from the start");
    v.pause();

    // Attempts: listed newest first, watchable, scriptable.
    const QVariantList attempts = v.attempts();
    for (const QVariant &a : attempts) {
        const QVariantMap m = a.toMap();
        std::printf("  attempt %s takeover=%lld end=%lld cp=%s live=%d\n", qPrintable(m.value("name").toString()),
                    m.value("takeoverMs").toLongLong(), m.value("endMs").toLongLong(),
                    qPrintable(m.value("checkpoints").toString()), m.value("live").toBool());
    }
    const QString newest = attempts.value(0).toMap().value(QStringLiteral("id")).toString();
    v.watchAttempt(newest);
    Check(v.selectedRunId() == newest && v.playing(), "an attempt can be watched");
    v.pause();
    const QString script = v.attemptScript(newest);
    Check(script.contains(QStringLiteral("press")), "an attempt gives its inputs as a script");
    bool hidden = true;
    for (const QVariant &pose : v.runPoses()) {
        const QVariantMap m = pose.toMap();
        if (m.value("id").toString().startsWith(QStringLiteral("attempt-")) && !m.value("hidden").toBool()) hidden = false;
    }
    Check(hidden, "attempt cars are hidden unless watched");
    std::printf("%s (%d failed)\n", failures ? "FAILED" : "ALL OK", failures);
    return failures ? 1 : 0;
}
