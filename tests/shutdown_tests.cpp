#include "app/shutdown_controller.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <QProcess>
#include <QTimer>

#include <atomic>
#include <cstdlib>
#include <iostream>

namespace {
template <typename Predicate>
bool Wait(Predicate predicate) {
    QElapsedTimer clock;
    clock.start();
    while (!predicate() && clock.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    return predicate();
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    using forevertas::app::ShutdownController;
    if (app.arguments().contains(QStringLiteral("--force-child"))) {
        auto blocked = QThread::create([] { for (;;) QThread::msleep(100); });
        blocked->start();
        ShutdownController shutdown([] {}, [] { return false; }, [] {}, [] { std::_Exit(23); }, 10);
        shutdown.requestClose();
        QTimer::singleShot(150, &shutdown, &ShutdownController::forceExit);
        return app.exec();
    }
    std::atomic_bool release = false;
    auto thread = QThread::create([&] {
        while (!release.load()) QThread::msleep(1);
    });
    thread->start();
    int began = 0, persisted = 0, forced = 0, ready = 0, heartbeats = 0;
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, &app, [&] { ++heartbeats; });
    heartbeat.start(1);
    ShutdownController shutdown([&] { ++began; thread->requestInterruption(); },
                                [&] { return !thread->isRunning(); },
                                [&] { ++persisted; }, [&] { ++forced; }, 40);
    QObject::connect(&shutdown, &ShutdownController::readyToClose, &app, [&] { ++ready; });
    shutdown.forceExit();
    bool okay = !shutdown.requestClose() && !shutdown.requestClose() && began == 1 &&
                persisted == 1 && forced == 0 && Wait([&] { return shutdown.delayed(); }) &&
                heartbeats >= 10 && ready == 0;
    shutdown.forceExit();
    shutdown.forceExit();
    okay = okay && forced == 1 && persisted == 2;
    release = true;
    okay = Wait([&] { return ready == 1; }) && okay;
    okay = shutdown.requestClose() && okay;
    thread->wait();
    delete thread;
    ShutdownController idle([] {}, [] { return true; }, [&] { ++persisted; }, [&] { ++forced; });
    okay = idle.requestClose() && !idle.delayed() && okay;
    idle.forceExit();
    okay = forced == 1 && okay;
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--force-child")});
    const bool childExited = child.waitForFinished(3000);
    if (!childExited) {
        child.kill();
        child.waitForFinished(1000);
    }
    okay = childExited && child.exitStatus() == QProcess::NormalExit && child.exitCode() == 23 && okay;
    if (!okay) std::cerr << "shutdown blocked the event loop, repeated cancellation, or forced exit without confirmation\n";
    return okay ? 0 : 1;
}
