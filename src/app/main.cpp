#include "app/search_controller.h"
#include "app/block_editor_bridge.h"
#include "app/input_preview_binding.h"
#include "viewer/race_timeline_item.h"
#include "viewer/race_viewer_controller.h"

#include <QApplication>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QTimer>
#include <QVariant>
#include <QtWebEngineQuick/qtwebenginequickglobal.h>

int main(int argc, char **argv) {
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
    QtWebEngineQuick::initialize();
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
    forevertas::app::BlockEditorBridge blockEditorBridge(&controller);
    forevertas::viewer::RaceViewerController viewer;
    forevertas::app::BindInputPreview(controller, viewer);
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
                        improvement->improvementNumber);
            });
    QObject::connect(
            &controller,
            &forevertas::app::SearchController::searchCompleted,
            &viewer,
            [&viewer](forevertas::app::SearchCompletionPtr completion) {
                viewer.addSearchRun(completion->packsDirectory,
                                    completion->replayPath,
                                    completion->bestTimeline,
                                    completion->bestInputs,
                                    completion->simulationBackendId);
            });
    forevertas::viewer::RegisterRaceViewerQmlTypes();
    QQmlApplicationEngine engine;
    engine.setInitialProperties({
            {QStringLiteral("controller"),
             QVariant::fromValue(static_cast<QObject *>(&controller))},
            {QStringLiteral("viewer"),
             QVariant::fromValue(static_cast<QObject *>(&viewer))},
            {QStringLiteral("blockEditorBridge"),
             QVariant::fromValue(static_cast<QObject *>(&blockEditorBridge))}});
    QObject::connect(
            &engine,
            &QQmlApplicationEngine::objectCreationFailed,
            &application,
            []() { QCoreApplication::exit(1); },
            Qt::QueuedConnection);
    engine.loadFromModule(QStringLiteral("ForeverTAS"),
                          QStringLiteral("Main"));

    const QStringList arguments = application.arguments();
    if (arguments.contains(QStringLiteral("--qml-smoke-test"))) {
        QTimer::singleShot(0, &application, &QCoreApplication::quit);
    } else if (arguments.contains(QStringLiteral("--blockly-smoke-test"))) {
        const auto roots = engine.rootObjects();
        QObject *const root = roots.isEmpty() ? nullptr : roots.front();
        QObject *const settingsPanel =
                root != nullptr
                        ? root->findChild<QObject *>(
                                  QStringLiteral("settingsPanel"))
                        : nullptr;
        if (settingsPanel == nullptr) {
            QTimer::singleShot(0, &application,
                               []() { QCoreApplication::exit(2); });
        } else {
            QObject::connect(
                    &blockEditorBridge,
                    &forevertas::app::BlockEditorBridge::editorReadyChanged,
                    &application,
                    []() { QCoreApplication::exit(0); },
                    Qt::SingleShotConnection);
            settingsPanel->setProperty("panelPage", 1);
            QTimer::singleShot(30000, &application,
                               []() { QCoreApplication::exit(3); });
        }
    }
    return application.exec();
}
