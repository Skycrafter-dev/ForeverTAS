#include "app/input_preview_binding.h"
#include "app/search_controller.h"
#include "app/system_file_dialog.h"
#include "viewer/race_timeline_item.h"
#include "viewer/race_viewer_controller.h"

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QImage>
#include <QInputDevice>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QMetaProperty>
#include <QPalette>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickStyle>
#include <QSettings>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVector3D>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

#ifndef FOREVERTAS_SOURCE_DIR
#error "FOREVERTAS_SOURCE_DIR must be defined"
#endif

namespace {

template <typename Predicate>
bool WaitUntil(Predicate predicate, int timeoutMs = 60000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    return predicate();
}

forevertas::SandboxInputEvent SwitchInput(
        std::int32_t timeMs,
        forevertas::SandboxInputAction action,
        bool pressed) {
    using forevervalidator::experimental::PhysicsSandboxInputValueKind;
    using forevervalidator::experimental::PhysicsSandboxSwitchState;
    forevertas::SandboxInputEvent event;
    event.timeMs = timeMs;
    event.action = action;
    event.value.kind = PhysicsSandboxInputValueKind::Switch;
    event.value.switchState = pressed
            ? PhysicsSandboxSwitchState::Pressed
            : PhysicsSandboxSwitchState::Released;
    return event;
}

bool ModelsHaveState(const QList<QObject *> &models,
                     int expectedCount,
                     bool visible) {
    if (models.size() != expectedCount) {
        return false;
    }
    for (const QObject *model : models) {
        const QVariant geometry = model->property("geometry");
        if (model->property("visible").toBool() != visible ||
            !geometry.isValid() || geometry.isNull()) {
            return false;
        }
    }
    return true;
}

int VisibleModelCount(const QList<QObject *> &models) {
    return static_cast<int>(std::count_if(
            models.cbegin(),
            models.cend(),
            [](const QObject *model) {
                return model->property("visible").toBool();
            }));
}

bool ModelsHaveGeometry(const QList<QObject *> &models,
                        int expectedCount) {
    return models.size() == expectedCount &&
            std::all_of(
                    models.cbegin(),
                    models.cend(),
                    [](const QObject *model) {
                        const QVariant geometry =
                                model->property("geometry");
                        return geometry.isValid() && !geometry.isNull();
                    });
}

bool VisualMaterialsAreBoundAndShared(
        const QList<QObject *> &models,
        const QList<QObject *> &materials,
        const QList<QObject *> &baseTextures,
        const forevertas::viewer::RaceViewerController &viewer) {
    if (materials.size() != viewer.visualMaterials().size() ||
        baseTextures.size() != materials.size() ||
        materials.isEmpty() || materials.size() >= models.size()) {
        return false;
    }

    QSet<QObject *> baseTextureObjects(baseTextures.cbegin(),
                                       baseTextures.cend());
    for (const QObject *texture : baseTextures) {
        const QUrl source = texture->property("source").toUrl();
        if (source.scheme() != QStringLiteral("qrc") ||
            !source.path().startsWith(QStringLiteral("/materials/")) ||
            !qFuzzyCompare(texture->property("scaleU").toFloat(), 1.0f) ||
            !qFuzzyCompare(texture->property("scaleV").toFloat(), 1.0f)) {
            return false;
        }
    }

    const QVariantList materialDefinitions = viewer.visualMaterials();
    for (qsizetype index = 0; index < materials.size(); ++index) {
        const QObject *const material = materials.at(index);
        const QVariantMap definition =
                materialDefinitions.at(index).toMap();
        QObject *const baseMap =
                material->property("baseColorMap").value<QObject *>();
        QObject *const normalMap =
                material->property("normalMap").value<QObject *>();
        QObject *const emissiveMap =
                material->property("emissiveMap").value<QObject *>();
        const bool emissive =
                definition.value(QStringLiteral("emissiveStrength"))
                                .toFloat() > 0.0f;
        const QMetaProperty cullModeProperty =
                material->metaObject()->property(
                        material->metaObject()->indexOfProperty("cullMode"));
        const char *const cullModeName =
                cullModeProperty.enumerator().valueToKey(
                        material->property("cullMode").toInt());
        if (!baseTextureObjects.contains(baseMap) || normalMap != nullptr ||
            (emissive ? emissiveMap != baseMap : emissiveMap != nullptr) ||
            !qFuzzyCompare(material->property("opacity").toFloat(), 1.0f) ||
            cullModeName == nullptr ||
            QByteArray(cullModeName) != "NoCulling" ||
            material->property("vertexColorsEnabled").toBool() !=
                    definition.value(QStringLiteral("vertexColors"))
                            .toBool()) {
            return false;
        }
    }

    QSet<QObject *> usedMaterials;
    bool repeatedBinding = false;
    for (const QObject *model : models) {
        const int binding = model->property("materialBindingIndex").toInt();
        QObject *const material =
                model->property("sharedMaterial").value<QObject *>();
        if (binding < 0 || binding >= materials.size() ||
            material != materials.at(binding)) {
            return false;
        }
        repeatedBinding |= usedMaterials.contains(material);
        usedMaterials.insert(material);
    }
    return repeatedBinding && usedMaterials.size() < models.size();
}

bool FilledModelsHaveBakedRunPalettes(
        const QList<QObject *> &models,
        const QList<QObject *> &materials,
        int expectedCount) {
    if (models.size() != expectedCount ||
        materials.size() != expectedCount) {
        return false;
    }
    QSet<QObject *> geometries;
    for (const QObject *model : models) {
        const QVariant geometry = model->property("geometry");
        if (!geometry.canConvert<QObject *>()) return false;
        QObject *const object = geometry.value<QObject *>();
        if (object == nullptr) return false;
        geometries.insert(object);
    }
    for (const QObject *material : materials) {
        if (!material->property("vertexColorsEnabled").toBool() ||
            material->property("diffuseColor").value<QColor>() !=
                    QColor(Qt::white)) {
            return false;
        }
    }
    return !geometries.isEmpty();
}

bool ContainsStandardSlider(QObject *root) {
    const QList<QObject *> objects = root->findChildren<QObject *>();
    for (const QObject *object : objects) {
        if (QByteArray(object->metaObject()->className()).contains("Slider")) {
            return true;
        }
    }
    return false;
}

bool ContainsText(QObject *root, const QString &needle) {
    const QList<QObject *> objects = root->findChildren<QObject *>();
    for (const QObject *object : objects) {
        const QVariant text = object->property("text");
        if (text.isValid() && text.toString().contains(needle)) {
            return true;
        }
    }
    return false;
}

void CollectVisualTexts(QQuickItem *root, QSet<QString> &texts) {
    if (root == nullptr) {
        return;
    }
    const QVariant text = root->property("text");
    if (text.isValid()) {
        texts.insert(text.toString());
    }
    for (QQuickItem *child : root->childItems()) {
        CollectVisualTexts(child, texts);
    }
}

bool IsCenteredIcon(QQuickItem *item, qreal expectedSize) {
    if (item == nullptr || item->parentItem() == nullptr) {
        return false;
    }
    const QQuickItem *const parent = item->parentItem();
    constexpr qreal tolerance = 0.1;
    return std::abs(item->width() - expectedSize) < tolerance &&
            std::abs(item->height() - expectedSize) < tolerance &&
            std::abs(item->x() + item->width() * 0.5 -
                     parent->width() * 0.5) < tolerance &&
            std::abs(item->y() + item->height() * 0.5 -
                     parent->height() * 0.5) < tolerance;
}

QImage GrabItemImage(QQuickItem *item) {
    if (item == nullptr) return {};
    const QSharedPointer<QQuickItemGrabResult> grab =
            item->grabToImage(QSize(18, 18));
    if (!grab || !WaitUntil([&]() { return !grab->image().isNull(); }, 5000)) {
        return {};
    }
    return grab->image().convertToFormat(QImage::Format_ARGB32);
}

int OpaquePixelsInColumn(const QImage &image, int column) {
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        count += image.pixelColor(column, y).alpha() >= 64 ? 1 : 0;
    }
    return count;
}

bool HasRightFacingPlaySilhouette(QQuickItem *item) {
    const QImage image = GrabItemImage(item);
    return image.size() == QSize(18, 18) &&
            OpaquePixelsInColumn(image, 4) >= 10 &&
            OpaquePixelsInColumn(image, 15) <= 3 &&
            OpaquePixelsInColumn(image, 2) == 0 &&
            OpaquePixelsInColumn(image, 17) == 0;
}

bool HasJumpToEndSilhouette(QQuickItem *item) {
    const QImage image = GrabItemImage(item);
    return image.size() == QSize(18, 18) &&
            OpaquePixelsInColumn(image, 3) >= 10 &&
            OpaquePixelsInColumn(image, 11) <= 4 &&
            OpaquePixelsInColumn(image, 13) >= 10 &&
            OpaquePixelsInColumn(image, 15) >= 10 &&
            OpaquePixelsInColumn(image, 17) == 0;
}

QColor FirstDescendantColor(QObject *root) {
    if (root == nullptr) {
        return {};
    }
    for (QObject *const object : root->findChildren<QObject *>()) {
        const QVariant color = object->property("color");
        if (color.isValid() && color.canConvert<QColor>()) {
            return color.value<QColor>();
        }
    }
    return {};
}

bool InvokeSliderValueCommit(
        QObject *field,
        const QString &text,
        bool expectedResult) {
    if (field == nullptr || !field->setProperty("text", text)) {
        return false;
    }
    QVariant result;
    const bool invoked = QMetaObject::invokeMethod(
            field,
            "commitText",
            Qt::DirectConnection,
            Q_RETURN_ARG(QVariant, result));
    QCoreApplication::processEvents();
    return invoked && result.toBool() == expectedResult;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "usage: forevertas-viewer-qml-smoke <Packs> <replay>\n";
        return 2;
    }

    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ForeverTASTests"));
    QCoreApplication::setApplicationName(
            QStringLiteral("ViewerQmlSmoke"));
    QStandardPaths::setTestModeEnabled(true);
    QSettings().clear();

    forevertas::app::SearchController controller;
    forevertas::viewer::RaceViewerController viewer;
    forevertas::app::BindInputPreview(controller, viewer);
#if defined(Q_OS_LINUX)
    const bool nativeBrowseDialogsValid =
            forevertas::app::ActiveSystemFileDialogBackend() ==
            forevertas::app::SystemFileDialogBackend::XdgDesktopPortal;
#elif defined(Q_OS_WIN)
    const bool nativeBrowseDialogsValid =
            forevertas::app::ActiveSystemFileDialogBackend() ==
            forevertas::app::SystemFileDialogBackend::WindowsIFileDialog;
#else
    const bool nativeBrowseDialogsValid = false;
#endif
    forevertas::viewer::RegisterRaceViewerQmlTypes();
    QQmlApplicationEngine engine;
    QObject::connect(
            &engine,
            &QQmlApplicationEngine::warnings,
            [](const QList<QQmlError> &warnings) {
                for (const QQmlError &warning : warnings) {
                    std::cerr << warning.toString().toStdString() << '\n';
                }
            });
    engine.setInitialProperties({
            {QStringLiteral("controller"),
             QVariant::fromValue(static_cast<QObject *>(&controller))},
            {QStringLiteral("viewer"),
             QVariant::fromValue(static_cast<QObject *>(&viewer))}});

    int exitCode = 1;
    bool completed = false;
    bool verificationScheduled = false;
    bool editorStructure = false;
    bool manualActionKeysValid = false;
    bool cameraShortcutKeysValid = false;
    bool freeCameraManualRoutingValid = false;
    bool selectedCarRenderingValid = false;
    QObject::connect(
            &engine,
            &QQmlApplicationEngine::objectCreationFailed,
            &application,
            [&]() {
                completed = true;
                application.quit();
            },
            Qt::QueuedConnection);

    const QUrl mainQml = QUrl::fromLocalFile(
            QStringLiteral(FOREVERTAS_SOURCE_DIR "/qml/Main.qml"));
    engine.load(mainQml);
    if (engine.rootObjects().isEmpty()) {
        std::cerr << "failed to create Main.qml\n";
        return 1;
    }
    QObject *const root = engine.rootObjects().front();
    QObject *const settingsPanel =
            root->findChild<QObject *>(QStringLiteral("settingsPanel"));
    QObject *const darkModeToggle =
            root->findChild<QObject *>(QStringLiteral("darkModeToggle"));
    QObject *const whiteboardImportDialog =
            root->findChild<QObject *>(
                    QStringLiteral("whiteboardImportDialog"));
    QObject *const whiteboardExportDialog =
            root->findChild<QObject *>(
                    QStringLiteral("whiteboardExportDialog"));
    QObject *const whiteboardImageExportDialog =
            root->findChild<QObject *>(
                    QStringLiteral("whiteboardImageExportDialog"));
    const auto usesNativeFileDialog =
            [](const QObject *dialog) {
                return dialog != nullptr &&
                       (dialog->property("options").toInt() &
                        static_cast<int>(
                                QFileDialog::DontUseNativeDialog)) == 0;
            };
    const bool nativeFileDialogsValid =
            nativeBrowseDialogsValid &&
            usesNativeFileDialog(whiteboardImportDialog) &&
            usesNativeFileDialog(whiteboardExportDialog) &&
            usesNativeFileDialog(whiteboardImageExportDialog);
    QObject *const initialMainMapLight =
            root->findChild<QObject *>(QStringLiteral("mainMapLight"));
    QObject *const initialFillMapLight =
            root->findChild<QObject *>(QStringLiteral("fillMapLight"));
    QObject *const initialMapEnvironment =
            root->findChild<QObject *>(QStringLiteral("mapEnvironment"));
    QObject *const initialPlaybackDock =
            root->findChild<QObject *>(QStringLiteral("playbackDock"));
    QObject *const initialStartButton =
            root->findChild<QObject *>(QStringLiteral("startSearchButton"));
    QObject *const initialJumpStartButton =
            root->findChild<QObject *>(QStringLiteral("jumpStartButton"));
    QObject *const initialJumpStartIcon =
            root->findChild<QObject *>(
                    QStringLiteral("jumpStartTransportIcon"));
    QObject *const initialRaceTimeline =
            root->findChild<QObject *>(QStringLiteral("raceTimeline"));
    QList<QObject *> themedControls;
    QStringList unthemedControlTypes;
    for (QObject *const object : root->findChildren<QObject *>()) {
        if (object->property("themedControl").toBool()) {
            themedControls.append(object);
            continue;
        }
        const QByteArray className = object->metaObject()->className();
        const bool concreteButtonLike =
                className.contains("Button") ||
                className.contains("CheckBox") ||
                className.contains("Switch") ||
                className.contains("Slider") ||
                className.contains("ComboBox") ||
                className.contains("ItemDelegate") ||
                className.contains("MenuItem");
        if (concreteButtonLike &&
            !className.contains("IndicatorButton") &&
            object->property("pressed").isValid() &&
            object->property("hovered").isValid()) {
            unthemedControlTypes.append(
                    QString::fromUtf8(className) + QLatin1Char(':') +
                    object->objectName());
        }
    }
    const QColor lightDisabledButton =
            initialStartButton != nullptr
            ? initialStartButton
                      ->property("effectiveBackgroundColor")
                      .value<QColor>()
            : QColor();
    const QColor lightDisabledButtonBorder =
            initialStartButton != nullptr
            ? initialStartButton
                      ->property("effectiveBorderColor")
                      .value<QColor>()
            : QColor();
    const QColor lightSwitchTrack =
            darkModeToggle != nullptr
            ? darkModeToggle->property("effectiveTrackColor").value<QColor>()
            : QColor();
    const QColor lightPlaybackDock =
            initialPlaybackDock != nullptr
            ? initialPlaybackDock->property("color").value<QColor>()
            : QColor();
    const QColor lightDisabledTransportIcon =
            FirstDescendantColor(initialJumpStartIcon);
    const bool completeControlAudit =
            themedControls.size() >= 20 &&
            unthemedControlTypes.isEmpty() &&
            initialStartButton != nullptr &&
            !initialStartButton->property("enabled").toBool() &&
            lightDisabledButton ==
                    QColor(QStringLiteral("#ecefe9")) &&
            lightDisabledButtonBorder ==
                    QColor(QStringLiteral("#cbd1c8")) &&
            lightSwitchTrack ==
                    QColor(QStringLiteral("#e1e5df")) &&
            lightPlaybackDock ==
                    QColor(QStringLiteral("#edf4f5f2")) &&
            initialJumpStartButton != nullptr &&
            !initialJumpStartButton->property("enabled").toBool() &&
            lightDisabledTransportIcon ==
                    QColor(QStringLiteral("#92988f")) &&
            initialRaceTimeline != nullptr &&
            !initialRaceTimeline->property("darkMode").toBool();
    const QColor lightWindowColor = root->property("color").value<QColor>();
    const QColor lightPanelColor =
            settingsPanel != nullptr
            ? settingsPanel->property("color").value<QColor>()
            : QColor();
    const QColor mainLightColor =
            initialMainMapLight != nullptr
            ? initialMainMapLight->property("color").value<QColor>()
            : QColor();
    const QColor fillLightColor =
            initialFillMapLight != nullptr
            ? initialFillMapLight->property("color").value<QColor>()
            : QColor();
    const QColor environmentColor =
            initialMapEnvironment != nullptr
            ? initialMapEnvironment->property("clearColor").value<QColor>()
            : QColor();
    const QColor lightWidgetWindowColor =
            application.palette().color(QPalette::Window);
    const auto exerciseControlStates =
            [](QQuickItem *item) {
                if (item == nullptr) {
                    return false;
                }
                QMetaObject::invokeMethod(item, "forceActiveFocus");
                QCoreApplication::processEvents();
                const bool focusState =
                        item->property("activeFocus").toBool() &&
                        item->property("effectiveBorderColor")
                                        .value<QColor>() ==
                                QColor(QStringLiteral("#315f8f"));
                const QPointF scenePosition = item->mapToScene(
                        QPointF(item->width() * 0.5,
                                item->height() * 0.5));
                QHoverEvent hover(
                        QEvent::HoverEnter,
                        scenePosition,
                        scenePosition,
                        QPointF(-1, -1));
                QCoreApplication::sendEvent(item, &hover);
                QCoreApplication::processEvents();
                const bool hoverState =
                        item->property("hovered").toBool() &&
                        item->property("effectiveTrackColor")
                                        .value<QColor>() ==
                                QColor(QStringLiteral("#d9ded9"));
                const bool downWritable =
                        item->setProperty("down", true);
                QCoreApplication::processEvents();
                const bool pressedState =
                        item->property("down").toBool() &&
                        item->property("effectiveTrackColor")
                                        .value<QColor>() ==
                                QColor(QStringLiteral("#cbd2cc"));
                item->setProperty("down", false);
                QCoreApplication::processEvents();
                return focusState && hoverState && downWritable &&
                        pressedState;
            };
    const bool interactiveControlStates =
            exerciseControlStates(
                    qobject_cast<QQuickItem *>(darkModeToggle));
    controller.setDarkMode(true);
    QCoreApplication::processEvents();
    const QColor darkDisabledButton =
            initialStartButton != nullptr
            ? initialStartButton
                      ->property("effectiveBackgroundColor")
                      .value<QColor>()
            : QColor();
    const QColor darkDisabledButtonBorder =
            initialStartButton != nullptr
            ? initialStartButton
                      ->property("effectiveBorderColor")
                      .value<QColor>()
            : QColor();
    const QColor darkSwitchTrack =
            darkModeToggle != nullptr
            ? darkModeToggle->property("effectiveTrackColor").value<QColor>()
            : QColor();
    const QColor darkPlaybackDock =
            initialPlaybackDock != nullptr
            ? initialPlaybackDock->property("color").value<QColor>()
            : QColor();
    const bool darkThemeValid =
            controller.darkMode() && darkModeToggle != nullptr &&
            darkModeToggle->property("checked").toBool() &&
            nativeFileDialogsValid &&
            root->property("color").value<QColor>() != lightWindowColor &&
            settingsPanel != nullptr &&
            settingsPanel->property("color").value<QColor>() !=
                    lightPanelColor &&
            application.palette().color(QPalette::Window) !=
                    lightWidgetWindowColor &&
            application.palette().color(QPalette::Text) ==
                    QColor(QStringLiteral("#f0f3ef")) &&
            application.palette().color(
                    QPalette::Disabled, QPalette::Text) ==
                    QColor(QStringLiteral("#737b74")) &&
            darkDisabledButton ==
                    QColor(QStringLiteral("#252925")) &&
            darkDisabledButtonBorder ==
                    QColor(QStringLiteral("#4a534b")) &&
            darkSwitchTrack ==
                    QColor(QStringLiteral("#58c98c")) &&
            darkPlaybackDock ==
                    QColor(QStringLiteral("#ed111513")) &&
            initialRaceTimeline != nullptr &&
            initialRaceTimeline->property("darkMode").toBool() &&
            initialMainMapLight != nullptr &&
            initialMainMapLight->property("color").value<QColor>() ==
                    mainLightColor &&
            initialFillMapLight != nullptr &&
            initialFillMapLight->property("color").value<QColor>() ==
                    fillLightColor &&
            initialMapEnvironment != nullptr &&
            initialMapEnvironment->property("clearColor").value<QColor>() ==
                    environmentColor;
    controller.setDarkMode(false);
    QCoreApplication::processEvents();
    const bool lightThemeRestored =
            darkModeToggle != nullptr &&
            !darkModeToggle->property("checked").toBool() &&
            settingsPanel != nullptr &&
            root->property("color").value<QColor>() == lightWindowColor &&
            settingsPanel->property("color").value<QColor>() ==
                    lightPanelColor &&
            application.palette().color(QPalette::Window) ==
                    lightWidgetWindowColor &&
            initialRaceTimeline != nullptr &&
            !initialRaceTimeline->property("darkMode").toBool();
    if (!completeControlAudit || !interactiveControlStates ||
        !darkThemeValid || !lightThemeRestored) {
        std::cerr << "light/dark theme switching changed scene rendering or "
                     "failed to update the complete UI shell"
                  << " (toggle=" << (darkModeToggle != nullptr)
                  << ", settings=" << (settingsPanel != nullptr)
                  << ", light-window="
                  << lightWindowColor.name().toStdString()
                  << ", restored-window="
                  << root->property("color")
                             .value<QColor>()
                             .name()
                             .toStdString()
                  << ", light-panel="
                  << lightPanelColor.name().toStdString()
                  << ", restored-panel="
                  << (settingsPanel != nullptr
                              ? settingsPanel->property("color")
                                        .value<QColor>()
                                        .name()
                                        .toStdString()
                              : std::string("<missing>"))
                  << ", scene=" << (initialMainMapLight != nullptr)
                  << "/" << (initialFillMapLight != nullptr)
                  << "/" << (initialMapEnvironment != nullptr)
                  << ", themed-controls=" << themedControls.size()
                  << ", interactive-states="
                  << interactiveControlStates
                  << ", native-dialogs="
                  << nativeFileDialogsValid
                  << ", unthemed="
                  << unthemedControlTypes.join(QLatin1Char(','))
                             .toStdString()
                  << ", disabled-button="
                  << lightDisabledButton.name().toStdString()
                  << "->"
                  << darkDisabledButton.name().toStdString()
                  << ", switch="
                  << lightSwitchTrack.name().toStdString()
                  << "->"
                  << darkSwitchTrack.name().toStdString()
                  << ", playback="
                  << lightPlaybackDock.name(QColor::HexArgb).toStdString()
                  << "->"
                  << darkPlaybackDock.name(QColor::HexArgb).toStdString()
                  << ", disabled-icon="
                  << lightDisabledTransportIcon.name().toStdString()
                  << ")\n";
        return 1;
    }

    auto *const initialGlobalScript =
            qobject_cast<QQuickItem *>(
                    root->findChild<QObject *>(
                            QStringLiteral("baseInputScriptSection")));
    auto *const initialReplaySection =
            qobject_cast<QQuickItem *>(
                    root->findChild<QObject *>(
                            QStringLiteral("replaySection")));
    auto *const initialAppearanceControls =
            qobject_cast<QQuickItem *>(
                    root->findChild<QObject *>(
                            QStringLiteral("appearanceControls")));
    auto *const initialReplayPathField =
            qobject_cast<QQuickItem *>(
                    root->findChild<QObject *>(
                            QStringLiteral("replayPathField")));
    auto *const initialBrowseReplayButton =
            qobject_cast<QQuickItem *>(
                    root->findChild<QObject *>(
                            QStringLiteral("browseReplayButton")));
    auto *const initialLoadMapButton =
            qobject_cast<QQuickItem *>(
                    root->findChild<QObject *>(
                            QStringLiteral("loadMapButton")));
    auto *const initialExtractReplayInputsButton =
            qobject_cast<QQuickItem *>(
                    root->findChild<QObject *>(
                            QStringLiteral("extractReplayInputsButton")));
    auto *const initialToolTabs =
            qobject_cast<QQuickItem *>(
                    root->findChild<QObject *>(
                            QStringLiteral("toolTabs")));
    auto *const initialBruteforceContent =
            qobject_cast<QQuickItem *>(
                    root->findChild<QObject *>(
                            QStringLiteral("bruteforceTabContent")));
    auto *const initialDebuggerContent =
            qobject_cast<QQuickItem *>(
                    root->findChild<QObject *>(
                            QStringLiteral("simulationDebuggerPanel")));
    bool globalSettingsVisibleAcrossTabs =
            initialGlobalScript != nullptr &&
            initialReplaySection != nullptr &&
            initialAppearanceControls != nullptr &&
            initialReplayPathField != nullptr &&
            initialBrowseReplayButton != nullptr &&
            initialLoadMapButton != nullptr &&
            initialExtractReplayInputsButton != nullptr &&
            initialToolTabs != nullptr &&
            initialBruteforceContent != nullptr &&
            initialDebuggerContent != nullptr;
    bool debuggerCombinedNameValid = false;
    if (initialDebuggerContent != nullptr) {
        QVariant decoratedName;
        const QVariantMap combinedEntry{
                {QStringLiteral("name"), QStringLiteral("physics.cpp")},
                {QStringLiteral("modified"), true},
                {QStringLiteral("breakpoint"), true}};
        debuggerCombinedNameValid =
                QMetaObject::invokeMethod(
                        initialDebuggerContent,
                        "sourceName",
                        Q_RETURN_ARG(QVariant, decoratedName),
                        Q_ARG(QVariant, combinedEntry)) &&
                decoratedName.toString().count(
                        QStringLiteral("<font color=")) ==
                        QStringLiteral("physics.cpp").size();
    }
    if (globalSettingsVisibleAcrossTabs) {
        initialToolTabs->setProperty("currentIndex", 1);
        QCoreApplication::processEvents();
        globalSettingsVisibleAcrossTabs &=
                initialGlobalScript->isVisible() &&
                initialReplaySection->isVisible() &&
                initialAppearanceControls->isVisible() &&
                initialReplayPathField->isVisible() &&
                initialBrowseReplayButton->isVisible() &&
                initialLoadMapButton->isVisible() &&
                initialExtractReplayInputsButton->isVisible() &&
                !initialBruteforceContent->isVisible() &&
                initialDebuggerContent->isVisible();
        initialToolTabs->setProperty("currentIndex", 0);
        QCoreApplication::processEvents();
        globalSettingsVisibleAcrossTabs &=
                initialGlobalScript->isVisible() &&
                initialReplaySection->isVisible() &&
                initialAppearanceControls->isVisible() &&
                initialReplayPathField->isVisible() &&
                initialBrowseReplayButton->isVisible() &&
                initialLoadMapButton->isVisible() &&
                initialExtractReplayInputsButton->isVisible() &&
                initialBruteforceContent->isVisible() &&
                !initialDebuggerContent->isVisible();
    }

    QObject::connect(
            &viewer,
            &forevertas::viewer::RaceViewerController::stateChanged,
            &application,
            [&]() {
                if (completed || verificationScheduled || viewer.loading() ||
                    !viewer.loaded() || viewer.runCount() == 0) {
                    return;
                }
                verificationScheduled = true;
                QTimer::singleShot(500, &application, [&]() {
                    if (completed) {
                        return;
                    }
                    QObject *const filled = root->findChild<QObject *>(
                            QStringLiteral("trackFilledModel"));
                    QObject *const wire = root->findChild<QObject *>(
                            QStringLiteral("trackWireModel"));
                    auto *const renderModeSelector =
                            qobject_cast<QQuickItem *>(
                                    root->findChild<QObject *>(
                                            QStringLiteral(
                                                    "renderModeSelector")));
                    QObject *const gpuRayTracingView =
                            root->findChild<QObject *>(
                                    QStringLiteral("gpuRayTracingView"));
                    QObject *const rasterMapView =
                            root->findChild<QObject *>(
                                    QStringLiteral("rasterMapView"));
                    QObject *const viewCamera = root->findChild<QObject *>(
                            QStringLiteral("viewCamera"));
                    QObject *const mapEnvironment =
                            root->findChild<QObject *>(
                                    QStringLiteral("mapEnvironment"));
                    QObject *const daySkyTexture =
                            root->findChild<QObject *>(
                                    QStringLiteral("daySkyTexture"));
                    QObject *const mainMapLight = root->findChild<QObject *>(
                            QStringLiteral("mainMapLight"));
                    QObject *const fillMapLight = root->findChild<QObject *>(
                            QStringLiteral("fillMapLight"));
                    auto *const timeline = root->findChild<
                            forevertas::viewer::RaceTimelineItem *>(
                            QStringLiteral("raceTimeline"));
                    auto *const timelinePanel = qobject_cast<QQuickItem *>(
                            root->findChild<QObject *>(
                                    QStringLiteral("timelinePanel")));
                    auto *const viewport = qobject_cast<QQuickItem *>(
                            root->findChild<QObject *>(
                                    QStringLiteral("raceViewport")));
                    auto *const raceViewerHeader = qobject_cast<QQuickItem *>(
                            root->findChild<QObject *>(
                                    QStringLiteral("raceViewerHeader")));
                    auto *const headerControlsRow =
                            qobject_cast<QQuickItem *>(
                                    root->findChild<QObject *>(
                                            QStringLiteral(
                                                    "headerControlsRow")));
                    auto *const raceViewerTitleBlock =
                            qobject_cast<QQuickItem *>(
                                    root->findChild<QObject *>(
                                            QStringLiteral(
                                                    "raceViewerTitleBlock")));
                    auto *const runSelector = qobject_cast<QQuickItem *>(
                            root->findChild<QObject *>(
                                    QStringLiteral("runSelector")));
                    auto *const trajectoryVisibilityToggle =
                            qobject_cast<QQuickItem *>(
                                    root->findChild<QObject *>(QStringLiteral(
                                            "trajectoryVisibilityToggle")));
                    auto *const clearPreviewTrajectoriesButton =
                            qobject_cast<QQuickItem *>(
                                    root->findChild<QObject *>(QStringLiteral(
                                            "clearPreviewTrajectoriesButton")));
                    auto *const resetViewButton =
                            qobject_cast<QQuickItem *>(
                                    root->findChild<QObject *>(
                                            QStringLiteral(
                                                    "resetViewButton")));
                    auto *const playbackDock = qobject_cast<QQuickItem *>(
                            root->findChild<QObject *>(
                                    QStringLiteral("playbackDock")));
                    QObject *const playPause = root->findChild<QObject *>(
                            QStringLiteral("playPauseButton"));
                    auto *const playIcon = qobject_cast<QQuickItem *>(
                            root->findChild<QObject *>(
                                    QStringLiteral("playTransportIcon")));
                    auto *const pauseIcon = qobject_cast<QQuickItem *>(
                            root->findChild<QObject *>(
                                    QStringLiteral("pauseTransportIcon")));
                    QObject *const jumpStart = root->findChild<QObject *>(
                            QStringLiteral("jumpStartButton"));
                    auto *const jumpStartIcon = qobject_cast<QQuickItem *>(
                            root->findChild<QObject *>(
                                    QStringLiteral("jumpStartTransportIcon")));
                    QObject *const jumpEnd = root->findChild<QObject *>(
                            QStringLiteral("jumpEndButton"));
                    auto *const jumpEndIcon = qobject_cast<QQuickItem *>(
                            root->findChild<QObject *>(
                                    QStringLiteral("jumpEndTransportIcon")));
                    QObject *const manualDriveButton =
                            root->findChild<QObject *>(
                                    QStringLiteral("manualDriveButton"));
                    auto *const takeOverOnInputCheckBox =
                            qobject_cast<QQuickItem *>(
                                    root->findChild<QObject *>(
                                            QStringLiteral(
                                                    "takeOverOnInputCheckBox")));
                    auto *const manualDriveStatus =
                            qobject_cast<QQuickItem *>(
                                    root->findChild<QObject *>(
                                            QStringLiteral(
                                                    "manualDriveStatus")));
                    auto *const manualInputFocus =
                            qobject_cast<QQuickItem *>(
                                    root->findChild<QObject *>(
                                            QStringLiteral(
                                                    "manualInputFocus")));
                    auto *const cameraFocusToolbar =
                            qobject_cast<QQuickItem *>(
                                    root->findChild<QObject *>(
                                            QStringLiteral(
                                                    "cameraFocusToolbar")));
                    QObject *const freeCameraButton =
                            root->findChild<QObject *>(
                                    QStringLiteral("freeCameraButton"));
                    QObject *const orbitalCameraButton =
                            root->findChild<QObject *>(
                                    QStringLiteral("orbitalCameraButton"));
                    QObject *const focusCarButton =
                            root->findChild<QObject *>(
                                    QStringLiteral("focusCarButton"));
                    QObject *const nearCameraButton =
                            root->findChild<QObject *>(
                                    QStringLiteral("nearCameraButton"));
                    QObject *const internalCameraButton =
                            root->findChild<QObject *>(
                                    QStringLiteral("internalCameraButton"));
                    QObject *const focusObjectButton =
                            root->findChild<QObject *>(
                                    QStringLiteral("focusObjectButton"));
                    auto *const scriptedTelemetry =
                            qobject_cast<QQuickItem *>(
                                    root->findChild<QObject *>(
                                            QStringLiteral(
                                                    "scriptedTelemetry")));
                    QObject *const scriptedTelemetryText =
                            root->findChild<QObject *>(
                                    QStringLiteral(
                                            "scriptedTelemetryText"));
                    QObject *const editTelemetryButton =
                            root->findChild<QObject *>(QStringLiteral(
                                    "editTelemetryButton"));
                    QObject *const telemetryEditorDialog =
                            root->findChild<QObject *>(QStringLiteral(
                                    "telemetryEditorDialog"));
                    QObject *const telemetryScriptEditor =
                            root->findChild<QObject *>(QStringLiteral(
                                    "telemetryScriptEditor"));
                    QObject *const telemetryScriptPreview =
                            root->findChild<QObject *>(QStringLiteral(
                                    "telemetryScriptPreview"));
                    auto *const telemetryFieldCombo =
                            qobject_cast<QQuickItem *>(
                                    root->findChild<QObject *>(QStringLiteral(
                                            "telemetryFieldCombo")));
                    QObject *const telemetryFieldComboPopup =
                            root->findChild<QObject *>(QStringLiteral(
                                    "telemetryFieldComboPopup"));
                    auto *const telemetryFieldComboPopupList =
                            qobject_cast<QQuickItem *>(
                                    root->findChild<QObject *>(QStringLiteral(
                                            "telemetryFieldComboPopupList")));
                    QObject *const stepBackward = root->findChild<QObject *>(
                            QStringLiteral("stepBackwardShortcut"));
                    QObject *const stepForward = root->findChild<QObject *>(
                            QStringLiteral("stepForwardShortcut"));
                    QObject *const autoPacksSuggestion =
                            root->findChild<QObject *>(
                                    QStringLiteral("autoPacksSuggestion"));
                    QObject *const autoPacksSuggestionText =
                            root->findChild<QObject *>(QStringLiteral(
                                    "autoPacksSuggestionText"));
                    QObject *const applyAutoPacks = root->findChild<QObject *>(
                            QStringLiteral("applyAutoPacksButton"));
                    QObject *const searchAlgorithmCombo =
                            root->findChild<QObject *>(
                                    QStringLiteral("searchAlgorithmCombo"));
                    QObject *const simulationBackendCombo =
                            root->findChild<QObject *>(
                                    QStringLiteral("simulationBackendCombo"));
                    auto *const cpuWorkerSettings =
                            qobject_cast<QQuickItem *>(
                                    root->findChild<QObject *>(QStringLiteral(
                                            "cpuWorkerSettings")));
                    QObject *const cpuWorkerCountField =
                            root->findChild<QObject *>(QStringLiteral(
                                    "cpuWorkerCountField"));
                    QObject *const simulationHorizonField =
                            root->findChild<QObject *>(QStringLiteral(
                                    "simulationHorizonField"));
                    auto *const randomizeSeedsOnStartCheckBox =
                            qobject_cast<QQuickItem *>(
                                    root->findChild<QObject *>(QStringLiteral(
                                            "randomizeSeedsOnStartCheckBox")));
#if FOREVERVALIDATOR_HAS_CUDA
                    auto *const cudaParallelSampleSettings =
                            qobject_cast<QQuickItem *>(
                                    root->findChild<QObject *>(QStringLiteral(
                                            "cudaParallelSampleSettings")));
                    QObject *const cudaParallelSampleCountField =
                            root->findChild<QObject *>(QStringLiteral(
                                    "cudaParallelSampleCountField"));
                    auto *const cudaCalibrationCheckBox =
                            qobject_cast<QQuickItem *>(
                                    root->findChild<QObject *>(QStringLiteral(
                                            "cudaCalibrationCheckBox")));
                    auto *const cudaSessionSpecializationSection =
                            qobject_cast<QQuickItem *>(
                                    root->findChild<QObject *>(QStringLiteral(
                                            "cudaSessionSpecializationSection")));
                    QObject *const cudaSessionSpecializationSwitch =
                            root->findChild<QObject *>(QStringLiteral(
                                    "cudaSessionSpecializationSwitch"));
                    QObject *const cudaSessionSpecializationWarning =
                            root->findChild<QObject *>(QStringLiteral(
                                    "cudaSessionSpecializationWarning"));
#endif
                    QObject *const settingsScroll = root->findChild<QObject *>(
                            QStringLiteral("settingsScroll"));
                    QObject *const settingsWheelRedirector =
                            root->property("settingsWheelRedirectorObject")
                                    .value<QObject *>();
                    auto *const toolTabs = qobject_cast<QQuickItem *>(
                            root->findChild<QObject *>(
                                    QStringLiteral("toolTabs")));
                    auto *const bruteforceTabContent =
                            qobject_cast<QQuickItem *>(
                                    root->findChild<QObject *>(
                                            QStringLiteral(
                                                    "bruteforceTabContent")));
                    auto *const simulationDebuggerPanel =
                            qobject_cast<QQuickItem *>(
                                    root->findChild<QObject *>(
                                            QStringLiteral(
                                                    "simulationDebuggerPanel")));
                    auto *const simulationDebuggerPanelHost =
                            qobject_cast<QQuickItem *>(
                                    root->findChild<QObject *>(QStringLiteral(
                                            "simulationDebuggerPanelHost")));
                    auto *const workspaceContent =
                            qobject_cast<QQuickItem *>(
                                    root->findChild<QObject *>(QStringLiteral(
                                            "workspaceContent")));
                    auto *const settingsPanel =
                            qobject_cast<QQuickItem *>(
                                    root->findChild<QObject *>(QStringLiteral(
                                            "settingsPanel")));
                    QObject *const toggleCodeEditorExpansionButton =
                            root->findChild<QObject *>(QStringLiteral(
                                    "toggleCodeEditorExpansionButton"));
                    auto *const referenceLoadingWarning =
                            qobject_cast<QQuickItem *>(
                                    root->findChild<QObject *>(QStringLiteral(
                                            "referenceLoadingWarning")));
                    QObject *const referenceLoadingWarningText =
                            root->findChild<QObject *>(QStringLiteral(
                                    "referenceLoadingWarningText"));
                    QObject *const simulationDebuggerStatusText =
                            root->findChild<QObject *>(QStringLiteral(
                                    "simulationDebuggerStatusText"));
                    QObject *const simulationSourceTree =
                            root->findChild<QObject *>(
                                    QStringLiteral("simulationSourceTree"));
                    QObject *const simulationSourceTreeScrollBar =
                            root->findChild<QObject *>(QStringLiteral(
                                    "simulationSourceTreeScrollBar"));
                    auto *const simulationCodeViewer =
                            qobject_cast<QQuickItem *>(
                                    root->findChild<QObject *>(QStringLiteral(
                                            "simulationCodeViewer")));
                    auto *const debuggerWaitingForPauseOverlay =
                            qobject_cast<QQuickItem *>(
                                    root->findChild<QObject *>(QStringLiteral(
                                            "debuggerWaitingForPauseOverlay")));
                    QObject *const debuggerWaitingForPauseText =
                            root->findChild<QObject *>(QStringLiteral(
                                    "debuggerWaitingForPauseText"));
                    QObject *const simulationVariables =
                            root->findChild<QObject *>(
                                    QStringLiteral("simulationVariables"));
                    QObject *const simulationDebugOutput =
                            root->findChild<QObject *>(
                                    QStringLiteral("simulationDebugOutput"));
                    QObject *const simulationDebugOutputEmptyState =
                            root->findChild<QObject *>(QStringLiteral(
                                    "simulationDebugOutputEmptyState"));
                    QObject *const clearSimulationDebugOutputButton =
                            root->findChild<QObject *>(QStringLiteral(
                                    "clearSimulationDebugOutputButton"));
                    QObject *const restartLiveSimulationButton =
                            root->findChild<QObject *>(QStringLiteral(
                                    "restartLiveSimulationButton"));
                    QObject *const resetLiveEditsButton =
                            root->findChild<QObject *>(
                                    QStringLiteral("resetLiveEditsButton"));
                    QObject *const debuggerSubstepForwardButton =
                            root->findChild<QObject *>(QStringLiteral(
                                    "debuggerSubstepForwardButton"));
                    QObject *const debuggerSourceLineStepButton =
                            root->findChild<QObject *>(QStringLiteral(
                                    "debuggerSourceLineStepButton"));
                    QObject *const debuggerTickStepButton =
                            root->findChild<QObject *>(
                                    QStringLiteral("debuggerTickStepButton"));
                    auto *const evaluationSection = qobject_cast<QQuickItem *>(
                            root->findChild<QObject *>(
                                    QStringLiteral("evaluationSection")));
                    auto *const conditionsSection = qobject_cast<QQuickItem *>(
                            root->findChild<QObject *>(
                                    QStringLiteral("conditionsSection")));
                    QObject *const conditionScriptTextArea =
                            root->findChild<QObject *>(
                                    QStringLiteral("conditionScriptTextArea"));
                    auto *const modifierSection = qobject_cast<QQuickItem *>(
                            root->findChild<QObject *>(
                                    QStringLiteral("modifierSection")));
                    auto *const searchSection = qobject_cast<QQuickItem *>(
                            root->findChild<QObject *>(
                                    QStringLiteral("searchSection")));
                    QObject *const evaluationTargetSelector =
                            root->findChild<QObject *>(
                                    QStringLiteral("evaluationTargetSelector"));
                    QObject *const modifierComposition =
                            root->findChild<QObject *>(
                                    QStringLiteral("modifierComposition"));
                    QObject *const addModifierCombo =
                            root->findChild<QObject *>(
                                    QStringLiteral("addModifierCombo"));
                    QObject *const addModifierButton =
                            root->findChild<QObject *>(
                                    QStringLiteral("addModifierButton"));
                    QObject *const evaluationTargetCombo =
                            root->findChild<QObject *>(
                                    QStringLiteral("evaluationTargetCombo"));
                    QObject *const basicBruteForceSettings =
                            root->findChild<QObject *>(QStringLiteral(
                                    "basicBruteForceSearchSettings"));
                    QObject *const autoPromoteBestSwitch =
                            root->findChild<QObject *>(QStringLiteral(
                                    "autoPromoteBestSwitch"));
                    QObject *const velocitySettings =
                            root->findChild<QObject *>(QStringLiteral(
                                    "velocityEvaluationSettings"));
                    auto *const velocityModeCombo =
                            qobject_cast<QQuickItem *>(
                                    root->findChild<QObject *>(QStringLiteral(
                                            "velocityModeCombo")));
                    QObject *const velocityModeComboContent =
                            root->findChild<QObject *>(QStringLiteral(
                                    "velocityModeComboContent"));
                    QObject *const bestInputsScrollView =
                            root->findChild<QObject *>(QStringLiteral(
                                    "bestInputsScrollView"));
                    QObject *const bestInputsTextArea =
                            root->findChild<QObject *>(QStringLiteral(
                                    "bestInputsTextArea"));
                    QObject *const copyBestInputsButton =
                            root->findChild<QObject *>(QStringLiteral(
                                    "copyBestInputsButton"));
                    auto *const baseInputScriptSection =
                            qobject_cast<QQuickItem *>(
                                    root->findChild<QObject *>(QStringLiteral(
                                            "baseInputScriptSection")));
                    auto *const packsDirectorySection =
                            qobject_cast<QQuickItem *>(
                                    root->findChild<QObject *>(QStringLiteral(
                                            "packsDirectorySection")));
                    auto *const replaySection =
                            qobject_cast<QQuickItem *>(
                                    root->findChild<QObject *>(QStringLiteral(
                                            "replaySection")));
                    auto *const appearanceControls =
                            qobject_cast<QQuickItem *>(
                                    root->findChild<QObject *>(QStringLiteral(
                                            "appearanceControls")));
                    QObject *const replayPathField =
                            root->findChild<QObject *>(QStringLiteral(
                                    "replayPathField"));
                    QObject *const browseReplayButton =
                            root->findChild<QObject *>(QStringLiteral(
                                    "browseReplayButton"));
                    QObject *const baseInputScriptScrollView =
                            root->findChild<QObject *>(QStringLiteral(
                                    "baseInputScriptScrollView"));
                    QObject *const baseInputScriptTextArea =
                            root->findChild<QObject *>(QStringLiteral(
                                    "baseInputScriptTextArea"));
                    QObject *const baseInputScriptErrorLabel =
                            root->findChild<QObject *>(QStringLiteral(
                                    "baseInputScriptErrorLabel"));
                    QObject *const saveBaseInputScriptShortcut =
                            root->findChild<QObject *>(QStringLiteral(
                                    "saveBaseInputScriptShortcut"));
                    QObject *const undoBaseInputScriptShortcut =
                            root->findChild<QObject *>(QStringLiteral(
                                    "undoBaseInputScriptShortcut"));
                    QObject *const copyCurrentRaceInputsButton =
                            root->findChild<QObject *>(QStringLiteral(
                                    "copyCurrentRaceInputsButton"));
                    QObject *const loadMapButton =
                            root->findChild<QObject *>(
                                    QStringLiteral("loadMapButton"));
                    QObject *const extractReplayInputsButton =
                            root->findChild<QObject *>(QStringLiteral(
                                    "extractReplayInputsButton"));
                    QObject *const replaceBaseInputScriptDialog =
                            root->findChild<QObject *>(QStringLiteral(
                                    "replaceBaseInputScriptDialog"));
                    QObject *const startSearchButton =
                            root->findChild<QObject *>(QStringLiteral(
                                    "startSearchButton"));
                    QObject *const stopSearchButton =
                            root->findChild<QObject *>(QStringLiteral(
                                    "stopSearchButton"));
                    auto *const searchMetricsRow = qobject_cast<QQuickItem *>(
                            root->findChild<QObject *>(QStringLiteral(
                                    "searchMetricsRow")));
                    auto *const iterationsMetricCard =
                            qobject_cast<QQuickItem *>(
                                    root->findChild<QObject *>(QStringLiteral(
                                            "iterationsMetricCard")));
                    QObject *const iterationsMetricValue =
                            root->findChild<QObject *>(QStringLiteral(
                                    "iterationsMetricValue"));
                    auto *const throughputMetricCard =
                            qobject_cast<QQuickItem *>(
                                    root->findChild<QObject *>(QStringLiteral(
                                            "throughputMetricCard")));
                    QObject *const throughputMetricValue =
                            root->findChild<QObject *>(QStringLiteral(
                                    "throughputMetricValue"));
                    auto *const elapsedMetricCard = qobject_cast<QQuickItem *>(
                            root->findChild<QObject *>(QStringLiteral(
                                    "elapsedMetricCard")));
                    QObject *const elapsedMetricValue =
                            root->findChild<QObject *>(QStringLiteral(
                                    "elapsedMetricValue"));
                    const bool keyboardStepping =
                            stepBackward != nullptr &&
                            stepForward != nullptr &&
                            stepBackward->property("enabled").toBool() &&
                            stepForward->property("enabled").toBool() &&
                            stepBackward->property("sequence").toString() ==
                                    QStringLiteral("Left") &&
                            stepForward->property("sequence").toString() ==
                                    QStringLiteral("Right");
                    const auto manualMapping =
                            [root](Qt::Key key) {
                                QVariant result;
                                const bool invoked =
                                        QMetaObject::invokeMethod(
                                                root,
                                                "manualControlForKey",
                                                Q_RETURN_ARG(
                                                        QVariant, result),
                                                Q_ARG(
                                                        QVariant,
                                                        QVariant::fromValue(
                                                                static_cast<
                                                                        int>(
                                                                        key))));
                                return invoked
                                        ? result.toString()
                                        : QStringLiteral("<invoke failed>");
                            };
                    const auto manualActionMapping =
                            [root](Qt::Key key) {
                                QVariant result;
                                const bool invoked =
                                        QMetaObject::invokeMethod(
                                                root,
                                                "manualActionForKey",
                                                Q_RETURN_ARG(
                                                        QVariant, result),
                                                Q_ARG(
                                                        QVariant,
                                                        QVariant::fromValue(
                                                                static_cast<
                                                                        int>(
                                                                        key))));
                                return invoked
                                        ? result.toString()
                                        : QStringLiteral("<invoke failed>");
                            };
                    const auto sendMouseClick =
                            [root](QQuickItem *item) {
                                auto *const quickWindow =
                                        qobject_cast<QQuickWindow *>(root);
                                if (quickWindow == nullptr ||
                                    item == nullptr) {
                                    return false;
                                }
                                const QPointF position = item->mapToScene(
                                        QPointF(item->width() * 0.5,
                                                item->height() * 0.5));
                                const QPointF global =
                                        quickWindow->mapToGlobal(
                                                position.toPoint());
                                QMouseEvent press(
                                        QEvent::MouseButtonPress,
                                        position,
                                        position,
                                        global,
                                        Qt::LeftButton,
                                        Qt::LeftButton,
                                        Qt::NoModifier);
                                QCoreApplication::sendEvent(
                                        quickWindow, &press);
                                QMouseEvent release(
                                        QEvent::MouseButtonRelease,
                                        position,
                                        position,
                                        global,
                                        Qt::LeftButton,
                                        Qt::NoButton,
                                        Qt::NoModifier);
                                QCoreApplication::sendEvent(
                                        quickWindow, &release);
                                QCoreApplication::processEvents();
                                return press.isAccepted() &&
                                        release.isAccepted();
                            };
                    auto *const manualDriveItem =
                            qobject_cast<QQuickItem *>(manualDriveButton);
                    bool takeoverControlValid =
                            takeOverOnInputCheckBox != nullptr &&
                            manualDriveItem != nullptr &&
                            playbackDock != nullptr &&
                            playbackDock->width() >= 429.0 &&
                            takeOverOnInputCheckBox->parentItem() ==
                                    manualDriveItem->parentItem() &&
                            takeOverOnInputCheckBox->x() >=
                                    manualDriveItem->x() +
                                            manualDriveItem->width() &&
                            takeOverOnInputCheckBox
                                            ->property("text")
                                            .toString() ==
                                    QStringLiteral("Take Over on Input") &&
                            takeOverOnInputCheckBox
                                    ->property("enabled")
                                    .toBool() &&
                            !takeOverOnInputCheckBox
                                     ->property("checked")
                                     .toBool() &&
                            !viewer.takeOverOnInput();
                    if (takeoverControlValid) {
                        takeoverControlValid &=
                                sendMouseClick(
                                        takeOverOnInputCheckBox);
                        takeoverControlValid &=
                                viewer.takeOverOnInput() &&
                                takeOverOnInputCheckBox
                                        ->property("checked")
                                        .toBool();
                        viewer.play();
                        QCoreApplication::processEvents();
                        takeoverControlValid &=
                                viewer.playing() &&
                                !stepBackward
                                         ->property("enabled")
                                         .toBool() &&
                                !stepForward
                                         ->property("enabled")
                                         .toBool();
                        takeoverControlValid &=
                                sendMouseClick(manualInputFocus) &&
                                manualInputFocus
                                        ->property("activeFocus")
                                        .toBool();
                        viewer.pause();
                        viewer.setTakeOverOnInput(false);
                        QCoreApplication::processEvents();
                        takeoverControlValid &=
                                !viewer.takeOverOnInput() &&
                                !takeOverOnInputCheckBox
                                         ->property("checked")
                                         .toBool() &&
                                stepBackward
                                        ->property("enabled")
                                        .toBool() &&
                                stepForward
                                        ->property("enabled")
                                        .toBool();
                    }
                    bool freeCameraUiValid =
                            viewer.loaded() &&
                            viewer.carCameraAvailable() &&
                            viewer.cameraPreset() == 1 &&
                            viewport != nullptr &&
                            cameraFocusToolbar != nullptr &&
                            cameraFocusToolbar->isVisible() &&
                            scriptedTelemetry != nullptr &&
                            scriptedTelemetry->isVisible() &&
                            scriptedTelemetryText != nullptr &&
                            editTelemetryButton != nullptr &&
                            telemetryEditorDialog != nullptr &&
                            telemetryScriptEditor != nullptr &&
                            telemetryScriptPreview != nullptr &&
                            telemetryFieldCombo != nullptr &&
                            telemetryFieldComboPopup != nullptr &&
                            telemetryFieldComboPopupList != nullptr &&
                            freeCameraButton != nullptr &&
                            orbitalCameraButton != nullptr &&
                            focusCarButton != nullptr &&
                            nearCameraButton != nullptr &&
                            internalCameraButton != nullptr &&
                            focusObjectButton != nullptr &&
                            !viewport->property("freeCamera").toBool() &&
                            !viewport->property("orbitalCamera").toBool() &&
                            viewport->property("carCameraActive").toBool() &&
                            viewport->property("cameraFocusMode")
                                            .toString() ==
                                    QStringLiteral("preset") &&
                            !freeCameraButton->property("highlighted").toBool() &&
                            orbitalCameraButton->property("enabled").toBool() &&
                            !orbitalCameraButton->property("highlighted").toBool() &&
                            focusCarButton->property("enabled").toBool() &&
                            focusCarButton->property("highlighted").toBool() &&
                            nearCameraButton->property("enabled").toBool() &&
                            internalCameraButton->property("enabled").toBool() &&
                            freeCameraButton->property("text").toString() ==
                                    QStringLiteral("Free") &&
                            orbitalCameraButton->property("text").toString() ==
                                    QStringLiteral("Orbital") &&
                            focusCarButton->property("text").toString() ==
                                    QStringLiteral("Far") &&
                            nearCameraButton->property("text").toString() ==
                                    QStringLiteral("Near") &&
                            internalCameraButton->property("text").toString() ==
                                    QStringLiteral("Internal") &&
                            internalCameraButton->property("width").toDouble() +
                                            0.5 >=
                                    internalCameraButton
                                            ->property("implicitWidth")
                                            .toDouble() &&
                            !focusObjectButton->property("enabled").toBool();
                    const auto cameraTelemetryText = [](QVector3D position) {
                        const auto coordinate = [](float value) {
                            const double normalized =
                                    std::abs(value) < 0.005f ? 0.0 : value;
                            return QString::number(normalized, 'f', 2);
                        };
                        return QStringLiteral("Camera pos: X %1   Y %2   Z %3")
                                .arg(coordinate(position.x()),
                                     coordinate(position.y()),
                                     coordinate(position.z()));
                    };
                    freeCameraUiValid &=
                            viewCamera != nullptr &&
                            scriptedTelemetryText != nullptr &&
                            scriptedTelemetryText->property("text")
                                            .toString() ==
                                    cameraTelemetryText(
                                            viewCamera
                                                    ->property("scenePosition")
                                                    .value<QVector3D>());
                    const QString customTelemetryScript =
                            QStringLiteral("Camera X {camera.x:1}");
                    const qreal telemetryOriginalWidth =
                            root->property("width").toReal();
                    const qreal telemetryOriginalHeight =
                            root->property("height").toReal();
                    root->setProperty("width", 1240);
                    root->setProperty("height", 580);
                    QCoreApplication::processEvents();
                    bool telemetryEditorValid =
                            QMetaObject::invokeMethod(
                                    editTelemetryButton, "clicked");
                    QCoreApplication::processEvents();
                    bool telemetryComboCompactValid =
                            telemetryEditorValid &&
                            QMetaObject::invokeMethod(
                                    telemetryFieldComboPopup, "open") &&
                            WaitUntil([telemetryFieldComboPopup]() {
                                return telemetryFieldComboPopup
                                        ->property("visible").toBool();
                            });
                    QCoreApplication::processEvents();
                    const qreal telemetryPopupListHeight =
                            telemetryFieldComboPopupList != nullptr
                            ? telemetryFieldComboPopupList->height()
                            : -1.0;
                    const qreal telemetryPopupContentHeight =
                            telemetryFieldComboPopupList != nullptr
                            ? telemetryFieldComboPopupList
                                      ->property("contentHeight").toReal()
                            : -1.0;
                    const QPointF telemetryPopupTopLeft =
                            telemetryFieldComboPopupList != nullptr
                            ? telemetryFieldComboPopupList->mapToScene(
                                      QPointF(0.0, 0.0))
                            : QPointF(-1.0, -1.0);
                    const QPointF telemetryComboBottomLeft =
                            telemetryFieldCombo != nullptr
                            ? telemetryFieldCombo->mapToScene(
                                      QPointF(0.0,
                                              telemetryFieldCombo->height()))
                            : QPointF(-1.0, -1.0);
                    if (telemetryFieldComboPopupList != nullptr) {
                        telemetryFieldComboPopupList->setProperty(
                                "contentY",
                                telemetryPopupContentHeight -
                                        telemetryPopupListHeight);
                        QCoreApplication::processEvents();
                    }
                    telemetryComboCompactValid &=
                            telemetryFieldComboPopupList != nullptr &&
                            telemetryPopupContentHeight >
                                    telemetryPopupListHeight &&
                            telemetryPopupListHeight > 0.0 &&
                            telemetryPopupTopLeft.y() >=
                                    telemetryComboBottomLeft.y() - 1.0 &&
                            telemetryPopupTopLeft.y() +
                                            telemetryPopupListHeight <=
                                    root->property("height").toReal() - 7.0 &&
                            telemetryFieldComboPopupList
                                            ->property("contentY").toReal() >
                                    0.5;
                    if (!telemetryComboCompactValid) {
                        std::cerr
                                << "compact telemetry combo popup invalid: "
                                << "visible="
                                << telemetryFieldComboPopup
                                           ->property("visible").toBool()
                                << ", list="
                                << telemetryPopupListHeight << "/"
                                << telemetryPopupContentHeight
                                << ", sceneY="
                                << telemetryPopupTopLeft.y() << "/"
                                << telemetryComboBottomLeft.y()
                                << ", contentY="
                                << (telemetryFieldComboPopupList != nullptr
                                            ? telemetryFieldComboPopupList
                                                      ->property("contentY")
                                                      .toReal()
                                            : -1.0)
                                << "\n";
                    }
                    QMetaObject::invokeMethod(
                            telemetryFieldComboPopup, "close");
                    root->setProperty("width", telemetryOriginalWidth);
                    root->setProperty("height", telemetryOriginalHeight);
                    QCoreApplication::processEvents();
                    telemetryEditorValid &=
                            telemetryComboCompactValid &&
                            telemetryEditorDialog->property("visible")
                                    .toBool() &&
                            telemetryScriptEditor->property("text")
                                            .toString() ==
                                    viewer.defaultTelemetryScript() &&
                            telemetryScriptEditor->setProperty(
                                    "text", customTelemetryScript);
                    QCoreApplication::processEvents();
                    const QVector3D telemetryCameraPosition =
                            viewCamera->property("scenePosition")
                                    .value<QVector3D>();
                    telemetryEditorValid &=
                            telemetryScriptPreview->property("text")
                                            .toString() ==
                                    viewer.renderTelemetry(
                                            customTelemetryScript,
                                            telemetryCameraPosition) &&
                            QMetaObject::invokeMethod(
                                    telemetryEditorDialog, "accept");
                    QCoreApplication::processEvents();
                    telemetryEditorValid &=
                            viewer.telemetryScript() ==
                                    customTelemetryScript &&
                            scriptedTelemetryText->property("text")
                                            .toString() ==
                                    viewer.renderTelemetry(
                                            customTelemetryScript,
                                            telemetryCameraPosition);
                    viewer.setTelemetryScript(
                            viewer.defaultTelemetryScript());
                    QCoreApplication::processEvents();
                    freeCameraUiValid &= telemetryEditorValid &&
                            scriptedTelemetryText->property("text")
                                            .toString() ==
                                    cameraTelemetryText(
                                            viewCamera
                                                    ->property("scenePosition")
                                                    .value<QVector3D>());
                    if (!freeCameraUiValid) {
                        std::cerr
                                << "camera UI initial state: loaded="
                                << viewer.loaded()
                                << ", available="
                                << viewer.carCameraAvailable()
                                << ", preset=" << viewer.cameraPreset()
                                << ", free="
                                << (viewport != nullptr
                                            ? viewport->property("freeCamera")
                                                      .toBool()
                                            : false)
                                << ", active="
                                << (viewport != nullptr
                                            ? viewport->property("carCameraActive")
                                                      .toBool()
                                            : false)
                                << ", mode="
                                << (viewport != nullptr
                                            ? viewport->property("cameraFocusMode")
                                                      .toString()
                                                      .toStdString()
                                            : std::string("missing"))
                                << ", buttons="
                                << (freeCameraButton != nullptr) << '/'
                                << (orbitalCameraButton != nullptr) << '/'
                                << (focusCarButton != nullptr) << '/'
                                << (nearCameraButton != nullptr) << '/'
                                << (internalCameraButton != nullptr)
                                << ", highlights="
                                << (freeCameraButton != nullptr
                                            ? freeCameraButton
                                                      ->property("highlighted")
                                                      .toBool()
                                            : false)
                                << '/'
                                << (focusCarButton != nullptr
                                            ? focusCarButton
                                                      ->property("highlighted")
                                                      .toBool()
                                            : false)
                                << '\n';
                    }
                    if (freeCameraUiValid) {
                        const double orbitalYawBefore =
                                viewport->property("orbitYaw").toDouble();
                        const double orbitalPitchBefore =
                                viewport->property("orbitPitch").toDouble();
                        const double orbitalDistanceBefore =
                                viewport->property("orbitDistance").toDouble();
                        QVariant zoomed;
                        freeCameraUiValid &=
                                QMetaObject::invokeMethod(
                                        orbitalCameraButton, "clicked") &&
                                viewport->property("orbitalCamera").toBool() &&
                                !viewport->property("freeCamera").toBool() &&
                                !viewport->property("carCameraActive").toBool() &&
                                viewport->property("cameraFocusMode")
                                                .toString() ==
                                        QStringLiteral("orbital") &&
                                orbitalCameraButton
                                        ->property("highlighted").toBool() &&
                                viewport->property("cameraTarget")
                                                .value<QVector3D>() ==
                                        viewer.carPosition() &&
                                QMetaObject::invokeMethod(
                                        viewport, "beginViewRotation") &&
                                QMetaObject::invokeMethod(
                                        viewport,
                                        "updateViewRotation",
                                        Q_ARG(QVariant, QVariant(12.0)),
                                        Q_ARG(QVariant, QVariant(-7.0))) &&
                                std::abs(viewport->property("orbitYaw").toDouble()
                                         - (orbitalYawBefore - 12.0)) < 0.001 &&
                                std::abs(viewport->property("orbitPitch").toDouble()
                                         - (orbitalPitchBefore + 7.0)) < 0.001 &&
                                QMetaObject::invokeMethod(
                                        viewport,
                                        "zoomOrbit",
                                        Q_RETURN_ARG(QVariant, zoomed),
                                        Q_ARG(QVariant, QVariant(120.0))) &&
                                zoomed.toBool() &&
                                viewport->property("orbitDistance").toDouble() <
                                        orbitalDistanceBefore &&
                                QMetaObject::invokeMethod(
                                        focusCarButton, "clicked") &&
                                !viewport->property("orbitalCamera").toBool() &&
                                viewport->property("carCameraActive").toBool() &&
                                viewport->property("cameraFocusMode")
                                                .toString() ==
                                        QStringLiteral("preset");
                    }
                    if (freeCameraUiValid) {
                        const QVector3D carTargetBeforeFree =
                                viewport->property("cameraTarget")
                                        .value<QVector3D>();
                        const QVector3D carCameraPositionBeforeFree =
                                viewer.carCameraPosition();
                        freeCameraUiValid &=
                                QMetaObject::invokeMethod(
                                        viewport,
                                        "enableFreeCamera") &&
                                viewport->property("freeCamera").toBool() &&
                                viewport->property("cameraFocusMode")
                                                .toString() ==
                                        QStringLiteral("free") &&
                                (viewport->property("cameraTarget")
                                         .value<QVector3D>() -
                                 carTargetBeforeFree)
                                                .lengthSquared() <
                                        0.0001f &&
                                (viewport->property("freeCameraPosition")
                                         .value<QVector3D>() -
                                 carCameraPositionBeforeFree)
                                                .lengthSquared() <
                                        0.0001f;
                        const QVector3D freePositionBeforeLook =
                                viewport->property("freeCameraPosition")
                                        .value<QVector3D>();
                        const QVector3D freeTargetBeforeLook =
                                viewport->property("cameraTarget")
                                        .value<QVector3D>();
                        const double yawBeforeFreeLook =
                                viewport->property("orbitYaw").toDouble();
                        viewport->setProperty(
                                "orbitYaw", yawBeforeFreeLook + 20.0);
                        QCoreApplication::processEvents();
                        freeCameraUiValid &=
                                viewport->property("freeCameraPosition")
                                                .value<QVector3D>() ==
                                        freePositionBeforeLook &&
                                (viewCamera->property("scenePosition")
                                         .value<QVector3D>() -
                                 freePositionBeforeLook)
                                                .lengthSquared() <
                                        0.0001f &&
                                scriptedTelemetryText
                                                ->property("text")
                                                .toString() ==
                                        cameraTelemetryText(
                                                freePositionBeforeLook) &&
                                (viewport->property("cameraTarget")
                                         .value<QVector3D>() -
                                 freeTargetBeforeLook)
                                                .lengthSquared() >
                                        0.0001f;
                        viewport->setProperty(
                                "orbitYaw", yawBeforeFreeLook);
                        QCoreApplication::processEvents();
                        const double pitchBeforeFreeLook =
                                viewport->property("orbitPitch").toDouble();
                        QVariant rotationUpdated;
                        QVariant rotationStepped;
                        freeCameraUiValid &=
                                QMetaObject::invokeMethod(
                                        viewport, "beginViewRotation") &&
                                QMetaObject::invokeMethod(
                                        viewport,
                                        "updateViewRotation",
                                        Q_RETURN_ARG(
                                                QVariant,
                                                rotationUpdated),
                                        Q_ARG(QVariant, QVariant(20.0)),
                                        Q_ARG(QVariant, QVariant(10.0))) &&
                                std::abs(
                                        viewport
                                                        ->property(
                                                                "viewRotationTargetYaw")
                                                        .toDouble() -
                                        (yawBeforeFreeLook - 10.0)) < 0.001 &&
                                std::abs(
                                        viewport
                                                        ->property(
                                                                "viewRotationTargetPitch")
                                                        .toDouble() -
                                        (pitchBeforeFreeLook - 5.0)) < 0.001 &&
                                QMetaObject::invokeMethod(
                                        viewport,
                                        "stepViewRotation",
                                        Q_RETURN_ARG(
                                                QVariant,
                                                rotationStepped),
                                        Q_ARG(
                                                QVariant,
                                                QVariant(1.0 / 60.0))) &&
                                rotationStepped.toBool() &&
                                viewport->property("orbitYaw").toDouble() <
                                        yawBeforeFreeLook &&
                                viewport->property("orbitYaw").toDouble() >
                                        yawBeforeFreeLook - 10.0;
                        viewport->setProperty(
                                "orbitYaw", yawBeforeFreeLook);
                        viewport->setProperty(
                                "orbitPitch", pitchBeforeFreeLook);
                        QMetaObject::invokeMethod(
                                viewport, "beginViewRotation");
                        QVariant movementAccepted;
                        freeCameraUiValid &=
                                QMetaObject::invokeMethod(
                                        viewport,
                                        "setFreeMovement",
                                        Q_RETURN_ARG(
                                                QVariant,
                                                movementAccepted),
                                        Q_ARG(
                                                QVariant,
                                                QVariant(
                                                        QStringLiteral(
                                                                "forward"))),
                                        Q_ARG(
                                                QVariant,
                                                QVariant(true))) &&
                                movementAccepted.toBool();
                        const double movementStart =
                                viewport->property("freeMoveStartedAt")
                                        .toDouble();
                        const QVector3D movementTarget =
                                viewport->property("freeCameraTarget")
                                        .value<QVector3D>();
                        QVariant movementStepped;
                        freeCameraUiValid &=
                                movementStart > 0.0 &&
                                QMetaObject::invokeMethod(
                                        viewport,
                                        "stepFreeCameraMovement",
                                        Q_RETURN_ARG(
                                                QVariant,
                                                movementStepped),
                                        Q_ARG(
                                                QVariant,
                                                QVariant(
                                                        movementStart +
                                                        1000.0))) &&
                                movementStepped.toBool();
                        const double firstMovementSpeed =
                                viewport->property("freeMoveSpeed")
                                        .toDouble();
                        const QVector3D movedTarget =
                                viewport->property("freeCameraTarget")
                                        .value<QVector3D>();
                        freeCameraUiValid &=
                                firstMovementSpeed >= 38.9 &&
                                (movedTarget - movementTarget)
                                                .lengthSquared() >
                                        0.000001f;
                        QVariant movementChanged;
                        QVariant movementReleased;
                        freeCameraUiValid &=
                                QMetaObject::invokeMethod(
                                        viewport,
                                        "setFreeMovement",
                                        Q_RETURN_ARG(
                                                QVariant,
                                                movementChanged),
                                        Q_ARG(
                                                QVariant,
                                                QVariant(
                                                        QStringLiteral(
                                                                "right"))),
                                        Q_ARG(
                                                QVariant,
                                                QVariant(true))) &&
                                movementChanged.toBool() &&
                                QMetaObject::invokeMethod(
                                        viewport,
                                        "setFreeMovement",
                                        Q_RETURN_ARG(
                                                QVariant,
                                                movementReleased),
                                        Q_ARG(
                                                QVariant,
                                                QVariant(
                                                        QStringLiteral(
                                                                "forward"))),
                                        Q_ARG(
                                                QVariant,
                                                QVariant(false))) &&
                                movementReleased.toBool() &&
                                viewport->property("freeMoveSpeed")
                                                .toDouble() ==
                                        firstMovementSpeed &&
                                viewport->property("freeMoveStartedAt")
                                                .toDouble() ==
                                        movementStart;
                        freeCameraUiValid &=
                                QMetaObject::invokeMethod(
                                        viewport,
                                        "stepFreeCameraMovement",
                                        Q_RETURN_ARG(
                                                QVariant,
                                                movementStepped),
                                        Q_ARG(
                                                QVariant,
                                                QVariant(
                                                        movementStart +
                                                        2000.0))) &&
                                viewport->property("freeMoveSpeed")
                                                .toDouble() >
                                        firstMovementSpeed;
                        freeCameraUiValid &=
                                QMetaObject::invokeMethod(
                                        viewport,
                                        "setFreeMovement",
                                        Q_RETURN_ARG(
                                                QVariant,
                                                movementReleased),
                                        Q_ARG(
                                                QVariant,
                                                QVariant(
                                                        QStringLiteral(
                                                                "right"))),
                                        Q_ARG(
                                                QVariant,
                                                QVariant(false))) &&
                                !viewport->property("freeMoveRight")
                                         .toBool() &&
                                viewport->property("freeMoveSpeed")
                                                .toDouble() == 0.0;

                        const QVector3D objectCenter(
                                17.0f, 9.0f, -6.0f);
                        freeCameraUiValid &=
                                QMetaObject::invokeMethod(
                                        viewport,
                                        "focusCuboid",
                                        Q_ARG(
                                                QVariant,
                                                QVariant::fromValue(
                                                        objectCenter)),
                                        Q_ARG(
                                                QVariant,
                                                QVariant::fromValue(
                                                        QVector3D(
                                                                4.0f,
                                                                5.0f,
                                                                6.0f)))) &&
                                viewport->property("cuboidFocused")
                                        .toBool() &&
                                viewport->property("hasObjectFocus")
                                        .toBool() &&
                                viewport->property("cameraTarget")
                                                .value<QVector3D>() ==
                                        objectCenter &&
                                focusObjectButton->property("enabled")
                                        .toBool();
                        freeCameraUiValid &=
                                QMetaObject::invokeMethod(
                                        viewport,
                                        "enableFreeCamera") &&
                                (viewport->property("cameraTarget")
                                         .value<QVector3D>() -
                                 objectCenter)
                                                .lengthSquared() <
                                        0.0001f;
                        const QVector3D independentFreeTarget =
                                viewport->property("cameraTarget")
                                        .value<QVector3D>();
                        const qint64 tickBeforeFreeCameraCheck =
                                viewer.currentTick();
                        if (viewer.tickCount() > 1) {
                            viewer.setCurrentTick(
                                    tickBeforeFreeCameraCheck == 0 ? 1 : 0);
                            QCoreApplication::processEvents();
                            freeCameraUiValid &=
                                    (viewport->property("cameraTarget")
                                             .value<QVector3D>() -
                                     independentFreeTarget)
                                                    .lengthSquared() <
                                            0.0001f;
                            viewer.setCurrentTick(
                                    tickBeforeFreeCameraCheck);
                            QCoreApplication::processEvents();
                        }
                        freeCameraUiValid &=
                                QMetaObject::invokeMethod(
                                        viewport,
                                        "setFreeMovement",
                                        Q_RETURN_ARG(
                                                QVariant,
                                                movementChanged),
                                        Q_ARG(
                                                QVariant,
                                                QVariant(
                                                        QStringLiteral(
                                                                "up"))),
                                        Q_ARG(
                                                QVariant,
                                                QVariant(true))) &&
                                movementChanged.toBool() &&
                                viewport->property("freeMoveUp").toBool() &&
                                QMetaObject::invokeMethod(
                                        viewport,
                                        "focusLastObject") &&
                                !viewport->property("freeMoveForward")
                                         .toBool() &&
                                !viewport->property("freeMoveBackward")
                                         .toBool() &&
                                !viewport->property("freeMoveLeft").toBool() &&
                                !viewport->property("freeMoveRight").toBool() &&
                                !viewport->property("freeMoveUp").toBool() &&
                                !viewport->property("freeMoveDown").toBool() &&
                                viewport->property("freeMoveSpeed")
                                                .toDouble() == 0.0 &&
                                viewport->property("cameraTarget")
                                                .value<QVector3D>() ==
                                        objectCenter &&
                                QMetaObject::invokeMethod(
                                        viewport,
                                        "focusCurrentCar") &&
                                !viewport->property("freeCamera").toBool() &&
                                !viewport->property("cuboidFocused")
                                         .toBool() &&
                                !viewport->property("orbitalCamera")
                                         .toBool() &&
                                viewport->property("cameraFocusMode")
                                                .toString() ==
                                        QStringLiteral("preset") &&
                                viewport->property("cameraTarget")
                                                .value<QVector3D>() ==
                                        viewer.carCameraTarget();
                    }
                    if (freeCameraUiValid) {
                        freeCameraUiValid &=
                                QMetaObject::invokeMethod(
                                        viewport, "enableFreeCamera") &&
                                viewport->property("freeCamera").toBool();
                        viewport->setProperty("orbitYaw", 35.0);
                        viewport->setProperty("orbitPitch", -20.0);
                        viewport->setProperty("orbitDistance", 38.0);
                        QMetaObject::invokeMethod(
                                viewport, "beginViewRotation");
                        QCoreApplication::processEvents();
                    }
                    const bool manualDrivingUi =
                            playbackDock != nullptr &&
                            playbackDock->width() >= 285.0 &&
                            takeoverControlValid &&
                            manualDriveButton != nullptr &&
                            manualDriveButton->property("text").toString() ==
                                    QStringLiteral("Drive") &&
                            manualDriveButton
                                    ->property("enabled")
                                    .toBool() ==
                                    (viewer.loaded() &&
                                     !viewer.loading()) &&
                            manualDriveStatus != nullptr &&
                            !manualDriveStatus->isVisible() &&
                            manualInputFocus != nullptr &&
                            manualMapping(Qt::Key_Left) ==
                                    QStringLiteral("left") &&
                            manualMapping(Qt::Key_A) ==
                                    QStringLiteral("left") &&
                            manualMapping(Qt::Key_Q) ==
                                    QStringLiteral("left") &&
                            manualMapping(Qt::Key_Right) ==
                                    QStringLiteral("right") &&
                            manualMapping(Qt::Key_D) ==
                                    QStringLiteral("right") &&
                            manualMapping(Qt::Key_Up) ==
                                    QStringLiteral("accelerate") &&
                            manualMapping(Qt::Key_W) ==
                                    QStringLiteral("accelerate") &&
                            manualMapping(Qt::Key_Z) ==
                                    QStringLiteral("accelerate") &&
                            manualMapping(Qt::Key_Down) ==
                                    QStringLiteral("brake") &&
                            manualMapping(Qt::Key_S) ==
                                    QStringLiteral("brake") &&
                            manualMapping(Qt::Key_Escape).isEmpty() &&
                            manualActionMapping(Qt::Key_Delete) ==
                                    QStringLiteral("give-up") &&
                            manualActionMapping(Qt::Key_Return) ==
                                    QStringLiteral("respawn") &&
                            manualActionMapping(Qt::Key_Enter) ==
                                    QStringLiteral("respawn") &&
                            manualActionMapping(Qt::Key_Backspace) ==
                                    QStringLiteral("respawn") &&
                            manualActionMapping(Qt::Key_Escape).isEmpty() &&
                            freeCameraUiValid;
                    const qreal originalWindowWidth =
                            root->property("width").toReal();
                    root->setProperty("width", 1240);
                    QCoreApplication::processEvents();
                    const bool compactViewerHeader =
                            raceViewerHeader != nullptr &&
                            headerControlsRow != nullptr &&
                            runSelector != nullptr &&
                            renderModeSelector != nullptr &&
                            resetViewButton != nullptr &&
                            raceViewerTitleBlock != nullptr &&
                            raceViewerTitleBlock->x() >= -0.1 &&
                            runSelector->x() >=
                                    raceViewerTitleBlock->x() +
                                            raceViewerTitleBlock->width() &&
                            renderModeSelector->x() >=
                                    runSelector->x() + runSelector->width() &&
                            resetViewButton->x() >=
                                    renderModeSelector->x() +
                                            renderModeSelector->width() &&
                            resetViewButton->x() +
                                            resetViewButton->width() <=
                                    headerControlsRow->width() + 0.1;
                    root->setProperty("width", originalWindowWidth);
                    QCoreApplication::processEvents();
                    const auto rowCenter =
                            [](const QQuickItem *item) {
                                return item != nullptr
                                        ? item->y() + item->height() * 0.5
                                        : -1.0;
                            };
                    const bool runSelectorValid =
                            raceViewerHeader != nullptr &&
                            headerControlsRow != nullptr &&
                            raceViewerTitleBlock != nullptr &&
                            trajectoryVisibilityToggle != nullptr &&
                            runSelector != nullptr &&
                            renderModeSelector != nullptr &&
                            resetViewButton != nullptr &&
                            headerControlsRow->parentItem() ==
                                    raceViewerHeader &&
                            raceViewerTitleBlock->parentItem() ==
                                    headerControlsRow &&
                            runSelector->parentItem() ==
                                    headerControlsRow &&
                            renderModeSelector->parentItem() ==
                                    headerControlsRow &&
                            resetViewButton->parentItem() ==
                                    headerControlsRow &&
                            trajectoryVisibilityToggle->x() +
                                            trajectoryVisibilityToggle->width() <=
                                    runSelector->x() + 0.1 &&
                            std::abs(rowCenter(raceViewerTitleBlock) -
                                     rowCenter(runSelector)) < 0.6 &&
                            std::abs(rowCenter(runSelector) -
                                     rowCenter(renderModeSelector)) < 0.6 &&
                            std::abs(rowCenter(renderModeSelector) -
                                     rowCenter(resetViewButton)) < 0.6 &&
                            renderModeSelector->width() >= 179.0 &&
                            runSelector->property("count").toInt() == 1 &&
                            runSelector->property("enabled").toBool();
                    bool globalSettingsPlacement =
                            globalSettingsVisibleAcrossTabs &&
                            packsDirectorySection != nullptr &&
                            replaySection != nullptr &&
                            baseInputScriptSection != nullptr &&
                            appearanceControls != nullptr &&
                            toolTabs != nullptr &&
                            bruteforceTabContent != nullptr &&
                            simulationDebuggerPanel != nullptr &&
                            root->findChildren<QObject *>(
                                        QStringLiteral(
                                                "baseInputScriptSection"))
                                            .size() == 1 &&
                            root->findChildren<QObject *>(
                                        QStringLiteral("replaySection"))
                                            .size() == 1 &&
                            root->findChildren<QObject *>(
                                        QStringLiteral("appearanceControls"))
                                            .size() == 1 &&
                            root->findChildren<QObject *>(
                                        QStringLiteral("replayPathField"))
                                            .size() == 1 &&
                            root->findChildren<QObject *>(
                                        QStringLiteral("browseReplayButton"))
                                            .size() == 1 &&
                            root->findChildren<QObject *>(
                                        QStringLiteral("loadMapButton"))
                                            .size() == 1 &&
                            root->findChildren<QObject *>(
                                        QStringLiteral(
                                                "extractReplayInputsButton"))
                                            .size() == 1 &&
                            packsDirectorySection->parentItem() ==
                                    replaySection->parentItem() &&
                            replaySection->parentItem() ==
                                    baseInputScriptSection->parentItem() &&
                            baseInputScriptSection->parentItem() ==
                                    appearanceControls->parentItem() &&
                            appearanceControls->parentItem() ==
                                    toolTabs->parentItem() &&
                            packsDirectorySection->y() +
                                            packsDirectorySection->height() <=
                                    replaySection->y() &&
                            replaySection->y() + replaySection->height() <=
                                    baseInputScriptSection->y() &&
                            baseInputScriptSection->y() +
                                            baseInputScriptSection->height() <=
                                    appearanceControls->y() &&
                            appearanceControls->y() +
                                            appearanceControls->height() <=
                                    toolTabs->y();
                    const bool baseInputScriptUiValid =
                            baseInputScriptSection != nullptr &&
                            replayPathField != nullptr &&
                            browseReplayButton != nullptr &&
                            baseInputScriptScrollView != nullptr &&
                            baseInputScriptTextArea != nullptr &&
                            baseInputScriptErrorLabel != nullptr &&
                            saveBaseInputScriptShortcut != nullptr &&
                            undoBaseInputScriptShortcut != nullptr &&
                            copyCurrentRaceInputsButton != nullptr &&
                            copyCurrentRaceInputsButton
                                     ->property("enabled").toBool() &&
                            root->findChild<QObject *>(
                                    QStringLiteral(
                                            "saveInputTrajectoryButton")) ==
                                    nullptr &&
                            root->findChild<QObject *>(
                                    QStringLiteral(
                                            "saveInputTrajectoryShortcut")) ==
                                    nullptr &&
                            !ContainsText(
                                    root,
                                    QStringLiteral("Save trajectory")) &&
                            loadMapButton != nullptr &&
                            extractReplayInputsButton != nullptr &&
                            replaceBaseInputScriptDialog != nullptr &&
                            !baseInputScriptTextArea
                                     ->property("readOnly")
                                     .toBool() &&
                            baseInputScriptTextArea
                                    ->property("enabled")
                                    .toBool() &&
                            baseInputScriptErrorLabel
                                    ->property("text")
                                    .toString()
                                    .isEmpty() &&
                            loadMapButton->property("text").toString() ==
                                    QStringLiteral("Load map") &&
                            extractReplayInputsButton
                                            ->property("text")
                                            .toString() ==
                                    QStringLiteral("Extract inputs to script");
                    const bool bestInputsUiValid =
                            bestInputsScrollView != nullptr &&
                            bestInputsTextArea != nullptr &&
                            copyBestInputsButton != nullptr &&
                            bestInputsTextArea->property("readOnly").toBool() &&
                            copyBestInputsButton->property("text").toString() ==
                                    QStringLiteral("Copy all");
                    const bool searchControlsValid =
                            startSearchButton != nullptr &&
                            stopSearchButton != nullptr &&
                            startSearchButton->property("text").toString() ==
                                    QStringLiteral("Start") &&
                            stopSearchButton->property("text").toString() ==
                                    QStringLiteral("Stop") &&
                            !stopSearchButton->property("enabled").toBool();
                    const bool searchMetricsUiValid =
                            searchMetricsRow != nullptr &&
                            iterationsMetricCard != nullptr &&
                            iterationsMetricValue != nullptr &&
                            throughputMetricCard != nullptr &&
                            throughputMetricValue != nullptr &&
                            elapsedMetricCard != nullptr &&
                            elapsedMetricValue != nullptr &&
                            !searchMetricsRow->isVisible() &&
                            std::abs(iterationsMetricCard->height() -
                                     throughputMetricCard->height()) < 0.1 &&
                            std::abs(throughputMetricCard->height() -
                                     elapsedMetricCard->height()) < 0.1 &&
                            iterationsMetricValue->property("text")
                                    .toString().isEmpty() &&
                            throughputMetricValue->property("text")
                                    .toString().isEmpty() &&
                            elapsedMetricValue->property("text")
                                    .toString().isEmpty();
                    const bool removedSectionDescriptions =
                            !ContainsText(
                                    root,
                                    QStringLiteral("Build an ordered pipeline")) &&
                            !ContainsText(
                                    root,
                                    QStringLiteral("Choose what makes one")) &&
                            !ContainsText(
                                    root,
                                    QStringLiteral("Choose how iterations")) &&
                            !ContainsText(
                                    root,
                                    QStringLiteral("Runs continuously until"));
                    const bool automaticPacksUi =
                            autoPacksSuggestion != nullptr &&
                            autoPacksSuggestionText != nullptr &&
                            autoPacksSuggestionText->property("text")
                                    .toString() ==
                                    QStringLiteral(
                                            "This location was found "
                                            "automatically and should work. "
                                            "Apply?") &&
                            applyAutoPacks != nullptr &&
                            applyAutoPacks->property("text").toString() ==
                                    QStringLiteral("Apply");
                    bool backendSelectorValid =
                            simulationBackendCombo != nullptr &&
                            simulationBackendCombo->property("count").toInt() ==
#if FOREVERVALIDATOR_HAS_CUDA
                                    4 &&
#else
                                    3 &&
#endif
                            simulationBackendCombo->property("currentValue")
                                            .toString() ==
                                    QStringLiteral("reference") &&
                            simulationBackendCombo->property("displayText")
                                            .toString() ==
                                    QStringLiteral("Reference") &&
                            randomizeSeedsOnStartCheckBox != nullptr &&
                            randomizeSeedsOnStartCheckBox
                                    ->property("checked")
                                    .toBool() &&
                            simulationHorizonField != nullptr &&
                            simulationHorizonField
                                            ->property("dragStep")
                                            .toDouble() == 1000.0 &&
                            !simulationHorizonField
                                     ->property("liveScrub")
                                     .toBool();
                    if (backendSelectorValid) {
                        controller.setSimulationBackendId(
                                QStringLiteral("optimized-cpu"));
                        QCoreApplication::processEvents();
                        backendSelectorValid =
                                simulationBackendCombo
                                                ->property("currentValue")
                                                .toString() ==
                                        QStringLiteral("optimized-cpu") &&
                                simulationBackendCombo
                                                ->property("displayText")
                                                .toString() ==
                                        QStringLiteral("CPU Optimized") &&
                                ContainsText(
                                        root,
                                        QStringLiteral(
                                                "Faster runtime optimized for "
                                                "Stadium, may break "
                                                "compatibility in other "
                                                "environments"));
                        if (backendSelectorValid) {
                            controller.setSimulationBackendId(
                                    QStringLiteral(
                                            "multi-threaded-cpu"));
                            controller.setCpuWorkerCount(
                                    QStringLiteral("2"));
                            QCoreApplication::processEvents();
                            backendSelectorValid =
                                    simulationBackendCombo
                                                    ->property("currentValue")
                                                    .toString() ==
                                            QStringLiteral(
                                                    "multi-threaded-cpu") &&
                                    simulationBackendCombo
                                                    ->property("displayText")
                                                    .toString() ==
                                            QStringLiteral(
                                                    "CPU Multi-threaded") &&
                                    cpuWorkerSettings != nullptr &&
                                    cpuWorkerSettings->isVisible() &&
                                    cpuWorkerCountField != nullptr &&
                                    cpuWorkerCountField
                                                    ->property("text")
                                                    .toString() ==
                                            QStringLiteral("2") &&
                                    ContainsText(
                                            root,
                                            QStringLiteral(
                                                    "Runs independent "
                                                    "optimized CPU "
                                                    "simulations across "
                                                    "multiple worker "
                                                    "threads"));
                        }
#if FOREVERVALIDATOR_HAS_CUDA
                        if (backendSelectorValid) {
                            controller.setSimulationBackendId(
                                    QStringLiteral("cuda"));
                            QCoreApplication::processEvents();
                            backendSelectorValid =
                                    simulationBackendCombo
                                                    ->property("currentValue")
                                                    .toString() ==
                                            QStringLiteral("cuda") &&
                                    simulationBackendCombo
                                                    ->property("displayText")
                                                    .toString() ==
                                            QStringLiteral("CUDA") &&
                                    cudaParallelSampleSettings != nullptr &&
                                    cudaParallelSampleSettings->isVisible() &&
                                    cudaParallelSampleCountField != nullptr &&
                                    cudaParallelSampleCountField
                                                    ->property("text")
                                                    .toString() ==
                                            QStringLiteral("256") &&
                                    cudaCalibrationCheckBox != nullptr &&
                                    cudaCalibrationCheckBox->isVisible() &&
                                    cudaCalibrationCheckBox->y() >=
                                            cudaParallelSampleSettings->y() +
                                                    cudaParallelSampleSettings
                                                            ->height() &&
                                    !cudaCalibrationCheckBox
                                             ->property("checked")
                                             .toBool() &&
                                    cudaSessionSpecializationSection != nullptr &&
                                    cudaSessionSpecializationSection->isVisible() &&
                                    searchSection != nullptr &&
                                    cudaSessionSpecializationSection->parentItem() ==
                                            searchSection->parentItem() &&
                                    cudaSessionSpecializationSection->y() >=
                                            searchSection->y() +
                                                    searchSection->height() &&
                                    qobject_cast<QQuickItem *>(
                                            startSearchButton) != nullptr &&
                                    cudaSessionSpecializationSection->y() +
                                                    cudaSessionSpecializationSection
                                                            ->height() <=
                                            qobject_cast<QQuickItem *>(
                                                    startSearchButton)
                                                    ->mapToItem(
                                                            cudaSessionSpecializationSection
                                                                    ->parentItem(),
                                                            QPointF{})
                                                    .y() &&
                                    cudaSessionSpecializationSwitch != nullptr &&
                                    cudaSessionSpecializationSwitch
                                            ->property("checked")
                                            .toBool() &&
                                    cudaSessionSpecializationWarning != nullptr &&
                                    cudaSessionSpecializationWarning
                                            ->property("text")
                                            .toString()
                                            .contains(QStringLiteral("stunts")) &&
                                    cudaSessionSpecializationWarning
                                            ->property("text")
                                            .toString()
                                            .contains(QStringLiteral("respawns")) &&
                                    cudaSessionSpecializationWarning
                                            ->property("text")
                                            .toString()
                                            .contains(QStringLiteral(
                                                    "Regular CUDA is safer")) &&
                                    ContainsText(
                                            root,
                                            QStringLiteral(
                                                    "Fastest runtime optimized "
                                                    "for Stadium, needs a "
                                                    "modern NVIDIA GPU and may "
                                                    "break compatibility in "
                                                    "other environments"));
                            controller.setCudaCalibrationEnabled(true);
                            controller.setCudaParallelSampleCount(
                                    QStringLiteral("512"));
                            controller.setCudaSessionSpecializationEnabled(
                                    false);
                            QCoreApplication::processEvents();
                            backendSelectorValid &=
                                    cudaCalibrationCheckBox
                                            ->property("checked")
                                            .toBool() &&
                                    cudaParallelSampleCountField
                                                    ->property("text")
                                                    .toString() ==
                                            QStringLiteral("512") &&
                                    !cudaSessionSpecializationSwitch
                                             ->property("checked")
                                             .toBool() &&
                                    cudaSessionSpecializationWarning
                                            ->property("text")
                                            .toString()
                                            .startsWith(QStringLiteral(
                                                    "Fast mode is off."));
                            controller.setCudaSessionSpecializationEnabled(
                                    true);
                            QCoreApplication::processEvents();
                            backendSelectorValid &=
                                    cudaSessionSpecializationSwitch
                                            ->property("checked")
                                            .toBool() &&
                                    cudaSessionSpecializationWarning
                                            ->property("text")
                                            .toString()
                                            .startsWith(QStringLiteral(
                                                    "Fast mode is on."));
                        }
#endif
                        controller.setSimulationBackendId(
                                QStringLiteral("reference"));
                        QCoreApplication::processEvents();
#if FOREVERVALIDATOR_HAS_CUDA
                        backendSelectorValid &=
                                cudaSessionSpecializationSection != nullptr &&
                                !cudaSessionSpecializationSection->isVisible();
#endif
                    }
                    const bool algorithmSelectorsValid =
                            searchAlgorithmCombo != nullptr &&
                            modifierComposition != nullptr &&
                            addModifierCombo != nullptr &&
                            addModifierButton != nullptr &&
                            evaluationTargetCombo != nullptr &&
                            searchAlgorithmCombo->property("count").toInt() ==
                                    1 &&
                            modifierComposition
                                            ->property("firstPassOptionCount")
                                            .toInt() == 5 &&
                            addModifierCombo->property("count").toInt() == 5 &&
                            evaluationTargetCombo->property("count").toInt() ==
                                    7 &&
                            searchAlgorithmCombo->property("currentValue")
                                            .toString() ==
                                    QStringLiteral("basic-brute-force") &&
                            searchAlgorithmCombo->property("displayText")
                                            .toString() ==
                                    QStringLiteral("Basic bruteforce") &&
                            modifierComposition
                                            ->property("firstPassSelectedId")
                                            .toString() ==
                                    QStringLiteral("random-steering") &&
                            evaluationTargetCombo->property("currentValue")
                                            .toString() ==
                                    QStringLiteral("velocity") &&
                            basicBruteForceSettings != nullptr &&
                            autoPromoteBestSwitch != nullptr &&
                            !autoPromoteBestSwitch
                                     ->property("checked")
                                     .toBool() &&
                            modifierComposition
                                    ->property("firstPassSettingsLoaded")
                                    .toBool() &&
                            velocitySettings != nullptr;
                    controller.setSearchAlgorithmSetting(
                            QStringLiteral("autoPromoteBest"),
                            QStringLiteral("true"));
                    QCoreApplication::processEvents();
                    const bool autoPromoteBestValid =
                            autoPromoteBestSwitch != nullptr &&
                            autoPromoteBestSwitch
                                    ->property("checked")
                                    .toBool();
                    controller.setSearchAlgorithmSetting(
                            QStringLiteral("autoPromoteBest"),
                            QStringLiteral("false"));
                    QCoreApplication::processEvents();
                    const bool settingComboTextValid =
                            velocityModeCombo != nullptr &&
                            velocityModeComboContent != nullptr &&
                            velocityModeCombo->property("displayText")
                                            .toString() ==
                                    QStringLiteral("Total speed") &&
                            velocityModeComboContent->property("text")
                                            .toString() ==
                                    QStringLiteral("Total speed") &&
                            !velocityModeComboContent->property("truncated")
                                     .toBool() &&
                            velocityModeCombo->width() >= 160.0;
                    const bool configurationSectionsValid =
                            conditionsSection != nullptr &&
                            conditionScriptTextArea != nullptr &&
                            evaluationSection != nullptr &&
                            modifierSection != nullptr &&
                            searchSection != nullptr &&
                            evaluationSection->parentItem() ==
                                    modifierSection->parentItem() &&
                            modifierSection->parentItem() ==
                                    searchSection->parentItem() &&
                            conditionsSection->y() < evaluationSection->y() &&
                            evaluationSection->y() < modifierSection->y() &&
                            modifierSection->y() < searchSection->y() &&
                            evaluationSection->property("radius").toReal() >
                                    0.0 &&
                            modifierSection->property("radius").toReal() >
                                    0.0 &&
                            searchSection->property("radius").toReal() > 0.0;
                    const bool comboSlotsStyled =
                            simulationBackendCombo != nullptr &&
                            searchAlgorithmCombo != nullptr &&
                            evaluationTargetCombo != nullptr &&
                            addModifierCombo != nullptr &&
                            simulationBackendCombo->property("slotStyled")
                                    .toBool() &&
                            searchAlgorithmCombo->property("slotStyled")
                                    .toBool() &&
                            evaluationTargetCombo->property("slotStyled")
                                    .toBool() &&
                            addModifierCombo->property("slotStyled").toBool() &&
                            modifierComposition
                                    ->property("firstPassSlotStyled").toBool();
                    const bool modifierPassLayoutValid =
                            modifierComposition != nullptr &&
                            modifierComposition
                                    ->property("firstPassHeaderLayoutValid")
                                    .toBool();
                    const bool debuggerSourceTreeScrollable = [&]() {
                        if (simulationSourceTree == nullptr ||
                            simulationSourceTreeScrollBar == nullptr) {
                            return false;
                        }
                        const qreal height =
                                simulationSourceTree->property("height")
                                        .toReal();
                        const qreal contentHeight =
                                simulationSourceTree
                                        ->property("contentHeight")
                                        .toReal();
                        const qreal maximumContentY =
                                std::max<qreal>(0.0, contentHeight - height);
                        const bool overflowState =
                                maximumContentY > 1.0 &&
                                simulationSourceTree
                                        ->property("interactive")
                                        .toBool() &&
                                simulationSourceTreeScrollBar
                                                ->property("size")
                                                .toReal() < 0.999;
                        simulationSourceTree->setProperty(
                                "contentY", maximumContentY);
                        QCoreApplication::processEvents();
                        const bool movedToEnd =
                                simulationSourceTree
                                        ->property("contentY")
                                        .toReal() > 1.0;
                        simulationSourceTree->setProperty("contentY", 0.0);
                        QCoreApplication::processEvents();
                        return overflowState && movedToEnd;
                    }();
                    const bool codeExpansionValid = [&]() {
                        if (simulationDebuggerPanel == nullptr ||
                            simulationDebuggerPanelHost == nullptr ||
                            workspaceContent == nullptr ||
                            settingsPanel == nullptr ||
                            toggleCodeEditorExpansionButton == nullptr ||
                            simulationCodeViewer == nullptr ||
                            bruteforceTabContent == nullptr) {
                            return false;
                        }
                        const int originalWidth =
                                root->property("width").toInt();
                        const int originalHeight =
                                root->property("height").toInt();
                        simulationDebuggerPanelHost->setVisible(true);
                        simulationDebuggerPanel->setVisible(true);
                        bruteforceTabContent->setVisible(false);
                        QCoreApplication::processEvents();
                        const qreal compactWidth =
                                simulationDebuggerPanel->width();
                        const QString compactIcon =
                                toggleCodeEditorExpansionButton
                                        ->property("text")
                                        .toString();
                        const bool expanded =
                                QMetaObject::invokeMethod(
                                        toggleCodeEditorExpansionButton,
                                        "clicked",
                                        Qt::DirectConnection);
                        QCoreApplication::processEvents();
                        QCoreApplication::processEvents();
                        bool valid =
                                expanded &&
                                root->property("codeEditorExpanded")
                                        .toBool() &&
                                simulationDebuggerPanel
                                        ->property("expanded")
                                        .toBool() &&
                                simulationDebuggerPanel->parentItem() ==
                                        settingsPanel &&
                                !workspaceContent->isVisible() &&
                                settingsPanel->width() >=
                                        originalWidth - 1.0 &&
                                simulationDebuggerPanel->width() >=
                                        originalWidth - 1.0 &&
                                simulationDebuggerPanel->height() >=
                                        originalHeight - 1.0 &&
                                simulationDebuggerPanel->width() >
                                        compactWidth * 2.0 &&
                                toggleCodeEditorExpansionButton
                                                ->property("text")
                                                .toString() != compactIcon;

                        root->setProperty("width", 1240);
                        root->setProperty("height", 580);
                        QCoreApplication::processEvents();
                        QCoreApplication::processEvents();
                        valid &= settingsPanel->width() >= 1239.0 &&
                                simulationDebuggerPanel->width() >= 1239.0 &&
                                simulationDebuggerPanel->height() >= 579.0 &&
                                simulationCodeViewer->width() > 1100.0 &&
                                simulationCodeViewer->height() >= 145.0;

                        const bool collapsed =
                                QMetaObject::invokeMethod(
                                        toggleCodeEditorExpansionButton,
                                        "clicked",
                                        Qt::DirectConnection);
                        root->setProperty("width", originalWidth);
                        root->setProperty("height", originalHeight);
                        QEventLoop settle;
                        QTimer::singleShot(
                                60, &settle, &QEventLoop::quit);
                        settle.exec();
                        valid &= collapsed &&
                                !root->property("codeEditorExpanded")
                                         .toBool() &&
                                !simulationDebuggerPanel
                                         ->property("expanded")
                                         .toBool() &&
                                simulationDebuggerPanel->parentItem() ==
                                        simulationDebuggerPanelHost &&
                                workspaceContent->isVisible() &&
                                settingsPanel->width() <= 480.0;
                        simulationDebuggerPanelHost->setVisible(false);
                        simulationDebuggerPanel->setVisible(false);
                        bruteforceTabContent->setVisible(true);
                        QObject *const settingsContent =
                                settingsScroll->property("contentItem")
                                        .value<QObject *>();
                        if (settingsContent != nullptr) {
                            settingsContent->setProperty("contentY", 0.0);
                        }
                        QCoreApplication::processEvents();
                        return valid;
                    }();
                    const bool debuggerUiValid =
                            toolTabs != nullptr &&
                            toolTabs->property("count").toInt() == 2 &&
                            toolTabs->property("currentIndex").toInt() == 0 &&
                            bruteforceTabContent != nullptr &&
                            bruteforceTabContent->isVisible() &&
                            simulationDebuggerPanel != nullptr &&
                            !simulationDebuggerPanel->isVisible() &&
                            simulationDebuggerPanel->height() + 0.5 >=
                                    simulationDebuggerPanel
                                            ->implicitHeight() &&
                            referenceLoadingWarning != nullptr &&
                            !referenceLoadingWarning->isVisible() &&
                            referenceLoadingWarning
                                            ->property("color")
                                            .value<QColor>()
                                            .red() >
                                    referenceLoadingWarning
                                            ->property("color")
                                            .value<QColor>()
                                            .green() &&
                            referenceLoadingWarning
                                            ->property("radius")
                                            .toReal() <= 8.0 &&
                            referenceLoadingWarningText != nullptr &&
                            simulationDebuggerStatusText != nullptr &&
                            referenceLoadingWarningText
                                            ->property("font")
                                            .value<QFont>()
                                            .pixelSize() >= 18 &&
                            referenceLoadingWarningText
                                            ->property("font")
                                            .value<QFont>()
                                            .weight() >= QFont::Bold &&
                            referenceLoadingWarningText
                                            ->property("color")
                                            .value<QColor>()
                                            .red() >
                                    referenceLoadingWarningText
                                            ->property("color")
                                            .value<QColor>()
                                            .green() &&
                            referenceLoadingWarningText
                                            ->property("color")
                                            .value<QColor>() !=
                                    simulationDebuggerStatusText
                                            ->property("color")
                                            .value<QColor>() &&
                            simulationSourceTree != nullptr &&
                            debuggerSourceTreeScrollable &&
                            simulationCodeViewer != nullptr &&
                            debuggerWaitingForPauseOverlay != nullptr &&
                            !debuggerWaitingForPauseOverlay->isVisible() &&
                            debuggerWaitingForPauseOverlay->height() == 26.0 &&
                            debuggerWaitingForPauseOverlay->width() <=
                                    simulationCodeViewer->width() - 15.0 &&
                            debuggerWaitingForPauseText != nullptr &&
                            debuggerWaitingForPauseText
                                            ->property("text")
                                            .toString() ==
                                    QStringLiteral("Waiting for pause") &&
                            !debuggerWaitingForPauseText
                                     ->property("truncated")
                                     .toBool() &&
                            !simulationDebuggerPanel
                                     ->property("waitingForPause")
                                     .toBool() &&
                            simulationVariables == nullptr &&
                            simulationDebugOutput != nullptr &&
                            simulationDebugOutputEmptyState != nullptr &&
                            simulationDebugOutputEmptyState
                                            ->property("text")
                                            .toString() ==
                                    QStringLiteral(
                                            "No printed output for this run.") &&
                            clearSimulationDebugOutputButton != nullptr &&
                            !clearSimulationDebugOutputButton
                                     ->property("enabled")
                                     .toBool() &&
                            restartLiveSimulationButton != nullptr &&
                            resetLiveEditsButton != nullptr &&
                            debuggerSubstepForwardButton != nullptr &&
                            debuggerSubstepForwardButton
                                            ->property("text")
                                            .toString() ==
                                    QStringLiteral("Substep Forward") &&
                            debuggerSourceLineStepButton != nullptr &&
                            debuggerSourceLineStepButton
                                            ->property("text")
                                            .toString() ==
                                    QStringLiteral("Source Line Step") &&
                            debuggerTickStepButton != nullptr &&
                            debuggerTickStepButton
                                            ->property("text")
                                            .toString() ==
                                    QStringLiteral("Tick Step") &&
                            codeExpansionValid &&
                            debuggerCombinedNameValid;

                    bool globalComboWheelValid = false;
                    bool globalSliderWheelValid = false;
                    bool baseInputWheelValid = false;
                    bool bestInputsWheelValid = false;
                    bool sourceTreeWheelValid = false;
                    bool codeViewerWheelValid = false;
                    bool perturbationSliderEditorsValid = false;
                    bool wheelScrollingValid =
                            settingsScroll != nullptr &&
                            settingsWheelRedirector != nullptr &&
                            settingsWheelRedirector->property("blocking")
                                    .toBool();
                    if (wheelScrollingValid) {
                        QObject *const flickable =
                                settingsScroll->property("contentItem")
                                        .value<QObject *>();
                        QObject *const bestInputsFlickable =
                                bestInputsScrollView == nullptr
                                ? nullptr
                                : bestInputsScrollView
                                          ->property("contentItem")
                                          .value<QObject *>();
                        QObject *const baseInputFlickable =
                                baseInputScriptScrollView == nullptr
                                ? nullptr
                                : baseInputScriptScrollView
                                          ->property("contentItem")
                                          .value<QObject *>();
                        auto *const scrollItem =
                                qobject_cast<QQuickItem *>(settingsScroll);
                        auto *const redirectorItem =
                                qobject_cast<QQuickItem *>(
                                        settingsWheelRedirector);
                        auto *const comboItem =
                                qobject_cast<QQuickItem *>(
                                        evaluationTargetCombo);
                        auto *const bestInputsItem =
                                qobject_cast<QQuickItem *>(
                                        bestInputsScrollView);
                        auto *const baseInputItem =
                                qobject_cast<QQuickItem *>(
                                        baseInputScriptScrollView);
                        auto *const sourceTreeItem =
                                qobject_cast<QQuickItem *>(
                                        simulationSourceTree);
                        auto *const codeViewerItem =
                                qobject_cast<QQuickItem *>(
                                        simulationCodeViewer);
                        auto *const quickWindow =
                                qobject_cast<QQuickWindow *>(root);
                        wheelScrollingValid &= flickable != nullptr &&
                                bestInputsFlickable != nullptr &&
                                baseInputFlickable != nullptr &&
                                scrollItem != nullptr &&
                                redirectorItem != nullptr &&
                                comboItem != nullptr &&
                                bestInputsItem != nullptr &&
                                baseInputItem != nullptr &&
                                sourceTreeItem != nullptr &&
                                codeViewerItem != nullptr &&
                                quickWindow != nullptr;
                        const auto sendWheel = [quickWindow](
                                                       QQuickItem *item,
                                                       const QPointF &local,
                                                       int delta) {
                            const QPointF position = item->mapToScene(local);
                            const QPoint global = quickWindow->mapToGlobal(
                                    position.toPoint());
                            QWheelEvent event(position,
                                              QPointF(global),
                                              {},
                                              QPoint(0, delta),
                                              Qt::NoButton,
                                              Qt::NoModifier,
                                              Qt::ScrollUpdate,
                                              false);
                            QCoreApplication::sendEvent(quickWindow, &event);
                            QEventLoop settle;
                            QTimer::singleShot(
                                    60, &settle, &QEventLoop::quit);
                            settle.exec();
                            return event.isAccepted();
                        };
                        if (wheelScrollingValid) {
                            const double comboContentHeight =
                                    flickable->property("contentHeight")
                                            .toDouble();
                            const double comboMaximum = std::max(
                                    0.0,
                                    comboContentHeight -
                                            scrollItem->height());
                            const QPointF panelTopLeft =
                                    redirectorItem->mapToScene(QPointF());
                            const QPointF comboBefore =
                                    comboItem->mapToScene(
                                            QPointF(
                                                    comboItem->width() * 0.5,
                                                    comboItem->height() * 0.5));
                            flickable->setProperty(
                                    "contentY",
                                    std::clamp(
                                            comboBefore.y() -
                                                    panelTopLeft.y() -
                                                    redirectorItem->height() *
                                                            0.5,
                                            0.0,
                                            comboMaximum));
                            QCoreApplication::processEvents();
                            const double beforeCombo =
                                    flickable->property("contentY").toDouble();
                            const bool comboScrollsDown =
                                    beforeCombo < comboMaximum - 1.0;
                            const bool comboAccepted = sendWheel(
                                    comboItem,
                                    QPointF(comboItem->width() * 0.5,
                                            comboItem->height() * 0.5),
                                    comboScrollsDown ? -120 : 120);
                            const double afterCombo =
                                    flickable->property("contentY").toDouble();
                            globalComboWheelValid =
                                    comboAccepted &&
                                    (comboScrollsDown
                                     ? afterCombo > beforeCombo
                                     : afterCombo < beforeCombo);
                            wheelScrollingValid &=
                                    globalComboWheelValid;

                            controller.setModifierPassId(
                                    0,
                                    QStringLiteral(
                                            "existing-event-perturbation"));
                            QCoreApplication::processEvents();
                            QCoreApplication::processEvents();
                            QObject *const perturbationSettings =
                                    modifierComposition
                                            ->property("firstPassSettingsItem")
                                            .value<QObject *>();
                            auto *const absoluteMinimumSlider =
                                    perturbationSettings == nullptr
                                    ? nullptr
                                    : qobject_cast<QQuickItem *>(
                                              perturbationSettings
                                                      ->findChild<QObject *>(
                                                              QStringLiteral(
                                                                      "perturbationAbsoluteMinimumSlider")));
                            QObject *const perturbationMinimumField =
                                    perturbationSettings == nullptr
                                    ? nullptr
                                    : perturbationSettings
                                              ->findChild<QObject *>(
                                                      QStringLiteral(
                                                              "perturbationAbsoluteMinimumSliderValueField"));
                            QObject *const perturbationMaximumField =
                                    perturbationSettings == nullptr
                                    ? nullptr
                                    : perturbationSettings
                                              ->findChild<QObject *>(
                                                      QStringLiteral(
                                                              "perturbationAbsoluteMaximumSliderValueField"));
                            perturbationSliderEditorsValid =
                                    perturbationMinimumField != nullptr &&
                                    perturbationMaximumField != nullptr &&
                                    perturbationMinimumField
                                            ->property("exactValueEditor")
                                            .toBool() &&
                                    perturbationMaximumField
                                            ->property("exactValueEditor")
                                            .toBool();
                            wheelScrollingValid &=
                                    absoluteMinimumSlider != nullptr;
                            if (wheelScrollingValid) {
                                const double sliderContentHeight =
                                        flickable->property("contentHeight")
                                                .toDouble();
                                const double sliderMaximum = std::max(
                                        0.0,
                                        sliderContentHeight -
                                                scrollItem->height());
                                const QPointF panelTopLeft =
                                        redirectorItem->mapToScene(QPointF());
                                const QPointF sliderBefore =
                                        absoluteMinimumSlider->mapToScene(
                                                QPointF(
                                                        absoluteMinimumSlider
                                                                        ->width() *
                                                                0.5,
                                                        absoluteMinimumSlider
                                                                        ->height() *
                                                                0.5));
                                const double desiredSceneY =
                                        panelTopLeft.y() +
                                        redirectorItem->height() * 0.5;
                                const double currentY =
                                        flickable->property("contentY")
                                                .toDouble();
                                flickable->setProperty(
                                        "contentY",
                                        std::clamp(
                                                currentY + sliderBefore.y() -
                                                        desiredSceneY,
                                                0.0,
                                                sliderMaximum));
                                QCoreApplication::processEvents();
                                const double beforeSlider =
                                        flickable->property("contentY")
                                                .toDouble();
                                const bool sliderScrollsDown =
                                        beforeSlider <
                                        sliderMaximum - 1.0;
                                const bool sliderAccepted = sendWheel(
                                        absoluteMinimumSlider,
                                        QPointF(
                                                absoluteMinimumSlider->width() *
                                                        0.5,
                                                absoluteMinimumSlider->height() *
                                                        0.5),
                                        sliderScrollsDown ? -120 : 120);
                                const double afterSlider =
                                        flickable->property("contentY")
                                                .toDouble();
                                globalSliderWheelValid =
                                        sliderAccepted &&
                                        (sliderScrollsDown
                                         ? afterSlider > beforeSlider
                                         : afterSlider < beforeSlider);
                                wheelScrollingValid &=
                                        globalSliderWheelValid;
                            }
                            controller.setModifierPassId(
                                    0, QStringLiteral("random-steering"));
                            QCoreApplication::processEvents();
                            QCoreApplication::processEvents();

                            const auto positionOuterForItem =
                                    [&](QQuickItem *item) {
                                const QPointF localPoint(
                                        item->width() * 0.5,
                                        std::min(
                                                10.0,
                                                item->height() * 0.5));
                                const double contentHeight =
                                        flickable
                                                ->property("contentHeight")
                                                .toDouble();
                                const double maximum = std::max(
                                        0.0,
                                        contentHeight - scrollItem->height());
                                const QPointF panelCenter =
                                        redirectorItem->mapToScene(
                                                QPointF(
                                                        redirectorItem->width() *
                                                                0.5,
                                                        redirectorItem->height() *
                                                                0.5));
                                const QPointF itemCenter =
                                        item->mapToScene(
                                                localPoint);
                                const double current =
                                        flickable->property("contentY")
                                                .toDouble();
                                flickable->setProperty(
                                        "contentY",
                                        std::clamp(
                                                current + itemCenter.y() -
                                                        panelCenter.y(),
                                                0.0,
                                                maximum));
                                QCoreApplication::processEvents();
                            };
                            const auto nestedWheelMoves =
                                    [&](QQuickItem *item,
                                        QObject *nested) {
                                positionOuterForItem(item);
                                auto *const nestedItem =
                                        qobject_cast<QQuickItem *>(nested);
                                if (nestedItem == nullptr) {
                                    return false;
                                }
                                const double nestedMaximum = std::max(
                                        0.0,
                                        nested->property("contentHeight")
                                                        .toDouble() -
                                                nestedItem->height());
                                nested->setProperty("contentY", 0.0);
                                QCoreApplication::processEvents();
                                const double beforeOuter =
                                        flickable->property("contentY")
                                                .toDouble();
                                const bool accepted = sendWheel(
                                        item,
                                        QPointF(
                                                item->width() * 0.5,
                                                std::min(
                                                        10.0,
                                                        item->height() * 0.5)),
                                        -120);
                                const double afterOuter =
                                        flickable->property("contentY")
                                                .toDouble();
                                const double afterNested =
                                        nested->property("contentY")
                                                .toDouble();
                                const bool moved =
                                        nestedMaximum > 1.0 && accepted &&
                                        std::abs(
                                                afterOuter - beforeOuter) <
                                                0.1 &&
                                        afterNested >= std::min(
                                                nestedMaximum, 119.0);
                                if (!moved) {
                                    std::cerr
                                            << "nested wheel "
                                            << item->objectName().toStdString()
                                            << ": class="
                                            << nested->metaObject()->className()
                                            << ", max=" << nestedMaximum
                                            << ", accepted=" << accepted
                                            << ", outer=" << beforeOuter
                                            << "->" << afterOuter
                                            << ", inner=0->" << afterNested
                                            << ", scene="
                                            << item
                                                       ->mapToScene(QPointF(
                                                               item->width() *
                                                                       0.5,
                                                               item->height() *
                                                                       0.5))
                                                       .x()
                                            << ","
                                            << item
                                                       ->mapToScene(QPointF(
                                                               item->width() *
                                                                       0.5,
                                                               item->height() *
                                                                       0.5))
                                                       .y()
                                            << '\n';
                                }
                                return moved;
                            };

                            QStringList longInputLines;
                            for (int line = 0; line < 80; ++line) {
                                longInputLines.push_back(
                                        QStringLiteral("%1 steer 0")
                                                .arg(
                                                        line,
                                                        2,
                                                        10,
                                                        QLatin1Char('0')));
                            }
                            const QString originalBaseInput =
                                    controller.baseInputScript();
                            const QString originalBestInputs =
                                    bestInputsTextArea
                                            ->property("text")
                                            .toString();
                            const QString longInput =
                                    longInputLines.join(QLatin1Char('\n'));
                            controller.setBaseInputScript(longInput);
                            bestInputsTextArea->setProperty("text", longInput);
                            QCoreApplication::processEvents();
                            QCoreApplication::processEvents();
                            baseInputWheelValid = nestedWheelMoves(
                                    baseInputItem, baseInputFlickable);
                            positionOuterForItem(bestInputsItem);
                            bestInputsFlickable->setProperty(
                                    "contentY", 0.0);
                            QCoreApplication::processEvents();
                            const double bestOuterBefore =
                                    flickable->property("contentY")
                                            .toDouble();
                            const bool bestAccepted = sendWheel(
                                    bestInputsItem,
                                    QPointF(
                                            bestInputsItem->width() * 0.5,
                                            std::min(
                                                    10.0,
                                                    bestInputsItem->height() *
                                                            0.5)),
                                    -120);
                            const double bestOuterAfter =
                                    flickable->property("contentY")
                                            .toDouble();
                            bestInputsWheelValid =
                                    bestAccepted &&
                                    std::abs(
                                            bestOuterAfter -
                                            bestOuterBefore) < 0.1;
                            wheelScrollingValid &=
                                    baseInputWheelValid &&
                                    bestInputsWheelValid;
                            controller.setBaseInputScript(originalBaseInput);
                            bestInputsTextArea->setProperty(
                                    "text", originalBestInputs);
                            QCoreApplication::processEvents();

                            simulationDebuggerPanelHost->setVisible(true);
                            simulationDebuggerPanel->setVisible(true);
                            bruteforceTabContent->setVisible(false);
                            QCoreApplication::processEvents();
                            QCoreApplication::processEvents();
                            sourceTreeWheelValid = nestedWheelMoves(
                                    sourceTreeItem, simulationSourceTree);
                            codeViewerWheelValid = nestedWheelMoves(
                                    codeViewerItem, simulationCodeViewer);
                            wheelScrollingValid &=
                                    settingsWheelRedirector
                                            ->property("enabled")
                                            .toBool() &&
                                    sourceTreeWheelValid &&
                                    codeViewerWheelValid;
                            simulationDebuggerPanel->setVisible(false);
                            simulationDebuggerPanelHost->setVisible(false);
                            bruteforceTabContent->setVisible(true);
                            QCoreApplication::processEvents();
                        }
                    }
                    bool everyOwnedPanelLoaded =
                            evaluationTargetSelector != nullptr &&
                            modifierComposition != nullptr;
                    const std::array<std::pair<const char *, const char *>, 7>
                            evaluationPanels{{
                                    {"velocity",
                                     "velocityEvaluationSettings"},
                                    {"stunt-points",
                                     "stuntPointsEvaluationSettings"},
                                    {"precise-finish-time",
                                     "preciseFinishTimeEvaluationSettings"},
                                    {"volume-entry-time",
                                     "volumeEntryEvaluationSettings"},
                                    {"custom-volume-entry-time",
                                     "volumeEntryEvaluationSettings"},
                                    {"point-target",
                                     "pointTargetEvaluationSettings"},
                                    {"pose-target",
                                     "poseTargetEvaluationSettings"}}};
                    for (const auto &[id, objectName] : evaluationPanels) {
                        controller.setEvaluationTargetId(
                                QString::fromLatin1(id));
                        QCoreApplication::processEvents();
                        everyOwnedPanelLoaded &=
                                evaluationTargetSelector != nullptr &&
                                evaluationTargetSelector
                                                ->property("settingsLoaded")
                                                .toBool() &&
                                evaluationTargetSelector
                                                ->property(
                                                        "settingsObjectName")
                                                .toString() ==
                                        QString::fromLatin1(objectName);
                    }
                    controller.setEvaluationTargetId(
                            QStringLiteral("pose-target"));
                    QCoreApplication::processEvents();
                    const qreal expandedEvaluationHeight =
                            evaluationSection == nullptr
                            ? 0.0
                            : evaluationSection->height();
                    const qreal expandedSelectorHeight =
                            evaluationTargetSelector == nullptr
                            ? 0.0
                            : evaluationTargetSelector
                                      ->property("height")
                                      .toReal();
                    const QPointer<QObject> expandedSettingsItem =
                            evaluationTargetSelector == nullptr
                            ? nullptr
                            : evaluationTargetSelector
                                      ->property("settingsItem")
                                      .value<QObject *>();
                    const QString expandedSettingsObjectName =
                            expandedSettingsItem == nullptr
                            ? QString()
                            : expandedSettingsItem->objectName();
                    controller.setEvaluationTargetId(
                            QStringLiteral("precise-finish-time"));
                    QCoreApplication::processEvents();
                    const qreal compactEvaluationHeight =
                            evaluationSection == nullptr
                            ? 0.0
                            : evaluationSection->height();
                    const qreal compactSelectorHeight =
                            evaluationTargetSelector == nullptr
                            ? 0.0
                            : evaluationTargetSelector
                                      ->property("height")
                                      .toReal();
                    QObject *const compactSettingsItem =
                            evaluationTargetSelector == nullptr
                            ? nullptr
                            : evaluationTargetSelector
                                      ->property("settingsItem")
                                      .value<QObject *>();
                    const bool targetLayoutUpdatesImmediately =
                            expandedSettingsItem != nullptr &&
                            expandedSettingsObjectName ==
                                    QStringLiteral(
                                            "poseTargetEvaluationSettings") &&
                            compactSettingsItem != nullptr &&
                            compactSettingsItem != expandedSettingsItem.data() &&
                            compactSettingsItem->objectName() ==
                                    QStringLiteral(
                                            "preciseFinishTimeEvaluationSettings") &&
                            expandedEvaluationHeight >
                                    compactEvaluationHeight + 250.0 &&
                            expandedSelectorHeight >
                                    compactSelectorHeight + 250.0 &&
                            compactSelectorHeight < 90.0;
                    controller.setEvaluationTargetId(
                            QStringLiteral("stunt-points"));
                    QCoreApplication::processEvents();
                    QObject *const stuntPointsTimeField =
                            root->findChild<QObject *>(
                                    QStringLiteral("stuntPointsTimeField"));
                    const bool stuntPointsFieldValid =
                            stuntPointsTimeField != nullptr &&
                            stuntPointsTimeField->property("text").toString() ==
                                    QStringLiteral("6000") &&
                            stuntPointsTimeField->property("minimum").toReal() ==
                                    0.0;
                    const std::array<std::pair<const char *, const char *>, 5>
                            modifierPanels{{
                                    {"random-steering",
                                     "randomSteeringMutationSettings"},
                                    {"existing-event-perturbation",
                                     "existingEventPerturbationSettings"},
                                    {"smooth-steering",
                                     "smoothSteeringSettings"},
                                    {"input-insertion",
                                     "inputInsertionSettings"},
                                    {"input-deletion",
                                     "inputDeletionSettings"}}};
                    for (const auto &[id, objectName] : modifierPanels) {
                        controller.setModifierPassId(
                                0, QString::fromLatin1(id));
                        QCoreApplication::processEvents();
                        everyOwnedPanelLoaded &=
                                modifierComposition != nullptr &&
                                modifierComposition
                                                ->property(
                                                        "firstPassSettingsLoaded")
                                                .toBool() &&
                                modifierComposition
                                                ->property(
                                                        "firstPassSettingsObjectName")
                                                .toString() ==
                                        QString::fromLatin1(objectName);
                    }

                    const auto activateCombo = [](QObject *combo, int index) {
                        return combo != nullptr && QMetaObject::invokeMethod(
                                combo,
                                "activated",
                                Qt::DirectConnection,
                                Q_ARG(int, index));
                    };
                    QObject *const firstPassForCombo =
                            modifierComposition
                                    ->property("firstRenderedPass")
                                    .value<QObject *>();
                    QObject *const modifierPassCombo =
                            firstPassForCombo == nullptr
                            ? nullptr
                            : firstPassForCombo->findChild<QObject *>(
                                      QStringLiteral("modifierPassCombo0"));

                    bool dropdownStateUpdates =
                            activateCombo(modifierPassCombo, 2);
                    QCoreApplication::processEvents();
                    QCoreApplication::processEvents();
                    dropdownStateUpdates &=
                            controller.modifierPasses()
                                            .front()
                                            .toMap()
                                            .value(QStringLiteral("id"))
                                            .toString() ==
                                    QStringLiteral("smooth-steering") &&
                            modifierComposition
                                            ->property(
                                                    "firstPassSettingsObjectName")
                                            .toString() ==
                                    QStringLiteral("smoothSteeringSettings");

                    dropdownStateUpdates &=
                            activateCombo(modifierPassCombo, 3);
                    QCoreApplication::processEvents();
                    QCoreApplication::processEvents();
                    QObject *const insertionSettings =
                            modifierComposition
                                    ->property("firstPassSettingsItem")
                                    .value<QObject *>();
                    QObject *const insertionModeCombo =
                            insertionSettings == nullptr
                            ? nullptr
                            : insertionSettings->findChild<QObject *>(
                                      QStringLiteral(
                                              "insertionSteeringModeCombo"));
                    QObject *const insertionMinimumSlider =
                            insertionSettings == nullptr
                            ? nullptr
                            : insertionSettings->findChild<QObject *>(
                                      QStringLiteral(
                                              "insertionAbsoluteMinimumSlider"));
                    QObject *const insertionMaximumSlider =
                            insertionSettings == nullptr
                            ? nullptr
                            : insertionSettings->findChild<QObject *>(
                                      QStringLiteral(
                                              "insertionAbsoluteMaximumSlider"));
                    QObject *const insertionMinimumField =
                            insertionSettings == nullptr
                            ? nullptr
                            : insertionSettings->findChild<QObject *>(
                                      QStringLiteral(
                                              "insertionAbsoluteMinimumSliderValueField"));
                    QObject *const insertionMaximumField =
                            insertionSettings == nullptr
                            ? nullptr
                            : insertionSettings->findChild<QObject *>(
                                      QStringLiteral(
                                              "insertionAbsoluteMaximumSliderValueField"));
                    dropdownStateUpdates &=
                            activateCombo(insertionModeCombo, 1);
                    QCoreApplication::processEvents();
                    const QVariantMap insertionPass =
                            controller.modifierPasses().front().toMap();
                    dropdownStateUpdates &=
                            insertionPass.value(QStringLiteral("settings"))
                                            .toMap()
                                            .value(QStringLiteral("steerMode"))
                                            .toString() ==
                                    QStringLiteral("absolute");

                    bool insertionSlidersValid =
                            insertionMinimumSlider != nullptr &&
                            insertionMaximumSlider != nullptr &&
                            insertionMinimumField != nullptr &&
                            insertionMaximumField != nullptr &&
                            insertionMinimumField
                                    ->property("exactValueEditor").toBool() &&
                            insertionMaximumField
                                    ->property("exactValueEditor").toBool() &&
                            insertionMinimumSlider->property("from").toReal() ==
                                    -1.0 &&
                            insertionMinimumSlider->property("to").toReal() ==
                                    1.0 &&
                            insertionMaximumSlider->property("from").toReal() ==
                                    -1.0 &&
                            insertionMaximumSlider->property("to").toReal() ==
                                    1.0;
                    insertionSlidersValid &=
                            InvokeSliderValueCommit(
                                    insertionMinimumField,
                                    QStringLiteral("-0.375"),
                                    true) &&
                            controller.modifierPasses()
                                            .front()
                                            .toMap()
                                            .value(QStringLiteral("settings"))
                                            .toMap()
                                            .value(
                                                    QStringLiteral(
                                                            "steerAbsoluteMin"))
                                            .toString() ==
                                    QStringLiteral("-0.375") &&
                            std::abs(
                                    insertionMinimumSlider
                                                    ->property("value")
                                                    .toReal() +
                                    0.375) < 0.000001 &&
                            InvokeSliderValueCommit(
                                    insertionMinimumField,
                                    QStringLiteral("-1.01"),
                                    false) &&
                            insertionMinimumField
                                    ->property("validationFailed").toBool() &&
                            !insertionMinimumField
                                     ->property("inputValid").toBool() &&
                            insertionMinimumField
                                            ->property("effectiveBorderColor")
                                            .value<QColor>() ==
                                    QColor(QStringLiteral("#a23434")) &&
                            controller.modifierPasses()
                                            .front()
                                            .toMap()
                                            .value(QStringLiteral("settings"))
                                            .toMap()
                                            .value(
                                                    QStringLiteral(
                                                            "steerAbsoluteMin"))
                                            .toString() ==
                                    QStringLiteral("-0.375") &&
                            InvokeSliderValueCommit(
                                    insertionMinimumField,
                                    QStringLiteral("not-a-number"),
                                    false);
                    controller.setModifierPassSetting(
                            0,
                            QStringLiteral("steerAbsoluteMin"),
                            QStringLiteral("-0.625"));
                    QCoreApplication::processEvents();
                    insertionSlidersValid &=
                            insertionMinimumField != nullptr &&
                            insertionMinimumField->property("text").toString() ==
                                    QStringLiteral("-0.625") &&
                            !insertionMinimumField
                                     ->property("validationFailed").toBool() &&
                            std::abs(
                                    insertionMinimumSlider
                                                    ->property("value")
                                                    .toReal() +
                                    0.625) < 0.000001 &&
                            InvokeSliderValueCommit(
                                    insertionMinimumField,
                                    QStringLiteral("-1"),
                                    true);

                    dropdownStateUpdates &=
                            activateCombo(evaluationTargetCombo, 5);
                    QCoreApplication::processEvents();
                    QCoreApplication::processEvents();
                    dropdownStateUpdates &=
                            controller.evaluationTargetId() ==
                                    QStringLiteral("point-target") &&
                            evaluationTargetSelector
                                            ->property("settingsObjectName")
                                            .toString() ==
                                    QStringLiteral(
                                            "pointTargetEvaluationSettings");

                    dropdownStateUpdates &=
                            activateCombo(evaluationTargetCombo, 6);
                    QCoreApplication::processEvents();
                    QCoreApplication::processEvents();
                    QObject *const poseEditor =
                            evaluationTargetSelector
                                    ->property("settingsItem")
                                    .value<QObject *>();
                    QObject *const rotationWeightSlider =
                            poseEditor == nullptr ? nullptr
                            : poseEditor->findChild<QObject *>(
                                      QStringLiteral("rotationWeightSlider"));
                    QObject *const rotationWeightField =
                            poseEditor == nullptr ? nullptr
                            : poseEditor->findChild<QObject *>(
                                      QStringLiteral(
                                              "rotationWeightSliderValueField"));
                    QObject *const poseSelector =
                            poseEditor == nullptr ? nullptr
                            : poseEditor->findChild<QObject *>(
                                      QStringLiteral("poseTargetSelector"));
                    QObject *const poseNameField =
                            poseEditor == nullptr ? nullptr
                            : poseEditor->findChild<QObject *>(
                                      QStringLiteral("poseTargetNameField"));
                    QObject *const posePositionSettings =
                            poseEditor == nullptr ? nullptr
                            : poseEditor->findChild<QObject *>(
                                      QStringLiteral(
                                              "poseTargetPositionSettings"));
                    QObject *const poseRotationSettings =
                            poseEditor == nullptr ? nullptr
                            : poseEditor->findChild<QObject *>(
                                      QStringLiteral(
                                              "poseTargetRotationSettings"));
                    QObject *const movePoseToCameraButton =
                            poseEditor == nullptr ? nullptr
                            : poseEditor->findChild<QObject *>(
                                      QStringLiteral(
                                              "movePoseTargetToCameraButton"));
                    QObject *const movePoseToCarButton =
                            poseEditor == nullptr ? nullptr
                            : poseEditor->findChild<QObject *>(
                                      QStringLiteral(
                                              "movePoseTargetToCarButton"));
                    const int placedPoseIndex =
                            controller.poseTargets()->addTarget(
                                    7.0,
                                    3.0,
                                    -4.0,
                                    QQuaternion::fromEulerAngles(
                                            10.0F, 20.0F, 30.0F));
                    QCoreApplication::processEvents();
                    const int poseModels =
                            root->findChildren<QObject *>(
                                        QStringLiteral(
                                                "poseTargetCarModel"))
                                    .size();
                    const double initialPoseX =
                            controller.poseTargets()
                                    ->selectedTarget()
                                    .value(QStringLiteral("x"))
                                    .toDouble();
                    const double initialPoseYaw =
                            controller.poseTargets()
                                    ->selectedTarget()
                                    .value(QStringLiteral("yawDegrees"))
                                    .toDouble();
                    QVariant beganPoseMove;
                    QVariant beganPoseRotation;
                    bool poseSliderValid =
                            rotationWeightSlider != nullptr &&
                            rotationWeightField != nullptr &&
                            rotationWeightField
                                    ->property("exactValueEditor").toBool() &&
                            rotationWeightSlider->property("from").toReal() ==
                                    0.0 &&
                            rotationWeightSlider->property("to").toReal() ==
                                    100.0 &&
                            poseEditor != nullptr &&
                            poseSelector != nullptr &&
                            poseNameField != nullptr &&
                            posePositionSettings != nullptr &&
                            poseRotationSettings != nullptr &&
                            movePoseToCameraButton != nullptr &&
                            movePoseToCameraButton
                                    ->property("enabled").toBool() &&
                            movePoseToCarButton != nullptr &&
                            movePoseToCarButton
                                    ->property("enabled").toBool() &&
                            placedPoseIndex == 1 &&
                            poseSelector->property("count").toInt() == 2 &&
                            poseSelector->property("currentIndex").toInt() ==
                                    1 &&
                            poseModels >= 2 &&
                            QMetaObject::invokeMethod(
                                    viewport,
                                    "beginPoseInteraction",
                                    Q_RETURN_ARG(
                                            QVariant, beganPoseMove),
                                    Q_ARG(
                                            QVariant,
                                            QVariant(
                                                    QStringLiteral(
                                                            "pose-move"))),
                                    Q_ARG(
                                            QVariant,
                                            QVariant(
                                                    QStringLiteral("x"))),
                                    Q_ARG(QVariant, QVariant(100.0)),
                                    Q_ARG(QVariant, QVariant(100.0))) &&
                            beganPoseMove.toBool() &&
                            QMetaObject::invokeMethod(
                                    viewport,
                                    "updatePoseInteraction",
                                    Q_ARG(QVariant, QVariant(140.0)),
                                    Q_ARG(QVariant, QVariant(100.0))) &&
                            QMetaObject::invokeMethod(
                                    viewport, "endPoseInteraction") &&
                            QMetaObject::invokeMethod(
                                    viewport,
                                    "beginPoseInteraction",
                                    Q_RETURN_ARG(
                                            QVariant,
                                            beganPoseRotation),
                                    Q_ARG(
                                            QVariant,
                                            QVariant(
                                                    QStringLiteral(
                                                            "pose-rotate"))),
                                    Q_ARG(
                                            QVariant,
                                            QVariant(
                                                    QStringLiteral("yaw"))),
                                    Q_ARG(QVariant, QVariant(100.0)),
                                    Q_ARG(QVariant, QVariant(100.0))) &&
                            beganPoseRotation.toBool() &&
                            QMetaObject::invokeMethod(
                                    viewport,
                                    "updatePoseInteraction",
                                    Q_ARG(QVariant, QVariant(140.0)),
                                    Q_ARG(QVariant, QVariant(100.0))) &&
                            QMetaObject::invokeMethod(
                                    viewport, "endPoseInteraction");
                    controller.focusSelectedPoseTarget();
                    QCoreApplication::processEvents();
                    poseSliderValid &=
                            controller.poseTargets()
                                            ->selectedTarget()
                                            .value(QStringLiteral("x"))
                                            .toDouble() > initialPoseX &&
                            controller.poseTargets()
                                            ->selectedTarget()
                                            .value(
                                                    QStringLiteral(
                                                            "yawDegrees"))
                                            .toDouble() > initialPoseYaw &&
                            controller.evaluationTargetSettings()
                                            .value(QStringLiteral("x"))
                                            .toDouble() > initialPoseX &&
                            viewport->property("cuboidFocused").toBool() &&
                            viewport->property("cameraTarget")
                                            .value<QVector3D>() ==
                                    controller.poseTargets()
                                            ->selectedTarget()
                                            .value(
                                                    QStringLiteral(
                                                            "position"))
                                            .value<QVector3D>() &&
                            root->findChildren<QObject *>(
                                        QStringLiteral(
                                                "poseTargetMoveHandle"))
                                            .size() >= 6 &&
                            root->findChildren<QObject *>(
                                        QStringLiteral(
                                                "poseTargetRotationHandle"))
                                            .size() >= 6;
                    poseSliderValid &=
                            InvokeSliderValueCommit(
                                    rotationWeightField,
                                    QStringLiteral("37.5"),
                                    true) &&
                            controller.evaluationTargetSettings()
                                            .value(
                                                    QStringLiteral(
                                                            "rotationWeightPercent"))
                                            .toString() ==
                                    QStringLiteral("37.5") &&
                            std::abs(
                                    rotationWeightSlider
                                                    ->property("value")
                                                    .toReal() -
                                    37.5) < 0.000001 &&
                            InvokeSliderValueCommit(
                                    rotationWeightField,
                                    QStringLiteral("50"),
                                    true);
                    const QVector3D cameraPlacement = viewport
                            ->property("sceneCameraPosition")
                            .value<QVector3D>();
                    const QQuaternion cameraPlacementRotation = viewport
                            ->property("sceneCameraRotation")
                            .value<QQuaternion>();
                    poseSliderValid &= QMetaObject::invokeMethod(
                            movePoseToCameraButton, "clicked");
                    QCoreApplication::processEvents();
                    poseSliderValid &=
                            controller.poseTargets()
                                    ->selectedTarget()
                                    .value(QStringLiteral("position"))
                                    .value<QVector3D>() == cameraPlacement &&
                            std::abs(QQuaternion::dotProduct(
                                    controller.poseTargets()
                                            ->selectedTarget()
                                            .value(QStringLiteral("rotation"))
                                            .value<QQuaternion>(),
                                    cameraPlacementRotation)) > 0.99999F &&
                            QMetaObject::invokeMethod(
                                    movePoseToCarButton, "clicked");
                    QCoreApplication::processEvents();
                    poseSliderValid &=
                            controller.poseTargets()
                                    ->selectedTarget()
                                    .value(QStringLiteral("position"))
                                    .value<QVector3D>() ==
                                    viewer.carPosition() &&
                            std::abs(QQuaternion::dotProduct(
                                    controller.poseTargets()
                                            ->selectedTarget()
                                            .value(QStringLiteral("rotation"))
                                            .value<QQuaternion>(),
                                    viewer.carRotation())) > 0.99999F;
                    if (!poseSliderValid) {
                        std::cerr
                                << "pose slider editor: field="
                                << (rotationWeightField != nullptr
                                            ? rotationWeightField
                                                      ->property("text")
                                                      .toString()
                                                      .toStdString()
                                            : "<missing>")
                                << ", stored="
                                << controller.evaluationTargetSettings()
                                           .value(
                                                   QStringLiteral(
                                                           "rotationWeightPercent"))
                                           .toString()
                                           .toStdString()
                                << ", slider="
                                << (rotationWeightSlider != nullptr
                                            ? rotationWeightSlider
                                                      ->property("value")
                                                      .toReal()
                                            : -999.0)
                                << '\n';
                    }

                    dropdownStateUpdates &=
                            activateCombo(evaluationTargetCombo, 3);
                    QCoreApplication::processEvents();
                    QCoreApplication::processEvents();
                    QObject *const cuboidEditor =
                            evaluationTargetSelector
                                    ->property("settingsItem")
                                    .value<QObject *>();
                    QObject *const cuboidSelector =
                            cuboidEditor == nullptr ? nullptr
                            : cuboidEditor->findChild<QObject *>(
                                    QStringLiteral("shapeTargetSelector"));
                    QObject *const addCuboidButton =
                            cuboidEditor == nullptr ? nullptr
                            : cuboidEditor->findChild<QObject *>(
                                    QStringLiteral("addShapeTargetButton"));
                    QObject *const addShapeMenu =
                            cuboidEditor == nullptr ? nullptr
                            : cuboidEditor->findChild<QObject *>(
                                    QStringLiteral("addShapeTargetMenu"));
                    QObject *const duplicateCuboidButton =
                            cuboidEditor == nullptr ? nullptr
                            : cuboidEditor->findChild<QObject *>(
                                    QStringLiteral(
                                            "duplicateShapeTargetButton"));
                    QObject *const focusCuboidButton =
                            cuboidEditor == nullptr ? nullptr
                            : cuboidEditor->findChild<QObject *>(
                                    QStringLiteral(
                                            "focusShapeTargetButton"));
                    QObject *const removeCuboidButton =
                            cuboidEditor == nullptr ? nullptr
                            : cuboidEditor->findChild<QObject *>(
                                    QStringLiteral(
                                            "removeShapeTargetButton"));
                    QObject *const cuboidNameField =
                            cuboidEditor == nullptr ? nullptr
                            : cuboidEditor->findChild<QObject *>(
                                    QStringLiteral("shapeTargetNameField"));
                    QObject *const moveCuboidToCameraButton =
                            cuboidEditor == nullptr ? nullptr
                            : cuboidEditor->findChild<QObject *>(
                                      QStringLiteral(
                                              "moveShapeTargetToCameraButton"));
                    QObject *const moveCuboidToCarButton =
                            cuboidEditor == nullptr ? nullptr
                            : cuboidEditor->findChild<QObject *>(
                                      QStringLiteral(
                                              "moveShapeTargetToCarButton"));
                    const int placedIndex =
                            controller.cuboidTargets()->addTarget(
                                    14.0, 3.0, -2.0);
                    QCoreApplication::processEvents();
                    const int initialCuboidModels =
                            root->findChildren<QObject *>(
                                        QStringLiteral("cuboidTargetModel"))
                                    .size();
                    const QList<QObject *> initialCuboidModelObjects =
                            root->findChildren<QObject *>(
                                    QStringLiteral("cuboidTargetModel"));
                    const double initialCuboidSize =
                            controller.cuboidTargets()
                                    ->selectedTarget()
                                    .value(QStringLiteral("sizeX"))
                                    .toDouble();
                    const bool addMenuOpened =
                            addCuboidButton != nullptr &&
                            QMetaObject::invokeMethod(
                                    addCuboidButton, "clicked");
                    QCoreApplication::processEvents();
                    auto *const addButtonItem =
                            qobject_cast<QQuickItem *>(addCuboidButton);
                    auto *const addMenuContent = addShapeMenu == nullptr
                            ? nullptr
                            : qobject_cast<QQuickItem *>(
                                      addShapeMenu->property("contentItem")
                                              .value<QObject *>());
                    const bool addMenuBelowButton =
                            addMenuOpened && addShapeMenu != nullptr &&
                            addButtonItem != nullptr &&
                            addMenuContent != nullptr &&
                            addShapeMenu->property("visible").toBool() &&
                            addMenuContent->mapToScene(QPointF()).y() + 0.5 >=
                                    addButtonItem
                                            ->mapToScene(QPointF(
                                                    0,
                                                    addButtonItem->height()))
                                            .y();
                    if (addShapeMenu != nullptr)
                        QMetaObject::invokeMethod(addShapeMenu, "close");
                    QVariant beganResize;
                    bool cuboidEditorValid =
                            cuboidEditor != nullptr &&
                            cuboidSelector != nullptr &&
                            addCuboidButton != nullptr &&
                            addShapeMenu != nullptr &&
                            addMenuBelowButton &&
                            duplicateCuboidButton != nullptr &&
                            focusCuboidButton != nullptr &&
                            removeCuboidButton != nullptr &&
                            cuboidNameField != nullptr &&
                            moveCuboidToCameraButton != nullptr &&
                            moveCuboidToCameraButton
                                    ->property("enabled").toBool() &&
                            moveCuboidToCarButton != nullptr &&
                            moveCuboidToCarButton
                                    ->property("enabled").toBool() &&
                            placedIndex == 1 &&
                            cuboidSelector->property("count").toInt() == 3 &&
                            cuboidSelector->property("currentIndex").toInt() ==
                                    1 &&
                            initialCuboidModels >= 4 &&
                            removeCuboidButton->property("enabled").toBool();
                    controller.focusSelectedCuboid();
                    QCoreApplication::processEvents();
                    QElapsedTimer cuboidStressTimer;
                    cuboidStressTimer.start();
                    for (int edit = 0; edit < 1000; ++edit) {
                        controller.cuboidTargets()->resizeSelected(
                                QStringLiteral("x"), 0.001);
                    }
                    QCoreApplication::processEvents();
                    const QList<QObject *> cuboidModelsAfterStress =
                            root->findChildren<QObject *>(
                                    QStringLiteral("cuboidTargetModel"));
                    cuboidEditorValid &=
                            viewport != nullptr &&
                            viewport->property("cuboidFocused").toBool() &&
                            viewport->property("cameraTarget")
                                            .value<QVector3D>() ==
                                    QVector3D(14.0F, 3.0F, -2.0F) &&
                            QMetaObject::invokeMethod(
                                    viewport,
                                    "beginCuboidInteraction",
                                    Q_RETURN_ARG(QVariant, beganResize),
                                    Q_ARG(QVariant,
                                          QVariant(QStringLiteral("resize"))),
                                    Q_ARG(QVariant,
                                          QVariant(QStringLiteral("x"))),
                                    Q_ARG(QVariant, QVariant(100.0)),
                                    Q_ARG(QVariant, QVariant(100.0))) &&
                            beganResize.toBool() &&
                            QMetaObject::invokeMethod(
                                    viewport,
                                    "updateCuboidInteraction",
                                    Q_ARG(QVariant, QVariant(140.0)),
                                    Q_ARG(QVariant, QVariant(100.0))) &&
                            QMetaObject::invokeMethod(
                                    viewport, "endCuboidInteraction");
                    QCoreApplication::processEvents();
                    cuboidEditorValid &=
                            controller.cuboidTargets()
                                            ->selectedTarget()
                                            .value(QStringLiteral("sizeX"))
                                            .toDouble() >
                                    initialCuboidSize &&
                            cuboidStressTimer.elapsed() < 250 &&
                            cuboidModelsAfterStress ==
                                    initialCuboidModelObjects &&
                            controller.evaluationTargetSettings()
                                            .value(QStringLiteral("sizeX"))
                                            .toDouble() >
                                    initialCuboidSize;
                    const QVector3D cuboidCameraPlacement = viewport
                            ->property("sceneCameraPosition")
                            .value<QVector3D>();
                    cuboidEditorValid &= QMetaObject::invokeMethod(
                            moveCuboidToCameraButton, "clicked");
                    QCoreApplication::processEvents();
                    cuboidEditorValid &=
                            controller.cuboidTargets()
                                    ->selectedTarget()
                                    .value(QStringLiteral("center"))
                                    .value<QVector3D>() ==
                                    cuboidCameraPlacement &&
                            QMetaObject::invokeMethod(
                                    moveCuboidToCarButton, "clicked");
                    QCoreApplication::processEvents();
                    cuboidEditorValid &=
                            controller.cuboidTargets()
                                    ->selectedTarget()
                                    .value(QStringLiteral("center"))
                                    .value<QVector3D>() == viewer.carPosition();
                    if (!cuboidEditorValid) {
                        std::cerr
                                << "cuboid editor checks: objects="
                                << (cuboidEditor != nullptr) << "/"
                                << (cuboidSelector != nullptr) << "/"
                                << (addCuboidButton != nullptr) << "/"
                                << (duplicateCuboidButton != nullptr) << "/"
                                << (focusCuboidButton != nullptr) << "/"
                                << (removeCuboidButton != nullptr) << "/"
                                << (cuboidNameField != nullptr)
                                << ", placed=" << placedIndex
                                << ", combo="
                                << (cuboidSelector == nullptr
                                            ? -1
                                            : cuboidSelector
                                                      ->property("count")
                                                      .toInt())
                                << "/"
                                << (cuboidSelector == nullptr
                                            ? -1
                                            : cuboidSelector
                                                      ->property("currentIndex")
                                                      .toInt())
                                << ", models=" << initialCuboidModels
                                << ", focus="
                                << (viewport != nullptr &&
                                    viewport->property("cuboidFocused").toBool())
                                << ", begin=" << beganResize.toBool()
                                << ", size="
                                << controller.cuboidTargets()
                                           ->selectedTarget()
                                           .value(QStringLiteral("sizeX"))
                                           .toDouble()
                                << "/" << initialCuboidSize << '\n';
                    }
                    dropdownStateUpdates &= cuboidEditorValid;

                    dropdownStateUpdates &=
                            activateCombo(evaluationTargetCombo, 4);
                    QCoreApplication::processEvents();
                    QCoreApplication::processEvents();
                    QObject *const customEditor =
                            evaluationTargetSelector
                                    ->property("settingsItem")
                                    .value<QObject *>();
                    QObject *const customPlaneSetting =
                            customEditor == nullptr ? nullptr
                            : customEditor->findChild<QObject *>(
                                      QStringLiteral(
                                              "customVolumePlaneSetting"));
                    QObject *const customDepthField =
                            customEditor == nullptr ? nullptr
                            : customEditor->findChild<QObject *>(
                                      QStringLiteral(
                                              "customVolumeDepthField"));
                    QObject *const drawCustomVolumeButton =
                            customEditor == nullptr ? nullptr
                            : customEditor->findChild<QObject *>(
                                      QStringLiteral(
                                              "drawCustomVolumeButton"));
                    QObject *const cancelCustomDrawingButton =
                            customEditor == nullptr ? nullptr
                            : customEditor->findChild<QObject *>(
                                      QStringLiteral(
                                              "cancelCustomVolumeDrawingButton"));
                    QObject *const moveCustomToCameraButton =
                            customEditor == nullptr ? nullptr
                            : customEditor->findChild<QObject *>(
                                      QStringLiteral(
                                              "moveShapeTargetToCameraButton"));
                    QObject *const moveCustomToCarButton =
                            customEditor == nullptr ? nullptr
                            : customEditor->findChild<QObject *>(
                                      QStringLiteral(
                                              "moveShapeTargetToCarButton"));
                    const int initialCustomModels =
                            root->findChildren<QObject *>(
                                        QStringLiteral(
                                                "customVolumeTargetModel"))
                                    .size();
                    controller.customVolumeTargets()->setDepth(
                            0, QStringLiteral("6.5"));
                    controller.beginCustomVolumeDrawing();
                    QVariant projectedPlanePoint;
                    QVariant secondProjectedPlanePoint;
                    bool customVolumeEditorValid =
                            customEditor != nullptr &&
                            customPlaneSetting != nullptr &&
                            customDepthField != nullptr &&
                            drawCustomVolumeButton != nullptr &&
                            cancelCustomDrawingButton != nullptr &&
                            moveCustomToCameraButton != nullptr &&
                            moveCustomToCarButton != nullptr &&
                            controller.customVolumeDrawing() &&
                            initialCustomModels >= 2 &&
                            QMetaObject::invokeMethod(
                                    viewport,
                                    "customPlanePoint",
                                    Q_RETURN_ARG(
                                            QVariant,
                                            projectedPlanePoint),
                                    Q_ARG(QVariant, QVariant(400.0)),
                                    Q_ARG(QVariant, QVariant(300.0))) &&
                            QMetaObject::invokeMethod(
                                    viewport,
                                    "customPlanePoint",
                                    Q_RETURN_ARG(
                                            QVariant,
                                            secondProjectedPlanePoint),
                                    Q_ARG(QVariant, QVariant(600.0)),
                                    Q_ARG(QVariant, QVariant(450.0))) &&
                            projectedPlanePoint.canConvert<QVector3D>() &&
                            secondProjectedPlanePoint.canConvert<QVector3D>() &&
                            projectedPlanePoint.value<QVector3D>()
                                            .distanceToPoint(
                                                    secondProjectedPlanePoint
                                                            .value<QVector3D>()) >
                                    0.01F &&
                            controller.customVolumeTargets()->addVertexWorld(
                                    -2.0, 0.0, -2.0) &&
                            controller.customVolumeTargets()->addVertexWorld(
                                    2.0, 0.0, -2.0) &&
                            controller.customVolumeTargets()->addVertexWorld(
                                    0.0, 0.0, 2.0);
                    controller.finishCustomVolumeDrawing();
                    controller.focusSelectedCustomVolume();
                    QCoreApplication::processEvents();
                    if (projectedPlanePoint.canConvert<QVector3D>() &&
                        secondProjectedPlanePoint.canConvert<QVector3D>() &&
                        projectedPlanePoint.value<QVector3D>()
                                        .distanceToPoint(
                                                secondProjectedPlanePoint
                                                        .value<QVector3D>()) <=
                                0.01F) {
                        const QVector3D first =
                                projectedPlanePoint.value<QVector3D>();
                        const QVector3D second =
                                secondProjectedPlanePoint.value<QVector3D>();
                        std::cerr
                                << "custom plane projection collapsed: "
                                << first.x() << "," << first.y() << ","
                                << first.z() << " / " << second.x() << ","
                                << second.y() << "," << second.z() << '\n';
                    }
                    customVolumeEditorValid &=
                            !controller.customVolumeDrawing() &&
                            controller.evaluationTargetSettings()
                                            .value(QStringLiteral("depth"))
                                            .toString() ==
                                    QStringLiteral("6.5") &&
                            controller.evaluationTargetSettings()
                                            .value(QStringLiteral("polygon"))
                                            .toString() ==
                                    QStringLiteral("-2,-2;2,-2;0,2") &&
                            viewport->property("cuboidFocused").toBool() &&
                            root->findChildren<QObject *>(
                                        QStringLiteral(
                                                "customVolumePlaneChoiceXZ"))
                                            .size() >= 2 &&
                            root->findChildren<QObject *>(
                                        QStringLiteral(
                                                "customVolumeDepthHandle"))
                                            .size() >= 2;
                    const QVector3D customCameraPlacement = viewport
                            ->property("sceneCameraPosition")
                            .value<QVector3D>();
                    customVolumeEditorValid &= QMetaObject::invokeMethod(
                            moveCustomToCameraButton, "clicked");
                    QCoreApplication::processEvents();
                    customVolumeEditorValid &=
                            controller.customVolumeTargets()
                                    ->selectedTarget()
                                    .value(QStringLiteral("origin"))
                                    .value<QVector3D>() ==
                                    customCameraPlacement &&
                            QMetaObject::invokeMethod(
                                    moveCustomToCarButton, "clicked");
                    QCoreApplication::processEvents();
                    customVolumeEditorValid &=
                            controller.customVolumeTargets()
                                    ->selectedTarget()
                                    .value(QStringLiteral("origin"))
                                    .value<QVector3D>() == viewer.carPosition();
                    dropdownStateUpdates &= customVolumeEditorValid;

                    dropdownStateUpdates &=
                            activateCombo(evaluationTargetCombo, 0);
                    dropdownStateUpdates &=
                            activateCombo(modifierPassCombo, 0);
                    QCoreApplication::processEvents();
                    QCoreApplication::processEvents();
                    QObject *const velocityEditor =
                            evaluationTargetSelector
                                    ->property("settingsItem")
                                    .value<QObject *>();
                    QObject *const minimumAlignmentSlider =
                            velocityEditor == nullptr ? nullptr
                            : velocityEditor->findChild<QObject *>(
                                      QStringLiteral(
                                              "minimumAlignmentSlider"));
                    QObject *const minimumAlignmentField =
                            velocityEditor == nullptr ? nullptr
                            : velocityEditor->findChild<QObject *>(
                                      QStringLiteral(
                                              "minimumAlignmentSliderValueField"));
                    bool velocitySliderValid =
                            minimumAlignmentSlider != nullptr &&
                            minimumAlignmentField != nullptr &&
                            minimumAlignmentField
                                    ->property("exactValueEditor").toBool() &&
                            minimumAlignmentSlider->property("from").toReal() ==
                                    -100.0 &&
                            minimumAlignmentSlider->property("to").toReal() ==
                                    100.0;
                    velocitySliderValid &=
                            InvokeSliderValueCommit(
                                    minimumAlignmentField,
                                    QStringLiteral("-12.5"),
                                    true) &&
                            controller.evaluationTargetSettings()
                                            .value(
                                                    QStringLiteral(
                                                            "minAlignmentPercent"))
                                            .toString() ==
                                    QStringLiteral("-12.5") &&
                            std::abs(
                                    minimumAlignmentSlider
                                                    ->property("value")
                                                    .toReal() +
                                    12.5) < 0.000001 &&
                            InvokeSliderValueCommit(
                                    minimumAlignmentField,
                                    QStringLiteral("-100"),
                                    true);
                    if (!velocitySliderValid) {
                        std::cerr
                                << "velocity slider editor: field="
                                << (minimumAlignmentField != nullptr
                                            ? minimumAlignmentField
                                                      ->property("text")
                                                      .toString()
                                                      .toStdString()
                                            : "<missing>")
                                << ", stored="
                                << controller.evaluationTargetSettings()
                                           .value(
                                                   QStringLiteral(
                                                           "minAlignmentPercent"))
                                           .toString()
                                           .toStdString()
                                << ", slider="
                                << (minimumAlignmentSlider != nullptr
                                            ? minimumAlignmentSlider
                                                      ->property("value")
                                                      .toReal()
                                            : -999.0)
                                << '\n';
                    }

                    controller.setModifierPassId(
                            0, QStringLiteral("random-steering"));
                    controller.setEvaluationTargetId(
                            QStringLiteral("velocity"));
                    QCoreApplication::processEvents();
                    QObject *const firstPassBefore =
                            modifierComposition
                                    ->property("firstRenderedPass")
                                    .value<QObject *>();
                    QObject *const firstPassSettings =
                            modifierComposition
                                    ->property("firstPassSettingsItem")
                                    .value<QObject *>();
                    QObject *const minimumTimeField = firstPassSettings == nullptr
                            ? nullptr
                            : firstPassSettings->findChild<QObject *>(
                                      QStringLiteral("minimumTimeField"));
                    const int rebuildCountBefore =
                            modifierComposition->property("rebuildCount").toInt();
                    const bool focusRequested = minimumTimeField != nullptr &&
                            QMetaObject::invokeMethod(
                                    minimumTimeField,
                                    "forceActiveFocus",
                                    Qt::DirectConnection);
                    QCoreApplication::processEvents();
                    const bool focusedBeforeUpdate =
                            minimumTimeField != nullptr &&
                            (minimumTimeField->property("activeFocus").toBool() ||
                             minimumTimeField->property("focus").toBool());
                    controller.setModifierPassSetting(
                            0,
                            QStringLiteral("minTimeMs"),
                            QStringLiteral("1010"));
                    QCoreApplication::processEvents();
                    QCoreApplication::processEvents();
                    QObject *const firstPassAfter =
                            modifierComposition
                                    ->property("firstRenderedPass")
                                    .value<QObject *>();
                    const QVariantMap updatedPass =
                            controller.modifierPasses().front().toMap();
                    const bool modifierFocusStable = focusRequested &&
                            focusedBeforeUpdate &&
                            firstPassBefore == firstPassAfter &&
                            modifierComposition->property("rebuildCount").toInt() ==
                                    rebuildCountBefore &&
                            minimumTimeField != nullptr &&
                            (minimumTimeField->property("activeFocus").toBool() ||
                             minimumTimeField->property("focus").toBool()) &&
                            updatedPass.value(QStringLiteral("settings"))
                                            .toMap()
                                            .value(QStringLiteral("minTimeMs"))
                                            .toString() ==
                                    QStringLiteral("1010");
                    const bool unboundedFieldsScrubbable =
                            minimumTimeField != nullptr &&
                            minimumTimeField->property("scrubbable").toBool();
                    controller.setModifierPassSetting(
                            0,
                            QStringLiteral("minTimeMs"),
                            QStringLiteral("1000"));
                    QCoreApplication::processEvents();
                    if (!algorithmSelectorsValid) {
                        const auto count = [](QObject *object) {
                            return object == nullptr
                                    ? -1
                                    : object->property("count").toInt();
                        };
                        const auto current = [](QObject *object) {
                            return object == nullptr
                                    ? QStringLiteral("<missing>")
                                    : object->property("currentValue")
                                              .toString();
                        };
                        std::cerr
                                << "algorithm structure failed: search="
                                << count(searchAlgorithmCombo) << "/"
                                << current(searchAlgorithmCombo).toStdString()
                                << ", modifierComposition="
                                << (modifierComposition != nullptr)
                                << "/"
                                << (modifierComposition == nullptr
                                            ? -1
                                            : modifierComposition
                                                      ->property("passCount")
                                                      .toInt())
                                << "/"
                                << (modifierComposition == nullptr
                                            ? -1
                                            : modifierComposition
                                                      ->property(
                                                              "passModelCount")
                                                      .toInt())
                                << "/"
                                << (modifierComposition == nullptr
                                            ? -1
                                            : modifierComposition
                                                      ->property(
                                                              "renderedPassCount")
                                                      .toInt())
                                << ", controllerPasses="
                                << controller.modifierPasses().size()
                                << ", firstPass="
                                << modifierComposition
                                           ->property("firstPassOptionCount")
                                           .toInt() << "/"
                                << modifierComposition
                                           ->property("firstPassSelectedId")
                                           .toString().toStdString()
                                << "/"
                                << modifierComposition
                                           ->property("firstPassSettingsLoaded")
                                           .toBool()
                                << ", addCombo=" << count(addModifierCombo)
                                << ", addButton="
                                << (addModifierButton != nullptr)
                                << ", evaluation="
                                << count(evaluationTargetCombo) << "/"
                                << current(evaluationTargetCombo).toStdString()
                                << ", basicSettings="
                                << (basicBruteForceSettings != nullptr)
                                << ", velocitySettings="
                                << (velocitySettings != nullptr) << '\n';
                    }
                    editorStructure = timeline != nullptr &&
                            timeline->viewer() == &viewer &&
                            timeline->isEnabled() &&
                            timelinePanel != nullptr && viewport != nullptr &&
                            timelinePanel->x() < viewport->x() &&
                            runSelectorValid &&
                            globalSettingsPlacement &&
                            baseInputScriptUiValid &&
                            bestInputsUiValid &&
                            searchControlsValid && searchMetricsUiValid &&
                            removedSectionDescriptions &&
                            automaticPacksUi && backendSelectorValid &&
                            algorithmSelectorsValid &&
                            autoPromoteBestValid &&
                            everyOwnedPanelLoaded && stuntPointsFieldValid &&
                            targetLayoutUpdatesImmediately &&
                            configurationSectionsValid &&
                            comboSlotsStyled && settingComboTextValid &&
                            modifierPassLayoutValid && debuggerUiValid &&
                            wheelScrollingValid &&
                            dropdownStateUpdates &&
                            perturbationSliderEditorsValid &&
                            insertionSlidersValid &&
                            poseSliderValid && velocitySliderValid &&
                            modifierFocusStable && unboundedFieldsScrubbable &&
                            playPause != nullptr && jumpStart != nullptr &&
                            jumpEnd != nullptr &&
                            playPause->property("enabled").toBool() &&
                            std::abs(playPause->property("width").toReal() -
                                     42.0) < 0.1 &&
                            std::abs(playPause->property("height").toReal() -
                                     42.0) < 0.1 &&
                            std::abs(jumpStart->property("width").toReal() -
                                     42.0) < 0.1 &&
                            std::abs(jumpStart->property("height").toReal() -
                                     42.0) < 0.1 &&
                            std::abs(jumpEnd->property("width").toReal() -
                                     42.0) < 0.1 &&
                            std::abs(jumpEnd->property("height").toReal() -
                                     42.0) < 0.1 &&
                            IsCenteredIcon(playIcon, 18.0) &&
                            IsCenteredIcon(pauseIcon, 18.0) &&
                            IsCenteredIcon(jumpStartIcon, 18.0) &&
                            IsCenteredIcon(jumpEndIcon, 18.0) &&
                            HasRightFacingPlaySilhouette(playIcon) &&
                            HasJumpToEndSilhouette(jumpEndIcon) &&
                            playIcon->isVisible() && !pauseIcon->isVisible() &&
                            ContainsStandardSlider(root) &&
                            !ContainsText(
                                    root,
                                    QStringLiteral("INPUT TIMELINE")) &&
                            keyboardStepping && manualDrivingUi &&
                            compactViewerHeader;
                    if (!editorStructure) {
                        std::cerr
                                << "editor checks: runSelector=" << runSelectorValid
                                << ", baseInputScript="
                                << baseInputScriptUiValid
                                << ", globalSettings="
                                << globalSettingsPlacement
                                << " (packs="
                                << (packsDirectorySection != nullptr
                                            ? packsDirectorySection->y()
                                            : -1.0)
                                << "+"
                                << (packsDirectorySection != nullptr
                                            ? packsDirectorySection->height()
                                            : -1.0)
                                << ", replay="
                                << (replaySection != nullptr
                                            ? replaySection->y() : -1.0)
                                << "+"
                                << (replaySection != nullptr
                                            ? replaySection->height() : -1.0)
                                << ", script="
                                << (baseInputScriptSection != nullptr
                                            ? baseInputScriptSection->y()
                                            : -1.0)
                                << "+"
                                << (baseInputScriptSection != nullptr
                                            ? baseInputScriptSection->height()
                                            : -1.0)
                                << ", appearance="
                                << (appearanceControls != nullptr
                                            ? appearanceControls->y() : -1.0)
                                << "+"
                                << (appearanceControls != nullptr
                                            ? appearanceControls->height()
                                            : -1.0)
                                << ", tabs="
                                << (toolTabs != nullptr
                                            ? toolTabs->y() : -1.0)
                                << ")"
                                << ", bestInputs=" << bestInputsUiValid
                                << ", searchControls=" << searchControlsValid
                                << ", autoPacks=" << automaticPacksUi
                                << ", backend=" << backendSelectorValid
                                << ", selectors=" << algorithmSelectorsValid
                                << ", panels=" << everyOwnedPanelLoaded
                                << ", stuntField=" << stuntPointsFieldValid
                                << ", sections=" << configurationSectionsValid
                                << ", comboStyle=" << comboSlotsStyled
                                << ", comboText=" << settingComboTextValid
                                << ", passLayout=" << modifierPassLayoutValid
                                << ", debugger=" << debuggerUiValid
                                << "/" << codeExpansionValid
                                << ", wheel=" << wheelScrollingValid
                                << "/" << globalComboWheelValid
                                << "/" << globalSliderWheelValid
                                << "/" << baseInputWheelValid
                                << "/" << bestInputsWheelValid
                                << "/" << sourceTreeWheelValid
                                << "/" << codeViewerWheelValid
                                << ", dropdown=" << dropdownStateUpdates
                                << ", insertion=" << insertionSlidersValid
                                << ", perturbationFields="
                                << perturbationSliderEditorsValid
                                << ", pose=" << poseSliderValid
                                << ", velocity=" << velocitySliderValid
                                << ", focus=" << modifierFocusStable
                                << ", scrub=" << unboundedFieldsScrubbable
                                << ", keyboard=" << keyboardStepping
                                << ", manual=" << manualDrivingUi
                                << ", compactHeader="
                                << compactViewerHeader
                                << " (dock="
                                << (playbackDock != nullptr
                                            ? playbackDock->width()
                                            : -1.0)
                                << ", button="
                                << (manualDriveButton != nullptr)
                                << "/"
                                << (manualDriveButton != nullptr
                                            ? manualDriveButton
                                                      ->property("enabled")
                                                      .toBool()
                                            : true)
                                << ", status="
                                << (manualDriveStatus != nullptr)
                                << "/"
                                << (manualDriveStatus != nullptr
                                            ? manualDriveStatus->isVisible()
                                            : true)
                                << ", focus="
                                << (manualInputFocus != nullptr)
                                << ", map="
                                << manualMapping(Qt::Key_Left)
                                           .toStdString()
                                << "/"
                                << manualMapping(Qt::Key_A).toStdString()
                                << "/"
                                << manualMapping(Qt::Key_Q).toStdString()
                                << "/"
                                << manualMapping(Qt::Key_Right)
                                           .toStdString()
                                << "/"
                                << manualMapping(Qt::Key_Up).toStdString()
                                << "/"
                                << manualMapping(Qt::Key_W).toStdString()
                                << "/"
                                << manualMapping(Qt::Key_Z).toStdString()
                                << "/"
                                << manualMapping(Qt::Key_Down).toStdString()
                                << "/"
                                << manualMapping(Qt::Key_S).toStdString()
                                << ")"
                                << '\n';
                    }
                    if (filled == nullptr || wire == nullptr) {
                        std::cerr << "track models were not created\n";
                        completed = true;
                        application.quit();
                        return;
                    }
                    QQuickWindow *const quickWindow =
                            qobject_cast<QQuickWindow *>(root);
                    if (quickWindow == nullptr) {
                        std::cerr << "Main.qml root is not a window\n";
                        completed = true;
                        application.quit();
                        return;
                    }

                    const QString shortInputScript = QStringLiteral(
                            "0.00 press up\n"
                            "0.11 rel up");
                    const QString previousInputScript =
                            controller.baseInputScript();
                    const bool editorFocusedForSave =
                            baseInputScriptTextArea != nullptr &&
                            QMetaObject::invokeMethod(
                                    baseInputScriptTextArea,
                                    "forceActiveFocus");
                    baseInputScriptTextArea->setProperty(
                            "text", shortInputScript);
                    QCoreApplication::processEvents();
                    const bool editWaitedForCommit =
                            controller.baseInputScript() ==
                                    previousInputScript;
                    const bool ctrlSaveCommitted =
                            editorFocusedForSave &&
                            saveBaseInputScriptShortcut != nullptr &&
                            saveBaseInputScriptShortcut
                                    ->property("enabled")
                                    .toBool() &&
                            QMetaObject::invokeMethod(
                                    saveBaseInputScriptShortcut,
                                    "activated");
                    const bool ctrlSavePreviewReady = WaitUntil([&]() {
                        return viewer.runCount() == 1 &&
                                viewer.inputSample(5).accelerate > 0.99f &&
                                viewer.inputSample(20).accelerate < 0.01f;
                    });
                    const bool ctrlSaveUpdatedPreview =
                            editWaitedForCommit &&
                            ctrlSaveCommitted &&
                            ctrlSavePreviewReady &&
                            controller.baseInputScript() == shortInputScript &&
                            viewer.previewInputScript() == shortInputScript &&
                            viewer.trajectoryCount() == 1;
                    const QVariantList initialPreviewPaths =
                            viewer.trajectoryPaths();
                    QObject *const previewGeometry =
                            initialPreviewPaths.size() == 1
                            ? initialPreviewPaths.front()
                                      .toMap()
                                      .value(QStringLiteral("geometry"))
                                      .value<QObject *>()
                            : nullptr;
                    viewer.jumpToEnd();
                    const QVector3D shortAccelerationPosition =
                            viewer.carPosition();
                    const QString longerInputScript = QStringLiteral(
                            "0.00 press up\n"
                            "0.21 rel up");
                    baseInputScriptTextArea->setProperty(
                            "text", longerInputScript);
                    QCoreApplication::processEvents();
                    const bool secondEditWaitedForCommit =
                            controller.baseInputScript() == shortInputScript;
                    QMetaObject::invokeMethod(
                            manualInputFocus, "forceActiveFocus");
                    const bool focusLossPreviewReady = WaitUntil([&]() {
                        return viewer.runCount() == 1 &&
                                viewer.inputSample(15).accelerate > 0.99f &&
                                viewer.inputSample(30).accelerate < 0.01f;
                    });
                    viewer.jumpToEnd();
                    const QVector3D valueEditedPosition =
                            viewer.carPosition();
                    const bool valueEditUpdatedPreview =
                            ctrlSaveUpdatedPreview &&
                            secondEditWaitedForCommit &&
                            focusLossPreviewReady &&
                            controller.baseInputScript() ==
                                    longerInputScript &&
                            viewer.previewInputScript() == longerInputScript &&
                            viewer.trajectoryCount() == 1 &&
                            viewer.runCount() == 1 &&
                            viewer.currentInputScript().contains(
                                    QStringLiteral("0.21 rel up")) &&
                            (valueEditedPosition -
                             shortAccelerationPosition)
                                            .lengthSquared() >
                                    0.000001f;
                    controller.setBaseInputScript(
                            QStringLiteral(
                                    "0.00 press up\n"
                                    "0.00 press left\n"
                                    "0.20 rel left\n"
                                    "0.20 rel up"));
                    const bool eventPreviewReady = WaitUntil([&]() {
                        return viewer.runCount() == 1 &&
                                viewer.inputSample(1).steering < -0.99f;
                    });
                    viewer.jumpToEnd();
                    const bool eventEditUpdatedPreview =
                            eventPreviewReady &&
                            viewer.trajectoryCount() == 1 &&
                            viewer.runCount() == 1 &&
                            viewer.trajectoryPaths()
                                            .front()
                                            .toMap()
                                            .value(QStringLiteral("geometry"))
                                            .value<QObject *>() ==
                                    previewGeometry &&
                            viewer.previewInputScript().contains(
                                    QStringLiteral("press left")) &&
                            viewer.inputSample(1).steering < -0.99f;
                    controller.setBaseInputScript(
                            QStringLiteral("not a command"));
                    const bool invalidPreviewReady = WaitUntil([&]() {
                        return viewer.trajectoryCount() == 0 &&
                                viewer.runCount() == 0;
                    });
                    QCoreApplication::sendPostedEvents(
                            nullptr, QEvent::DeferredDelete);
                    const bool invalidEditRemovedStalePreview =
                            invalidPreviewReady &&
                            viewer.trajectoryCount() == 0 &&
                            viewer.runCount() == 0 &&
                            root->findChildren<QObject *>(
                                        QStringLiteral(
                                                "trajectoryPathModel"))
                                    .isEmpty();
                    const bool carFocusUnavailableWithoutRun =
                            focusCarButton != nullptr &&
                            !focusCarButton->property("enabled").toBool();
                    controller.setBaseInputScript(
                            QStringLiteral(
                                    "0.00 press up\n"
                                    "0.00 press left\n"
                                    "0.20 rel left\n"
                                    "0.20 rel up"));
                    const bool finalPreviewReady = WaitUntil([&]() {
                        return viewer.runCount() == 1 &&
                                viewer.inputSample(1).steering < -0.99f;
                    });
                    QCoreApplication::sendPostedEvents(
                            nullptr, QEvent::DeferredDelete);
                    const QList<QObject *> trajectoryModels =
                            root->findChildren<QObject *>(
                                    QStringLiteral("trajectoryPathModel"));
                    const QList<QObject *> rayTracingTrajectoryModels =
                            root->findChildren<QObject *>(
                                    QStringLiteral(
                                            "rayTracingTrajectoryPathModel"));
                    QObject *const rayTracingTrajectoryOverlay =
                            root->findChild<QObject *>(
                                    QStringLiteral(
                                            "rayTracingTrajectoryOverlay"));
                    QObject *const rasterCuboidEditorScene =
                            root->findChild<QObject *>(QStringLiteral(
                                    "rasterCuboidEditorScene"));
                    QObject *const overlayCuboidEditorScene =
                            root->findChild<QObject *>(QStringLiteral(
                                    "rayTracingCuboidEditorScene"));
                    QObject *const drawTargetsThroughBlocksCheckBox =
                            root->findChild<QObject *>(QStringLiteral(
                                    "drawTargetsThroughBlocksCheckBox"));
                    bool targetDrawThroughToggleValid =
                            rasterCuboidEditorScene != nullptr &&
                            overlayCuboidEditorScene != nullptr &&
                            drawTargetsThroughBlocksCheckBox != nullptr &&
                            rasterCuboidEditorScene
                                    ->property("visible").toBool() &&
                            !overlayCuboidEditorScene
                                     ->property("visible").toBool() &&
                            !drawTargetsThroughBlocksCheckBox
                                     ->property("checked").toBool();
                    controller.setDrawTargetsThroughBlocks(true);
                    QCoreApplication::processEvents();
                    targetDrawThroughToggleValid &=
                            !rasterCuboidEditorScene
                                     ->property("visible").toBool() &&
                            overlayCuboidEditorScene
                                    ->property("visible").toBool() &&
                            rayTracingTrajectoryOverlay
                                    ->property("visible").toBool() &&
                            drawTargetsThroughBlocksCheckBox
                                    ->property("checked").toBool();
                    controller.setDrawTargetsThroughBlocks(false);
                    QCoreApplication::processEvents();
                    targetDrawThroughToggleValid &=
                            rasterCuboidEditorScene
                                    ->property("visible").toBool() &&
                            !overlayCuboidEditorScene
                                     ->property("visible").toBool();
                    const QVariantList finalPreviewPaths =
                            viewer.trajectoryPaths();
                    const bool trajectoryPreviewUiValid =
                            valueEditUpdatedPreview &&
                            eventEditUpdatedPreview &&
                            invalidEditRemovedStalePreview &&
                            carFocusUnavailableWithoutRun &&
                            finalPreviewReady &&
                            root->findChild<QObject *>(
                                    QStringLiteral(
                                            "saveInputTrajectoryButton")) ==
                                    nullptr &&
                            root->findChild<QObject *>(
                                    QStringLiteral(
                                            "saveInputTrajectoryShortcut")) ==
                                    nullptr &&
                            !ContainsText(
                                    root,
                                    QStringLiteral("Save trajectory")) &&
                            viewer.previewInputScript() ==
                                    controller.baseInputScript() &&
                            viewer.trajectoryCount() == 1 &&
                            viewer.runCount() == 1 &&
                            viewer.selectedRunId() ==
                                    QStringLiteral("preview") &&
                            finalPreviewPaths.size() == 1 &&
                            finalPreviewPaths.front()
                                            .toMap()
                                            .value(QStringLiteral("kind"))
                                            .toString() ==
                                    QStringLiteral("preview") &&
                            finalPreviewPaths.front()
                                            .toMap()
                                            .value(QStringLiteral("name"))
                                            .toString() ==
                                    QStringLiteral("Inputs") &&
                            clearPreviewTrajectoriesButton != nullptr &&
                            !clearPreviewTrajectoriesButton->isVisible() &&
                            trajectoryModels.size() == 1 &&
                            rayTracingTrajectoryModels.size() == 1 &&
                            rayTracingTrajectoryOverlay != nullptr &&
                            targetDrawThroughToggleValid &&
                            !rayTracingTrajectoryOverlay
                                     ->property("visible").toBool() &&
                            trajectoryModels.front()
                                    ->property("geometry")
                                    .value<QObject *>() != nullptr &&
                            trajectoryModels.front()
                                    ->property("visible").toBool() &&
                            trajectoryModels.front()
                                            ->property("geometry")
                                            .value<QObject *>() ==
                                    previewGeometry;
                    if (!trajectoryPreviewUiValid) {
                        std::cerr
                                << "automatic preview UI checks failed: value="
                                << valueEditUpdatedPreview
                                << ", event=" << eventEditUpdatedPreview
                                << ", invalid="
                                << invalidEditRemovedStalePreview
                                << ", ready=" << ctrlSavePreviewReady
                                << "/" << focusLossPreviewReady
                                << "/" << eventPreviewReady
                                << "/" << invalidPreviewReady
                                << "/" << finalPreviewReady
                                << ", commit=" << ctrlSaveCommitted
                                << "/" << secondEditWaitedForCommit
                                << ", base='"
                                << controller.baseInputScript().toStdString()
                                << "', current='"
                                << viewer.currentInputScript().toStdString()
                                << "'"
                                << ", paths=" << finalPreviewPaths.size()
                                << ", runs=" << viewer.runCount()
                                << ", selected="
                                << viewer.selectedRunId().toStdString()
                                << ", raster=" << trajectoryModels.size()
                                << ", ray="
                                << rayTracingTrajectoryModels.size()
                                << ", geometry="
                                << (trajectoryModels.size() == 1 &&
                                    trajectoryModels.front()
                                                    ->property("geometry")
                                                    .value<QObject *>() ==
                                            previewGeometry)
                                << ", scriptSync="
                                << (viewer.previewInputScript() ==
                                    controller.baseInputScript())
                                << '\n';
                    }

                    const QVector3D baselinePosition = viewer.carPosition();
                    const QVector3D bestPosition =
                            baselinePosition + QVector3D(5.0f, 0.0f, 0.0f);
                    std::vector<forevertas::SearchTimelineFrame> bestFrames;
                    bestFrames.reserve(3u);
                    for (std::int64_t timeMs : {0, 10, 20}) {
                        forevertas::SearchTimelineFrame frame;
                        frame.timeMs = timeMs;
                        frame.positionX = baselinePosition.x() +
                                5.0f + static_cast<float>(timeMs) / 10.0f;
                        frame.positionY = baselinePosition.y();
                        frame.positionZ = baselinePosition.z();
                        frame.rotationW = 1.0f;
                        frame.accelerate = timeMs >= 10 ? 1.0f : 0.0f;
                        frame.steering = static_cast<float>(timeMs) / 20.0f;
                        frame.checkpointsCollected =
                                timeMs >= 20 ? 12u : timeMs >= 10 ? 1u : 0u;
                        frame.checkpointsTotal = 12u;
                        frame.totalLaps = 1u;
                        frame.raceCompleted = timeMs >= 20;
                        if (frame.raceCompleted) {
                            frame.finishTimeMs = 20u;
                        }
                        bestFrames.push_back(frame);
                    }
                    const std::vector<forevertas::SandboxInputEvent>
                            bestInputs{
                                    SwitchInput(
                                            0,
                                            forevertas::SandboxInputAction::
                                                    RaceRunning,
                                            true),
                                    SwitchInput(
                                            0,
                                            forevertas::SandboxInputAction::
                                                    Accelerate,
                                            true),
                                    SwitchInput(
                                            20,
                                            forevertas::SandboxInputAction::
                                                    Brake,
                                            true)};
                    viewer.addSearchRun(QString::fromLocal8Bit(argv[1]),
                                        QString::fromLocal8Bit(argv[2]),
                                        bestFrames,
                                        bestInputs);
                    std::vector<forevertas::SearchTimelineFrame>
                            firstImprovement = bestFrames;
                    std::vector<forevertas::SearchTimelineFrame>
                            secondImprovement = bestFrames;
                    for (forevertas::SearchTimelineFrame &frame :
                         firstImprovement) {
                        frame.positionZ += 2.0f;
                    }
                    for (forevertas::SearchTimelineFrame &frame :
                         secondImprovement) {
                        frame.positionZ += 4.0f;
                    }
                    viewer.addSearchImprovement(
                            QString::fromLocal8Bit(argv[1]),
                            QString::fromLocal8Bit(argv[2]),
                            firstImprovement,
                            QStringLiteral("optimized-cpu"),
                            9u,
                            1u);
                    viewer.addSearchImprovement(
                            QString::fromLocal8Bit(argv[1]),
                            QString::fromLocal8Bit(argv[2]),
                            secondImprovement,
                            QStringLiteral("optimized-cpu"),
                            9u,
                            2u);
                    QCoreApplication::processEvents();
                    QCoreApplication::processEvents();
                    const QVariantList improvementPaths =
                            viewer.trajectoryPaths();
                    const bool bestToggleInitiallyVisible =
                            trajectoryVisibilityToggle != nullptr &&
                            trajectoryVisibilityToggle
                                    ->property("enabled").toBool() &&
                            trajectoryVisibilityToggle
                                    ->property("checked").toBool() &&
                            viewer.hasTrajectoryForRun(
                                    QStringLiteral("best")) &&
                            viewer.trajectoryVisibleForRun(
                                    QStringLiteral("best"));
                    viewer.setTrajectoryVisibleForRun(
                            QStringLiteral("best"), false);
                    QCoreApplication::processEvents();
                    QObject *const bestTrajectoryGeometry =
                            improvementPaths.at(1)
                                    .toMap()
                                    .value(QStringLiteral("geometry"))
                                    .value<QObject *>();
                    const auto hiddenBestModel =
                            [bestTrajectoryGeometry](
                                    const QList<QObject *> &models) {
                                return std::any_of(
                                        models.begin(),
                                        models.end(),
                                        [bestTrajectoryGeometry](
                                                const QObject *model) {
                                            return model->property("geometry")
                                                                   .value<
                                                                           QObject *>() ==
                                                    bestTrajectoryGeometry &&
                                                    !model->property("visible")
                                                             .toBool();
                                        });
                            };
                    const bool bestRasterModelHidden = hiddenBestModel(
                            root->findChildren<QObject *>(QStringLiteral(
                                    "trajectoryPathModel")));
                    const bool bestRayModelHidden = hiddenBestModel(
                            root->findChildren<QObject *>(QStringLiteral(
                                    "rayTracingTrajectoryPathModel")));
                    const bool bestToggleHidesOnlyBest =
                            trajectoryVisibilityToggle != nullptr &&
                            !trajectoryVisibilityToggle
                                     ->property("checked").toBool() &&
                            !viewer.trajectoryVisibleForRun(
                                    QStringLiteral("best")) &&
                            bestRasterModelHidden &&
                            bestRayModelHidden &&
                            viewer.trajectoryVisibleForRun(
                                    QStringLiteral("preview"));
                    viewer.setTrajectoryVisibleForRun(
                            QStringLiteral("best"), true);
                    QCoreApplication::processEvents();
                    const bool improvementTrajectoryUiValid =
                            bestToggleInitiallyVisible &&
                            bestToggleHidesOnlyBest &&
                            trajectoryVisibilityToggle != nullptr &&
                            trajectoryVisibilityToggle
                                    ->property("checked").toBool() &&
                            viewer.trajectoryCount() == 4 &&
                            improvementPaths.size() == 4 &&
                            improvementPaths.at(1)
                                            .toMap()
                                            .value(QStringLiteral("name"))
                                            .toString() ==
                                    QStringLiteral("Best") &&
                            improvementPaths.at(1)
                                            .toMap()
                                            .value(QStringLiteral("runId"))
                                            .toString() ==
                                    QStringLiteral("best") &&
                            improvementPaths.at(2)
                                            .toMap()
                                            .value(QStringLiteral("name"))
                                            .toString() ==
                                    QStringLiteral("Improvement 1") &&
                            improvementPaths.at(2)
                                            .toMap()
                                            .value(QStringLiteral("opacity"))
                                            .toDouble() < 0.31 &&
                            improvementPaths.at(3)
                                            .toMap()
                                            .value(QStringLiteral("name"))
                                            .toString() ==
                                    QStringLiteral("Improvement 2") &&
                            improvementPaths.at(3)
                                            .toMap()
                                            .value(QStringLiteral("opacity"))
                                            .toDouble() > 0.95;
                    const QObject *const inputTrajectoryGeometry =
                            improvementPaths.at(0)
                                    .toMap()
                                    .value(QStringLiteral("geometry"))
                                    .value<QObject *>();
                    const bool clearButtonReady =
                            clearPreviewTrajectoriesButton != nullptr &&
                            clearPreviewTrajectoriesButton->isVisible() &&
                            clearPreviewTrajectoriesButton
                                    ->property("enabled").toBool() &&
                            clearPreviewTrajectoriesButton
                                    ->property("text").toString() ==
                                    QStringLiteral("Clear previews") &&
                            viewer.hasPreviewTrajectories();
                    const bool firstClearInvoked =
                            clearButtonReady &&
                            QMetaObject::invokeMethod(
                                    clearPreviewTrajectoriesButton,
                                    "clicked");
                    QCoreApplication::processEvents();
                    QCoreApplication::sendPostedEvents(
                            nullptr, QEvent::DeferredDelete);
                    QCoreApplication::processEvents();
                    const QVariantList firstClearedPaths =
                            viewer.trajectoryPaths();
                    const bool firstClearRemovedPreviews =
                            firstClearInvoked &&
                            viewer.selectedRunId() ==
                                    QStringLiteral("best") &&
                            viewer.trajectoryCount() == 2 &&
                            !viewer.hasPreviewTrajectories() &&
                            firstClearedPaths.size() == 2 &&
                            firstClearedPaths.at(0)
                                            .toMap()
                                            .value(QStringLiteral("kind"))
                                            .toString() ==
                                    QStringLiteral("preview") &&
                            firstClearedPaths.at(0)
                                            .toMap()
                                            .value(QStringLiteral("geometry"))
                                            .value<QObject *>() ==
                                    inputTrajectoryGeometry &&
                            firstClearedPaths.at(1)
                                            .toMap()
                                            .value(QStringLiteral("runId"))
                                            .toString() ==
                                    QStringLiteral("best") &&
                            firstClearedPaths.at(1)
                                            .toMap()
                                            .value(QStringLiteral("geometry"))
                                            .value<QObject *>() ==
                                    bestTrajectoryGeometry &&
                            clearPreviewTrajectoriesButton->isVisible() &&
                            !clearPreviewTrajectoriesButton
                                     ->property("enabled").toBool();
                    viewer.addSearchImprovement(
                            QString::fromLocal8Bit(argv[1]),
                            QString::fromLocal8Bit(argv[2]),
                            firstImprovement,
                            QStringLiteral("optimized-cpu"),
                            9u,
                            1u);
                    QCoreApplication::processEvents();
                    const bool clearedKeyWasReleased =
                            viewer.hasPreviewTrajectories() &&
                            viewer.trajectoryCount() == 3 &&
                            clearPreviewTrajectoriesButton
                                    ->property("enabled").toBool();
                    const bool secondClearInvoked =
                            QMetaObject::invokeMethod(
                                    clearPreviewTrajectoriesButton,
                                    "clicked");
                    QCoreApplication::processEvents();
                    QCoreApplication::sendPostedEvents(
                            nullptr, QEvent::DeferredDelete);
                    QCoreApplication::processEvents();
                    const QVariantList finalClearedPaths =
                            viewer.trajectoryPaths();
                    const bool clearPreviewTrajectoriesUiValid =
                            improvementTrajectoryUiValid &&
                            firstClearRemovedPreviews &&
                            clearedKeyWasReleased &&
                            secondClearInvoked &&
                            viewer.trajectoryCount() == 2 &&
                            !viewer.hasPreviewTrajectories() &&
                            finalClearedPaths.size() == 2 &&
                            std::none_of(
                                    finalClearedPaths.begin(),
                                    finalClearedPaths.end(),
                                    [](const QVariant &entry) {
                                        return entry.toMap()
                                                       .value(QStringLiteral(
                                                               "kind"))
                                                       .toString() ==
                                                QStringLiteral("improvement");
                                    });
                    viewer.jumpToStart();
                    QCoreApplication::processEvents();
                    auto *const checkpointSplitOverlay =
                            qobject_cast<QQuickItem *>(
                                    root->findChild<QObject *>(
                                            QStringLiteral(
                                                    "checkpointSplitOverlay")));
                    auto *const checkpointSplitList =
                            qobject_cast<QQuickItem *>(
                                    root->findChild<QObject *>(
                                            QStringLiteral(
                                                    "checkpointSplitList")));
                    const bool splitOverlayEmptyState =
                            checkpointSplitOverlay != nullptr &&
                            checkpointSplitList != nullptr &&
                            !checkpointSplitOverlay->isVisible() &&
                            checkpointSplitList
                                            ->property("count")
                                            .toInt() == 0;
                    viewer.jumpToEnd();
                    QCoreApplication::processEvents();
                    QCoreApplication::processEvents();
                    QEventLoop checkpointSplitRenderLoop;
                    QTimer::singleShot(
                            60,
                            &checkpointSplitRenderLoop,
                            &QEventLoop::quit);
                    checkpointSplitRenderLoop.exec();
                    const int checkpointSplitEndCount =
                            checkpointSplitList != nullptr
                            ? checkpointSplitList
                                      ->property("count")
                                      .toInt()
                            : -1;
                    const bool checkpointSplitEndVisible =
                            checkpointSplitOverlay != nullptr &&
                            checkpointSplitOverlay->isVisible();
                    const qsizetype controllerSplitEndCount =
                            viewer.checkpointSplits().size();
                    const qreal checkpointSplitEndHeight =
                            checkpointSplitOverlay != nullptr
                            ? checkpointSplitOverlay->height()
                            : -1.0;
                    const qreal checkpointSplitListEndHeight =
                            checkpointSplitList != nullptr
                            ? checkpointSplitList->height()
                            : -1.0;
                    const qreal checkpointSplitContentEndHeight =
                            checkpointSplitList != nullptr
                            ? checkpointSplitList
                                      ->property("contentHeight")
                                      .toReal()
                            : -1.0;
                    QSet<QString> renderedSplitTexts;
                    CollectVisualTexts(
                            checkpointSplitList,
                            renderedSplitTexts);
                    const qreal originalSplitReviewWidth =
                            root->property("width").toReal();
                    const qreal originalSplitReviewHeight =
                            root->property("height").toReal();
                    root->setProperty("width", 1240);
                    root->setProperty("height", 580);
                    QCoreApplication::processEvents();
                    const bool compactSplitLayout =
                            checkpointSplitOverlay != nullptr &&
                            cameraFocusToolbar != nullptr &&
                            scriptedTelemetry != nullptr &&
                            playbackDock != nullptr &&
                            checkpointSplitOverlay->x() >= 13.9 &&
                            checkpointSplitOverlay->width() >= 197.9 &&
                            std::abs(
                                    checkpointSplitOverlay->x() +
                                            checkpointSplitOverlay->width() -
                                    cameraFocusToolbar->x() -
                                            cameraFocusToolbar->width()) <= 2.1 &&
                            checkpointSplitOverlay->y() >=
                                    scriptedTelemetry->y() +
                                            scriptedTelemetry->height() +
                                            7.9 &&
                            checkpointSplitOverlay->y() +
                                            checkpointSplitOverlay->height() <=
                                    playbackDock->y() - 7.9;
                    root->setProperty(
                            "width", originalSplitReviewWidth);
                    root->setProperty(
                            "height", originalSplitReviewHeight);
                    viewer.jumpToStart();
                    QCoreApplication::processEvents();
                    const bool checkpointSplitOverlayUiValid =
                            splitOverlayEmptyState &&
                            checkpointSplitOverlay != nullptr &&
                            checkpointSplitList != nullptr &&
                            compactSplitLayout &&
                            checkpointSplitEndVisible &&
                            checkpointSplitEndCount == 13 &&
                            controllerSplitEndCount == 13 &&
                            checkpointSplitContentEndHeight >
                                    checkpointSplitListEndHeight &&
                            checkpointSplitList
                                            ->property("count")
                                            .toInt() == 0 &&
                            renderedSplitTexts.contains(
                                    QStringLiteral("CP 12")) &&
                            renderedSplitTexts.contains(
                                    QStringLiteral("Finish")) &&
                            renderedSplitTexts.contains(
                                    QStringLiteral("0.020"));
                    if (!checkpointSplitOverlayUiValid) {
                        std::cerr
                                << "checkpoint split overlay checks failed: "
                                << "empty=" << splitOverlayEmptyState
                                << ", overlay="
                                << (checkpointSplitOverlay != nullptr)
                                << ", list="
                                << (checkpointSplitList != nullptr)
                                << ", compact=" << compactSplitLayout
                                << ", end="
                                << checkpointSplitEndVisible << "/"
                                << checkpointSplitEndCount << "/"
                                << controllerSplitEndCount
                                << "/h=" << checkpointSplitEndHeight
                                << "/" << checkpointSplitListEndHeight
                                << "/" << checkpointSplitContentEndHeight
                                << ", currentCount="
                                << (checkpointSplitList != nullptr
                                            ? checkpointSplitList
                                                      ->property("count")
                                                      .toInt()
                                            : -1)
                                << ", geometry="
                                << (checkpointSplitOverlay != nullptr
                                            ? checkpointSplitOverlay->y()
                                            : -1.0)
                                << "+"
                                << (checkpointSplitOverlay != nullptr
                                            ? checkpointSplitOverlay->height()
                                            : -1.0)
                                << "/list="
                                << (checkpointSplitList != nullptr
                                            ? checkpointSplitList->height()
                                            : -1.0)
                                << "/content="
                                << (checkpointSplitList != nullptr
                                            ? checkpointSplitList
                                                      ->property(
                                                              "contentHeight")
                                                      .toDouble()
                                            : -1.0)
                                << ", renderedTexts="
                                << renderedSplitTexts.size()
                                << '\n';
                    }

                    QTimer::singleShot(
                            250, &application,
                            [&, filled, wire, quickWindow, runSelector,
                             renderModeSelector, gpuRayTracingView,
                             rasterMapView, viewCamera,
                             mapEnvironment, daySkyTexture, mainMapLight,
                             fillMapLight, bestPosition,
                             baseInputScriptTextArea,
                             copyCurrentRaceInputsButton,
                             rayTracingTrajectoryOverlay,
                             trajectoryPreviewUiValid,
                             improvementTrajectoryUiValid,
                             clearPreviewTrajectoriesUiValid,
                             checkpointSplitOverlayUiValid]() {
                                QCoreApplication::sendPostedEvents(
                                        nullptr, QEvent::DeferredDelete);
                                const QList<QObject *> carRoots =
                                        root->findChildren<QObject *>(
                                                QStringLiteral("runCarRoot"));
                                const QList<QObject *> carFilledModels =
                                        root->findChildren<QObject *>(
                                                QStringLiteral(
                                                        "runCarFilledModel"));
                                const QList<QObject *> carFilledMaterials =
                                        root->findChildren<QObject *>(
                                                QStringLiteral(
                                                        "runCarFilledMaterial"));
                                const QList<QObject *> carWireModels =
                                        root->findChildren<QObject *>(
                                                QStringLiteral(
                                                        "runCarWireModel"));
                                const QString originalSelectedRun =
                                        viewer.selectedRunId();
                                const qint64 originalRunTime = viewer.timeMs();
                                const QVariantList stressRunOptions =
                                        viewer.runOptions();
                                for (int cycle = 0;
                                     cycle < 200 &&
                                     stressRunOptions.size() > 1;
                                     ++cycle) {
                                    viewer.setSelectedRunId(
                                            stressRunOptions[
                                                    cycle %
                                                    stressRunOptions.size()]
                                                    .toMap()
                                                    .value(QStringLiteral("id"))
                                                    .toString());
                                    viewer.setCurrentTick(
                                            cycle % std::max<qint64>(
                                                    1, viewer.tickCount()));
                                }
                                viewer.setSelectedRunId(originalSelectedRun);
                                viewer.setTimeMs(originalRunTime);
                                QCoreApplication::processEvents();
                                const bool carDelegatesStable =
                                        carRoots ==
                                                root->findChildren<QObject *>(
                                                        QStringLiteral(
                                                                "runCarRoot")) &&
                                        carFilledModels ==
                                                root->findChildren<QObject *>(
                                                        QStringLiteral(
                                                                "runCarFilledModel")) &&
                                        carWireModels ==
                                                root->findChildren<QObject *>(
                                                        QStringLiteral(
                                                                "runCarWireModel"));
                                QObject *const selectedCarRoot =
                                        root->findChild<QObject *>(
                                                QStringLiteral(
                                                        "selectedRunCarRoot"));
                                const QList<QObject *>
                                        selectedCarFilledModels =
                                                root->findChildren<QObject *>(
                                                        QStringLiteral(
                                                                "selectedRunCarFilledModel"));
                                const QList<QObject *>
                                        selectedCarWireModels =
                                                root->findChildren<QObject *>(
                                                        QStringLiteral(
                                                                "selectedRunCarWireModel"));
                                const QList<QObject *> allTrajectoryModels =
                                        root->findChildren<QObject *>(
                                                QStringLiteral(
                                                        "trajectoryPathModel"));
                                const QList<QObject *>
                                        allRayTracingTrajectoryModels =
                                                root->findChildren<QObject *>(
                                                        QStringLiteral(
                                                                "rayTracingTrajectoryPathModel"));
                                const bool allTrajectoryModelsRendered =
                                        allTrajectoryModels.size() ==
                                                viewer.trajectoryCount() &&
                                        allRayTracingTrajectoryModels.size() ==
                                                viewer.trajectoryCount();
                                const QList<QObject *> visualModels =
                                        root->findChildren<QObject *>(
                                                QStringLiteral(
                                                        "trackVisualModel"));
                                const QList<QObject *> visualMaterials =
                                        root->findChildren<QObject *>(
                                                QStringLiteral(
                                                        "trackVisualMaterial"));
                                const QList<QObject *> visualBaseTextures =
                                        root->findChildren<QObject *>(
                                                QStringLiteral(
                                                        "trackVisualBaseTexture"));
                                const auto materialState =
                                        [](const QObject *material) {
                                            return QVariantList{
                                                    material->property(
                                                            "baseColor"),
                                                    material->property(
                                                            "baseColorMap"),
                                                    material->property(
                                                            "normalMap"),
                                                    material->property(
                                                            "roughness"),
                                                    material->property(
                                                            "metalness"),
                                                    material->property(
                                                            "opacity"),
                                                    material->property(
                                                            "cullMode")};
                                        };
                                std::vector<QVariantList>
                                        materialStatesBeforeThemeChange;
                                materialStatesBeforeThemeChange.reserve(
                                        static_cast<std::size_t>(
                                                visualMaterials.size()));
                                for (const QObject *material :
                                     visualMaterials) {
                                    materialStatesBeforeThemeChange.push_back(
                                            materialState(material));
                                }
                                const QVariantList sceneStateBeforeThemeChange{
                                        filled->property("geometry"),
                                        wire->property("geometry"),
                                        mapEnvironment->property(
                                                "clearColor"),
                                        mapEnvironment->property(
                                                "lightProbe"),
                                        mapEnvironment->property(
                                                "backgroundMode"),
                                        mainMapLight->property("color"),
                                        mainMapLight->property("brightness"),
                                        fillMapLight->property("color"),
                                        fillMapLight->property("brightness"),
                                        viewCamera->property("fieldOfView"),
                                        viewCamera->property("clipNear"),
                                        viewCamera->property("clipFar")};
                                controller.setDarkMode(true);
                                QCoreApplication::processEvents();
                                bool loadedSceneThemeInvariant =
                                        sceneStateBeforeThemeChange ==
                                                QVariantList{
                                                        filled->property(
                                                                "geometry"),
                                                        wire->property(
                                                                "geometry"),
                                                        mapEnvironment
                                                                ->property(
                                                                        "clearColor"),
                                                        mapEnvironment
                                                                ->property(
                                                                        "lightProbe"),
                                                        mapEnvironment
                                                                ->property(
                                                                        "backgroundMode"),
                                                        mainMapLight->property(
                                                                "color"),
                                                        mainMapLight->property(
                                                                "brightness"),
                                                        fillMapLight->property(
                                                                "color"),
                                                        fillMapLight->property(
                                                                "brightness"),
                                                        viewCamera->property(
                                                                "fieldOfView"),
                                                        viewCamera->property(
                                                                "clipNear"),
                                                        viewCamera->property(
                                                                "clipFar")};
                                for (qsizetype index = 0;
                                     index < visualMaterials.size();
                                     ++index) {
                                    loadedSceneThemeInvariant &=
                                            materialStatesBeforeThemeChange
                                                    .at(static_cast<
                                                        std::size_t>(index)) ==
                                            materialState(
                                                    visualMaterials.at(index));
                                }
                                controller.setDarkMode(false);
                                QCoreApplication::processEvents();
                                const int expectedCarModels =
                                        static_cast<int>(
                                                viewer.ellipsoidCount() *
                                                viewer.runCount());
                                bool rootsVisible =
                                        carRoots.size() ==
                                                viewer.runCount() &&
                                        selectedCarRoot != nullptr &&
                                        selectedCarRoot
                                                ->property("visible")
                                                .toBool();
                                int visibleComparisonRoots = 0;
                                for (const QObject *rootNode : carRoots) {
                                    visibleComparisonRoots += rootNode
                                                                      ->property(
                                                                              "visible")
                                                                      .toBool()
                                            ? 1
                                            : 0;
                                    rootsVisible &= std::abs(rootNode
                                                             ->property("opacity")
                                                             .toReal() -
                                                     1.0) < 0.001;
                                }
                                rootsVisible &= visibleComparisonRoots ==
                                        std::max<qint64>(
                                                0, viewer.runCount() - 1);

                                const QVariant filledGeometry =
                                        filled->property("geometry");
                                const QVariant wireGeometry =
                                        wire->property("geometry");
                                const bool geometryAttached =
                                        filledGeometry.isValid() &&
                                        !filledGeometry.isNull() &&
                                        wireGeometry.isValid() &&
                                        !wireGeometry.isNull();
                                const int initialVisibleVisualModels =
                                        VisibleModelCount(visualModels);
                                const bool rayTracingSupported =
                                        gpuRayTracingView != nullptr &&
                                        gpuRayTracingView
                                                ->property("supported")
                                                .toBool();
                                const int wireframeIndex =
                                        rayTracingSupported ? 4 : 3;
                                const int highContrastIndex =
                                        rayTracingSupported ? 5 : 4;
                                bool renderModeOptionsValid =
                                        renderModeSelector != nullptr &&
                                        renderModeSelector->property("count")
                                                        .toInt() ==
                                                (rayTracingSupported ? 6 : 5);
                                if (renderModeOptionsValid) {
                                    if (rayTracingSupported) {
                                        renderModeSelector->setProperty(
                                                "currentIndex", 1);
                                        renderModeOptionsValid =
                                                renderModeSelector
                                                                ->property(
                                                                        "currentValue")
                                                                .toString() ==
                                                        QStringLiteral(
                                                                "textured-rt") &&
                                                renderModeSelector
                                                                ->property(
                                                                        "displayText")
                                                                .toString() ==
                                                        QStringLiteral(
                                                                "Textured (RT)");
                                    }
                                    renderModeSelector->setProperty(
                                            "currentIndex", wireframeIndex);
                                    renderModeOptionsValid &=
                                            renderModeSelector
                                                    ->property("currentValue")
                                                    .toString() ==
                                                    QStringLiteral(
                                                            "wireframe") &&
                                            renderModeSelector
                                                    ->property("displayText")
                                                    .toString() ==
                                                    QStringLiteral(
                                                            "Wireframe");
                                    renderModeSelector->setProperty(
                                            "currentIndex",
                                            highContrastIndex);
                                    renderModeOptionsValid &=
                                            renderModeSelector
                                                    ->property("currentValue")
                                                    .toString() ==
                                                    QStringLiteral(
                                                            "material-debug") &&
                                            renderModeSelector
                                                    ->property("displayText")
                                                    .toString() ==
                                                    QStringLiteral(
                                                            "High Contrast");
                                    renderModeSelector->setProperty(
                                            "currentIndex", 0);
                                }
                                const bool initialModelState =
                                        !filled->property("visible").toBool() &&
                                        !wire->property("visible").toBool() &&
                                        renderModeOptionsValid &&
                                        renderModeSelector
                                                        ->property("currentValue")
                                                        .toString() ==
                                                QStringLiteral("textured") &&
                                        visualModels.size() ==
                                                viewer.visualInstances().size() &&
                                        viewer.visualTriangleCount() > 0 &&
                                        viewer.visualMeshCount() > 0 &&
                                        viewer.materialCount() > 0 &&
                                        initialVisibleVisualModels > 0 &&
                                        ModelsHaveGeometry(
                                                visualModels,
                                                visualModels.size()) &&
                                        VisualMaterialsAreBoundAndShared(
                                                visualModels,
                                                visualMaterials,
                                                visualBaseTextures,
                                                viewer) &&
                                        root->findChildren<QObject *>(
                                                    QStringLiteral(
                                                            "trackVisualNormalTexture"))
                                                .isEmpty() &&
                                        ModelsHaveState(carFilledModels,
                                                        expectedCarModels,
                                                        true) &&
                                        FilledModelsHaveBakedRunPalettes(
                                                carFilledModels,
                                                carFilledMaterials,
                                                expectedCarModels) &&
                                        ModelsHaveState(carWireModels,
                                                        expectedCarModels,
                                                        false) &&
                                        ModelsHaveState(
                                                selectedCarFilledModels,
                                                static_cast<int>(
                                                        viewer.ellipsoidCount()),
                                                true) &&
                                        ModelsHaveState(
                                                selectedCarWireModels,
                                                static_cast<int>(
                                                        viewer.ellipsoidCount()),
                                                false);
                                bool rayTracingModeValid =
                                        gpuRayTracingView != nullptr &&
                                        rasterMapView != nullptr &&
                                        rayTracingTrajectoryOverlay != nullptr &&
                                        !gpuRayTracingView
                                                 ->property("visible")
                                                 .toBool() &&
                                        !gpuRayTracingView
                                                 ->property("active")
                                                 .toBool() &&
                                        !gpuRayTracingView
                                                 ->property("status")
                                                 .toString()
                                                 .isEmpty();
                                if (rayTracingSupported) {
                                    root->setProperty(
                                            "renderMode",
                                            QStringLiteral("textured-rt"));
                                    QCoreApplication::processEvents();
                                    rayTracingModeValid &=
                                            root->property(
                                                        "rayTracingEnabled")
                                                            .toBool() &&
                                            gpuRayTracingView
                                                    ->property("visible")
                                                    .toBool() &&
                                            gpuRayTracingView
                                                    ->property("active")
                                                    .toBool() &&
                                            rayTracingTrajectoryOverlay
                                                    ->property("visible")
                                                    .toBool() &&
                                            !rasterMapView
                                                     ->property("visible")
                                                     .toBool();
                                    root->setProperty(
                                            "renderMode",
                                            QStringLiteral("textured"));
                                    QCoreApplication::processEvents();
                                    rayTracingModeValid &=
                                            !root->property(
                                                         "rayTracingEnabled")
                                                     .toBool() &&
                                            !gpuRayTracingView
                                                     ->property("visible")
                                                     .toBool() &&
                                            !gpuRayTracingView
                                                     ->property("active")
                                                     .toBool() &&
                                            !rayTracingTrajectoryOverlay
                                                     ->property("visible")
                                                     .toBool() &&
                                            rasterMapView
                                                    ->property("visible")
                                                    .toBool();
                                }
                                const bool optimizedRenderState =
                                        viewCamera != nullptr &&
                                        viewCamera->property("clipNear")
                                                        .toDouble() >= 0.05 &&
                                        viewCamera->property("clipFar")
                                                        .toDouble() >
                                                viewCamera->property("clipNear")
                                                        .toDouble() &&
                                        viewCamera->property("clipFar")
                                                                .toDouble() /
                                                        viewCamera
                                                                ->property(
                                                                        "clipNe"
                                                                        "ar")
                                                                .toDouble() <=
                                                50001.0 &&
                                        mainMapLight != nullptr &&
                                        !mainMapLight->property("castsShadow")
                                                 .toBool() &&
                                        std::all_of(
                                                visualModels.cbegin(),
                                                visualModels.cend(),
                                                [](const QObject *model) {
                                                    return !model->property(
                                                                         "casts"
                                                                         "Shado"
                                                                         "ws")
                                                                    .toBool();
                                                });
                                const QUrl skySource =
                                        daySkyTexture != nullptr
                                        ? daySkyTexture->property("source")
                                                  .toUrl()
                                        : QUrl();
                                const bool daylightEnvironment =
                                        mapEnvironment != nullptr &&
                                        daySkyTexture != nullptr &&
                                        mainMapLight != nullptr &&
                                        fillMapLight != nullptr &&
                                        mapEnvironment
                                                        ->property(
                                                                "probeExposure")
                                                        .toDouble() >=
                                                0.8 &&
                                        mapEnvironment
                                                        ->property(
                                                                "skyboxBlur"
                                                                "Amount")
                                                        .toDouble() ==
                                                0.0 &&
                                        skySource.scheme() ==
                                                QStringLiteral("qrc") &&
                                        skySource.path() ==
                                                QStringLiteral(
                                                        "/environment/"
                                                        "day_sky.png") &&
                                        mainMapLight
                                                        ->property("brightness")
                                                        .toDouble() >=
                                                1.0 &&
                                        fillMapLight
                                                        ->property("brightness")
                                                        .toDouble() >
                                                0.0;

                                const bool bestSelectedInitially =
                                        viewer.runCount() == 2 &&
                                        viewer.runOptions().size() == 2 &&
                                        viewer.runPoses().size() == 2 &&
                                        viewer.selectedRunId() ==
                                                QStringLiteral("best") &&
                                        viewer.tickCount() == 3 &&
                                        (viewer.carPosition() - bestPosition)
                                                        .length() < 0.001f &&
                                        runSelector != nullptr &&
                                        runSelector->property("count").toInt() ==
                                                2 &&
                                        runSelector
                                                        ->property("currentValue")
                                                        .toString() ==
                                                QStringLiteral("best") &&
                                        runSelector
                                                        ->property("displayText")
                                                        .toString() ==
                                                QStringLiteral("Best");

                                viewer.setTimeMs(10);
                                controller.setBaseInputScript(
                                        QStringLiteral(
                                                "0.00 press down"));
                                QCoreApplication::processEvents();
                                const bool copyInvoked =
                                        copyCurrentRaceInputsButton != nullptr &&
                                        copyCurrentRaceInputsButton
                                                ->property("enabled").toBool() &&
                                        QMetaObject::invokeMethod(
                                                copyCurrentRaceInputsButton,
                                                "clicked",
                                                Qt::DirectConnection);
                                QCoreApplication::processEvents();
                                const bool copyCurrentRaceInputsValid =
                                        copyInvoked &&
                                        controller.baseInputScript() ==
                                                QStringLiteral(
                                                        "0.00 press up") &&
                                        baseInputScriptTextArea != nullptr &&
                                        baseInputScriptTextArea
                                                        ->property("text")
                                                        .toString() ==
                                                controller.baseInputScript();
                                viewer.jumpToStart();
                                QCoreApplication::processEvents();

                                const auto activateRun =
                                        [runSelector](int index) {
                                            return runSelector != nullptr &&
                                                    QMetaObject::invokeMethod(
                                                            runSelector,
                                                            "activated",
                                                            Qt::DirectConnection,
                                                            Q_ARG(int, index));
                                        };
                                const bool bestActivated = activateRun(1);
                                QCoreApplication::processEvents();
                                QCoreApplication::processEvents();
                                const bool onlyBestSelected =
                                        bestActivated &&
                                        viewer.selectedRunId() ==
                                                QStringLiteral("best") &&
                                        viewer.tickCount() == 3 &&
                                        (viewer.carPosition() - bestPosition)
                                                .length() < 0.001f;

                                root->setProperty(
                                        "renderMode",
                                        QStringLiteral("neutral"));
                                QCoreApplication::processEvents();
                                const bool neutralModeState =
                                        filled->property("visible").toBool() &&
                                        !wire->property("visible").toBool() &&
                                        ModelsHaveState(
                                                visualModels,
                                                visualModels.size(),
                                                false) &&
                                        std::all_of(
                                                visualMaterials.cbegin(),
                                                visualMaterials.cend(),
                                                [](const QObject *material) {
                                                    return material
                                                            ->property(
                                                                    "baseColorMap")
                                                            .value<QObject *>() ==
                                                            nullptr &&
                                                            material
                                                                    ->property(
                                                                            "normalMap")
                                                                    .value<QObject *>() ==
                                                            nullptr;
                                                });
                                root->setProperty(
                                        "renderMode",
                                        QStringLiteral("collision"));
                                QCoreApplication::processEvents();
                                const bool collisionModeState =
                                        filled->property("visible").toBool() &&
                                        !wire->property("visible").toBool() &&
                                        ModelsHaveState(
                                                visualModels,
                                                visualModels.size(),
                                                false);
                                root->setProperty(
                                        "renderMode",
                                        QStringLiteral("material-debug"));
                                QCoreApplication::processEvents();
                                const bool materialDebugState =
                                        filled->property("visible").toBool() &&
                                        !wire->property("visible").toBool() &&
                                        ModelsHaveState(
                                                visualModels,
                                                visualModels.size(),
                                                false);
                                root->setProperty(
                                        "renderMode",
                                        QStringLiteral("wireframe"));
                                QCoreApplication::processEvents();
                                const bool wireframeState =
                                        !filled->property("visible").toBool() &&
                                        wire->property("visible").toBool() &&
                                        ModelsHaveState(
                                                visualModels,
                                                visualModels.size(),
                                                false) &&
                                        ModelsHaveState(carFilledModels,
                                                        expectedCarModels,
                                                        false) &&
                                        ModelsHaveState(carWireModels,
                                                        expectedCarModels,
                                                        true) &&
                                        ModelsHaveState(
                                                selectedCarFilledModels,
                                                static_cast<int>(
                                                        viewer.ellipsoidCount()),
                                                false) &&
                                        ModelsHaveState(
                                                selectedCarWireModels,
                                                static_cast<int>(
                                                        viewer.ellipsoidCount()),
                                                true);
                                root->setProperty(
                                        "renderMode",
                                        QStringLiteral("textured"));
                                QCoreApplication::processEvents();
                                const bool restoredState =
                                        !filled->property("visible").toBool() &&
                                        !wire->property("visible").toBool() &&
                                        VisibleModelCount(visualModels) ==
                                                initialVisibleVisualModels &&
                                        ModelsHaveState(carFilledModels,
                                                        expectedCarModels,
                                                        true) &&
                                        ModelsHaveState(carWireModels,
                                                        expectedCarModels,
                                                        false) &&
                                        ModelsHaveState(
                                                selectedCarFilledModels,
                                                static_cast<int>(
                                                        viewer.ellipsoidCount()),
                                                true) &&
                                        ModelsHaveState(
                                                selectedCarWireModels,
                                                static_cast<int>(
                                                        viewer.ellipsoidCount()),
                                                false);

                                auto *const whiteboardOverlay =
                                        qobject_cast<QQuickItem *>(
                                                root->findChild<QObject *>(
                                                        QStringLiteral(
                                                                "whiteboardOverlay")));
                                auto *const whiteboardViewport =
                                        qobject_cast<QQuickItem *>(
                                                root->findChild<QObject *>(
                                                        QStringLiteral(
                                                                "raceViewport")));
                                auto *const whiteboardToolbar =
                                        qobject_cast<QQuickItem *>(
                                                root->findChild<QObject *>(
                                                        QStringLiteral(
                                                                "whiteboardToolbar")));
                                QObject *const whiteboardModeToggle =
                                        root->findChild<QObject *>(
                                                QStringLiteral(
                                                        "whiteboardModeToggle"));
                                QObject *const whiteboardModeToggleLabel =
                                        root->findChild<QObject *>(
                                                QStringLiteral(
                                                        "whiteboardModeToggleLabel"));
                                QObject *const whiteboardInactiveListButton =
                                        root->findChild<QObject *>(
                                                QStringLiteral(
                                                        "whiteboardInactiveListButton"));
                                QObject *const whiteboardActiveListButton =
                                        root->findChild<QObject *>(
                                                QStringLiteral(
                                                        "whiteboardActiveListButton"));
                                QObject *const whiteboardPlaceButton =
                                        root->findChild<QObject *>(
                                                QStringLiteral(
                                                        "whiteboardPlaceButton"));
                                QObject *const whiteboardPickUpButton =
                                        root->findChild<QObject *>(
                                                QStringLiteral(
                                                        "whiteboardPickUpButton"));
                                QObject *const whiteboardDrawingInput =
                                        root->findChild<QObject *>(
                                                QStringLiteral(
                                                        "whiteboardDrawingInput"));
                                QObject *const whiteboardDrawingRepeater =
                                        root->findChild<QObject *>(
                                                QStringLiteral(
                                                        "whiteboardDrawingRepeater"));
                                QObject *const whiteboardPlaneRepeater =
                                        root->findChild<QObject *>(
                                                QStringLiteral(
                                                        "whiteboardPlaneRepeater"));
                                auto *const whiteboardPlaneView =
                                        qobject_cast<QQuickItem *>(
                                                root->findChild<QObject *>(
                                                        QStringLiteral(
                                                                "whiteboardPlaneView")));
                                QObject *const whiteboardDrawingList =
                                        root->findChild<QObject *>(
                                                QStringLiteral(
                                                        "whiteboardDrawingList"));
                                auto *const whiteboard =
                                        viewer.whiteboard();
                                whiteboard->setActive(true);
                                whiteboard->setTool(
                                        QStringLiteral("line"));
                                const bool whiteboardLineAdded =
                                        whiteboard->beginItem(0.15, 0.2) &&
                                        whiteboard->updateItem(0.7, 0.6) &&
                                        whiteboard->finishItem();
                                whiteboard->setTool(
                                        QStringLiteral("text"));
                                const bool whiteboardTextAdded =
                                        whiteboard->addText(
                                                0.24,
                                                0.3,
                                                QStringLiteral(
                                                        "Apex note")) == 1;
                                QCoreApplication::processEvents();
                                const QColor lightWhiteboardToolText =
                                        whiteboardOverlay != nullptr
                                        ? whiteboardOverlay
                                                  ->property(
                                                          "toolbarControlText")
                                                  .value<QColor>()
                                        : QColor();
                                controller.setDarkMode(true);
                                QCoreApplication::processEvents();
                                const QColor darkWhiteboardToolText =
                                        whiteboardOverlay != nullptr
                                        ? whiteboardOverlay
                                                  ->property(
                                                          "toolbarControlText")
                                                  .value<QColor>()
                                        : QColor();
                                controller.setDarkMode(false);
                                QCoreApplication::processEvents();
                                const bool whiteboardToolThemeContrast =
                                        whiteboardOverlay != nullptr &&
                                        lightWhiteboardToolText ==
                                                QColor(QStringLiteral(
                                                        "#202421")) &&
                                        darkWhiteboardToolText ==
                                                QColor(QStringLiteral(
                                                        "#f0f3ef")) &&
                                        whiteboardOverlay
                                                        ->property(
                                                                "toolbarControlText")
                                                        .value<QColor>() ==
                                                lightWhiteboardToolText;
                                const bool whiteboardActiveState =
                                        whiteboardOverlay != nullptr &&
                                        whiteboardToolbar != nullptr &&
                                        whiteboardModeToggle != nullptr &&
                                        whiteboardModeToggle
                                                ->property("checked")
                                                .toBool() &&
                                        whiteboardDrawingInput != nullptr &&
                                        whiteboardDrawingInput
                                                ->property("enabled")
                                                .toBool() &&
                                        whiteboardLineAdded &&
                                        whiteboardTextAdded &&
                                        whiteboard->count() == 2 &&
                                        whiteboardDrawingRepeater != nullptr &&
                                        whiteboardDrawingRepeater
                                                        ->property("count")
                                                        .toInt() == 2 &&
                                        VisibleModelCount(visualModels) ==
                                                initialVisibleVisualModels;
                                const auto buttonTextFits = [](QObject *button) {
                                    QObject *const content = button == nullptr
                                            ? nullptr
                                            : button->property("contentItem")
                                                      .value<QObject *>();
                                    return content != nullptr &&
                                            !content->property("truncated")
                                                     .toBool();
                                };
                                const bool whiteboardActionTextFits =
                                        buttonTextFits(whiteboardModeToggle) &&
                                        buttonTextFits(
                                                whiteboardActiveListButton) &&
                                        buttonTextFits(
                                                whiteboardPickUpButton) &&
                                        buttonTextFits(whiteboardPlaceButton);
                                auto *const compactCameraFocusToolbar =
                                        qobject_cast<QQuickItem *>(
                                                root->findChild<QObject *>(
                                                        QStringLiteral(
                                                                "cameraFocusToolbar")));
                                auto *const compactRaceViewerHeader =
                                        qobject_cast<QQuickItem *>(
                                                root->findChild<QObject *>(
                                                        QStringLiteral(
                                                                "raceViewerHeader")));
                                auto *const compactWhiteboardToolbar =
                                        qobject_cast<QQuickItem *>(
                                                root->findChild<QObject *>(
                                                        QStringLiteral(
                                                                "whiteboardToolbar")));
                                QVariant compactClosedX;
                                QVariant compactClosedTopMargin;
                                QVariant compactOpenX;
                                QVariant compactOpenTopMargin;
                                const bool compactLayoutInvoked =
                                        compactCameraFocusToolbar != nullptr &&
                                        QMetaObject::invokeMethod(
                                                compactCameraFocusToolbar,
                                                "layoutX",
                                                Q_RETURN_ARG(
                                                        QVariant,
                                                        compactClosedX),
                                                Q_ARG(QVariant,
                                                      QVariant(591.0)),
                                                Q_ARG(QVariant,
                                                      QVariant(false))) &&
                                        QMetaObject::invokeMethod(
                                                compactCameraFocusToolbar,
                                                "layoutTopMargin",
                                                Q_RETURN_ARG(
                                                        QVariant,
                                                        compactClosedTopMargin),
                                                Q_ARG(QVariant,
                                                      QVariant(591.0)),
                                                Q_ARG(QVariant,
                                                      QVariant(false))) &&
                                        QMetaObject::invokeMethod(
                                                compactCameraFocusToolbar,
                                                "layoutX",
                                                Q_RETURN_ARG(
                                                        QVariant,
                                                        compactOpenX),
                                                Q_ARG(QVariant,
                                                      QVariant(591.0)),
                                                Q_ARG(QVariant,
                                                      QVariant(true))) &&
                                        QMetaObject::invokeMethod(
                                                compactCameraFocusToolbar,
                                                "layoutTopMargin",
                                                Q_RETURN_ARG(
                                                        QVariant,
                                                        compactOpenTopMargin),
                                                Q_ARG(QVariant,
                                                      QVariant(591.0)),
                                                Q_ARG(QVariant,
                                                      QVariant(true)));
                                const qreal compactCameraClosedY =
                                        compactRaceViewerHeader != nullptr
                                        ? compactRaceViewerHeader->y() +
                                                  compactRaceViewerHeader
                                                          ->height() +
                                                  compactClosedTopMargin
                                                          .toReal()
                                        : 0;
                                const qreal compactCameraOpenY =
                                        compactRaceViewerHeader != nullptr
                                        ? compactRaceViewerHeader->y() +
                                                  compactRaceViewerHeader
                                                          ->height() +
                                                  compactOpenTopMargin.toReal()
                                        : 0;
                                const bool compactWhiteboardToolbarsSeparated =
                                        compactLayoutInvoked &&
                                        compactWhiteboardToolbar != nullptr &&
                                        compactCameraFocusToolbar != nullptr &&
                                        compactRaceViewerHeader != nullptr &&
                                        qAbs(compactClosedX.toReal() -
                                             (591.0 -
                                              compactCameraFocusToolbar
                                                      ->width() -
                                              12.0)) < 0.1 &&
                                        qAbs(compactOpenX.toReal() -
                                             compactClosedX.toReal()) < 0.1 &&
                                        qAbs(compactClosedTopMargin.toReal() -
                                             10.0) < 0.1 &&
                                        qAbs(compactOpenTopMargin.toReal() -
                                             10.0) < 0.1 &&
                                        compactWhiteboardToolbar->x() +
                                                        compactWhiteboardToolbar
                                                                ->width() +
                                                        7.9 <=
                                                compactCameraFocusToolbar->x();
                                if (whiteboardOverlay != nullptr) {
                                    whiteboardOverlay->setProperty(
                                            "drawingListOpen", true);
                                    QCoreApplication::processEvents();
                                }
                                const bool drawingListBelowWhiteboard =
                                        whiteboardDrawingList != nullptr &&
                                        compactWhiteboardToolbar != nullptr &&
                                        whiteboardDrawingList
                                                        ->property("y")
                                                        .toReal() >=
                                                compactWhiteboardToolbar->y() +
                                                        compactWhiteboardToolbar
                                                                ->height() +
                                                        7.9 &&
                                        qAbs(whiteboardDrawingList
                                                     ->property("x").toReal() -
                                             14.0) < 0.1 &&
                                        whiteboardDrawingList
                                                                ->property("x")
                                                                .toReal() +
                                                        whiteboardDrawingList
                                                                ->property("width")
                                                                .toReal() +
                                                        7.9 <=
                                                compactCameraFocusToolbar->x();
                                if (whiteboardOverlay != nullptr) {
                                    whiteboardOverlay->setProperty(
                                            "drawingListOpen", false);
                                    QCoreApplication::processEvents();
                                }
                                const bool compactWhiteboardListSeparated =
                                        compactLayoutInvoked &&
                                        drawingListBelowWhiteboard &&
                                        qAbs(compactCameraOpenY -
                                             compactCameraClosedY) < 0.1;
                                QVariant whiteboardCaptureValue;
                                const bool whiteboardViewCaptured =
                                        whiteboardViewport != nullptr &&
                                        QMetaObject::invokeMethod(
                                                whiteboardViewport,
                                                "captureWhiteboardView",
                                                Q_RETURN_ARG(
                                                        QVariant,
                                                        whiteboardCaptureValue));
                                const QVariantMap whiteboardCapture =
                                        whiteboardCaptureValue.toMap();
                                const bool whiteboardPlaced =
                                        whiteboardViewCaptured &&
                                        whiteboard->captureCurrentBoard(
                                                QStringLiteral(
                                                        "Smoke drawing"),
                                                whiteboardCapture) == 0;
                                QCoreApplication::processEvents();
                                QVariant pickedWhiteboard;
                                const bool whiteboardWorldPick =
                                        whiteboardPlaneView != nullptr &&
                                        QMetaObject::invokeMethod(
                                                whiteboardPlaneView,
                                                "pickBoard",
                                                Q_RETURN_ARG(
                                                        QVariant,
                                                        pickedWhiteboard),
                                                Q_ARG(
                                                        QVariant,
                                                        QVariant(
                                                                whiteboardPlaneView
                                                                        ->width()
                                                                * 0.5)),
                                                Q_ARG(
                                                        QVariant,
                                                        QVariant(
                                                                whiteboardPlaneView
                                                                        ->height()
                                                                * 0.5))) &&
                                        pickedWhiteboard.toInt() == 0;
                                const QVariantMap placedBoard =
                                        whiteboard->boards()
                                                .value(0)
                                                .toMap();
                                const QString planeObjectName =
                                        QStringLiteral("whiteboardPlane_")
                                        + placedBoard
                                                  .value(
                                                          QStringLiteral(
                                                                  "id"))
                                                  .toString();
                                QObject *const placedPlane =
                                        root->findChild<QObject *>(
                                                planeObjectName);
                                const QString planeSurfaceObjectName =
                                        QStringLiteral(
                                                "whiteboardPlaneSurface_")
                                        + placedBoard
                                                  .value(
                                                          QStringLiteral(
                                                                  "id"))
                                                  .toString();
                                QObject *const placedPlaneSurface =
                                        root->findChild<QObject *>(
                                                planeSurfaceObjectName);
                                auto *const viewerWindow =
                                        qobject_cast<QQuickWindow *>(root);
                                const double savedYaw =
                                        placedBoard.value(
                                                           QStringLiteral(
                                                                   "yaw"))
                                                .toDouble();
                                const double savedPitch =
                                        placedBoard.value(
                                                           QStringLiteral(
                                                                   "pitch"))
                                                .toDouble();
                                const double savedDistance =
                                        placedBoard.value(
                                                           QStringLiteral(
                                                                   "distance"))
                                                .toDouble();
                                const double savedFieldOfView =
                                        placedBoard.value(
                                                           QStringLiteral(
                                                                   "fieldOfView"))
                                                .toDouble();
                                if (whiteboardViewport != nullptr) {
                                    whiteboardViewport->setProperty(
                                            "orbitYaw", savedYaw + 19.0);
                                    whiteboardViewport->setProperty(
                                            "orbitPitch", savedPitch + 11.0);
                                    whiteboardViewport->setProperty(
                                            "orbitDistance",
                                            savedDistance + 7.0);
                                    whiteboardViewport->setProperty(
                                            "cameraFieldOfView", 71.0);
                                }
                                const bool whiteboardViewRestored =
                                        whiteboardViewport != nullptr &&
                                        QMetaObject::invokeMethod(
                                                whiteboardViewport,
                                                "restoreWhiteboardView",
                                                Q_ARG(
                                                        QVariant,
                                                        QVariant(
                                                                placedBoard)));
                                QCoreApplication::processEvents();
                                const auto waitForWhiteboardFrame = []() {
                                    QEventLoop loop;
                                    QTimer::singleShot(
                                            100, &loop, &QEventLoop::quit);
                                    loop.exec();
                                    QCoreApplication::processEvents();
                                };
                                waitForWhiteboardFrame();
                                const QImage transparentPlaneImage =
                                        viewerWindow != nullptr
                                        ? viewerWindow->grabWindow()
                                        : QImage();
                                const QPointF emptyPlanePoint =
                                        whiteboardPlaneView != nullptr
                                        ? whiteboardPlaneView->mapToScene(
                                                  QPointF(
                                                          whiteboardPlaneView
                                                                  ->width() *
                                                                  0.9,
                                                          whiteboardPlaneView
                                                                  ->height() *
                                                                  0.85))
                                        : QPointF();
                                const bool whiteboardHiddenForTransparency =
                                        whiteboard->setBoardVisible(0, false);
                                waitForWhiteboardFrame();
                                const QImage unobstructedViewerImage =
                                        viewerWindow != nullptr
                                        ? viewerWindow->grabWindow()
                                        : QImage();
                                const bool whiteboardReshownAfterTransparency =
                                        whiteboard->setBoardVisible(0, true);
                                waitForWhiteboardFrame();
                                const auto emptyRegionUnchanged =
                                        [](const QImage &withPlane,
                                           const QImage &withoutPlane,
                                           const QPointF &center) {
                                            if (withPlane.isNull() ||
                                                withoutPlane.isNull() ||
                                                withPlane.size() !=
                                                        withoutPlane.size()) {
                                                return false;
                                            }
                                            const QRect region(
                                                    qRound(center.x()) - 8,
                                                    qRound(center.y()) - 8,
                                                    17,
                                                    17);
                                            const QRect bounded =
                                                    region.intersected(
                                                            withPlane.rect());
                                            if (bounded.size() !=
                                                region.size()) {
                                                return false;
                                            }
                                            for (int y = bounded.top();
                                                 y <= bounded.bottom(); ++y) {
                                                for (int x = bounded.left();
                                                     x <= bounded.right();
                                                     ++x) {
                                                    const QColor first =
                                                            withPlane.pixelColor(
                                                                    x, y);
                                                    const QColor second =
                                                            withoutPlane
                                                                    .pixelColor(
                                                                            x,
                                                                            y);
                                                    if (first != second) {
                                                        return false;
                                                    }
                                                }
                                            }
                                            return true;
                                        };
                                const bool whiteboardTransparency =
                                        placedPlaneSurface != nullptr &&
                                        placedPlaneSurface
                                                        ->property("color")
                                                        .value<QColor>()
                                                        .alpha() == 0 &&
                                        whiteboardHiddenForTransparency &&
                                        whiteboardReshownAfterTransparency &&
                                        emptyRegionUnchanged(
                                                transparentPlaneImage,
                                                unobstructedViewerImage,
                                                emptyPlanePoint);
                                const auto projectedBounds =
                                        [whiteboardPlaneView]() {
                                            QVariant result;
                                            if (whiteboardPlaneView == nullptr ||
                                                !QMetaObject::invokeMethod(
                                                        whiteboardPlaneView,
                                                        "projectedPlaneBounds",
                                                        Q_RETURN_ARG(
                                                                QVariant,
                                                                result),
                                                        Q_ARG(
                                                                QVariant,
                                                                QVariant(0)))) {
                                                return QVariantMap{};
                                            }
                                            return result.toMap();
                                        };
                                const QVariantMap wideProjection =
                                        projectedBounds();
                                const double projectionTolerance = 2.0;
                                const bool exactWideProjection =
                                        wideProjection.value(
                                                              QStringLiteral(
                                                                      "valid"))
                                                .toBool() &&
                                        std::abs(
                                                wideProjection.value(
                                                                      QStringLiteral(
                                                                              "left"))
                                                                .toDouble()) <=
                                                projectionTolerance &&
                                        std::abs(
                                                wideProjection.value(
                                                                      QStringLiteral(
                                                                              "top"))
                                                                .toDouble() -
                                                52.0) <=
                                                projectionTolerance &&
                                        std::abs(
                                                wideProjection.value(
                                                                      QStringLiteral(
                                                                              "right"))
                                                                .toDouble() -
                                                whiteboardPlaneView->width()) <=
                                                projectionTolerance &&
                                        std::abs(
                                                wideProjection.value(
                                                                      QStringLiteral(
                                                                              "bottom"))
                                                                .toDouble() -
                                                whiteboardPlaneView->height()) <=
                                                projectionTolerance;
                                const int originalWindowWidth =
                                        viewerWindow != nullptr
                                        ? viewerWindow->width() : 0;
                                const int originalWindowHeight =
                                        viewerWindow != nullptr
                                        ? viewerWindow->height() : 0;
                                if (viewerWindow != nullptr) {
                                    viewerWindow->setWidth(1240);
                                    viewerWindow->setHeight(620);
                                    QCoreApplication::processEvents();
                                    QCoreApplication::processEvents();
                                }
                                const QVariantMap compactProjection =
                                        projectedBounds();
                                const bool exactCompactProjection =
                                        compactProjection.value(
                                                                 QStringLiteral(
                                                                         "valid"))
                                                .toBool() &&
                                        std::abs(
                                                compactProjection.value(
                                                                         QStringLiteral(
                                                                                 "left"))
                                                        .toDouble()) <=
                                                projectionTolerance &&
                                        std::abs(
                                                compactProjection.value(
                                                                         QStringLiteral(
                                                                                 "top"))
                                                        .toDouble() -
                                                52.0) <=
                                                projectionTolerance &&
                                        std::abs(
                                                compactProjection.value(
                                                                         QStringLiteral(
                                                                                 "right"))
                                                        .toDouble() -
                                                whiteboardPlaneView->width()) <=
                                                projectionTolerance &&
                                        std::abs(
                                                compactProjection.value(
                                                                         QStringLiteral(
                                                                                 "bottom"))
                                                        .toDouble() -
                                                whiteboardPlaneView->height()) <=
                                                projectionTolerance;
                                if (viewerWindow != nullptr) {
                                    viewerWindow->setWidth(
                                            originalWindowWidth);
                                    viewerWindow->setHeight(
                                            originalWindowHeight);
                                    QCoreApplication::processEvents();
                                    QCoreApplication::processEvents();
                                }
                                const bool whiteboardFreeMode =
                                        whiteboardViewport != nullptr &&
                                        QMetaObject::invokeMethod(
                                                whiteboardViewport,
                                                "enableFreeCamera");
                                QCoreApplication::processEvents();
                                const bool whiteboardDetached =
                                        whiteboardFreeMode &&
                                        whiteboardViewport
                                                        ->property(
                                                                "exactWhiteboardBoardIndex")
                                                        .toInt() == -1 &&
                                        whiteboardViewport
                                                        ->property(
                                                                "cameraFieldOfView")
                                                        .toDouble() == 55.0;
                                QVariant whiteboardPlaneFocusEnabled;
                                const bool whiteboardFreePlaneDetached =
                                        whiteboardDetached &&
                                        QMetaObject::invokeMethod(
                                                whiteboardViewport,
                                                "whiteboardPlaneFocusEnabled",
                                                Q_RETURN_ARG(
                                                        QVariant,
                                                        whiteboardPlaneFocusEnabled)) &&
                                        !whiteboardPlaneFocusEnabled.toBool();
                                const bool whiteboardTargetRefocused =
                                        whiteboardViewport != nullptr &&
                                        QMetaObject::invokeMethod(
                                                whiteboardViewport,
                                                "focusLastObject");
                                QCoreApplication::processEvents();
                                const bool whiteboardExactProjection =
                                        whiteboardViewRestored &&
                                        placedBoard.value(
                                                           QStringLiteral(
                                                                   "projectionVersion"))
                                                        .toInt() == 1 &&
                                        placedBoard.value(
                                                           QStringLiteral(
                                                                   "projection"))
                                                        .toString() ==
                                                QStringLiteral(
                                                        "perspective-vertical") &&
                                        whiteboardViewport
                                                        ->property(
                                                                "exactWhiteboardBoardIndex")
                                                        .toInt() == 0 &&
                                        whiteboardViewport
                                                        ->property(
                                                                "exactWhiteboardBoardId")
                                                        .toString() ==
                                                placedBoard.value(
                                                           QStringLiteral(
                                                                   "id"))
                                                        .toString() &&
                                        whiteboardDetached &&
                                        whiteboardTargetRefocused &&
                                        std::abs(
                                                whiteboardViewport
                                                        ->property("orbitYaw")
                                                        .toDouble() -
                                                savedYaw) <= 0.0001 &&
                                        std::abs(
                                                whiteboardViewport
                                                        ->property("orbitPitch")
                                                        .toDouble() -
                                                savedPitch) <= 0.0001 &&
                                        std::abs(
                                                whiteboardViewport
                                                        ->property(
                                                                "orbitDistance")
                                                        .toDouble() -
                                                savedDistance) <= 0.0001 &&
                                        std::abs(
                                                whiteboardViewport
                                                        ->property(
                                                                "cameraFieldOfView")
                                                        .toDouble() -
                                                savedFieldOfView) <= 0.0001 &&
                                        whiteboardFreePlaneDetached &&
                                        exactWideProjection &&
                                        exactCompactProjection;
                                const bool whiteboardPlaneState =
                                        whiteboardPlaced &&
                                        whiteboardExactProjection &&
                                        whiteboardTransparency &&
                                        whiteboardWorldPick &&
                                        whiteboard->count() == 0 &&
                                        whiteboard->boardCount() == 1 &&
                                        whiteboardPlaneRepeater != nullptr &&
                                        whiteboardPlaneRepeater
                                                        ->property("count")
                                                        .toInt() == 1 &&
                                        whiteboardDrawingRepeater
                                                        ->property("count")
                                                        .toInt() == 0 &&
                                        whiteboardDrawingList != nullptr &&
                                        placedPlane != nullptr &&
                                        whiteboard->setBoardVisible(0, false);
                                QCoreApplication::processEvents();
                                const bool planeInactiveWhenHidden =
                                        whiteboard->visibleBoards().isEmpty();
                                const int hiddenRepeaterCount =
                                        whiteboardPlaneRepeater
                                                ->property("count")
                                                .toInt();
                                const bool hiddenRole =
                                        !whiteboard->boards()
                                                 .value(0)
                                                 .toMap()
                                                 .value(
                                                         QStringLiteral(
                                                                 "visible"))
                                                 .toBool();
                                const bool whiteboardHiddenState =
                                        whiteboardPlaneState &&
                                        planeInactiveWhenHidden &&
                                        hiddenRepeaterCount == 0 &&
                                        hiddenRole &&
                                        whiteboard->boardCount() == 1;
                                const bool whiteboardShownAgain =
                                        whiteboard->setBoardVisible(0, true);
                                QCoreApplication::processEvents();
                                whiteboard->setActive(false);
                                QCoreApplication::processEvents();
                                const bool inactiveWhiteboardActionTextFits =
                                        buttonTextFits(whiteboardModeToggle) &&
                                        buttonTextFits(
                                                whiteboardInactiveListButton);
                                const QColor lightWhiteboardModeText =
                                        whiteboardModeToggleLabel != nullptr
                                        ? whiteboardModeToggleLabel
                                                  ->property("color")
                                                  .value<QColor>()
                                        : QColor();
                                controller.setDarkMode(true);
                                QCoreApplication::processEvents();
                                const QColor darkWhiteboardModeText =
                                        whiteboardModeToggleLabel != nullptr
                                        ? whiteboardModeToggleLabel
                                                  ->property("color")
                                                  .value<QColor>()
                                        : QColor();
                                controller.setDarkMode(false);
                                QCoreApplication::processEvents();
                                const bool whiteboardModeThemeContrast =
                                        whiteboardModeToggleLabel != nullptr &&
                                        lightWhiteboardModeText ==
                                                QColor(QStringLiteral(
                                                        "#202421")) &&
                                        darkWhiteboardModeText ==
                                                QColor(QStringLiteral(
                                                        "#f0f3ef")) &&
                                        whiteboardModeToggleLabel
                                                        ->property("color")
                                                        .value<QColor>() ==
                                                lightWhiteboardModeText;
                                const bool whiteboardIntegrated =
                                        whiteboardActiveState &&
                                        compactWhiteboardToolbarsSeparated &&
                                        compactWhiteboardListSeparated &&
                                        whiteboardToolThemeContrast &&
                                        whiteboardModeThemeContrast &&
                                        whiteboardHiddenState &&
                                        whiteboardShownAgain &&
                                        whiteboardPlaneRepeater
                                                        ->property("count")
                                                        .toInt() == 1 &&
                                        !whiteboardModeToggle
                                                 ->property("checked")
                                                 .toBool() &&
                                        !whiteboardDrawingInput
                                                 ->property("enabled")
                                                 .toBool() &&
                                        whiteboardActionTextFits &&
                                        inactiveWhiteboardActionTextFits &&
                                        whiteboard->count() == 0 &&
                                        whiteboard->boardCount() == 1 &&
                                        whiteboardDrawingRepeater
                                                        ->property("count")
                                                        .toInt() == 0 &&
                                        VisibleModelCount(visualModels) ==
                                                initialVisibleVisualModels;

                                QTemporaryDir imageExportDirectory;
                                const QString backgroundImagePath =
                                        imageExportDirectory.filePath(
                                                QStringLiteral(
                                                        "board-background.png"));
                                QVariant backgroundExportStarted;
                                const bool backgroundExportInvoked =
                                        imageExportDirectory.isValid() &&
                                        whiteboardViewport != nullptr &&
                                        whiteboard->setBoardVisible(0, false) &&
                                        QMetaObject::invokeMethod(
                                                whiteboardViewport,
                                                "exportWhiteboardBackground",
                                                Q_RETURN_ARG(
                                                        QVariant,
                                                        backgroundExportStarted),
                                                Q_ARG(QVariant, QVariant(0)),
                                                Q_ARG(
                                                        QVariant,
                                                                QVariant(
                                                                        QUrl::fromLocalFile(
                                                                        backgroundImagePath))));
                                const bool exportStartedState =
                                        backgroundExportStarted.toBool();
                                const bool exportBusyState =
                                        whiteboardViewport
                                                ->property(
                                                        "exportingWhiteboardImage")
                                                .toBool();
                                auto *const exportOverlay =
                                        qobject_cast<QQuickItem *>(
                                                root->findChild<QObject *>(
                                                        QStringLiteral(
                                                                "whiteboardOverlay")));
                                auto *const exportHeader =
                                        qobject_cast<QQuickItem *>(
                                                root->findChild<QObject *>(
                                                        QStringLiteral(
                                                                "raceViewerHeader")));
                                auto *const exportDock =
                                        qobject_cast<QQuickItem *>(
                                                root->findChild<QObject *>(
                                                        QStringLiteral(
                                                                "playbackDock")));
                                const bool exportOverlayHidden =
                                        exportOverlay != nullptr &&
                                        !exportOverlay->isVisible();
                                const bool exportHeaderHidden =
                                        exportHeader != nullptr &&
                                        !exportHeader->isVisible();
                                const bool exportDockHidden =
                                        exportDock != nullptr &&
                                        !exportDock->isVisible();
                                const bool exportPlaneMode =
                                        whiteboardPlaneView != nullptr &&
                                        whiteboardPlaneView
                                                ->property("exportMode")
                                                .toBool();
                                const bool exportForcedPlane =
                                        whiteboardPlaneRepeater != nullptr &&
                                        whiteboardPlaneRepeater
                                                        ->property("count")
                                                        .toInt() == 1;
                                const bool exportCaptureState =
                                        backgroundExportInvoked &&
                                        exportStartedState &&
                                        exportBusyState &&
                                        exportOverlayHidden &&
                                        exportHeaderHidden &&
                                        exportDockHidden &&
                                        exportPlaneMode &&
                                        exportForcedPlane;
                                QEventLoop imageExportLoop;
                                QTimer::singleShot(
                                        800,
                                        &imageExportLoop,
                                        &QEventLoop::quit);
                                imageExportLoop.exec();
                                const QImage backgroundImage(
                                        backgroundImagePath);
                                auto *const postExportViewport =
                                        qobject_cast<QQuickItem *>(
                                                root->findChild<QObject *>(
                                                        QStringLiteral(
                                                                "raceViewport")));
                                auto *const postExportPlaneView =
                                        qobject_cast<QQuickItem *>(
                                                root->findChild<QObject *>(
                                                        QStringLiteral(
                                                                "whiteboardPlaneView")));
                                const bool fullBackgroundExportValid =
                                        exportCaptureState &&
                                        postExportViewport != nullptr &&
                                        !postExportViewport
                                                 ->property(
                                                         "exportingWhiteboardImage")
                                                 .toBool() &&
                                        postExportPlaneView != nullptr &&
                                        !postExportPlaneView
                                                 ->property("exportMode")
                                                 .toBool() &&
                                        postExportPlaneView
                                                        ->property(
                                                                "forcedBoardIndex")
                                                        .toInt() == -1 &&
                                        QFileInfo(backgroundImagePath).size() >
                                                0 &&
                                        !backgroundImage.isNull() &&
                                        backgroundImage.width() ==
                                                qRound(
                                                        postExportViewport
                                                                ->width()) &&
                                        backgroundImage.height() ==
                                                qRound(
                                                        postExportViewport
                                                                ->height()) &&
                                        whiteboard->operationMessage().contains(
                                                QStringLiteral(
                                                        "with background exported"));
                                const bool whiteboardVisibilityRestored =
                                        whiteboard->setBoardVisible(0, true);
                                whiteboard->setActive(true);
                                const bool pickupViewRestored =
                                        whiteboardViewport != nullptr &&
                                        QMetaObject::invokeMethod(
                                                whiteboardViewport,
                                                "restoreWhiteboardView",
                                                Q_ARG(
                                                        QVariant,
                                                        QVariant(placedBoard)));
                                QCoreApplication::processEvents();
                                const QVector3D cameraBeforePickup =
                                        viewCamera != nullptr
                                        ? viewCamera
                                                  ->property("scenePosition")
                                                  .value<QVector3D>()
                                        : QVector3D();
                                const bool pickupInvoked =
                                        pickupViewRestored &&
                                        whiteboardPickUpButton != nullptr &&
                                        whiteboardPickUpButton
                                                ->property("enabled").toBool() &&
                                        QMetaObject::invokeMethod(
                                                whiteboardPickUpButton,
                                                "clicked");
                                QCoreApplication::processEvents();
                                QCoreApplication::processEvents();
                                const QVector3D cameraAfterPickup =
                                        viewCamera != nullptr
                                        ? viewCamera
                                                  ->property("scenePosition")
                                                  .value<QVector3D>()
                                        : QVector3D();
                                const bool pickupPreservesFreeCamera =
                                        pickupInvoked &&
                                        whiteboardViewport
                                                ->property("freeCamera")
                                                .toBool() &&
                                        (cameraAfterPickup - cameraBeforePickup)
                                                        .length() < 0.001f &&
                                        whiteboard->boardCount() == 0 &&
                                        whiteboard->count() == 2;

                                if (viewer.loaded() &&
                                    !viewer.loading()) {
                                    QObject *const currentRoot =
                                            engine.rootObjects().isEmpty()
                                            ? nullptr
                                            : engine.rootObjects().front();
                                    const auto invokeCurrentManualKey =
                                            [currentRoot](
                                                    Qt::Key key,
                                                    bool active,
                                                    bool autoRepeat = false) {
                                                if (currentRoot == nullptr) {
                                                    return false;
                                                }
                                                QVariant handled;
                                                const bool invoked =
                                                        QMetaObject::invokeMethod(
                                                                currentRoot,
                                                                "handleManualActionKey",
                                                                Q_RETURN_ARG(
                                                                        QVariant,
                                                                        handled),
                                                                Q_ARG(
                                                                        QVariant,
                                                                        QVariant::fromValue(
                                                                                static_cast<int>(
                                                                                        key))),
                                                                Q_ARG(
                                                                        QVariant,
                                                                        QVariant::fromValue(
                                                                                active)),
                                                                Q_ARG(
                                                                        QVariant,
                                                                        QVariant::fromValue(
                                                                                autoRepeat)));
                                                return invoked &&
                                                        handled.toBool();
                                            };
                                    QObject *const currentViewport =
                                            currentRoot->findChild<QObject *>(
                                                    QStringLiteral(
                                                            "raceViewport"));
                                    auto *const currentManualInputFocus =
                                            qobject_cast<QQuickItem *>(
                                                    currentRoot->findChild<QObject *>(
                                                            QStringLiteral(
                                                                    "manualInputFocus")));
                                    const auto sendCameraKeyPhase =
                                            [currentManualInputFocus](
                                                    QEvent::Type type,
                                                    int key,
                                                    Qt::KeyboardModifiers modifiers) {
                                                if (currentManualInputFocus ==
                                                    nullptr) {
                                                    return false;
                                                }
                                                QKeyEvent event(type, key, modifiers);
                                                QCoreApplication::sendEvent(
                                                        currentManualInputFocus,
                                                        &event);
                                                QCoreApplication::processEvents();
                                                return event.isAccepted();
                                            };
                                    const auto sendCameraKey =
                                            [&sendCameraKeyPhase](
                                                    int key,
                                                    Qt::KeyboardModifiers modifiers) {
                                                return sendCameraKeyPhase(
                                                               QEvent::KeyPress,
                                                               key,
                                                               modifiers) &&
                                                        sendCameraKeyPhase(
                                                               QEvent::KeyRelease,
                                                               key,
                                                               modifiers);
                                            };
                                    if (currentViewport != nullptr &&
                                        currentManualInputFocus != nullptr) {
                                        QMetaObject::invokeMethod(
                                                currentManualInputFocus,
                                                "forceActiveFocus");
                                        QCoreApplication::processEvents();
                                        const bool rowNear =
                                                sendCameraKey(
                                                        Qt::Key_2,
                                                        Qt::NoModifier) &&
                                                viewer.cameraPreset() == 2 &&
                                                !currentViewport
                                                         ->property("freeCamera")
                                                         .toBool() &&
                                                currentViewport
                                                        ->property(
                                                                "carCameraActive")
                                                        .toBool();
                                        const bool keypadInternal =
                                                sendCameraKey(
                                                        Qt::Key_3,
                                                        Qt::KeypadModifier) &&
                                                viewer.cameraPreset() == 3 &&
                                                viewer.hideSelectedCar() &&
                                                !currentViewport
                                                         ->property("freeCamera")
                                                         .toBool();
                                        const bool keypadFar =
                                                sendCameraKey(
                                                        Qt::Key_1,
                                                        Qt::KeypadModifier) &&
                                                viewer.cameraPreset() == 1 &&
                                                !viewer.hideSelectedCar() &&
                                                !currentViewport
                                                         ->property("freeCamera")
                                                         .toBool();
                                        const bool firstFree =
                                                sendCameraKey(
                                                        Qt::Key_7,
                                                        Qt::NoModifier) &&
                                                currentViewport
                                                        ->property("freeCamera")
                                                        .toBool();
                                        const QVector3D repeatedFreePosition =
                                                currentViewport
                                                        ->property(
                                                                "freeCameraPosition")
                                                        .value<QVector3D>();
                                        const bool repeatedFree =
                                                sendCameraKey(
                                                        Qt::Key_7,
                                                        Qt::NoModifier) &&
                                                currentViewport
                                                        ->property("freeCamera")
                                                        .toBool() &&
                                                currentViewport
                                                                ->property(
                                                                        "freeCameraPosition")
                                                                .value<QVector3D>() ==
                                                        repeatedFreePosition;
                                        viewer.setCameraPreset(1);
                                        const bool restoredFar =
                                                QMetaObject::invokeMethod(
                                                        currentViewport,
                                                        "focusCurrentCar") &&
                                                viewer.cameraPreset() == 1 &&
                                                !currentViewport
                                                         ->property("freeCamera")
                                                         .toBool();
                                        cameraShortcutKeysValid =
                                                rowNear && keypadInternal &&
                                                keypadFar && firstFree &&
                                                repeatedFree && restoredFar;
                                    }
                                    QObject *const currentManualDriveButton =
                                            currentRoot->findChild<QObject *>(
                                                    QStringLiteral(
                                                            "manualDriveButton"));
                                    const bool driveClicked =
                                            currentManualDriveButton != nullptr &&
                                            QMetaObject::invokeMethod(
                                                    currentManualDriveButton,
                                                    "clicked");
                                    QCoreApplication::processEvents();
                                    const bool driveFocusedRealCar =
                                            driveClicked &&
                                            viewer.manualDriving() &&
                                            viewer.selectedRunId() ==
                                                    QStringLiteral("manual") &&
                                            currentViewport != nullptr &&
                                            !currentViewport
                                                     ->property("freeCamera")
                                                     .toBool() &&
                                            currentViewport
                                                            ->property(
                                                                    "cameraFocusMode")
                                                            .toString() ==
                                                    QStringLiteral("preset") &&
                                            currentViewport
                                                    ->property(
                                                            "carCameraActive")
                                                    .toBool();
                                    QCoreApplication::processEvents();
                                    QObject *const currentSelectedCarRoot =
                                            currentRoot->findChild<QObject *>(
                                                    QStringLiteral(
                                                            "selectedRunCarRoot"));
                                    const QList<QObject *>
                                            currentSelectedFilledModels =
                                                    currentRoot
                                                            ->findChildren<
                                                                    QObject *>(
                                                                    QStringLiteral(
                                                                            "selectedRunCarFilledModel"));
                                    selectedCarRenderingValid =
                                            driveFocusedRealCar &&
                                            currentSelectedCarRoot != nullptr &&
                                            currentSelectedCarRoot
                                                    ->property("visible")
                                                    .toBool() &&
                                            (currentSelectedCarRoot
                                                             ->property(
                                                                     "position")
                                                             .value<QVector3D>() -
                                             viewer.carPosition())
                                                            .lengthSquared() <
                                                    0.0000001f &&
                                            ModelsHaveState(
                                                    currentSelectedFilledModels,
                                                    static_cast<int>(
                                                            viewer.ellipsoidCount()),
                                                    true);
                                    if (driveFocusedRealCar &&
                                        currentViewport != nullptr &&
                                        currentManualInputFocus != nullptr) {
                                        const bool switchedToFree =
                                                sendCameraKey(
                                                        Qt::Key_7,
                                                        Qt::NoModifier) &&
                                                currentViewport
                                                        ->property("freeCamera")
                                                        .toBool() &&
                                                !viewer.manualLeft() &&
                                                !viewer.manualRight() &&
                                                !viewer.manualAccelerate() &&
                                                !viewer.manualBrake();
                                        const QVector3D positionBeforeMovement =
                                                currentViewport
                                                        ->property(
                                                                "freeCameraPosition")
                                                        .value<QVector3D>();
                                        const bool forwardPressed =
                                                sendCameraKeyPhase(
                                                        QEvent::KeyPress,
                                                        Qt::Key_W,
                                                        Qt::NoModifier) &&
                                                currentViewport
                                                        ->property(
                                                                "freeMoveForward")
                                                        .toBool() &&
                                                !viewer.manualAccelerate();
                                        const double movementStart =
                                                currentViewport
                                                        ->property(
                                                                "freeMoveStartedAt")
                                                        .toDouble();
                                        QVariant movementStepped;
                                        const bool movementAdvanced =
                                                movementStart > 0.0 &&
                                                QMetaObject::invokeMethod(
                                                        currentViewport,
                                                        "stepFreeCameraMovement",
                                                        Q_RETURN_ARG(
                                                                QVariant,
                                                                movementStepped),
                                                        Q_ARG(
                                                                QVariant,
                                                                QVariant(
                                                                        movementStart +
                                                                        1000.0))) &&
                                                movementStepped.toBool() &&
                                                (currentViewport
                                                                 ->property(
                                                                         "freeCameraPosition")
                                                                 .value<QVector3D>() -
                                                 positionBeforeMovement)
                                                                .lengthSquared() >
                                                        0.000001f &&
                                                !viewer.manualAccelerate();
                                        const bool forwardReleased =
                                                sendCameraKeyPhase(
                                                        QEvent::KeyRelease,
                                                        Qt::Key_W,
                                                        Qt::NoModifier) &&
                                                !currentViewport
                                                         ->property(
                                                                 "freeMoveForward")
                                                         .toBool() &&
                                                !viewer.manualAccelerate();
                                        const bool arrowPressed =
                                                sendCameraKeyPhase(
                                                        QEvent::KeyPress,
                                                        Qt::Key_Left,
                                                        Qt::NoModifier) &&
                                                currentViewport
                                                        ->property(
                                                                "freeMoveLeft")
                                                        .toBool() &&
                                                !viewer.manualLeft();
                                        const QVector3D arrowStartPosition =
                                                currentViewport
                                                        ->property(
                                                                "freeCameraPosition")
                                                        .value<QVector3D>();
                                        const double arrowMovementStart =
                                                currentViewport
                                                        ->property(
                                                                "freeMoveStartedAt")
                                                        .toDouble();
                                        QVariant arrowMovementStepped;
                                        const bool arrowMovedSideways =
                                                arrowPressed &&
                                                arrowMovementStart > 0.0 &&
                                                QMetaObject::invokeMethod(
                                                        currentViewport,
                                                        "stepFreeCameraMovement",
                                                        Q_RETURN_ARG(
                                                                QVariant,
                                                                arrowMovementStepped),
                                                        Q_ARG(
                                                                QVariant,
                                                                QVariant(
                                                                        arrowMovementStart +
                                                                        1000.0))) &&
                                                arrowMovementStepped.toBool() &&
                                                (currentViewport
                                                                 ->property(
                                                                         "freeCameraPosition")
                                                                 .value<QVector3D>() -
                                                 arrowStartPosition)
                                                                .lengthSquared() >
                                                        0.000001f;
                                        const bool arrowReleased =
                                                sendCameraKeyPhase(
                                                        QEvent::KeyRelease,
                                                        Qt::Key_Left,
                                                        Qt::NoModifier) &&
                                                !currentViewport
                                                         ->property(
                                                                 "freeMoveLeft")
                                                         .toBool() &&
                                                !viewer.manualLeft();
                                        freeCameraManualRoutingValid =
                                                switchedToFree &&
                                                forwardPressed &&
                                                movementAdvanced &&
                                                forwardReleased &&
                                                arrowPressed &&
                                                arrowMovedSideways &&
                                                arrowReleased;
                                        viewer.setCameraPreset(1);
                                        QMetaObject::invokeMethod(
                                                currentViewport,
                                                "focusCurrentCar");
                                        QCoreApplication::processEvents();
                                    }
                                    const bool enterRespawn =
                                            invokeCurrentManualKey(
                                                    Qt::Key_Enter, true);
                                    QEventLoop enterRespawnLoop;
                                    QTimer::singleShot(
                                            30,
                                            &enterRespawnLoop,
                                            &QEventLoop::quit);
                                    enterRespawnLoop.exec();
                                    const bool enterRespawnExecuted =
                                            viewer.currentInputScript().count(
                                                    QStringLiteral(
                                                            "press enter")) ==
                                                    1;
                                    const bool releaseDidNotRepeat =
                                            invokeCurrentManualKey(
                                                    Qt::Key_Enter, false) &&
                                            viewer.currentInputScript().count(
                                                    QStringLiteral(
                                                            "press enter")) ==
                                                    1;
                                    const bool autoRepeatIgnored =
                                            invokeCurrentManualKey(
                                                    Qt::Key_Enter,
                                                    true,
                                                    true) &&
                                            viewer.currentInputScript().count(
                                                    QStringLiteral(
                                                            "press enter")) ==
                                                    1;
                                    const bool deleteGaveUp =
                                            invokeCurrentManualKey(
                                                    Qt::Key_Delete, true) &&
                                            viewer.manualDriving() &&
                                            viewer.tickCount() == 1 &&
                                            viewer.timeMs() == 0 &&
                                            !viewer.currentInputScript()
                                                     .contains(
                                                             QStringLiteral(
                                                                     "press enter"));
                                    const bool backspaceRespawn =
                                            invokeCurrentManualKey(
                                                    Qt::Key_Backspace,
                                                    true);
                                    QEventLoop backspaceRespawnLoop;
                                    QTimer::singleShot(
                                            30,
                                            &backspaceRespawnLoop,
                                            &QEventLoop::quit);
                                    backspaceRespawnLoop.exec();
                                    const bool backspaceRespawnExecuted =
                                            viewer.currentInputScript().count(
                                                    QStringLiteral(
                                                            "press enter")) ==
                                                    1;
                                    viewer.stopManualDrive();
                                    QCoreApplication::processEvents();
                                    manualActionKeysValid =
                                            cameraShortcutKeysValid &&
                                            freeCameraManualRoutingValid &&
                                            selectedCarRenderingValid &&
                                            driveFocusedRealCar &&
                                            enterRespawn &&
                                            enterRespawnExecuted &&
                                            releaseDidNotRepeat &&
                                            autoRepeatIgnored &&
                                            deleteGaveUp &&
                                            backspaceRespawn &&
                                            backspaceRespawnExecuted &&
                                            !viewer.manualDriving();
                                }

                                completed = true;
                                exitCode =
                                        geometryAttached && rootsVisible &&
                                                        carDelegatesStable &&
                                                        initialModelState &&
                                                        bestSelectedInitially &&
                                                        onlyBestSelected &&
                                                        neutralModeState &&
                                                        collisionModeState &&
                                                        materialDebugState &&
                                                        wireframeState &&
                                                        restoredState &&
                                                        rayTracingModeValid &&
                                                        optimizedRenderState &&
                                                        daylightEnvironment &&
                                                        loadedSceneThemeInvariant &&
                                                        trajectoryPreviewUiValid &&
                                                        improvementTrajectoryUiValid &&
                                                        clearPreviewTrajectoriesUiValid &&
                                                        checkpointSplitOverlayUiValid &&
                                                        allTrajectoryModelsRendered &&
                                                        copyCurrentRaceInputsValid &&
                                                        editorStructure &&
                                                        cameraShortcutKeysValid &&
                                                        manualActionKeysValid &&
                                                        whiteboardIntegrated &&
                                                        fullBackgroundExportValid &&
                                                        whiteboardVisibilityRestored &&
                                                        pickupPreservesFreeCamera
                                                ? 0
                                                : 1;
                                if (exitCode != 0) {
                                    std::cerr
                                            << "viewer run switching failed: "
                                               "geometry="
                                            << geometryAttached
                                            << ", roots=" << carRoots.size()
                                            << ", filledModels="
                                            << carFilledModels.size()
                                            << ", filledMaterials="
                                            << carFilledMaterials.size()
                                            << ", wireModels="
                                            << carWireModels.size()
                                            << ", visualModels="
                                            << visualModels.size()
                                            << ", visualInstances="
                                            << viewer.visualInstances().size()
                                            << ", visualTriangles="
                                            << viewer.visualTriangleCount()
                                            << ", visualMeshes="
                                            << viewer.visualMeshCount()
                                            << ", materials="
                                            << viewer.materialCount()
                                            << ", expectedModels="
                                            << expectedCarModels
                                            << ", initial=" << initialModelState
                                            << ", bestInitial="
                                            << bestSelectedInitially
                                            << ", onlyBestSelected="
                                            << onlyBestSelected
                                            << ", copyCurrentRaceInputs="
                                            << copyCurrentRaceInputsValid
                                            << ", trajectoryPreview="
                                            << trajectoryPreviewUiValid
                                            << "/"
                                            << viewer.trajectoryCount()
                                            << "/"
                                            << improvementTrajectoryUiValid
                                            << "/clear="
                                            << clearPreviewTrajectoriesUiValid
                                            << "/splits="
                                            << checkpointSplitOverlayUiValid
                                            << "/"
                                            << allTrajectoryModelsRendered
                                            << "/"
                                            << allTrajectoryModels.size()
                                            << "/"
                                            << allRayTracingTrajectoryModels
                                                       .size()
                                            << "/"
                                            << (copyCurrentRaceInputsButton !=
                                                                nullptr
                                                        ? copyCurrentRaceInputsButton
                                                                  ->property(
                                                                          "enabled")
                                                                  .toBool()
                                                        : false)
                                            << " script='"
                                            << controller.baseInputScript()
                                                       .toStdString()
                                            << "'"
                                            << ", collisionMode="
                                            << collisionModeState
                                            << ", neutralMode="
                                            << neutralModeState
                                            << ", materialDebug="
                                            << materialDebugState
                                            << ", wireframe=" << wireframeState
                                            << ", restored=" << restoredState
                                            << ", whiteboard="
                                            << whiteboardActiveState << "/"
                                            << whiteboardIntegrated << "("
                                            << compactWhiteboardToolbarsSeparated
                                            << "/"
                                            << compactWhiteboardListSeparated
                                            << "/"
                                            << whiteboardActionTextFits
                                            << "/w="
                                            << (whiteboardToolbar
                                                        ? whiteboardToolbar
                                                                  ->width()
                                                        : -1.0)
                                            << ")/"
                                            << whiteboardToolThemeContrast << "/"
                                            << whiteboardModeThemeContrast << "/"
                                            << lightWhiteboardToolText
                                                       .name()
                                                       .toStdString()
                                            << "/"
                                            << darkWhiteboardToolText
                                                       .name()
                                                       .toStdString()
                                            << "/"
                                            << whiteboard->count()
                                            << "/placed="
                                            << whiteboardPlaced
                                            << "/plane="
                                            << whiteboardPlaneState
                                            << "/worldPick="
                                            << whiteboardWorldPick
                                            << "/hidden="
                                            << whiteboardHiddenState
                                            << "/boards="
                                            << whiteboard->boardCount()
                                            << "/repeater="
                                            << (whiteboardPlaneRepeater
                                                        ? whiteboardPlaneRepeater
                                                                  ->property(
                                                                          "count")
                                                                  .toInt()
                                                        : -1)
                                            << "/planeObject="
                                            << (placedPlane != nullptr)
                                            << "/hiddenObject="
                                            << planeInactiveWhenHidden
                                            << "/hiddenRepeater="
                                            << hiddenRepeaterCount
                                            << "/hiddenRole="
                                            << hiddenRole
                                            << "/modelVisible="
                                            << whiteboard->boards()
                                                       .value(0)
                                                       .toMap()
                                                       .value(
                                                               QStringLiteral(
                                                                       "visible"))
                                                       .toBool()
                                            << "/shownAgain="
                                            << whiteboardShownAgain
                                            << "/pickup="
                                            << pickupPreservesFreeCamera
                                            << "/pickupInvoked="
                                            << pickupInvoked
                                            << "/pickupView="
                                            << pickupViewRestored
                                            << "/free="
                                            << (whiteboardViewport
                                                        ? whiteboardViewport
                                                                  ->property(
                                                                          "freeCamera")
                                                                  .toBool()
                                                        : false)
                                            << "/cameraDelta="
                                            << (cameraAfterPickup -
                                                cameraBeforePickup)
                                                       .length()
                                            << "/imageExport="
                                            << fullBackgroundExportValid
                                            << "/captureState="
                                            << exportCaptureState
                                            << "/file="
                                            << QFileInfo(
                                                       backgroundImagePath)
                                                       .size()
                                            << "/captureBits="
                                            << backgroundExportInvoked << "/"
                                            << exportStartedState << "/"
                                            << exportBusyState << "/"
                                            << exportOverlayHidden << "/"
                                            << exportHeaderHidden << "/"
                                            << exportDockHidden << "/"
                                            << exportPlaneMode << "/"
                                            << exportForcedPlane
                                            << "/image="
                                            << backgroundImage.width() << "x"
                                            << backgroundImage.height()
                                            << "/viewport="
                                            << (postExportViewport
                                                        ? postExportViewport
                                                                  ->width()
                                                        : -1.0)
                                            << "x"
                                            << (postExportViewport
                                                        ? postExportViewport
                                                                  ->height()
                                                        : -1.0)
                                            << "/message="
                                            << whiteboard->operationMessage()
                                                       .toStdString()
                                            << ", optimizedRenderState="
                                            << optimizedRenderState
                                            << ", daylightEnvironment="
                                            << daylightEnvironment
                                            << ", themeSceneInvariant="
                                            << loadedSceneThemeInvariant
                                            << ", clipNear="
                                            << (viewCamera
                                                        ? viewCamera
                                                                  ->property(
                                                                          "clip"
                                                                          "Nea"
                                                                          "r")
                                                                  .toDouble()
                                                        : -1.0)
                                            << ", clipFar="
                                            << (viewCamera
                                                        ? viewCamera
                                                                  ->property(
                                                                          "clip"
                                                                          "Far")
                                                                  .toDouble()
                                                        : -1.0)
                                            << ", mainCastsShadow="
                                            << (mainMapLight &&
                                                mainMapLight
                                                        ->property(
                                                                "castsShadow")
                                                        .toBool())
                                            << ", editorStructure="
                                            << editorStructure
                                            << ", cameraShortcutKeys="
                                            << cameraShortcutKeysValid
                                            << ", freeCameraManualRouting="
                                            << freeCameraManualRoutingValid
                                            << ", manualActionKeys="
                                            << manualActionKeysValid
                                            << ", selectedCar="
                                            << selectedCarRenderingValid << '\n';
                                }
                                static_cast<void>(quickWindow);
                                application.quit();
                            });
                });
            });

    QTimer::singleShot(170000, &application, [&]() {
        if (completed) {
            return;
        }
        completed = true;
        std::cerr << "viewer QML smoke test timed out\n";
        application.quit();
    });

    viewer.loadMap(QString::fromLocal8Bit(argv[1]),
                   QString::fromLocal8Bit(argv[2]));
    application.exec();
    return exitCode;
}
