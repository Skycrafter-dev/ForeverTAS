#include "viewer/race_viewer_controller.h"

#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
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

// A map that cannot be loaded is explained in plain words, with the exact
// message kept as details, and the next good load clears the explanation.
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    QGuiApplication app(argc, argv);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QCoreApplication::setOrganizationName(QStringLiteral("viewer-failure-test"));
    QCoreApplication::setApplicationName(QStringLiteral("viewer-failure-test"));
    forevertas::viewer::RaceViewerController viewer;
    const auto check = [](bool okay, const char *message) {
        if (!okay) std::cerr << message << '\n';
        return okay;
    };
    const QString packs = QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath();
    const QString replay = QFileInfo(QString::fromLocal8Bit(argv[2])).absoluteFilePath();
    // Both paths exist, so the load gets past the path checks and fails in
    // the simulation: a file that is not a replay, and a folder without the
    // game's packs.
    QTemporaryDir files;
    const QString broken = files.filePath(QStringLiteral("broken.Replay.Gbx"));
    QFile brokenFile(broken);
    if (!brokenFile.open(QIODevice::WriteOnly) || brokenFile.write("not a replay") < 0) return 1;
    brokenFile.close();
    const QString emptyPacks = files.filePath(QStringLiteral("Packs"));
    if (!QDir().mkpath(emptyPacks)) return 1;
    const struct {
        QString packs;
        QString replay;
        const char *category;
    } failures[]{{packs, broken, "input"}, {emptyPacks, replay, "assets"}};
    for (const auto &failure : failures) {
        viewer.loadMap(failure.packs, failure.replay);
        if (!check(WaitUntil([&] { return !viewer.loading(); }), "the failed load did not finish")) return 1;
        const QVariantMap explained = viewer.statusFailure();
        if (!check(!viewer.loaded() && explained.value(QStringLiteral("category")) == failure.category &&
                           !explained.value(QStringLiteral("reason")).toString().isEmpty() &&
                           !explained.value(QStringLiteral("guidance")).toString().isEmpty() &&
                           explained.value(QStringLiteral("details")) == viewer.statusText() &&
                           explained.value(QStringLiteral("stage")) == QStringLiteral("Loading the map"),
                   "a failed map load was not explained in plain words")) {
            std::cerr << explained.value(QStringLiteral("category")).toString().toStdString() << ": "
                      << viewer.statusText().toStdString() << '\n';
            return 1;
        }
    }
    viewer.loadMap(packs, replay);
    if (!check(WaitUntil([&] { return viewer.loaded() && !viewer.loading(); }) &&
                       viewer.statusFailure().isEmpty(),
               "a good load kept the earlier failure")) return 1;
    viewer.requestShutdown();
    WaitUntil([&] { return viewer.shutdownReady(); }, 60000);
    return 0;
}
