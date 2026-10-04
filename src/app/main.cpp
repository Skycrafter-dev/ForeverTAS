#include "app/search_controller.h"
#include "app/input_preview_binding.h"
#include "app/update_controller.h"
#include "app/shutdown_controller.h"
#include "viewer/race_timeline_item.h"
#include "viewer/race_viewer_controller.h"

#include <QApplication>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QSslSocket>
#include <QTimer>
#include <QThreadPool>
#include <QVariant>
#include <QJSValue>
#include <iostream>
#include <cstdlib>

int main(int argc, char **argv) {
#if defined(Q_OS_LINUX)
    if (argc == 4 && QString::fromLocal8Bit(argv[1]) ==
                             QStringLiteral("--apply-update")) {
        QCoreApplication updaterProcess(argc, argv);
        return forevertas::app::ApplyAppImageUpdate(
                qEnvironmentVariable("APPIMAGE"),
                QString::fromLocal8Bit(argv[2]),
                QString::fromLocal8Bit(argv[3]).toLongLong());
    }
#endif
#if defined(Q_OS_LINUX)
    const QString currentDesktop =
            qEnvironmentVariable("XDG_CURRENT_DESKTOP");
    if (qEnvironmentVariableIsSet("KDE_FULL_SESSION") ||
        currentDesktop.contains(QStringLiteral("KDE"), Qt::CaseInsensitive)) {
        // KDE Plasma's Qt platform theme can leave Qt Quick 3D View3D
        // offscreen output transparent on affected Qt/Wayland combinations.
        // ForeverTAS owns its application palette and QML styling, so avoid
        // importing desktop settings while retaining the native Wayland
        // platform and the user's KDE session environment.
        QGuiApplication::setDesktopSettingsAware(false);
    }
#endif
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ForeverTAS"));
    QCoreApplication::setOrganizationDomain(
            QStringLiteral("forevertas.local"));
    QCoreApplication::setApplicationName(QStringLiteral("ForeverTAS"));
    QCoreApplication::setApplicationVersion(
            QStringLiteral(FOREVERTAS_VERSION));
    application.setWindowIcon(
            QIcon(QStringLiteral(":/icons/forevertas.svg")));

    forevertas::app::SearchController controller;
    forevertas::app::UpdateController updates;
    forevertas::viewer::RaceViewerController viewer;
    forevertas::app::ShutdownController shutdown(
            [&] { controller.requestShutdown(); viewer.requestShutdown(); updates.cancelDownload(); },
            [&] { return controller.shutdownReady() && viewer.shutdownReady() &&
                         QThreadPool::globalInstance()->activeThreadCount() == 0; },
            [&] { controller.flushSettings(); },
            [] { std::_Exit(EXIT_SUCCESS); });
    forevertas::app::BindInputPreview(controller, viewer);
    QObject::connect(&controller, &forevertas::app::SearchController::searchStarted,
                     &viewer, &forevertas::viewer::RaceViewerController::beginSearchPreview);
    QObject::connect(&controller, &forevertas::app::SearchController::runningChanged,
                     &viewer, [&]() {
        if (!controller.running()) viewer.endSearchPreview();
    });
    QObject::connect(
            &controller,
            &forevertas::app::SearchController::searchImprovement,
            &viewer,
            [&viewer](forevertas::app::SearchImprovementPtr improvement) {
                viewer.addSearchImprovement(
                        improvement->packsDirectory,
                        improvement->replayPath,
                        improvement->timeline,
                        improvement->simulationBackendId,
                        improvement->searchId,
                        improvement->improvementNumber,
                        improvement->restartNumber,
                        improvement->generatedAt);
            });
    QObject::connect(
            &controller,
            &forevertas::app::SearchController::searchCompleted,
            &viewer,
            [&viewer](forevertas::app::SearchCompletionPtr completion) {
                viewer.completeSearchPreview(std::move(completion));
            });
    forevertas::viewer::RegisterRaceViewerQmlTypes();
    QQmlApplicationEngine engine;
    engine.addImportPath(QCoreApplication::applicationDirPath() +
                         QStringLiteral("/qml"));
    engine.setInitialProperties({
            {QStringLiteral("shutdown"), QVariant::fromValue(static_cast<QObject *>(&shutdown))},
            {QStringLiteral("controller"),
             QVariant::fromValue(static_cast<QObject *>(&controller))},
            {QStringLiteral("viewer"),
             QVariant::fromValue(static_cast<QObject *>(&viewer))},
            {QStringLiteral("updater"),
             QVariant::fromValue(static_cast<QObject *>(&updates))}});
    QObject::connect(
            &engine,
            &QQmlApplicationEngine::objectCreationFailed,
            &application,
            []() { QCoreApplication::exit(1); },
            Qt::QueuedConnection);
    engine.loadFromModule(QStringLiteral("ForeverTAS"),
                          QStringLiteral("Main"));

    if (application.arguments().contains(
                QStringLiteral("--qml-smoke-test"))) {
        QTimer::singleShot(100, &application, [&]() {
            QObject *const root = engine.rootObjects().value(0);
            QObject *const inspector = root != nullptr
                    ? root->findChild<QObject *>(
                              QStringLiteral("graphicsInspector"))
                    : nullptr;
            QObject *const renderMode = root != nullptr
                    ? root->findChild<QObject *>(
                              QStringLiteral("renderModeSelector"))
                    : nullptr;
            const QVariant rows = inspector != nullptr
                    ? inspector->property("rows") : QVariant{};
            const bool valid = inspector != nullptr && renderMode != nullptr &&
                    rows.value<QJSValue>().toVariant().toList().size() >= 5 &&
                    renderMode->property("currentValue").toString() ==
                            QStringLiteral("textured") &&
                    QSslSocket::supportsSsl();
            if (!valid)
                std::cerr << "Graphics inspector or TLS failed to initialize\n";
            QCoreApplication::exit(valid ? 0 : 1);
        });
    } else {
        QTimer::singleShot(0, &updates,
                           &forevertas::app::UpdateController::checkForUpdates);
    }
    return application.exec();
}
