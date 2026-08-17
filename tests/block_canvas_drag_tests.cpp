#include "app/search_controller.h"

#include <QApplication>
#include <QCoreApplication>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QStandardPaths>
#include <QSettings>
#include <QTimer>

#include <cmath>
#include <iostream>

namespace {

using forevertas::app::SearchController;

bool Check(bool condition, const char *message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

QVariantList WindowIds(const SearchController &controller) {
    QVariantList ids;
    const QVariantList groups = controller.blockScript()
            .value(QStringLiteral("groups"))
            .toList();
    for (const QVariant &value : groups) {
        ids.push_back(value.toMap().value(QStringLiteral("blockId")));
    }
    return ids;
}

int FirstWindowAtomId(const SearchController &controller) {
    const QVariantList groups = controller.blockScript()
            .value(QStringLiteral("groups"))
            .toList();
    if (groups.isEmpty()) return 0;
    const QVariantList atoms = groups.front()
            .toMap()
            .value(QStringLiteral("atoms"))
            .toList();
    return atoms.isEmpty() ? 0 : atoms.front().toInt();
}

// Visual-tree lookup: Repeater delegates can carry a null QObject parent
// while remaining fully parented in the scene, so findChildren misses
// them. The scene graph's childItems list is authoritative.
QQuickItem *FindItem(QQuickItem *root, const QString &name) {
    if (root->objectName() == name)
        return root;
    for (QQuickItem *const child : root->childItems()) {
        if (QQuickItem *const found = FindItem(child, name))
            return found;
    }
    return nullptr;
}

QList<QQuickItem *> FindItemsNamed(QQuickItem *root, const QString &name) {
    QList<QQuickItem *> matches;
    if (root->objectName() == name)
        matches.push_back(root);
    for (QQuickItem *const child : root->childItems())
        matches.append(FindItemsNamed(child, name));
    return matches;
}

// Drags a block card: presses on its header, moves through interpolated
// positions, and releases with the cursor at cursorWorld (canvas/world
// coordinates).
void DragCardTo(QQuickWindow *window,
                QQuickItem *world,
                QQuickItem *view,
                const QPointF &cursorWorld) {
    const QPointF grabLocal(view->width() * 0.3, 8.0);
    const QPointF pressWorld = world->mapFromItem(view, grabLocal);

    auto sendEvent = [&](QEvent::Type type,
                         const QPointF &scene,
                         Qt::MouseButton button,
                         Qt::MouseButtons buttons) {
        const QPoint global = window->mapToGlobal(scene.toPoint());
        QMouseEvent event(type, scene, scene, global, button, buttons,
                          Qt::NoModifier);
        QCoreApplication::sendEvent(window, &event);
        QCoreApplication::processEvents();
    };

    // Scene coordinates equal item scene positions for a top-level
    // QQuickWindow, so events are sent directly at the mapped points.
    sendEvent(QEvent::MouseButtonPress,
              world->mapToScene(pressWorld),
              Qt::LeftButton, Qt::LeftButton);
    for (int step = 1; step <= 6; ++step) {
        const QPointF cursor = pressWorld + (cursorWorld - pressWorld)
                * (static_cast<qreal>(step) / 6.0);
        sendEvent(QEvent::MouseMove,
                  world->mapToScene(cursor),
                  Qt::NoButton, Qt::LeftButton);
    }
    sendEvent(QEvent::MouseButtonRelease,
              world->mapToScene(cursorWorld),
              Qt::LeftButton, Qt::NoButton);
}

bool RunCanvasDragChecks() {
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    int argc = 1;
    char applicationName[] = "canvas-drag-tests";
    char *applicationArgv[] = {applicationName, nullptr};
    QApplication application(argc, applicationArgv);
    QCoreApplication::setOrganizationName(QStringLiteral("ForeverTASTests"));
    QCoreApplication::setApplicationName(
            QStringLiteral("BlockCanvasDragTests"));
    QStandardPaths::setTestModeEnabled(true);
    QSettings().clear();

    SearchController controller;
    QQmlEngine engine;
    QObject::connect(
            &engine,
            &QQmlEngine::warnings,
            [](const QList<QQmlError> &warnings) {
                for (const QQmlError &warning : warnings) {
                    std::cerr << warning.toString().toStdString() << '\n';
                }
            });
    QQmlComponent component(
            &engine,
            QUrl::fromLocalFile(QStringLiteral(
                    FOREVERTAS_SOURCE_DIR
                    "/qml/blocks/BlockCanvas.qml")));
    if (!component.isReady()) {
        for (const QQmlError &error : component.errors()) {
            std::cerr << error.toString().toStdString() << '\n';
        }
        return false;
    }
    QScopedPointer<QObject> canvasObject(component.createWithInitialProperties(
            {{QStringLiteral("controller"),
              QVariant::fromValue(static_cast<QObject *>(&controller))}}));
    auto *const canvas = qobject_cast<QQuickItem *>(canvasObject.data());
    if (canvas == nullptr)
        return Check(false, "canvas component did not create an item");
    canvas->setWidth(640);
    canvas->setHeight(680);

    QQuickWindow window;
    canvas->setParentItem(window.contentItem());
    window.resize(660, 700);
    window.show();
    for (int step = 0; step < 10; ++step)
        QCoreApplication::processEvents();

    auto *const world = FindItem(canvas, QStringLiteral("blockWorld"));
    if (!Check(world != nullptr, "canvas world item not found"))
        return false;

    // 1) Drag the script's only mutation window out to empty space.
    const QVariantList windows = WindowIds(controller);
    if (!Check(windows.size() == 1, "default program lacks a window"))
        return false;
    const int windowId = windows.front().toInt();
    const int atomBefore = FirstWindowAtomId(controller);
    const int canvasBefore = controller.blockCanvas().size();
    auto *const windowView = FindItem(
            canvas, QStringLiteral("blockView") + QString::number(windowId));
    if (!Check(windowView != nullptr, "window block view not found"))
        return false;

    DragCardTo(&window, world, windowView, QPointF(430, 80));
    bool okay = Check(controller.blockCanvas().size() == canvasBefore + 1 &&
                      WindowIds(controller).isEmpty(),
                      "dragging the window out did not detach it to the "
                      "canvas");

    // 2) Drag it back over the hat's empty window gap.
    auto *const hatSequence = [&]() {
        const auto candidates = FindItemsNamed(
                canvas, QStringLiteral("blockSequence"));
        for (QQuickItem *const candidate : candidates) {
            if (candidate->property("ownerIsHat").toBool())
                return candidate;
        }
        return static_cast<QQuickItem *>(nullptr);
    }();
    okay &= Check(hatSequence != nullptr, "hat sequence not found");
    if (hatSequence != nullptr) {
        auto *const looseView = FindItem(
                canvas,
                QStringLiteral("blockView") + QString::number(windowId));
        okay &= Check(looseView != nullptr,
                      "detached window did not render on the canvas");
        if (looseView != nullptr) {
            const qreal ghostWidth = std::max<qreal>(170.0, looseView->width());
            const QPointF gapWorld = world->mapFromItem(
                    hatSequence,
                    QPointF(hatSequence->width() * 0.5, 4.0));
            const QPointF dropCursor(gapWorld.x() - ghostWidth / 2 + 40,
                                     gapWorld.y() + 12);
            DragCardTo(&window, world, looseView, dropCursor);
            okay &= Check(controller.blockCanvas().size() == canvasBefore &&
                          WindowIds(controller).size() == 1 &&
                          FirstWindowAtomId(controller) == atomBefore,
                          "dragging the window back did not re-attach it");
        }
    }

    // 3) Drag a loose number block into the window's "From (ms)" slot.
    const int numberId = controller.addLooseBlock(
            QStringLiteral("values/number"), 420, 320);
    okay &= Check(numberId != 0, "loose number block was not created");
    QCoreApplication::processEvents();
    if (numberId != 0) {
        auto *const numberView = FindItem(
                canvas,
                QStringLiteral("blockView") + QString::number(numberId));
        QQuickItem *const minTimeSlot = [&]() {
            const auto slotItems = FindItemsNamed(
                    canvas, QStringLiteral("blockSlot"));
            for (QQuickItem *const slot : slotItems) {
                if (slot->property("blockId").toInt() == windowId &&
                    slot->property("fieldKey").toString() ==
                            QStringLiteral("minTimeMs")) {
                    return slot;
                }
            }
            return static_cast<QQuickItem *>(nullptr);
        }();
        okay &= Check(numberView != nullptr && minTimeSlot != nullptr,
                      "number block or min-time slot not found");
        if (numberView != nullptr && minTimeSlot != nullptr) {
            const QPointF slotWorld = world->mapFromItem(
                    minTimeSlot,
                    QPointF(minTimeSlot->width() * 0.5,
                            minTimeSlot->height() * 0.5));
            const qreal ghostWidth =
                    std::max<qreal>(170.0, numberView->width());
            DragCardTo(&window, world, numberView,
                       QPointF(slotWorld.x() - ghostWidth / 2 + 20,
                               slotWorld.y() - 6));
            bool chipFound = false;
            const QVariantList fields =
                    controller.blockData(windowId)
                            .value(QStringLiteral("fields"))
                            .toList();
            for (const QVariant &value : fields) {
                const QVariantMap field = value.toMap();
                if (field.value(QStringLiteral("key")).toString() !=
                    QStringLiteral("minTimeMs")) {
                    continue;
                }
                chipFound = field.value(QStringLiteral("reporter"))
                                    .toMap()
                                    .value(QStringLiteral("blockId"))
                                    .toInt() == numberId;
            }
            okay &= Check(chipFound,
                          "dragging the number block did not graft it into "
                          "the slot");
            okay &= Check(!controller.blockCanvas().isEmpty() &&
                          controller.blockScript()
                                  .value(QStringLiteral("groups"))
                                  .toList()
                                  .size() == 1,
                          "grafting disturbed the script structure");
        }
    }

    QSettings().clear();
    return okay;
}

}  // namespace

int main(int argc, char **argv) {
    Q_UNUSED(argc)
    Q_UNUSED(argv)
    return RunCanvasDragChecks() ? 0 : 1;
}
