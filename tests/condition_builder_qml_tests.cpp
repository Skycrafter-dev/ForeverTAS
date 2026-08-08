#include "app/condition_editor_model.h"

#include <QColor>
#include <QFont>
#include <QGuiApplication>
#include <QImage>
#include <QMetaObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTest>
#include <QUrl>
#include <QVariant>

#include <cmath>
#include <iostream>

namespace {

bool Check(bool condition, const char *message) {
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

void PrintErrors(const QQmlComponent &component) {
    for (const QQmlError &error : component.errors()) {
        std::cerr << error.toString().toStdString() << '\n';
    }
}

QQuickItem *FindVisualItem(QQuickItem *root, const QString &objectName) {
    if (root == nullptr)
        return nullptr;
    if (root->objectName() == objectName)
        return root;
    for (QQuickItem *const child : root->childItems()) {
        if (QQuickItem *const match = FindVisualItem(child, objectName)) {
            return match;
        }
    }
    return nullptr;
}

void CaptureIfRequested(QQuickWindow *window, const char *environmentName) {
    if (window == nullptr)
        return;
    const QByteArray path = qgetenv(environmentName);
    if (!path.isEmpty()) {
        window->grabWindow().save(QString::fromLocal8Bit(path));
    }
}

bool TestConditionBuilderLoads() {
    QQmlEngine engine;
    forevertas::app::ConditionEditorModel assistance;
    engine.rootContext()->setContextProperty(QStringLiteral("realAssistance"),
                                             &assistance);
    QQmlComponent component(&engine);
    static const char qml[] = R"QML(
import QtQuick
import QtQuick.Controls
import "../qml" as FT

ApplicationWindow {
    id: window
    width: 900
    height: 700
    visible: false

    property QtObject mockScriptStore: QtObject {
        property string lastName: ""
        property string lastContent: ""

        function files(kind) {
            return [{"name": "saved.txt", "size": 18, "modified": ""}]
        }
        function save(kind, name, content, overwrite) {
            lastName = name
            lastContent = content
            return {"ok": true, "name": name + ".txt", "message": "Saved."}
        }
        function load(kind, name) {
            return {"ok": true, "name": name,
                    "content": "car.speed >= 123", "message": "Loaded."}
        }
    }

    property QtObject mockController: QtObject {
        property string conditionScript: "car.speed >= 0"
        property string conditionGateMode: "and"
        property bool running: false
        property string evaluationTargetId: "point-target"
        property var scriptFileStore: window.mockScriptStore
    }

    FT.ConditionBuilder {
        id: builder
        objectName: "qmlTestConditionBuilder"
        width: 470
        controller: window.mockController
        assistance: realAssistance
        onScriptEdited: function(text) {
            window.mockController.conditionScript = text
        }
        onGateModeEdited: function(mode) {
            window.mockController.conditionGateMode = mode
        }
    }
}
)QML";
    const QUrl baseUrl = QUrl::fromLocalFile(QStringLiteral(
            FOREVERTAS_SOURCE_DIR "/tests/condition_builder_inline.qml"));
    component.setData(qml, baseUrl);
    if (component.isError())
        PrintErrors(component);
    QObject *const root = component.create();
    if (root == nullptr)
        PrintErrors(component);
    bool okay = Check(root != nullptr,
                      "ConditionBuilder QML component did not instantiate");
    if (root == nullptr)
        return false;

    QCoreApplication::processEvents();
    QObject *const builder = root->findChild<QObject *>(
            QStringLiteral("qmlTestConditionBuilder"));
    QObject *const textArea = root->findChild<QObject *>(
            QStringLiteral("conditionScriptTextArea"));
    QObject *const completion = root->findChild<QObject *>(
            QStringLiteral("conditionCompletionPopup"));
    QObject *const completionNoMatches = root->findChild<QObject *>(
            QStringLiteral("conditionCompletionNoMatches"));
    QObject *const completionNoMatchesHint = root->findChild<QObject *>(
            QStringLiteral("conditionCompletionNoMatchesHint"));
    QObject *const symbolHover = root->findChild<QObject *>(
            QStringLiteral("conditionSymbolHoverPopup"));
    QObject *const symbolHoverTimer = root->findChild<QObject *>(
            QStringLiteral("conditionSymbolHoverTimer"));
    QObject *const gateModeLabel = root->findChild<QObject *>(
            QStringLiteral("conditionGateModeLabel"));
    QObject *const gateModeGroup = root->findChild<QObject *>(
            QStringLiteral("conditionGateModeGroup"));
    QObject *const gateAndButton = root->findChild<QObject *>(
            QStringLiteral("conditionGateAndButton"));
    QObject *const gateOrButton = root->findChild<QObject *>(
            QStringLiteral("conditionGateOrButton"));
    QObject *const saveButton =
            root->findChild<QObject *>(QStringLiteral("conditionSaveButton"));
    QObject *const loadButton =
            root->findChild<QObject *>(QStringLiteral("conditionLoadButton"));
    QObject *const libraryDialog = root->findChild<QObject *>(
            QStringLiteral("conditionScriptLibraryDialog"));
    QObject *const editorFrame =
            root->findChild<QObject *>(QStringLiteral("conditionEditorFrame"));
    QObject *const libraryActions = root->findChild<QObject *>(
            QStringLiteral("conditionLibraryActions"));
    QObject *const gateRail =
            root->findChild<QObject *>(QStringLiteral("conditionGateRail"));
    const auto reportMissing = [](QObject *object, const char *name) {
        if (object == nullptr)
            std::cerr << "Missing QML object: " << name << '\n';
    };
    reportMissing(builder, "qmlTestConditionBuilder");
    reportMissing(textArea, "conditionScriptTextArea");
    reportMissing(completion, "conditionCompletionPopup");
    reportMissing(completionNoMatches, "conditionCompletionNoMatches");
    reportMissing(completionNoMatchesHint,
                  "conditionCompletionNoMatchesHint");
    reportMissing(symbolHover, "conditionSymbolHoverPopup");
    reportMissing(symbolHoverTimer, "conditionSymbolHoverTimer");
    reportMissing(gateModeLabel, "conditionGateModeLabel");
    reportMissing(gateModeGroup, "conditionGateModeGroup");
    reportMissing(gateAndButton, "conditionGateAndButton");
    reportMissing(gateOrButton, "conditionGateOrButton");
    reportMissing(saveButton, "conditionSaveButton");
    reportMissing(loadButton, "conditionLoadButton");
    reportMissing(libraryDialog, "conditionScriptLibraryDialog");
    reportMissing(editorFrame, "conditionEditorFrame");
    reportMissing(gateRail, "conditionGateRail");
    okay &= Check(builder != nullptr && textArea != nullptr &&
                          completion != nullptr &&
                          completionNoMatches != nullptr &&
                          completionNoMatchesHint != nullptr &&
                          symbolHover != nullptr &&
                          symbolHoverTimer != nullptr &&
                          gateModeLabel != nullptr &&
                          gateModeGroup != nullptr &&
                          gateAndButton != nullptr && gateOrButton != nullptr &&
                          saveButton != nullptr &&
                          loadButton != nullptr && libraryDialog != nullptr &&
                          editorFrame != nullptr &&
                          libraryActions != nullptr && gateRail != nullptr,
                  "ConditionBuilder lost a required editor/assistance object");
    okay &= Check(symbolHoverTimer != nullptr &&
                          symbolHoverTimer->property("interval").toInt() == 350,
                  "symbol hover lost its IDE-style delay");
    QVariant enumIcon;
    const bool resolvedEnumIcon = completion != nullptr &&
            QMetaObject::invokeMethod(
                    completion,
                    "iconLetter",
                    Q_RETURN_ARG(QVariant, enumIcon),
                    Q_ARG(QVariant, QVariant(QStringLiteral("enum"))));
    okay &= Check(resolvedEnumIcon &&
                          enumIcon.toString() == QStringLiteral("E"),
                  "enum family lost its compact type icon");
    QVariant enumMemberIcon;
    const bool resolvedEnumMemberIcon = completion != nullptr &&
            QMetaObject::invokeMethod(
                    completion,
                    "iconLetter",
                    Q_RETURN_ARG(QVariant, enumMemberIcon),
                    Q_ARG(QVariant,
                          QVariant(QStringLiteral("enum-member"))));
    okay &= Check(resolvedEnumMemberIcon &&
                          enumMemberIcon.toString() == QStringLiteral("V"),
                  "enum value lost its distinct type icon");
    QVariant keywordIcon;
    const bool resolvedKeywordIcon = completion != nullptr &&
            QMetaObject::invokeMethod(
                    completion,
                    "iconLetter",
                    Q_RETURN_ARG(QVariant, keywordIcon),
                    Q_ARG(QVariant, QVariant(QStringLiteral("keyword"))));
    okay &= Check(resolvedKeywordIcon &&
                          keywordIcon.toString() == QStringLiteral("K"),
                  "boolean keyword lost its distinct type icon");
    okay &= Check(gateModeLabel != nullptr &&
                          gateModeLabel->property("text").toString() ==
                                  QStringLiteral("Combine enabled gates") &&
                          gateModeLabel->property("font")
                                          .value<QFont>()
                                          .pixelSize() == 12 &&
                          gateModeLabel->property("font")
                                          .value<QFont>()
                                          .weight() == QFont::Medium &&
                          gateAndButton != nullptr &&
                          gateAndButton->property("themedControl").toBool() &&
                          gateAndButton->property("highlighted").toBool() &&
                          gateOrButton != nullptr &&
                          gateOrButton->property("themedControl").toBool() &&
                          !gateOrButton->property("highlighted").toBool(),
                  "AND/OR segmented control lost its label or selected state");
    okay &= Check(saveButton != nullptr && loadButton != nullptr &&
                          saveButton->property("text").toString() ==
                                  QStringLiteral("Save file") &&
                          loadButton->property("text").toString() ==
                                  QStringLiteral("Load file") &&
                          !saveButton->property("highlighted").toBool() &&
                          loadButton->property("highlighted").toBool(),
                  "file actions lost their labels or Load emphasis");
    okay &= Check(root->findChild<QObject *>(
                          QStringLiteral("conditionStatusLabel")) == nullptr &&
                          root->findChild<QObject *>(QStringLiteral(
                                  "conditionCompletionButton")) == nullptr &&
                          root->findChild<QObject *>(QStringLiteral(
                          "browseConditionObjectsButton")) == nullptr &&
                          root->findChild<QObject *>(QStringLiteral(
                                  "conditionGateModeSwitch")) == nullptr &&
                          root->findChild<QObject *>(QStringLiteral(
                                  "conditionAndModeButton")) == nullptr &&
                          root->findChild<QObject *>(QStringLiteral(
                                  "conditionOrModeButton")) == nullptr,
                  "removed condition-builder controls reappeared");

    auto *const window = qobject_cast<QQuickWindow *>(root);
    if (window != nullptr) {
        window->show();
        QCoreApplication::processEvents();
        auto *const builderItem = qobject_cast<QQuickItem *>(builder);
        auto *const editorFrameItem = qobject_cast<QQuickItem *>(editorFrame);
        auto *const libraryActionsItem =
                qobject_cast<QQuickItem *>(libraryActions);
        auto *const loadButtonItem = qobject_cast<QQuickItem *>(loadButton);
        auto *const saveButtonItem = qobject_cast<QQuickItem *>(saveButton);
        auto *const gateRailItemForGeometry =
                qobject_cast<QQuickItem *>(gateRail);
        auto *const gateModeLabelItem =
                qobject_cast<QQuickItem *>(gateModeLabel);
        auto *const gateModeGroupItem =
                qobject_cast<QQuickItem *>(gateModeGroup);
        if (editorFrameItem != nullptr && gateRailItemForGeometry != nullptr) {
            const QPointF railOrigin = gateRailItemForGeometry->mapToItem(
                    editorFrameItem, QPointF{});
            okay &= Check(
                    railOrigin.x() >= 2.0 && railOrigin.y() >= 2.0,
                    "line rail still paints over the rounded input border");
            okay &= Check(gateRailItemForGeometry->width() <= 24.0,
                          "line-number rail is wider than the compact design");
        }
        if (builderItem != nullptr && gateModeLabelItem != nullptr &&
            gateModeGroupItem != nullptr) {
            const QPointF labelOrigin =
                    gateModeLabelItem->mapToItem(builderItem, QPointF{});
            const QPointF groupOrigin =
                    gateModeGroupItem->mapToItem(builderItem, QPointF{});
            okay &= Check(labelOrigin.x() < groupOrigin.x() &&
                                  gateModeGroupItem->width() <= 116.0,
                          "gate mode is not a compact label-left segmented control");
        }
        if (builderItem != nullptr && editorFrameItem != nullptr &&
            libraryActionsItem != nullptr &&
            loadButtonItem != nullptr && saveButtonItem != nullptr) {
            const QPointF editorOrigin =
                    editorFrameItem->mapToItem(builderItem, QPointF{});
            const QPointF actionsOrigin =
                    libraryActionsItem->mapToItem(builderItem, QPointF{});
            const QPointF loadOrigin =
                    loadButtonItem->mapToItem(libraryActionsItem, QPointF{});
            const QPointF saveOrigin =
                    saveButtonItem->mapToItem(libraryActionsItem, QPointF{});
            okay &= Check(
                    actionsOrigin.y() >=
                                    editorOrigin.y() +
                                            editorFrameItem->height() + 8.0 &&
                            actionsOrigin.y() <=
                                    editorOrigin.y() +
                                            editorFrameItem->height() + 12.0 &&
                            loadOrigin.x() < saveOrigin.x() &&
                            saveButtonItem->width() >= 200.0 &&
                            std::abs(loadButtonItem->width() -
                                     saveButtonItem->width()) <= 1.0,
                    "Save/Load file actions are not full-width below the editor");
        }
        const QImage rendered = window->grabWindow();
        okay &= Check(!rendered.isNull(),
                      "ConditionBuilder did not render offscreen");
        const QByteArray capturePath =
                qgetenv("FOREVERTAS_CONDITION_BUILDER_CAPTURE");
        if (!capturePath.isEmpty()) {
            rendered.save(QString::fromLocal8Bit(capturePath));
        }
    }

    auto *const textItem = qobject_cast<QQuickItem *>(textArea);
    if (window != nullptr && textItem != nullptr && completion != nullptr) {
        const auto processDeferred = []() {
            for (int pass = 0; pass < 4; ++pass) {
                QCoreApplication::sendPostedEvents();
                QCoreApplication::processEvents();
            }
        };
        auto clickItem = [window](QObject *object) {
            auto *const item = qobject_cast<QQuickItem *>(object);
            if (item == nullptr)
                return false;
            const QPoint position =
                    item->mapToScene(QPointF(item->width() / 2.0,
                                             item->height() / 2.0))
                            .toPoint();
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, position);
            QCoreApplication::sendPostedEvents();
            QCoreApplication::processEvents();
            return true;
        };
        okay &= Check(clickItem(gateOrButton) &&
                              root->property("mockController")
                                              .value<QObject *>()
                                              ->property("conditionGateMode")
                                              .toString() ==
                                      QStringLiteral("or") &&
                              !gateAndButton->property("highlighted").toBool() &&
                              gateOrButton->property("highlighted").toBool(),
                      "AND/OR segmented control did not select OR gate mode");
        textItem->setProperty("text", QStringLiteral("car.speed >= 0"));
        textItem->setProperty("cursorPosition", 5);
        textItem->forceActiveFocus();
        processDeferred();
        const QRectF hoverCursorRectangle =
                textItem->property("cursorRectangle").toRectF();
        const QPoint symbolHoverPosition =
                textItem
                        ->mapToScene(QPointF(
                                hoverCursorRectangle.x() + 1.0,
                                hoverCursorRectangle.center().y()))
                        .toPoint();
        QTest::mouseMove(window, symbolHoverPosition);
        QTest::qWait(380);
        processDeferred();
        QQuickItem *const symbolDefinition = FindVisualItem(
                window->contentItem(),
                QStringLiteral("conditionSymbolHoverDefinition"));
        QQuickItem *const symbolType = FindVisualItem(
                window->contentItem(),
                QStringLiteral("conditionSymbolHoverType"));
        QQuickItem *const symbolDescription = FindVisualItem(
                window->contentItem(),
                QStringLiteral("conditionSymbolHoverDescription"));
        okay &= Check(
                symbolHover->property("visible").toBool() &&
                        symbolDefinition != nullptr &&
                        symbolDefinition->property("text").toString() ==
                                QStringLiteral("car.speed") &&
                        symbolType != nullptr &&
                        symbolType->property("text").toString() ==
                                QStringLiteral("number \u00b7 m/s") &&
                        symbolDescription != nullptr &&
                        !symbolDescription->property("text")
                                 .toString()
                                 .isEmpty(),
                "hovering a symbol did not show its definition and type");
        CaptureIfRequested(window,
                           "FOREVERTAS_CONDITION_SYMBOL_HOVER_CAPTURE");
        QTest::mouseMove(window,
                         QPoint(static_cast<int>(window->width()) - 2,
                                static_cast<int>(window->height()) - 2));
        processDeferred();
        okay &= Check(!symbolHover->property("visible").toBool(),
                      "symbol hover did not close when leaving the editor");

        textItem->setProperty("text", QString{});
        textItem->setProperty("cursorPosition", 0);
        textItem->forceActiveFocus();
        processDeferred();
        QTest::keyClick(window, Qt::Key_K);
        QTest::keyClick(window, Qt::Key_M);
        QTest::keyClick(window, Qt::Key_H);
        QTest::keyClick(window, Qt::Key_ParenLeft);
        processDeferred();
        okay &= Check(textItem->property("text").toString() ==
                                      QStringLiteral("kmh()") &&
                              textItem->property("cursorPosition").toInt() == 4 &&
                              assistance.source() == QStringLiteral("kmh()"),
                      "typing kmh( did not produce kmh(|)");

        const auto completionIndex = [&assistance](const QString &symbol) {
            const QVariantList completions = assistance.completions();
            for (qsizetype index = 0; index < completions.size(); ++index) {
                if (completions[index]
                            .toMap()
                            .value(QStringLiteral("symbol"))
                            .toString() == symbol) {
                    return static_cast<int>(index);
                }
            }
            return -1;
        };
        const int carArgumentIndex = completionIndex(QStringLiteral("car"));
        const bool insertedCarArgument = carArgumentIndex >= 0 &&
                QMetaObject::invokeMethod(
                        builder,
                        "applyCompletion",
                        Q_ARG(QVariant, QVariant(carArgumentIndex)));
        processDeferred();
        const int speedArgumentIndex =
                completionIndex(QStringLiteral("car.speed"));
        const bool insertedSpeedArgument = speedArgumentIndex >= 0 &&
                QMetaObject::invokeMethod(
                        builder,
                        "applyCompletion",
                        Q_ARG(QVariant, QVariant(speedArgumentIndex)));
        processDeferred();
        okay &= Check(
                insertedCarArgument && insertedSpeedArgument &&
                        textItem->property("text").toString() ==
                                QStringLiteral("kmh(car.speed)") &&
                        textItem->property("cursorPosition").toInt() == 13 &&
                        assistance.source() ==
                                QStringLiteral("kmh(car.speed)"),
                "picker completion removed the auto-paired right parenthesis");
        QMetaObject::invokeMethod(completion, "close");
        processDeferred();

        const QString deepMemberPrefix =
                QStringLiteral("car.wheels.frontleft.");
        textItem->setProperty("text", deepMemberPrefix);
        textItem->setProperty("cursorPosition", deepMemberPrefix.size());
        textItem->forceActiveFocus();
        processDeferred();
        QTest::keyClick(window, Qt::Key_Space, Qt::ControlModifier);
        processDeferred();
        QTest::keyClick(window, Qt::Key_G);
        processDeferred();
        const QVariantList deepMatches = assistance.completions();
        okay &= Check(
                completion->property("visible").toBool() &&
                        deepMatches.size() == 1 &&
                        deepMatches.front()
                                        .toMap()
                                        .value(QStringLiteral("symbol"))
                                        .toString() ==
                                QStringLiteral(
                                        "car.wheels.frontleft.groundcontact"),
                "deep completion did not filter by its active member segment");
        QTest::keyClick(window, Qt::Key_X);
        processDeferred();
        okay &= Check(
                assistance.completions().isEmpty() &&
                        completion->property("visible").toBool() &&
                        completion->property("currentIndex").toInt() == -1 &&
                        completionNoMatches->property("visible").toBool() &&
                        completionNoMatchesHint->property("visible").toBool(),
                "zero matches closed the active completion session");
        QTest::keyClick(window, Qt::Key_Backspace);
        processDeferred();
        okay &= Check(
                assistance.completions().size() == 1 &&
                        completion->property("visible").toBool() &&
                        completion->property("currentIndex").toInt() == 0 &&
                        !completionNoMatches->property("visible").toBool() &&
                        !completionNoMatchesHint->property("visible").toBool(),
                "deleting a mistyped member did not restore suggestions");
        QMetaObject::invokeMethod(completion, "close");
        processDeferred();

        const QString surfaceComparison =
                QStringLiteral("car.wheels.frontleft.surface = ");
        textItem->setProperty("text", surfaceComparison);
        textItem->setProperty("cursorPosition", surfaceComparison.size());
        textItem->forceActiveFocus();
        processDeferred();
        QTest::keyClick(window, Qt::Key_Space, Qt::ControlModifier);
        processDeferred();
        const QVariantList enumRoots = assistance.completions();
        okay &= Check(
                completion->property("visible").toBool() &&
                        enumRoots.size() == 1 &&
                        enumRoots.front()
                                        .toMap()
                                        .value(QStringLiteral("symbol"))
                                        .toString() ==
                                QStringLiteral("surface") &&
                        enumRoots.front()
                                        .toMap()
                                        .value(QStringLiteral("kind"))
                                        .toString() ==
                                QStringLiteral("enum") &&
                        enumRoots.front()
                                        .toMap()
                                        .value(QStringLiteral("category"))
                                        .toString() ==
                                QStringLiteral("enum family"),
                "surface comparison did not offer its typed enum namespace");
        const bool expandedSurface = QMetaObject::invokeMethod(
                builder, "applyCompletion", Q_ARG(QVariant, QVariant(0)));
        processDeferred();
        okay &= Check(
                expandedSurface &&
                        textItem->property("text").toString() ==
                                surfaceComparison + QStringLiteral("surface.") &&
                        assistance.completions().size() == 31 &&
                        completion->property("visible").toBool(),
                "selecting the surface namespace did not expose its values");
        QTest::keyClick(window, Qt::Key_I);
        processDeferred();
        const QVariantList iceMatches = assistance.completions();
        QQuickItem *const enumGlyph = FindVisualItem(
                window->contentItem(),
                QStringLiteral("conditionCompletionTypeGlyph_0"));
        okay &= Check(
                iceMatches.size() == 1 &&
                        iceMatches.front()
                                        .toMap()
                                        .value(QStringLiteral("symbol"))
                                        .toString() ==
                                QStringLiteral("surface.ICE") &&
                        iceMatches.front()
                                        .toMap()
                                        .value(QStringLiteral("kind"))
                                        .toString() ==
                                QStringLiteral("enum-member") &&
                        iceMatches.front()
                                        .toMap()
                                        .value(QStringLiteral("label"))
                                        .toString() ==
                                QStringLiteral("ICE") &&
                        iceMatches.front()
                                        .toMap()
                                        .value(QStringLiteral("value"))
                                        .toDouble() == 3.0 &&
                        enumGlyph != nullptr &&
                        enumGlyph->property("text").toString() ==
                                QStringLiteral("V"),
                "surface values were not filtered or rendered as enums");
        CaptureIfRequested(window, "FOREVERTAS_CONDITION_ENUM_CAPTURE");
        QMetaObject::invokeMethod(completion, "close");
        processDeferred();

        const QStringList alignedLines{
                QStringLiteral("car.speed > 0"),
                QStringLiteral("iterations > 1"),
                QStringLiteral("race_time > 2"),
                QStringLiteral("last_improvement.time >= 3")};
        const QString alignedScript = alignedLines.join(QLatin1Char('\n'));
        textItem->setProperty("text", alignedScript);
        textItem->setProperty("cursorPosition", 0);
        processDeferred();
        int lineStart = 0;
        bool lineGeometryAligned = true;
        for (int line = 1; line <= alignedLines.size(); ++line) {
            QQuickItem *const gateLine = FindVisualItem(
                    window->contentItem(),
                    QStringLiteral("conditionGateLine_%1").arg(line));
            textItem->setProperty("cursorPosition", lineStart);
            processDeferred();
            const QRectF cursorRectangle =
                    textItem->property("cursorRectangle").toRectF();
            if (gateLine == nullptr) {
                lineGeometryAligned = false;
            } else {
                const qreal gateCenter =
                        gateLine
                                ->mapToScene(QPointF(
                                        0.0, gateLine->height() / 2.0))
                                .y();
                const qreal textCenter =
                        textItem
                                ->mapToScene(QPointF(
                                        0.0, cursorRectangle.center().y()))
                                .y();
                lineGeometryAligned &=
                        std::abs(gateCenter - textCenter) <= 0.75 &&
                        std::abs(gateLine->height() -
                                 cursorRectangle.height()) <= 0.75;
            }
            lineStart += alignedLines[line - 1].size() + 1;
        }
        okay &= Check(lineGeometryAligned,
                      "gate numbers do not follow the editor's laid-out lines");
        auto *const gateRailItem = qobject_cast<QQuickItem *>(gateRail);
        bool clickedGate = false;
        if (gateRailItem != nullptr) {
            const QPoint position =
                    gateRailItem->mapToScene(QPointF(12.0, 16.0)).toPoint();
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, position);
            QCoreApplication::processEvents();
            clickedGate = true;
        }
        okay &= Check(clickedGate &&
                              textItem->property("text").toString().startsWith(
                                      QStringLiteral("// ")),
                      "clicking a line number did not toggle the gate");

        textItem->setProperty("text", QString{});
        textItem->setProperty("cursorPosition", 0);
        textItem->forceActiveFocus();
        window->requestActivate();
        processDeferred();
        QTest::keyClick(window, Qt::Key_Space, Qt::ControlModifier);
        processDeferred();
        okay &= Check(completion->property("visible").toBool(),
                      "Ctrl+Space did not open condition completion");
        auto *const completionList = completion->findChild<QQuickItem *>(
                QStringLiteral("conditionCompletionList"));
        okay &= Check(completionList != nullptr,
                      "completion did not create its suggestion list");
        if (completionList != nullptr) {
            const QPoint stationaryHover =
                    completionList
                            ->mapToScene(QPointF(completionList->width() / 2.0,
                                                 90.0))
                            .toPoint();
            QTest::keyClick(window, Qt::Key_Escape);
            processDeferred();
            QTest::mouseMove(window, stationaryHover);
            processDeferred();
            QTest::keyClick(window, Qt::Key_Space, Qt::ControlModifier);
            processDeferred();
            okay &= Check(
                    completion->property("visible").toBool() &&
                            completion->property("currentIndex").toInt() == 0 &&
                            !completion->property("pointerSelectionArmed")
                                     .toBool(),
                    "stationary cursor changed completion selection on open");
            QTest::mouseMove(window, stationaryHover + QPoint(1, 0));
            QTest::mouseMove(window, stationaryHover + QPoint(2, 0));
            processDeferred();
            okay &= Check(
                    completion->property("pointerSelectionArmed").toBool() &&
                            completion->property("currentIndex").toInt() == 2,
                    "moving the cursor did not enable hover selection");
            const QPoint objectClick =
                    completionList
                            ->mapToScene(QPointF(completionList->width() / 2.0,
                                                 20.0))
                            .toPoint();
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                              objectClick);
            processDeferred();
            okay &= Check(
                    textItem->property("text").toString() ==
                                    QStringLiteral("car.") &&
                            textItem->property("cursorPosition").toInt() == 4 &&
                            assistance.source() == QStringLiteral("car.") &&
                            assistance.cursorPosition() == 4 &&
                            assistance.completionContext()
                                            .value(QStringLiteral("symbol"))
                                            .toString() ==
                                    QStringLiteral("car") &&
                            completion->property("visible").toBool(),
                    "clicking car did not immediately expand its members");
        }
        CaptureIfRequested(window, "FOREVERTAS_CONDITION_COMPLETION_CAPTURE");
        const QRectF cursorRectangle =
                textItem->property("cursorRectangle").toRectF();
        const QPointF cursorBottom =
                textItem->mapToScene(cursorRectangle.bottomLeft());
        okay &= Check(
                completion->property("padding").toReal() >= 2.0 &&
                        completion->property("width").toReal() <= 341.0 &&
                        completion->property("height").toReal() <= 178.0 &&
                        completion->property("y").toReal() >=
                                cursorBottom.y() + 8.0,
                "completion overlay size, border, or vertical offset regressed");
        QObject *const documentation = completion->findChild<QObject *>(
                QStringLiteral("conditionCompletionDocumentation"));
        QObject *const documentationContent = completion->findChild<QObject *>(
                QStringLiteral("conditionCompletionDocumentationContent"));
        QObject *const completionScrollBar = completion->findChild<QObject *>(
                QStringLiteral("conditionCompletionListScrollBar"));
        QQuickItem *const firstCompletion =
                FindVisualItem(window->contentItem(),
                               QStringLiteral("conditionCompletionItem_0"));
        QQuickItem *const typeIcon =
                FindVisualItem(window->contentItem(),
                               QStringLiteral("conditionCompletionTypeIcon_0"));
        QQuickItem *const typeGlyph =
                FindVisualItem(window->contentItem(),
                               QStringLiteral("conditionCompletionTypeGlyph_0"));
        QQuickItem *const documentationTypeIcon = FindVisualItem(
                window->contentItem(), QStringLiteral(
                        "conditionCompletionDocumentationTypeIcon"));
        QQuickItem *const documentationTypeGlyph = FindVisualItem(
                window->contentItem(), QStringLiteral(
                        "conditionCompletionDocumentationTypeGlyph"));
        const auto centered = [](QQuickItem *container, QQuickItem *content) {
            if (container == nullptr || content == nullptr)
                return false;
            const QPointF containerCenter = container->mapToScene(
                    QPointF(container->width() / 2.0,
                            container->height() / 2.0));
            const QPointF contentCenter = content->mapToScene(
                    QPointF(content->width() / 2.0,
                            content->height() / 2.0));
            return std::abs(containerCenter.x() - contentCenter.x()) <= 0.5 &&
                    std::abs(containerCenter.y() - contentCenter.y()) <= 0.5;
        };
        okay &= Check(completionList != nullptr &&
                              completionList->width() <= 200.0 &&
                              documentation != nullptr &&
                              documentation->property("width").toReal() <=
                                      136.0 &&
                              documentationContent != nullptr &&
                              documentationContent->property("y").toReal() >=
                                      8.0 &&
                              completionScrollBar != nullptr &&
                              completionScrollBar->property("visible").toBool() &&
                              firstCompletion != nullptr &&
                              firstCompletion->height() == 34.0 &&
                              firstCompletion->property("rightPadding").toReal() >
                                      completionScrollBar->property("width")
                                              .toReal() &&
                              typeIcon != nullptr &&
                              typeGlyph != nullptr &&
                              typeGlyph->property("text").toString() ==
                                      QStringLiteral("#") &&
                              typeGlyph->property("font")
                                              .value<QFont>()
                                              .pixelSize() == 8 &&
                              centered(typeIcon, typeGlyph) &&
                              documentationTypeGlyph != nullptr &&
                              documentationTypeGlyph->property("font")
                                              .value<QFont>()
                                              .pixelSize() == 8 &&
                              centered(documentationTypeIcon,
                                       documentationTypeGlyph),
                      "completion sizing, scrollbar lane, docs inset, or type icon regressed");
        const QVariantMap keyboardSelection = assistance.completionEdit(0);
        const QString keyboardExpected = QStringLiteral("car.") +
                keyboardSelection.value(QStringLiteral("insertText"))
                        .toString();
        QTest::keyClick(window, Qt::Key_Return);
        processDeferred();
        okay &= Check(textItem->property("text").toString() ==
                                      keyboardExpected &&
                              assistance.source() == keyboardExpected &&
                              textItem->property("cursorPosition").toInt() ==
                                      assistance.cursorPosition() &&
                              !completion->property("visible").toBool() &&
                              textItem->hasActiveFocus(),
                      "keyboard completion did not insert and restore focus");

        textItem->setProperty("text", QString{});
        textItem->setProperty("cursorPosition", 0);
        textItem->forceActiveFocus();
        processDeferred();
        const QList<Qt::Key> memberKeys{
                Qt::Key_C, Qt::Key_A, Qt::Key_R, Qt::Key_Period};
        const QStringList memberStates{
                QStringLiteral("c"),
                QStringLiteral("ca"),
                QStringLiteral("car"),
                QStringLiteral("car.")};
        for (qsizetype index = 0; index < memberKeys.size(); ++index) {
            QTest::keyClick(window, memberKeys[index]);
            processDeferred();
            const QString expected = memberStates[index];
            okay &= Check(
                    textItem->property("text").toString() == expected &&
                            textItem->property("cursorPosition").toInt() ==
                                    expected.size() &&
                            assistance.source() == expected &&
                            assistance.cursorPosition() == expected.size(),
                    "key-by-key document synchronization diverged");
        }
        okay &= Check(textItem->property("text").toString() ==
                                      QStringLiteral("car.") &&
                              assistance.completionContext()
                                              .value(QStringLiteral("symbol"))
                                              .toString() ==
                                      QStringLiteral("car") &&
                              completion->property("visible").toBool(),
                      "typing car. did not open members without a trailing space");
        if (completionList != nullptr) {
            const QVariantMap mouseSelection = assistance.completionEdit(0);
            const QString mouseExpected = QStringLiteral("car.") +
                    mouseSelection.value(QStringLiteral("insertText"))
                            .toString();
            const QPoint clickPosition =
                    completionList
                            ->mapToScene(QPointF(completionList->width() / 2.0,
                                                 20.0))
                            .toPoint();
            QTest::mouseMove(window, clickPosition);
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                              clickPosition);
            processDeferred();
            okay &= Check(textItem->property("text").toString() ==
                                          mouseExpected &&
                                  assistance.source() == mouseExpected &&
                                  textItem->property("cursorPosition").toInt() ==
                                          assistance.cursorPosition(),
                          "mouse completion did not insert its suggestion");
        }

        textItem->setProperty("text", QStringLiteral("car."));
        textItem->setProperty("cursorPosition", 4);
        textItem->forceActiveFocus();
        processDeferred();
        QTest::keyClick(window, Qt::Key_Space, Qt::ControlModifier);
        processDeferred();
        textItem->setProperty("text", QStringLiteral("car.s"));
        textItem->setProperty("cursorPosition", 5);
        const bool hadPendingDocumentSync =
                textItem->property("text").toString() ==
                        QStringLiteral("car.s") &&
                assistance.source() == QStringLiteral("car.");
        const bool appliedPendingCompletion = QMetaObject::invokeMethod(
                builder,
                "applyCompletion",
                Q_ARG(QVariant, QVariant(0)));
        processDeferred();
        okay &= Check(hadPendingDocumentSync && appliedPendingCompletion &&
                              textItem->property("text").toString() ==
                                      QStringLiteral("car.speed") &&
                              assistance.source() ==
                                      QStringLiteral("car.speed") &&
                              textItem->property("cursorPosition").toInt() ==
                                      assistance.cursorPosition(),
                      "completion did not flush a pending document edit before acceptance");

        textItem->setProperty("text", QStringLiteral("car."));
        textItem->setProperty("cursorPosition", 4);
        textItem->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Space, Qt::ControlModifier);
        QTest::keyClick(window, Qt::Key_Escape);
        QCoreApplication::processEvents();
        okay &= Check(!completion->property("visible").toBool() &&
                              textItem->property("text").toString() ==
                                      QStringLiteral("car."),
                      "Escape did not dismiss completion without editing");

        textItem->setProperty("text", QStringLiteral("car.speed >"));
        textItem->setProperty("cursorPosition", 11);
        QCoreApplication::processEvents();
        QQuickItem *const highlight =
                FindVisualItem(window->contentItem(),
                               QStringLiteral("conditionLineHighlight_1"));
        okay &= Check(!assistance.valid() && highlight != nullptr &&
                              highlight->property("color")
                                              .value<QColor>()
                                              .alpha() > 0,
                      "invalid condition line was not highlighted");

        okay &= Check(clickItem(saveButton) &&
                              libraryDialog->property("visible").toBool(),
                      "condition Save did not open the script library overlay");
        QQuickItem *const dialogFrame =
                FindVisualItem(window->contentItem(),
                               QStringLiteral("scriptLibraryDialogFrame"));
        QQuickItem *const dialogHeader =
                FindVisualItem(window->contentItem(), QStringLiteral(
                        "scriptLibraryDialogHeaderSurface"));
        QQuickItem *const dialogFooter =
                FindVisualItem(window->contentItem(), QStringLiteral(
                        "scriptLibraryDialogFooterSurface"));
        okay &= Check(dialogFrame != nullptr && dialogHeader != nullptr &&
                              dialogFooter != nullptr &&
                              dialogFrame->property("radius").toReal() == 6.0 &&
                              dialogHeader->property("radius").toReal() == 5.0 &&
                              dialogFooter->property("radius").toReal() == 5.0,
                      "script dialog frame uses inconsistent corner radii");
        CaptureIfRequested(window, "FOREVERTAS_SCRIPT_LIBRARY_CAPTURE");
        QObject *const nameField = root->findChild<QObject *>(
                QStringLiteral("scriptLibraryFileNameField"));
        QObject *const confirm = root->findChild<QObject *>(
                QStringLiteral("confirmScriptLibraryButton"));
        if (nameField != nullptr && confirm != nullptr) {
            nameField->setProperty("text", QStringLiteral("fast-finish"));
            QCoreApplication::processEvents();
            okay &= Check(clickItem(confirm),
                          "condition script Save could not be confirmed");
            QObject *const store =
                    root->property("mockScriptStore").value<QObject *>();
            okay &= Check(
                    store != nullptr &&
                            store->property("lastName").toString() ==
                                    QStringLiteral("fast-finish") &&
                            !store->property("lastContent")
                                     .toString()
                                     .isEmpty(),
                    "condition script was not passed to named .txt storage");
        }

        okay &= Check(clickItem(loadButton) &&
                              libraryDialog->property("visible").toBool(),
                      "condition Load did not open the script library overlay");
        QCoreApplication::processEvents();
        QQuickItem *const fileItem =
                FindVisualItem(window->contentItem(),
                               QStringLiteral("scriptLibraryFileItem_0"));
        QQuickItem *const fileIcon =
                FindVisualItem(window->contentItem(),
                               QStringLiteral("scriptLibraryFileIcon_0"));
        QQuickItem *const fileLabel =
                FindVisualItem(window->contentItem(),
                               QStringLiteral("scriptLibraryFileLabel_0"));
        bool selectedFileContentCentered = fileItem != nullptr &&
                fileIcon != nullptr && fileLabel != nullptr;
        if (selectedFileContentCentered) {
            const qreal itemCenter =
                    fileItem
                            ->mapToScene(QPointF(
                                    0.0, fileItem->height() / 2.0))
                            .y();
            const qreal iconCenter =
                    fileIcon
                            ->mapToScene(QPointF(
                                    0.0, fileIcon->height() / 2.0))
                            .y();
            const qreal labelCenter =
                    fileLabel
                            ->mapToScene(QPointF(
                                    0.0, fileLabel->height() / 2.0))
                            .y();
            selectedFileContentCentered =
                    std::abs(itemCenter - iconCenter) <= 0.75 &&
                    std::abs(itemCenter - labelCenter) <= 0.75;
        }
        okay &= Check(selectedFileContentCentered,
                      "selected script row content is not vertically centered");
        CaptureIfRequested(window,
                           "FOREVERTAS_SCRIPT_LIBRARY_LOAD_CAPTURE");
        QObject *const loadConfirm = root->findChild<QObject *>(
                QStringLiteral("confirmScriptLibraryButton"));
        if (loadConfirm != nullptr) {
            okay &= Check(
                    clickItem(loadConfirm) &&
                            textItem->property("text").toString() ==
                                    QStringLiteral("car.speed >= 123"),
                    "loading a named condition script did not update the editor");
        }
    }

    if (libraryDialog != nullptr &&
        libraryDialog->property("visible").toBool()) {
        const bool invoked = QMetaObject::invokeMethod(libraryDialog, "close");
        QCoreApplication::processEvents();
        okay &= Check(invoked, "script library dialog could not close");
    }

    delete root;
    return okay;
}

} // namespace

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    qputenv("QT_QUICK_BACKEND", QByteArrayLiteral("software"));
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QGuiApplication application(argc, argv);
    return TestConditionBuilderLoads() ? 0 : 1;
}
