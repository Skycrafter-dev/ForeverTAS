#include "viewer/whiteboard_canvas_item.h"
#include "viewer/whiteboard_model.h"
#include "viewer/race_timeline_item.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQuickStyle>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>

namespace {

using forevertas::viewer::WhiteboardCanvasItem;
using forevertas::viewer::WhiteboardModel;

int OpaquePixelCount(const QImage &image) {
    int result = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) > 0) {
                ++result;
            }
        }
    }
    return result;
}

QImage Render(const QVariantMap &drawing) {
    QImage image(400, 240, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    WhiteboardCanvasItem canvas;
    canvas.setWidth(image.width());
    canvas.setHeight(image.height());
    canvas.setDrawing(drawing);
    QPainter painter(&image);
    canvas.paint(&painter);
    painter.end();
    return image;
}

bool Near(double lhs, double rhs, double tolerance = 0.0001) {
    return std::abs(lhs - rhs) <= tolerance;
}

QList<QQuickItem *> FindVisualItems(QQuickItem *root,
                                    const QString &objectName) {
    QList<QQuickItem *> result;
    if (root == nullptr) {
        return result;
    }
    if (root->objectName() == objectName) {
        result.push_back(root);
    }
    for (QQuickItem *child : root->childItems()) {
        result.append(FindVisualItems(child, objectName));
    }
    return result;
}

}  // namespace

int main(int argc, char **argv) {
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QGuiApplication application(argc, argv);
    QCoreApplication::setOrganizationName(
            QStringLiteral("ForeverTAS Tests"));
    QCoreApplication::setApplicationName(
            QStringLiteral("Whiteboard Tests"));
    QStandardPaths::setTestModeEnabled(true);
    QSettings().clear();

    int failures = 0;
    const auto expect = [&failures](bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            ++failures;
        }
    };

    WhiteboardModel model;
    expect(!model.active(), "whiteboard starts inactive");
    expect(model.count() == 0, "whiteboard starts empty");
    expect(model.tool() == QStringLiteral("select"),
           "select is the initial tool");
    expect(model.maximumCount() == 512, "item limit is exposed");
    expect(!model.beginItem(0.1, 0.1),
           "drawing is rejected while inactive");
    expect(model.addText(0.1, 0.1, QStringLiteral("note")) == -1,
           "text is rejected while inactive");

    model.setTool(QStringLiteral("unknown"));
    expect(model.tool() == QStringLiteral("select"),
           "unknown tools are rejected");
    model.setColor(QColor());
    expect(model.color().isValid(), "invalid colors are rejected");
    model.setSize(std::numeric_limits<double>::quiet_NaN());
    expect(Near(model.size(), 4.0), "non-finite sizes are rejected");
    model.setSize(-100.0);
    expect(Near(model.size(), 1.0), "size is clamped at the lower bound");
    model.setSize(100.0);
    expect(Near(model.size(), 100.0), "typed sizes are not capped at 24 px");
    model.setSize(6.0);
    model.setColor(QColor(QStringLiteral("#42d3c6")));

    model.setActive(true);
    model.setTool(QStringLiteral("pen"));
    expect(model.beginItem(-1.0, 2.0),
           "pen accepts and clamps a finite starting point");
    expect(model.drawing(), "draft state is exposed");
    expect(model.updateItem(0.25, 0.35), "pen records a second point");
    expect(!model.updateItem(0.25001, 0.35001),
           "pen suppresses near-duplicate points");
    expect(model.updateItem(0.7, 0.65), "pen records subsequent points");
    expect(model.finishItem(), "pen gesture is committed");
    expect(!model.drawing() && model.count() == 1,
           "committed pen leaves drawing state");
    QVariantMap pen = model.items().front().toMap();
    expect(pen.value(QStringLiteral("type")).toString() ==
                   QStringLiteral("pen"),
           "pen item retains its vector type");
    expect(pen.value(QStringLiteral("points")).toList().size() == 3,
           "pen item retains vector points");
    expect(pen.value(QStringLiteral("color")).value<QColor>() ==
                   QColor(QStringLiteral("#42d3c6")),
           "pen item retains its chosen color");
    expect(Near(pen.value(QStringLiteral("strokeWidth")).toDouble(), 6.0),
           "pen item retains its chosen size");

    model.setTool(QStringLiteral("line"));
    expect(model.beginItem(0.1, 0.1), "line starts");
    expect(model.updateItem(0.9, 0.9), "line endpoint updates");
    expect(model.finishItem(), "line commits");
    expect(model.count() == 2, "line adds an independent item");

    model.setTool(QStringLiteral("rectangle"));
    expect(model.beginItem(0.8, 0.7), "rectangle starts");
    expect(model.updateItem(0.4, 0.2),
           "rectangle supports reverse-direction drawing");
    expect(model.finishItem(), "rectangle commits");
    const QVariantMap rectangle = model.items().back().toMap();
    expect(Near(rectangle.value(QStringLiteral("x")).toDouble(), 0.4) &&
                   Near(rectangle.value(QStringLiteral("y")).toDouble(), 0.2),
           "rectangle bounds are normalized");

    model.setTool(QStringLiteral("ellipse"));
    expect(model.beginItem(0.2, 0.3), "ellipse starts");
    expect(model.updateItem(0.6, 0.8), "ellipse bounds update");
    expect(model.finishItem(), "ellipse commits");
    expect(model.count() == 4, "all basic shapes are item based");

    expect(model.beginItem(0.4, 0.4), "tiny shape draft starts");
    expect(!model.finishItem(), "tiny shape draft is discarded");
    expect(model.count() == 4, "discarded draft leaves no placeholder");
    expect(model.beginItem(0.1, 0.1), "cancelable draft starts");
    model.cancelItem();
    expect(!model.drawing() && model.count() == 4,
           "cancel removes its draft");

    model.setTool(QStringLiteral("text"));
    expect(model.addText(0.95, 0.98, QStringLiteral("  Vector note  ")) == 4,
           "text is added as a distinct item");
    QVariantMap text = model.items().back().toMap();
    expect(text.value(QStringLiteral("type")).toString() ==
                   QStringLiteral("text") &&
                   text.value(QStringLiteral("text")).toString() ==
                           QStringLiteral("Vector note"),
           "text is normalized and remains editable");
    expect(text.value(QStringLiteral("x")).toDouble() +
                           text.value(QStringLiteral("width")).toDouble() <=
                   1.0001,
           "text is kept within the viewport");
    expect(model.setText(4, QStringLiteral("Edited annotation")),
           "text can be edited");
    expect(!model.setText(4, QStringLiteral("   ")),
           "empty edits are rejected");
    expect(!model.setText(0, QStringLiteral("not text")),
           "non-text items cannot be edited as text");

    const quint64 whiteboardHistoryToken = model.captureHistorySnapshot();
    expect(model.setText(4, QStringLiteral("Changed annotation")) &&
                   model.restoreHistorySnapshot(whiteboardHistoryToken) &&
                   model.items().back().toMap()
                                   .value(QStringLiteral("text"))
                                   .toString() ==
                           QStringLiteral("Edited annotation"),
           "whiteboard history restores in-progress drawing edits");
    model.pruneHistorySnapshots(
            {QVariant::fromValue(whiteboardHistoryToken)});

    model.setTool(QStringLiteral("select"));
    expect(model.itemAt(0.5, 0.5) >= 0,
           "persistent selection input can hit-test drawing items");
    expect(model.selectItem(1), "line can be selected");
    const QVariantMap lineBeforeMove = model.selectedItem();
    expect(model.moveSelected(10.0, -10.0),
           "movement clamps instead of leaving the viewport");
    const QVariantMap lineAfterMove = model.selectedItem();
    expect(lineAfterMove.value(QStringLiteral("x")).toDouble() >= 0.0 &&
                   lineAfterMove.value(QStringLiteral("y")).toDouble() >=
                           0.0 &&
                   lineAfterMove.value(QStringLiteral("x")).toDouble() +
                                   lineAfterMove
                                           .value(QStringLiteral("width"))
                                           .toDouble() <=
                           1.0001,
           "moved items stay inside the viewport");
    expect(lineAfterMove.value(QStringLiteral("x")).toDouble() !=
                           lineBeforeMove.value(QStringLiteral("x")).toDouble(),
           "movement changes item bounds");
    const double oldWidth =
            lineAfterMove.value(QStringLiteral("width")).toDouble();
    expect(model.resizeSelected(-oldWidth * 0.5, 0.05),
           "selected items can be resized");
    const QVariantMap lineAfterResize = model.selectedItem();
    expect(lineAfterResize.value(QStringLiteral("width")).toDouble() <
                           oldWidth &&
                   lineAfterResize.value(QStringLiteral("height")).toDouble() >
                           0.0,
           "resize updates selected item dimensions");

    model.selectItem(1);
    model.moveSelected(
            0.1 - model.selectedItem().value(QStringLiteral("x")).toDouble(),
            0.1 - model.selectedItem().value(QStringLiteral("y")).toDouble());
    const QVariantMap lineBeforeErase = model.selectedItem();
    const QVariantList linePoints =
            lineBeforeErase.value(QStringLiteral("points")).toList();
    const QPointF first = linePoints.front().toPointF();
    const QPointF last = linePoints.back().toPointF();
    const QPointF midpoint = (first + last) * 0.5;
    const int opaqueBefore = OpaquePixelCount(Render(lineBeforeErase));
    expect(!model.eraseSelected(0.99, 0.01, 0.01),
           "eraser rejects points outside the selected item");
    expect(model.eraseSelected(midpoint.x(), midpoint.y(), 0.05),
           "eraser records a selective cut on the selected item");
    expect(!model.eraseSelected(midpoint.x(), midpoint.y(), 0.05),
           "eraser suppresses redundant adjacent samples");
    const QVariantMap lineAfterErase = model.selectedItem();
    const int opaqueAfter = OpaquePixelCount(Render(lineAfterErase));
    expect(lineAfterErase.value(QStringLiteral("erasures"))
                           .toList()
                           .size() == 1,
           "erasures are retained on the selected item only");
    expect(opaqueBefore > 0 && opaqueAfter < opaqueBefore,
           "vector rendering applies transparent pixel erasure");
    expect(model.items().front()
                           .toMap()
                           .value(QStringLiteral("erasures"))
                           .toList()
                           .isEmpty(),
           "erasing one selected item leaves other items unchanged");

    const QImage textImage = Render(model.items().back().toMap());
    expect(OpaquePixelCount(textImage) > 0,
           "text renders as scalable vector-backed glyphs");

    const int beforeRemove = model.count();
    expect(model.removeSelected(), "selected item can be deleted");
    expect(model.count() == beforeRemove - 1,
           "delete removes exactly the selected item");
    model.clearSelection();
    expect(model.selectedIndex() == -1, "selection can be cleared");
    expect(!model.removeSelected(), "delete rejects an empty selection");

    const int retainedCount = model.count();
    model.setActive(false);
    expect(model.count() == retainedCount,
           "switching to normal 3D view retains drawings");
    expect(!model.moveSelected(0.1, 0.1) &&
                   !model.eraseSelected(0.1, 0.1, 0.02),
           "inactive whiteboard rejects editing operations");
    model.setActive(true);
    expect(model.count() == retainedCount,
           "switching back restores all drawing items");

    WhiteboardModel capacityModel;
    capacityModel.setActive(true);
    capacityModel.setTool(QStringLiteral("text"));
    for (int index = 0; index < capacityModel.maximumCount(); ++index) {
        expect(capacityModel.addText(
                       0.1,
                       0.1,
                       QStringLiteral("Item %1").arg(index)) == index,
               "items can be added through the documented capacity");
    }
    expect(capacityModel.addText(
                   0.1, 0.1, QStringLiteral("One too many")) == -1 &&
                   capacityModel.count() ==
                           capacityModel.maximumCount(),
           "the item limit rejects overflow without corrupting drawings");

    QSettings().clear();
    WhiteboardModel repositoryModel;
    repositoryModel.setMapIdentity(
            QStringLiteral("collision-sha256:stadium"),
            QStringLiteral(
                    "$fffStadium $$Mix $l[https://example.test]Training$l"
                    "\nCup$z"));
    expect(repositoryModel.mapName() ==
                   QStringLiteral("Stadium $Mix Training Cup"),
           "authoritative map names strip formatting, links, and controls "
           "without using a filename");
    repositoryModel.setMapIdentity(
            QStringLiteral("collision-sha256:stadium"),
            QStringLiteral("  $fff$i$z  "));
    repositoryModel.setActive(true);
    repositoryModel.setTool(QStringLiteral("line"));
    expect(repositoryModel.beginItem(0.1, 0.2) &&
                   repositoryModel.updateItem(0.8, 0.7) &&
                   repositoryModel.finishItem() &&
                   repositoryModel.captureCurrentBoard(
                           QStringLiteral("Unidentified"),
                           QVariantMap{}) == -1 &&
                   repositoryModel.boardCount() == 0,
           "a formatting-only challenge name cannot create an unidentified "
           "saved drawing");
    expect(repositoryModel.removeSelected() &&
                   repositoryModel.count() == 0,
           "rejected unidentified drawing remains editable as a draft");
    repositoryModel.setMapIdentity(
            QStringLiteral("collision-sha256:stadium"),
            QStringLiteral("$fffStadium $iTraining$z"));
    expect(repositoryModel.beginItem(0.1, 0.2) &&
                   repositoryModel.updateItem(0.8, 0.7) &&
                   repositoryModel.finishItem(),
           "persistent board source drawing is created");
    const QVariantMap firstCapture{
            {QStringLiteral("targetX"), 12.0},
            {QStringLiteral("targetY"), 3.0},
            {QStringLiteral("targetZ"), -4.0},
            {QStringLiteral("yaw"), 35.0},
            {QStringLiteral("pitch"), -20.0},
            {QStringLiteral("distance"), 38.0},
            {QStringLiteral("planeX"), 7.0},
            {QStringLiteral("planeY"), 4.0},
            {QStringLiteral("planeZ"), 8.0},
            {QStringLiteral("planeWidth"), 12.0},
            {QStringLiteral("planeHeight"), 7.0},
            {QStringLiteral("projection"),
             QStringLiteral("perspective-vertical")},
            {QStringLiteral("fieldOfView"), 63.0},
            {QStringLiteral("planeDistance"), 11.0},
            {QStringLiteral("viewportWidth"), 1420.0},
            {QStringLiteral("viewportHeight"), 820.0},
            {QStringLiteral("contentX"), 0.0},
            {QStringLiteral("contentY"), 52.0 / 820.0},
            {QStringLiteral("contentWidth"), 1.0},
            {QStringLiteral("contentHeight"), 768.0 / 820.0},
            {QStringLiteral("canvasWidth"), 1420.0},
            {QStringLiteral("canvasHeight"), 768.0}};
    QVariantMap invalidProjectionCapture = firstCapture;
    invalidProjectionCapture[QStringLiteral("projection")] =
            QStringLiteral("orthographic");
    expect(repositoryModel.captureCurrentBoard(
                   QStringLiteral("Invalid projection"),
                   invalidProjectionCapture) == -1 &&
                   repositoryModel.count() == 1 &&
                   repositoryModel.boardCount() == 0,
           "capture rejects unsupported projections without consuming items");
    invalidProjectionCapture = firstCapture;
    invalidProjectionCapture[QStringLiteral("contentHeight")] = 1.0;
    expect(repositoryModel.captureCurrentBoard(
                   QStringLiteral("Invalid content rectangle"),
                   invalidProjectionCapture) == -1 &&
                   repositoryModel.count() == 1 &&
                   repositoryModel.boardCount() == 0,
           "capture rejects content rectangles outside the saved viewport");
    invalidProjectionCapture = firstCapture;
    invalidProjectionCapture[QStringLiteral("canvasWidth")] = 8193.0;
    expect(repositoryModel.captureCurrentBoard(
                   QStringLiteral("Oversized canvas"),
                   invalidProjectionCapture) == -1 &&
                   repositoryModel.count() == 1 &&
                   repositoryModel.boardCount() == 0,
           "capture rejects canvas sizes that could exhaust texture memory");
    expect(repositoryModel.captureCurrentBoard(
                   QStringLiteral("Entry line"), firstCapture) == 0 &&
                   repositoryModel.count() == 0 &&
                   repositoryModel.boardCount() == 1,
           "placing a drawing captures its plane and clears the draft");
    QVariantMap firstBoard =
            repositoryModel.boards().front().toMap();
    expect(firstBoard.value(QStringLiteral("name")).toString() ==
                           QStringLiteral("Entry line") &&
                   firstBoard.value(QStringLiteral("isCurrentMap"))
                           .toBool() &&
                   firstBoard.value(QStringLiteral("mapName")).toString() ==
                           QStringLiteral("Stadium Training") &&
                   Near(firstBoard.value(
                                   QStringLiteral("targetX"))
                                .toDouble(),
                        12.0) &&
                   Near(firstBoard.value(
                                   QStringLiteral("planeWidth"))
                                .toDouble(),
                        12.0) &&
                   firstBoard.value(
                                     QStringLiteral("projectionVersion"))
                                   .toInt() == 1 &&
                   firstBoard.value(
                                     QStringLiteral("projection"))
                                   .toString() ==
                           QStringLiteral("perspective-vertical") &&
                   Near(firstBoard.value(
                                   QStringLiteral("fieldOfView"))
                                .toDouble(),
                        63.0) &&
                   Near(firstBoard.value(
                                   QStringLiteral("planeDistance"))
                                .toDouble(),
                        11.0) &&
                   Near(firstBoard.value(
                                   QStringLiteral("canvasWidth"))
                                .toDouble(),
                        1420.0) &&
                   Near(firstBoard.value(
                                   QStringLiteral("canvasHeight"))
                                .toDouble(),
                        768.0) &&
                   firstBoard.value(QStringLiteral("items"))
                                   .toList()
                                   .size() == 1,
           "placed drawing retains vector items and its exact projection");
    expect(repositoryModel.pickUpBoard(0) &&
                   repositoryModel.count() == 1 &&
                   repositoryModel.boardCount() == 0 &&
                   repositoryModel.captureCurrentBoard(
                           QStringLiteral("Entry line"), firstCapture) == 0,
           "picking up removes a placed plane and restores its editable items");

    repositoryModel.setTool(QStringLiteral("text"));
    expect(repositoryModel.addText(
                   0.2, 0.3, QStringLiteral("Brake here")) == 0,
           "a second persistent drawing can be authored");
    QVariantMap secondCapture = firstCapture;
    secondCapture[QStringLiteral("yaw")] = -42.0;
    secondCapture[QStringLiteral("pitch")] = 15.0;
    secondCapture[QStringLiteral("planeX")] = -9.0;
    expect(repositoryModel.captureCurrentBoard(
                   QStringLiteral("Exit note"), secondCapture) == 1 &&
                   repositoryModel.boardCount() == 2,
           "multiple drawings retain independent planes");
    expect(repositoryModel.selectBoard(0) &&
                   repositoryModel.selectedBoardIndex() == 0,
           "a current-map drawing can be selected for viewpoint restore");
    expect(repositoryModel.setBoardVisible(0, false) &&
                   !repositoryModel.boards()
                            .front()
                            .toMap()
                            .value(QStringLiteral("visible"))
                            .toBool() &&
                   repositoryModel.visibleBoards().size() == 1,
           "visibility hides a plane without removing its list entry");

    WhiteboardModel restoredRepository;
    restoredRepository.setMapIdentity(
            QStringLiteral("collision-sha256:stadium"),
            QStringLiteral("Stadium Training"));
    expect(restoredRepository.boardCount() == 2 &&
                   restoredRepository.boards()
                                   .front()
                                   .toMap()
                                   .value(QStringLiteral("mapName"))
                                   .toString() ==
                           QStringLiteral("Stadium Training") &&
                   !restoredRepository.boards()
                            .front()
                            .toMap()
                            .value(QStringLiteral("visible"))
                            .toBool(),
           "drawings and visibility persist across model sessions");
    restoredRepository.setMapIdentity(
            QStringLiteral("collision-sha256:other"),
            QStringLiteral("Sunrise Sprint"));
    expect(!restoredRepository.boards()
                    .front()
                    .toMap()
                    .value(QStringLiteral("isCurrentMap"))
                    .toBool() &&
                   restoredRepository.visibleBoards().isEmpty() &&
                   !restoredRepository.selectBoard(0),
           "other-map drawings remain listed but cannot alter this view");
    restoredRepository.setMapIdentity(
            QStringLiteral("collision-sha256:stadium"),
            QStringLiteral("Stadium Training"));

    QTemporaryDir setDirectory;
    expect(setDirectory.isValid(),
           "temporary export directory is available");
    const QString setPath = setDirectory.filePath(
            QStringLiteral("Stadium notes.json"));
    expect(restoredRepository.exportBoardSet(
                   QUrl::fromLocalFile(setPath)) &&
                   QFileInfo(setPath).size() > 0,
           "a named set exports to the selected local file");
    QFile authoritativeSetFile(setPath);
    expect(authoritativeSetFile.open(QIODevice::ReadOnly),
           "exported authoritative set can be inspected");
    const QByteArray authoritativeSet = authoritativeSetFile.readAll();
    authoritativeSetFile.close();
    expect(authoritativeSet.contains(
                   "\"mapName\": \"Stadium Training\"") &&
                   authoritativeSet.contains("\"version\": 3") &&
                   authoritativeSet.contains(
                           "\"projection\": \"perspective-vertical\"") &&
                   authoritativeSet.contains(
                           "\"projectionVersion\": 1") &&
                   authoritativeSet.contains(
                           "\"fieldOfView\": 63") &&
                   authoritativeSet.contains(
                           "\"canvasHeight\": 768") &&
                   !authoritativeSet.contains(
                           "\"mapName\": \"Stadium notes\"") &&
                   !authoritativeSet.contains(
                           "\"mapName\": \"Entry line\""),
           "exports persist challenge data rather than filenames or drawing labels");

    QJsonDocument versionTwoDocument =
            QJsonDocument::fromJson(authoritativeSet);
    QJsonObject versionTwoRoot = versionTwoDocument.object();
    versionTwoRoot[QStringLiteral("version")] = 2;
    QJsonArray versionTwoBoards =
            versionTwoRoot.value(QStringLiteral("boards")).toArray();
    const QStringList projectionFields{
            QStringLiteral("projectionVersion"),
            QStringLiteral("projection"),
            QStringLiteral("fieldOfView"),
            QStringLiteral("planeDistance"),
            QStringLiteral("viewportWidth"),
            QStringLiteral("viewportHeight"),
            QStringLiteral("contentX"),
            QStringLiteral("contentY"),
            QStringLiteral("contentWidth"),
            QStringLiteral("contentHeight"),
            QStringLiteral("canvasWidth"),
            QStringLiteral("canvasHeight")};
    for (qsizetype index = 0; index < versionTwoBoards.size(); ++index) {
        QJsonObject board = versionTwoBoards.at(index).toObject();
        for (const QString &field : projectionFields) {
            board.remove(field);
        }
        versionTwoBoards[index] = board;
    }
    versionTwoRoot[QStringLiteral("boards")] = versionTwoBoards;
    const QString versionTwoSetPath = setDirectory.filePath(
            QStringLiteral("version-two.json"));
    QFile versionTwoSetFile(versionTwoSetPath);
    const QByteArray versionTwoSet =
            QJsonDocument(versionTwoRoot).toJson();
    expect(versionTwoSetFile.open(QIODevice::WriteOnly) &&
                   versionTwoSetFile.write(versionTwoSet) ==
                           versionTwoSet.size(),
           "version-two migration fixture is written");
    versionTwoSetFile.close();
    QSettings().clear();
    WhiteboardModel versionTwoRepository;
    versionTwoRepository.setMapIdentity(
            QStringLiteral("collision-sha256:stadium"),
            QStringLiteral("Stadium Training"));
    expect(versionTwoRepository.importBoardSet(
                   QUrl::fromLocalFile(versionTwoSetPath)) &&
                   versionTwoRepository.boardCount() == 2 &&
                   versionTwoRepository.boards()
                                   .front()
                                   .toMap()
                                   .value(QStringLiteral(
                                           "projectionVersion"))
                                   .toInt() == 0 &&
                   Near(versionTwoRepository.boards()
                                .front()
                                .toMap()
                                .value(QStringLiteral("canvasWidth"))
                                .toDouble(),
                        1024.0) &&
                   Near(versionTwoRepository.boards()
                                .front()
                                .toMap()
                                .value(QStringLiteral("canvasHeight"))
                                .toDouble(),
                        576.0),
           "version-two drawings migrate with their legacy framing intact");
    WhiteboardModel persistedVersionTwoRepository;
    persistedVersionTwoRepository.setMapIdentity(
            QStringLiteral("collision-sha256:stadium"),
            QStringLiteral("Stadium Training"));
    expect(persistedVersionTwoRepository.boardCount() == 2 &&
                   persistedVersionTwoRepository.boards()
                                   .front()
                                   .toMap()
                                   .value(QStringLiteral(
                                           "projectionVersion"))
                                   .toInt() == 0 &&
                   Near(persistedVersionTwoRepository.boards()
                                .front()
                                .toMap()
                                .value(QStringLiteral("canvasWidth"))
                                .toDouble(),
                        1024.0) &&
                   Near(persistedVersionTwoRepository.boards()
                                .front()
                                .toMap()
                                .value(QStringLiteral("canvasHeight"))
                                .toDouble(),
                        576.0),
           "migrated version-two drawings survive version-three persistence");

    QJsonDocument legacyDocument =
            QJsonDocument::fromJson(authoritativeSet);
    QJsonObject legacyRoot = legacyDocument.object();
    legacyRoot[QStringLiteral("version")] = 1;
    QJsonArray legacyBoards =
            legacyRoot.value(QStringLiteral("boards")).toArray();
    for (qsizetype index = 0; index < legacyBoards.size(); ++index) {
        QJsonObject board = legacyBoards.at(index).toObject();
        board.remove(QStringLiteral("mapName"));
        legacyBoards[index] = board;
    }
    legacyRoot[QStringLiteral("boards")] = legacyBoards;

    const QString transparentImagePath = setDirectory.filePath(
            QStringLiteral("Entry line.png"));
    expect(restoredRepository.exportBoardContentImage(
                   0, QUrl::fromLocalFile(transparentImagePath)),
           "a placed drawing exports to the selected local image file");
    const QImage transparentImage(transparentImagePath);
    const int transparentOpaquePixels =
            OpaquePixelCount(transparentImage);
    expect(!transparentImage.isNull() &&
                   transparentImage.width() == 2048 &&
                   std::abs(transparentImage.height() - 1195) <= 1 &&
                   transparentOpaquePixels > 0 &&
                   transparentOpaquePixels <
                           transparentImage.width() *
                                   transparentImage.height(),
           "drawing-only export preserves plane aspect and transparency");
    expect(restoredRepository.operationMessage().contains(
                   QStringLiteral("Transparent drawing image exported")),
           "successful transparent export reports completion");
    expect(!restoredRepository.exportBoardContentImage(
                   -1, QUrl::fromLocalFile(transparentImagePath)) &&
                   !restoredRepository.exportBoardContentImage(
                           0, QUrl(QStringLiteral(
                                      "https://example.invalid/drawing.png"))),
           "image export rejects invalid boards and non-local destinations");
    const QString directoryAsImage = setDirectory.filePath(
            QStringLiteral("directory.png"));
    expect(QDir().mkpath(directoryAsImage) &&
                   !restoredRepository.exportBoardContentImage(
                           0, QUrl::fromLocalFile(directoryAsImage)),
           "image export reports unwritable destinations without crashing");
    expect(!restoredRepository.saveBoardBackgroundImage(
                   QVariant(QStringLiteral("not an image")),
                   QUrl::fromLocalFile(transparentImagePath)) &&
                   !restoredRepository.saveBoardBackgroundImage(
                           QVariant::fromValue(transparentImage),
                           QUrl(QStringLiteral(
                                      "https://example.invalid/background.png"))),
           "background image saving rejects invalid image values and destinations");

    QSettings().clear();
    WhiteboardModel importedRepository;
    importedRepository.setMapIdentity(
            QStringLiteral("collision-sha256:stadium"),
            QStringLiteral("Stadium Training"));
    expect(importedRepository.importBoardSet(
                   QUrl::fromLocalFile(setPath)) &&
                   importedRepository.boardCount() == 2 &&
                   importedRepository.boards()
                                   .front()
                                   .toMap()
                                   .value(QStringLiteral("items"))
                                   .toList()
                                   .size() == 1,
           "an arbitrary exported set imports with complete drawings");
    const QString legacySetPath = setDirectory.filePath(
            QStringLiteral("legacy-without-map-names.json"));
    QFile legacySetFile(legacySetPath);
    const QByteArray legacySet =
            QJsonDocument(legacyRoot).toJson();
    expect(legacySetFile.open(QIODevice::WriteOnly) &&
                   legacySetFile.write(legacySet) ==
                           legacySet.size(),
           "legacy import fixture without map names is written");
    legacySetFile.close();
    QSettings().clear();
    WhiteboardModel unrelatedLegacyImport;
    unrelatedLegacyImport.setMapIdentity(
            QStringLiteral("collision-sha256:other"),
            QStringLiteral("Sunrise Sprint"));
    expect(!unrelatedLegacyImport.importBoardSet(
                   QUrl::fromLocalFile(legacySetPath)) &&
                   unrelatedLegacyImport.boardCount() == 0 &&
                   unrelatedLegacyImport.operationMessage().contains(
                           QStringLiteral("supported whiteboard set")),
           "unidentified legacy drawings are rejected rather than mislabeled");
    QJsonObject unidentifiedRoot = legacyRoot;
    unidentifiedRoot[QStringLiteral("version")] = 2;
    const QString unidentifiedSetPath = setDirectory.filePath(
            QStringLiteral("current-without-map-names.json"));
    QFile unidentifiedSetFile(unidentifiedSetPath);
    const QByteArray unidentifiedSet =
            QJsonDocument(unidentifiedRoot).toJson();
    expect(unidentifiedSetFile.open(QIODevice::WriteOnly) &&
                   unidentifiedSetFile.write(unidentifiedSet) ==
                           unidentifiedSet.size(),
           "current-schema unidentified drawing fixture is written");
    unidentifiedSetFile.close();
    expect(!unrelatedLegacyImport.importBoardSet(
                   QUrl::fromLocalFile(unidentifiedSetPath)) &&
                   unrelatedLegacyImport.boardCount() == 0 &&
                   unrelatedLegacyImport.operationMessage().contains(
                           QStringLiteral("identity or map data")),
           "current-schema drawings require a nonempty authoritative map name");
    const int beforeInvalidImport =
            importedRepository.boardCount();
    const QString invalidPath = setDirectory.filePath(
            QStringLiteral("invalid.json"));
    QFile invalidFile(invalidPath);
    expect(invalidFile.open(QIODevice::WriteOnly) &&
                   invalidFile.write("{\"boards\":[42]}") > 0,
           "invalid import fixture is written");
    invalidFile.close();
    expect(!importedRepository.importBoardSet(
                   QUrl::fromLocalFile(invalidPath)) &&
                   importedRepository.boardCount() ==
                           beforeInvalidImport,
           "invalid imports fail atomically without changing the list");
    QFile exportedSet(setPath);
    expect(exportedSet.open(QIODevice::ReadOnly),
           "exported set can be reopened for corruption testing");
    QByteArray invalidVisibility = exportedSet.readAll();
    exportedSet.close();
    invalidVisibility.replace(
            "\"visible\": false", "\"visible\": \"yes\"");
    const QString invalidVisibilityPath = setDirectory.filePath(
            QStringLiteral("invalid-visibility.json"));
    QFile invalidVisibilityFile(invalidVisibilityPath);
    expect(invalidVisibilityFile.open(QIODevice::WriteOnly) &&
                   invalidVisibilityFile.write(invalidVisibility) ==
                           invalidVisibility.size(),
           "type-confused import fixture is written");
    invalidVisibilityFile.close();
    expect(!importedRepository.importBoardSet(
                   QUrl::fromLocalFile(invalidVisibilityPath)) &&
                   importedRepository.boardCount() ==
                           beforeInvalidImport,
           "schema type errors are rejected atomically");

    QJsonObject invalidProjectionRoot =
            QJsonDocument::fromJson(authoritativeSet).object();
    QJsonArray invalidProjectionBoards =
            invalidProjectionRoot.value(
                                     QStringLiteral("boards"))
                    .toArray();
    QJsonObject invalidProjectionBoard =
            invalidProjectionBoards.at(0).toObject();
    invalidProjectionBoard[QStringLiteral("fieldOfView")] = 180.0;
    invalidProjectionBoards[0] = invalidProjectionBoard;
    invalidProjectionRoot[QStringLiteral("boards")] =
            invalidProjectionBoards;
    const QString invalidProjectionPath = setDirectory.filePath(
            QStringLiteral("invalid-projection.json"));
    QFile invalidProjectionFile(invalidProjectionPath);
    const QByteArray invalidProjectionSet =
            QJsonDocument(invalidProjectionRoot).toJson();
    expect(invalidProjectionFile.open(QIODevice::WriteOnly) &&
                   invalidProjectionFile.write(invalidProjectionSet) ==
                           invalidProjectionSet.size(),
           "invalid projection fixture is written");
    invalidProjectionFile.close();
    expect(!importedRepository.importBoardSet(
                   QUrl::fromLocalFile(invalidProjectionPath)) &&
                   importedRepository.boardCount() ==
                           beforeInvalidImport &&
                   importedRepository.operationMessage().contains(
                           QStringLiteral("projection data")),
           "invalid projection metadata is rejected atomically");

    invalidProjectionBoard =
            invalidProjectionBoards.at(0).toObject();
    invalidProjectionBoard[QStringLiteral("fieldOfView")] = 63.0;
    invalidProjectionBoard[QStringLiteral("canvasWidth")] = 8193.0;
    invalidProjectionBoards[0] = invalidProjectionBoard;
    invalidProjectionRoot[QStringLiteral("boards")] =
            invalidProjectionBoards;
    const QString oversizedCanvasPath = setDirectory.filePath(
            QStringLiteral("oversized-canvas.json"));
    QFile oversizedCanvasFile(oversizedCanvasPath);
    const QByteArray oversizedCanvasSet =
            QJsonDocument(invalidProjectionRoot).toJson();
    expect(oversizedCanvasFile.open(QIODevice::WriteOnly) &&
                   oversizedCanvasFile.write(oversizedCanvasSet) ==
                           oversizedCanvasSet.size(),
           "oversized projection canvas fixture is written");
    oversizedCanvasFile.close();
    expect(!importedRepository.importBoardSet(
                   QUrl::fromLocalFile(oversizedCanvasPath)) &&
                   importedRepository.boardCount() ==
                           beforeInvalidImport &&
                   importedRepository.operationMessage().contains(
                           QStringLiteral("projection data")),
           "oversized projection canvases are rejected atomically");

    QByteArray mapNeutralSet = invalidVisibility;
    mapNeutralSet.replace(
            "\"visible\": \"yes\"", "\"visible\": false");
    mapNeutralSet.replace(
            "collision-sha256:stadium", "");
    const QString mapNeutralPath = setDirectory.filePath(
            QStringLiteral("map-neutral.json"));
    QFile mapNeutralFile(mapNeutralPath);
    expect(mapNeutralFile.open(QIODevice::WriteOnly) &&
                   mapNeutralFile.write(mapNeutralSet) ==
                           mapNeutralSet.size(),
           "map-neutral import fixture is written");
    mapNeutralFile.close();
    QSettings().clear();
    WhiteboardModel neutralRepository;
    expect(!neutralRepository.importBoardSet(
                   QUrl::fromLocalFile(mapNeutralPath)) &&
                   neutralRepository.boardCount() == 0,
           "map-neutral imports require a loaded map");
    neutralRepository.setMapIdentity(
            QStringLiteral("collision-sha256:stadium"),
            QStringLiteral("Stadium Training"));
    expect(neutralRepository.importBoardSet(
                   QUrl::fromLocalFile(mapNeutralPath)) &&
                   neutralRepository.boardCount() == 2 &&
                   neutralRepository.visibleBoards().size() == 1 &&
                   neutralRepository.boards()
                                   .front()
                                   .toMap()
                                   .value(QStringLiteral("isCurrentMap"))
                                   .toBool(),
           "map-neutral imports attach atomically to the loaded map");

    expect(importedRepository.removeBoard(0) &&
                   importedRepository.boardCount() == 1,
           "drawings can be removed individually from the list");

    forevertas::viewer::RegisterRaceViewerQmlTypes();
    QQmlEngine engine;
    QQmlComponent component(
            &engine,
            QUrl::fromLocalFile(
                    QStringLiteral(FOREVERTAS_SOURCE_DIR)
                    + QStringLiteral("/qml/WhiteboardOverlay.qml")));
    if (component.isError()) {
        for (const QQmlError &error : component.errors()) {
            std::cerr << error.toString().toStdString() << '\n';
        }
    }
    std::unique_ptr<QObject> overlay(component.createWithInitialProperties({
            {QStringLiteral("model"),
             QVariant::fromValue(static_cast<QObject *>(&model))},
            {QStringLiteral("available"), true}}));
    expect(overlay != nullptr, "whiteboard overlay QML instantiates");
    if (overlay != nullptr) {
        overlay->setProperty("width", 1000.0);
        overlay->setProperty("height", 700.0);
        QCoreApplication::processEvents();
        auto *const board = qobject_cast<QQuickItem *>(
                overlay->findChild<QObject *>(
                        QStringLiteral("whiteboardBoardArea")));
        auto *const toolbar = qobject_cast<QQuickItem *>(
                overlay->findChild<QObject *>(
                        QStringLiteral("whiteboardToolbar")));
        QObject *const toggle = overlay->findChild<QObject *>(
                QStringLiteral("whiteboardModeToggle"));
        auto *const toggleItem = qobject_cast<QQuickItem *>(toggle);
        QObject *const input = overlay->findChild<QObject *>(
                QStringLiteral("whiteboardDrawingInput"));
        QObject *const sizeSlider = overlay->findChild<QObject *>(
                QStringLiteral("whiteboardSizeSlider"));
        QObject *const sizeValueField = overlay->findChild<QObject *>(
                QStringLiteral("whiteboardSizeSliderValueField"));
        QObject *const customColor = overlay->findChild<QObject *>(
                QStringLiteral("whiteboardCustomColor"));
        QObject *const textEditor = overlay->findChild<QObject *>(
                QStringLiteral("whiteboardTextEditor"));
        auto *const resizeHandle = qobject_cast<QQuickItem *>(
                overlay->findChild<QObject *>(
                        QStringLiteral("whiteboardResizeHandle")));
        auto *const deleteButton = qobject_cast<QQuickItem *>(
                overlay->findChild<QObject *>(
                        QStringLiteral("whiteboardDeleteButton")));
        QObject *const drawingRepeater = overlay->findChild<QObject *>(
                QStringLiteral("whiteboardDrawingRepeater"));
        QObject *const placeButton = overlay->findChild<QObject *>(
                QStringLiteral("whiteboardPlaceButton"));
        QObject *const pickUpButton = overlay->findChild<QObject *>(
                QStringLiteral("whiteboardPickUpButton"));
        QObject *const inactiveListButton = overlay->findChild<QObject *>(
                QStringLiteral("whiteboardInactiveListButton"));
        QObject *const drawingList = overlay->findChild<QObject *>(
                QStringLiteral("whiteboardDrawingList"));
        QObject *const importButton = overlay->findChild<QObject *>(
                QStringLiteral("whiteboardImportButton"));
        QObject *const exportButton = overlay->findChild<QObject *>(
                QStringLiteral("whiteboardExportButton"));
        QObject *const imageExportMenu = overlay->findChild<QObject *>(
                QStringLiteral("whiteboardImageExportMenu"));
        QObject *const imageExportDialog = overlay->findChild<QObject *>(
                QStringLiteral("whiteboardImageExportDialog"));
        QObject *const backgroundExportItem =
                overlay->findChild<QObject *>(
                        QStringLiteral(
                                "whiteboardExportBackgroundMenuItem"));
        QObject *const transparentExportItem =
                overlay->findChild<QObject *>(
                        QStringLiteral(
                                "whiteboardExportTransparentMenuItem"));
        auto *const toolbarContent = qobject_cast<QQuickItem *>(
                overlay->findChild<QObject *>(
                        QStringLiteral("whiteboardToolbarContent")));
        auto *const primaryToolbarRow = qobject_cast<QQuickItem *>(
                overlay->findChild<QObject *>(
                        QStringLiteral("whiteboardPrimaryToolbarRow")));
        auto *const secondaryToolbarRow = qobject_cast<QQuickItem *>(
                overlay->findChild<QObject *>(
                        QStringLiteral("whiteboardSecondaryToolbarRow")));
        QList<QQuickItem *> toolButtons;
        for (const QString &toolName : {
                     QStringLiteral("select"), QStringLiteral("pen"),
                     QStringLiteral("line"), QStringLiteral("rectangle"),
                     QStringLiteral("ellipse"), QStringLiteral("text"),
                     QStringLiteral("eraser")}) {
            toolButtons.push_back(qobject_cast<QQuickItem *>(
                    overlay->findChild<QObject *>(
                            QStringLiteral("whiteboardToolButton_") +
                            toolName)));
        }
        auto *const colorButton = qobject_cast<QQuickItem *>(
                overlay->findChild<QObject *>(
                        QStringLiteral("whiteboardColorButton")));
        auto *const sizeSliderItem = qobject_cast<QQuickItem *>(sizeSlider);
        auto *const sizeValueFieldItem =
                qobject_cast<QQuickItem *>(sizeValueField);
        auto *const pickUpButtonItem = qobject_cast<QQuickItem *>(pickUpButton);
        auto *const placeButtonItem = qobject_cast<QQuickItem *>(placeButton);
        auto *const activeListButtonItem = qobject_cast<QQuickItem *>(
                overlay->findChild<QObject *>(
                        QStringLiteral("whiteboardActiveListButton")));
        expect(board != nullptr && Near(board->y(), 52.0) &&
                       Near(board->height(), 648.0),
               "drawing surface overlays the viewer below its header");
        expect(toolbar != nullptr && toolbar->width() <= 570.0 &&
                       toolbar->height() >= 84.0 &&
                       toolbar->height() <= 90.0,
               "active toolbar hugs its controls in a desktop viewport");
        const QQuickItem *const eraserButton = toolButtons.isEmpty()
                ? nullptr : toolButtons.constLast();
        const QPointF activeLeftPosition =
                toggleItem != nullptr && toolbar != nullptr
                ? toggleItem->mapToItem(toolbar, QPointF()) : QPointF();
        const QPointF eraserPosition =
                eraserButton != nullptr && toolbar != nullptr
                ? eraserButton->mapToItem(toolbar, QPointF()) : QPointF();
        const QPointF activeListPosition =
                activeListButtonItem != nullptr && toolbar != nullptr
                ? activeListButtonItem->mapToItem(toolbar, QPointF())
                : QPointF();
        const qreal activeControlsRight = std::max(
                eraserPosition.x() +
                        (eraserButton != nullptr ? eraserButton->width() : 0.0),
                activeListPosition.x() +
                        (activeListButtonItem != nullptr
                                 ? activeListButtonItem->width() : 0.0));
        expect(toolbar != nullptr && toggleItem != nullptr &&
                       eraserButton != nullptr &&
                       activeListButtonItem != nullptr &&
                       Near(activeLeftPosition.x(),
                            toolbar->width() - activeControlsRight, 0.6),
               "active toolbar has matching left and right padding");
        const QPointF togglePosition =
                toggleItem != nullptr && primaryToolbarRow != nullptr
                ? toggleItem->mapToItem(primaryToolbarRow, QPointF())
                : QPointF();
        bool primaryRowOrdered = toggleItem != nullptr &&
                primaryToolbarRow != nullptr;
        qreal primaryRight = toggleItem != nullptr
                ? togglePosition.x() + toggleItem->width() : 0.0;
        for (QQuickItem *const toolButton : toolButtons) {
            const QPointF position =
                    toolButton != nullptr && primaryToolbarRow != nullptr
                    ? toolButton->mapToItem(primaryToolbarRow, QPointF())
                    : QPointF();
            primaryRowOrdered = primaryRowOrdered && toolButton != nullptr &&
                    Near(position.y(), togglePosition.y(), 1.0) &&
                    position.x() >= primaryRight + 3.0;
            if (toolButton != nullptr) {
                primaryRight = position.x() + toolButton->width();
            }
        }
        primaryRowOrdered = primaryRowOrdered && toolbarContent != nullptr &&
                primaryRight <= toolbarContent->width() + 0.1;
        const QList<QQuickItem *> secondaryControls = {
                colorButton, sizeSliderItem, sizeValueFieldItem, deleteButton,
                pickUpButtonItem, placeButtonItem, activeListButtonItem};
        const QPointF colorPosition =
                colorButton != nullptr && secondaryToolbarRow != nullptr
                ? colorButton->mapToItem(secondaryToolbarRow, QPointF())
                : QPointF();
        bool secondaryRowOrdered = colorButton != nullptr &&
                secondaryToolbarRow != nullptr;
        qreal secondaryRight = 0.0;
        for (QQuickItem *const control : secondaryControls) {
            const QPointF position =
                    control != nullptr && secondaryToolbarRow != nullptr
                    ? control->mapToItem(secondaryToolbarRow, QPointF())
                    : QPointF();
            secondaryRowOrdered = secondaryRowOrdered && control != nullptr &&
                    Near(position.y(), colorPosition.y(), 1.0) &&
                    position.x() >= secondaryRight;
            if (control != nullptr) {
                secondaryRight = position.x() + control->width();
            }
        }
        secondaryRowOrdered = secondaryRowOrdered &&
                toolbarContent != nullptr &&
                secondaryRight <= toolbarContent->width() + 0.1;
        expect(primaryRowOrdered && secondaryRowOrdered,
               "active whiteboard controls form two compact ordered rows");
        if ((!primaryRowOrdered || !secondaryRowOrdered) &&
            toolbar != nullptr) {
            std::cerr << "desktop toolbar=" << toolbar->width() << 'x'
                      << toolbar->height() << ", primary="
                      << (primaryToolbarRow ? primaryToolbarRow->width() : -1.0)
                      << 'x'
                      << (primaryToolbarRow ? primaryToolbarRow->height() : -1.0)
                      << ", secondary="
                      << (secondaryToolbarRow ? secondaryToolbarRow->width()
                                              : -1.0)
                      << 'x'
                      << (secondaryToolbarRow ? secondaryToolbarRow->height()
                                              : -1.0)
                      << '\n';
            for (QQuickItem *const toolButton : toolButtons) {
                if (toolButton != nullptr) {
                    std::cerr << toolButton->objectName().toStdString() << '='
                              << toolButton->x() << ',' << toolButton->y() << ' '
                              << toolButton->width() << 'x'
                              << toolButton->height() << '\n';
                }
            }
        }
        expect(toggle != nullptr &&
                       toggle->property("enabled").toBool() &&
                       toggle->property("checked").toBool(),
               "available mode toggle reflects active state");
        expect(input != nullptr && input->property("enabled").toBool(),
               "active whiteboard captures drawing input");
        expect(resizeHandle != nullptr &&
                       resizeHandle->parentItem() == board &&
                       input->property("z").toReal() > 2.0,
               "move and resize input persists outside refreshed delegates");
        expect(sizeSlider != nullptr &&
                       sizeSlider->property("visible").toBool() &&
                       sizeValueField != nullptr &&
                       sizeValueField->property("visible").toBool() &&
                       sizeValueField
                               ->property("exactValueEditor").toBool() &&
                       sizeValueField->property("integer").toBool() &&
                       sizeValueField->property("from").toReal() == 1.0 &&
                       sizeValueField->property("to").toReal() == 24.0 &&
                       !sizeValueField
                                ->property("maximumEnabled").toBool() &&
                       customColor != nullptr &&
                       textEditor != nullptr,
               "size slider, exact entry, arbitrary color, and text controls "
               "are connected");
        if (sizeSlider != nullptr) {
            sizeSlider->setProperty("value", 9.0);
            QMetaObject::invokeMethod(sizeSlider, "moved");
            QCoreApplication::processEvents();
            expect(Near(model.size(), 9.0),
                   "keyboard and accessibility slider changes update size");
        }
        if (sizeSlider != nullptr && sizeValueField != nullptr) {
            QVariant committed;
            sizeValueField->setProperty("text", QStringLiteral("13"));
            const bool invoked = QMetaObject::invokeMethod(
                    sizeValueField,
                    "commitText",
                    Qt::DirectConnection,
                    Q_RETURN_ARG(QVariant, committed));
            QCoreApplication::processEvents();
            expect(invoked && committed.toBool() &&
                           Near(model.size(), 13.0) &&
                           Near(sizeSlider->property("value").toReal(), 13.0) &&
                           sizeValueField->property("text").toString() ==
                                   QStringLiteral("13"),
                   "typed drawing size updates the model and slider exactly");

            sizeValueField->setProperty("text", QStringLiteral("13.5"));
            committed.clear();
            const bool rejectedFraction = QMetaObject::invokeMethod(
                    sizeValueField,
                    "commitText",
                    Qt::DirectConnection,
                    Q_RETURN_ARG(QVariant, committed));
            QCoreApplication::processEvents();
            expect(rejectedFraction && !committed.toBool() &&
                           Near(model.size(), 13.0) &&
                           sizeValueField
                                   ->property("validationFailed").toBool(),
                   "drawing size entry rejects fractional values without "
                   "changing the current size");

            sizeValueField->setProperty("text", QStringLiteral("25"));
            committed.clear();
            const bool acceptedAboveSlider = QMetaObject::invokeMethod(
                    sizeValueField,
                    "commitText",
                    Qt::DirectConnection,
                    Q_RETURN_ARG(QVariant, committed));
            QCoreApplication::processEvents();
            expect(acceptedAboveSlider && committed.toBool() &&
                           Near(model.size(), 25.0),
                   "drawing size entry accepts values above the slider range");
        }
        expect(std::all_of(toolButtons.cbegin(), toolButtons.cend(),
                           [](QQuickItem *button) {
                               return button != nullptr;
                           }),
               "every whiteboard tool has a mode control");
        expect(placeButton != nullptr && pickUpButton != nullptr &&
                       drawingList != nullptr &&
                       importButton != nullptr &&
                       exportButton != nullptr &&
                       imageExportMenu != nullptr &&
                       imageExportDialog != nullptr &&
                       backgroundExportItem != nullptr &&
                       transparentExportItem != nullptr,
               "placement, set transfer, and both image export modes are present");
        expect(placeButton != nullptr &&
                       !placeButton->property("enabled").toBool(),
               "a draft without an authoritative loaded-map identity cannot "
               "be placed");
        expect(drawingRepeater != nullptr &&
                       drawingRepeater->property("count").toInt() ==
                               retainedCount,
               "each item is represented by an independent canvas delegate");

        overlay->setProperty("width", 520.0);
        overlay->setProperty("height", 390.0);
        QCoreApplication::processEvents();
        const QPointF deletePosition =
                deleteButton != nullptr && toolbar != nullptr
                ? deleteButton->mapToItem(toolbar, QPointF())
                : QPointF(-1.0, -1.0);
        const bool compactLayout =
                toolbar != nullptr && toolbar->width() <= 492.0 &&
                toolbar->height() >= 84.0 && toolbar->height() <= 180.0 &&
                board != nullptr && Near(board->width(), 520.0) &&
                Near(board->height(), 338.0) &&
                deleteButton != nullptr &&
                deletePosition.x() + deleteButton->width() <=
                        toolbar->width() - 7.0 &&
                deletePosition.y() + deleteButton->height() <=
                        toolbar->height() - 3.0;
        if (!compactLayout && toolbar != nullptr &&
            board != nullptr && deleteButton != nullptr) {
            std::cerr << "compact layout: toolbar="
                      << toolbar->width() << 'x' << toolbar->height()
                      << ", flow="
                      << (toolbarContent != nullptr
                                  ? toolbarContent->width()
                                  : -1.0)
                      << 'x'
                      << (toolbarContent != nullptr
                                  ? toolbarContent->height()
                                  : -1.0)
                      << ", toggle="
                      << (toggleItem != nullptr ? toggleItem->x() : -1.0)
                      << ','
                      << (toggleItem != nullptr ? toggleItem->y() : -1.0)
                      << ' '
                      << (toggleItem != nullptr ? toggleItem->width() : -1.0)
                      << 'x'
                      << (toggleItem != nullptr ? toggleItem->height() : -1.0)
                      << ", board=" << board->width() << 'x'
                      << board->height() << ", delete="
                      << deletePosition.x() << ',' << deletePosition.y()
                      << ' ' << deleteButton->width() << 'x'
                      << deleteButton->height() << '\n';
        }
        expect(compactLayout,
               "whiteboard remains bounded at a compact window size");
        overlay->setProperty("drawingListOpen", true);
        QCoreApplication::processEvents();
        expect(drawingList != nullptr &&
                       Near(drawingList->property("x").toDouble(), 14.0) &&
                       drawingList->property("y").toDouble() >=
                               toolbar->y() + toolbar->height() + 7.9 &&
                       drawingList->property("height").toDouble() <=
                               286.0,
               "drawing repository stays below the left whiteboard actions");
        overlay->setProperty("drawingListOpen", false);

        textEditor->setProperty("visible", true);
        model.setActive(false);
        QCoreApplication::processEvents();
        QObject *const inactiveListContent = inactiveListButton == nullptr
                ? nullptr
                : inactiveListButton->property("contentItem")
                          .value<QObject *>();
        if (toolbar == nullptr || toolbar->width() < 190.0 ||
            toolbar->width() > 240.0 || inactiveListContent == nullptr ||
            inactiveListContent->property("truncated").toBool() ||
            input == nullptr || input->property("enabled").toBool() ||
            toggle == nullptr || toggle->property("checked").toBool() ||
            textEditor->property("visible").toBool()) {
            std::cerr << "inactive whiteboard: toolbar="
                      << (toolbar != nullptr ? toolbar->width() : -1.0)
                      << ", list=" << (inactiveListButton != nullptr)
                      << ", content=" << (inactiveListContent != nullptr)
                      << ", truncated="
                      << (inactiveListContent != nullptr
                                  ? inactiveListContent
                                            ->property("truncated")
                                            .toBool()
                                  : true)
                      << ", input="
                      << (input != nullptr
                                  ? input->property("enabled").toBool()
                                  : true)
                      << ", toggle="
                      << (toggle != nullptr
                                  ? toggle->property("checked").toBool()
                                  : true)
                      << ", editor="
                      << textEditor->property("visible").toBool() << '\n';
        }
        expect(toolbar != nullptr && toolbar->width() >= 190.0 &&
                       toolbar->width() <= 240.0 &&
                       inactiveListContent != nullptr &&
                       !inactiveListContent->property("truncated").toBool() &&
                       input != nullptr &&
                       !input->property("enabled").toBool() &&
                       toggle != nullptr &&
                       !toggle->property("checked").toBool() &&
                       !textEditor->property("visible").toBool(),
               "normal 3D mode collapses controls and releases input");
        const QPointF inactiveLeftPosition =
                toggleItem != nullptr && toolbar != nullptr
                ? toggleItem->mapToItem(toolbar, QPointF()) : QPointF();
        const auto *const inactiveListItem =
                qobject_cast<QQuickItem *>(inactiveListButton);
        const QPointF inactiveListPosition =
                inactiveListItem != nullptr && toolbar != nullptr
                ? inactiveListItem->mapToItem(toolbar, QPointF()) : QPointF();
        const qreal inactiveControlsRight = inactiveListPosition.x() +
                (inactiveListItem != nullptr ? inactiveListItem->width() : 0.0);
        if (toolbar != nullptr && toggleItem != nullptr &&
            inactiveListItem != nullptr &&
            !Near(inactiveLeftPosition.x(),
                  toolbar->width() - inactiveControlsRight, 0.6)) {
            std::cerr << "inactive padding: left="
                      << inactiveLeftPosition.x() << ", right="
                      << toolbar->width() - inactiveControlsRight
                      << ", listX=" << inactiveListPosition.x() << '\n';
        }
        expect(toolbar != nullptr && toggleItem != nullptr &&
                       inactiveListItem != nullptr &&
                       Near(inactiveLeftPosition.x(),
                            toolbar->width() - inactiveControlsRight, 0.6),
               "inactive toolbar has matching left and right padding");
        expect(drawingRepeater != nullptr &&
                       drawingRepeater->property("count").toInt() ==
                               retainedCount &&
                       model.count() == retainedCount,
               "drawings remain visible when normal 3D interaction resumes");

        overlay->setProperty("available", false);
        QCoreApplication::processEvents();
        expect(toggle != nullptr &&
                       !toggle->property("enabled").toBool(),
               "empty viewer disables whiteboard activation");
    }

    restoredRepository.setMapIdentity(
            QStringLiteral("collision-sha256:other"),
            QStringLiteral("Sunrise Sprint"));
    QQuickWindow drawingListWindow;
    drawingListWindow.setWidth(520);
    drawingListWindow.setHeight(520);
    std::unique_ptr<QObject> drawingListOverlay(
            component.createWithInitialProperties({
                    {QStringLiteral("model"),
                     QVariant::fromValue(static_cast<QObject *>(
                             &restoredRepository))},
                    {QStringLiteral("available"), true},
                    {QStringLiteral("drawingListOpen"), true}}));
    expect(drawingListOverlay != nullptr,
           "drawing-list map-name view instantiates");
    if (drawingListOverlay != nullptr) {
        auto *const drawingListRoot =
                qobject_cast<QQuickItem *>(drawingListOverlay.get());
        if (drawingListRoot != nullptr) {
            drawingListRoot->setParentItem(
                    drawingListWindow.contentItem());
        }
        drawingListOverlay->setProperty("width", 520.0);
        drawingListOverlay->setProperty("height", 520.0);
        drawingListWindow.show();
        QCoreApplication::processEvents();
        const QList<QQuickItem *> mapLabels = FindVisualItems(
                drawingListRoot,
                QStringLiteral("whiteboardBoardMapName"));
        const bool everyLabelIsAuthoritative =
                mapLabels.size() == restoredRepository.boardCount() &&
                std::all_of(
                        mapLabels.cbegin(),
                        mapLabels.cend(),
                        [](const QQuickItem *label) {
                            const QString text =
                                    label->property("text").toString();
                            return text == QStringLiteral(
                                                   "Stadium Training") &&
                                    !text.contains(
                                            QStringLiteral("Other map"));
                        });
        if (!everyLabelIsAuthoritative) {
            QObject *const listView =
                    drawingListOverlay->findChild<QObject *>(
                            QStringLiteral("whiteboardBoardListView"));
            std::cerr << "other-map label count=" << mapLabels.size()
                      << ", boards="
                      << restoredRepository.boardCount()
                      << ", listCount="
                      << (listView != nullptr
                                  ? listView->property("count").toInt()
                                  : -1)
                      << ", listVisible="
                      << (listView != nullptr
                                  ? listView->property("visible").toBool()
                                  : false)
                      << ", listSize="
                      << (listView != nullptr
                                  ? listView->property("width").toDouble()
                                  : -1.0)
                      << 'x'
                      << (listView != nullptr
                                  ? listView->property("height").toDouble()
                                  : -1.0)
                      << '\n';
            for (const QQuickItem *label : mapLabels) {
                std::cerr << "  label=\""
                          << label->property("text")
                                     .toString()
                                     .toStdString()
                          << "\"\n";
            }
        }
        expect(everyLabelIsAuthoritative,
               "other-map drawing rows show their stored challenge name");
        restoredRepository.setMapIdentity(
                QStringLiteral("collision-sha256:stadium"),
                QStringLiteral("Stadium Training"));
        QCoreApplication::processEvents();
        const QList<QQuickItem *> currentMapLabels =
                FindVisualItems(
                        drawingListRoot,
                        QStringLiteral("whiteboardBoardMapName"));
        const bool everyCurrentLabelIsAuthoritative =
                currentMapLabels.size() ==
                        restoredRepository.boardCount() &&
                std::all_of(
                       currentMapLabels.cbegin(),
                       currentMapLabels.cend(),
                       [](const QQuickItem *label) {
                           const QString text =
                                   label->property("text").toString();
                           return text.startsWith(
                                          QStringLiteral(
                                                  "Stadium Training")) &&
                                   text.contains(
                                           QStringLiteral("Current"));
                       });
        if (!everyCurrentLabelIsAuthoritative) {
            std::cerr << "current-map label count="
                      << currentMapLabels.size() << ", boards="
                      << restoredRepository.boardCount() << '\n';
            for (const QQuickItem *label : currentMapLabels) {
                std::cerr << "  label=\""
                          << label->property("text")
                                     .toString()
                                     .toStdString()
                          << "\"\n";
            }
        }
        expect(everyCurrentLabelIsAuthoritative,
               "current-map rows keep the challenge name and add status");
    }
    restoredRepository.setMapIdentity(
            QStringLiteral("collision-sha256:stadium"),
            QStringLiteral("Stadium Training"));

    QQmlComponent planesComponent(
            &engine,
            QUrl::fromLocalFile(
                    QStringLiteral(FOREVERTAS_SOURCE_DIR)
                    + QStringLiteral("/qml/WhiteboardPlanes.qml")));
    if (planesComponent.isError()) {
        for (const QQmlError &error : planesComponent.errors()) {
            std::cerr << error.toString().toStdString() << '\n';
        }
    }
    std::unique_ptr<QObject> planes(
            planesComponent.createWithInitialProperties({
                    {QStringLiteral("model"),
                     QVariant::fromValue(static_cast<QObject *>(
                             &importedRepository))},
                    {QStringLiteral("cameraTarget"),
                     QVariant::fromValue(QVector3D())},
                    {QStringLiteral("cameraPosition"),
                     QVariant::fromValue(QVector3D())},
                    {QStringLiteral("freeCamera"), false},
                    {QStringLiteral("orbitYaw"), 35.0},
                    {QStringLiteral("orbitPitch"), -20.0},
                    {QStringLiteral("orbitDistance"), 38.0}}));
    expect(planes != nullptr,
           "persistent whiteboard plane view instantiates");
    if (planes != nullptr) {
        planes->setProperty("width", 1420.0);
        planes->setProperty("height", 820.0);
        planes->setProperty("fieldOfView", 63.0);
        planes->setProperty("contentTop", 52.0);
        planes->setProperty("exactBoardIndex", 0);
        QCoreApplication::processEvents();
        QObject *const planeRepeater = planes->findChild<QObject *>(
                QStringLiteral("whiteboardPlaneRepeater"));
        const QVariantMap exactBoard =
                importedRepository.boards().front().toMap();
        QObject *const exactPlane = planes->findChild<QObject *>(
                QStringLiteral("whiteboardPlane_") +
                exactBoard.value(QStringLiteral("id")).toString());
        const double fullHeight =
                2.0 * std::tan(
                              63.0 * std::acos(-1.0) / 360.0) *
                11.0;
        const double expectedWideHeight =
                fullHeight * (820.0 - 52.0) / 820.0;
        const double expectedWideWidth =
                fullHeight * 1420.0 / 820.0;
        expect(planeRepeater != nullptr &&
                       planeRepeater->property("count").toInt() ==
                               importedRepository.boardCount(),
               "every listed drawing has an independent 3D plane delegate");
        expect(exactPlane != nullptr &&
                       exactPlane
                               ->property("exactProjectionActive")
                               .toBool() &&
                       Near(exactPlane
                                    ->property("effectivePlaneWidth")
                                    .toDouble(),
                            expectedWideWidth) &&
                       Near(exactPlane
                                    ->property("effectivePlaneHeight")
                                    .toDouble(),
                            expectedWideHeight) &&
                       Near(exactPlane
                                    ->property("sourceCanvasWidth")
                                    .toDouble(),
                            1420.0) &&
                       Near(exactPlane
                                    ->property("sourceCanvasHeight")
                                    .toDouble(),
                            768.0),
               "exact focus reconstructs the saved vertical projection and "
               "native drawing canvas");
        planes->setProperty("width", 1240.0);
        planes->setProperty("height", 620.0);
        QCoreApplication::processEvents();
        const double expectedCompactHeight =
                fullHeight * (620.0 - 52.0) / 620.0;
        const double expectedCompactWidth =
                fullHeight * 1240.0 / 620.0;
        expect(exactPlane != nullptr &&
                       Near(exactPlane
                                    ->property("effectivePlaneWidth")
                                    .toDouble(),
                            expectedCompactWidth) &&
                       Near(exactPlane
                                    ->property("effectivePlaneHeight")
                                    .toDouble(),
                            expectedCompactHeight) &&
                       !Near(expectedWideWidth, expectedCompactWidth) &&
                       !Near(expectedWideHeight, expectedCompactHeight),
               "exact focused plane follows compact viewport aspect and "
               "header conversion instead of retaining stale framing");
        planes->setProperty("exactBoardIndex", -1);
        QCoreApplication::processEvents();
        expect(exactPlane != nullptr &&
                       !exactPlane
                                ->property("exactProjectionActive")
                                .toBool() &&
                       Near(exactPlane
                                    ->property("effectivePlaneWidth")
                                    .toDouble(),
                            exactBoard
                                    .value(QStringLiteral("planeWidth"))
                                    .toDouble()) &&
                       Near(exactPlane
                                    ->property("effectivePlaneHeight")
                                    .toDouble(),
                            exactBoard
                                    .value(QStringLiteral("planeHeight"))
                                    .toDouble()),
               "leaving exact focus restores the drawing's persisted world "
               "plane without altering it");
        importedRepository.setBoardVisible(0, false);
        planes->setProperty("forcedBoardIndex", 0);
        planes->setProperty("exportMode", true);
        QCoreApplication::processEvents();
        expect(planeRepeater != nullptr &&
                       planeRepeater->property("count").toInt() == 1 &&
                       planes->property("exportMode").toBool(),
               "background export can render a hidden chosen board without persisting visibility");
        expect(!importedRepository.boards()
                        .front()
                        .toMap()
                        .value(QStringLiteral("visible"))
                        .toBool(),
               "forcing an export plane leaves stored visibility unchanged");
    }

    QSettings().clear();
    if (failures == 0) {
        std::cout << "whiteboard model and vector rendering tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
