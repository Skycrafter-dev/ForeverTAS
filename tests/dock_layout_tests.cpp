#include "app/dock_layout.h"

#include <QCoreApplication>
#include <QJsonDocument>

#include <iostream>
#include <stdexcept>

namespace {

using forevertas::app::DockGroup;
using forevertas::app::DockLayoutModel;
using forevertas::app::DockPanelState;
using forevertas::app::DockTree;

void Check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

QVariantMap GroupWith(const DockLayoutModel &model, const QString &panel) {
    for (const QVariant &value : model.groups()) {
        const QVariantMap group = value.toMap();
        if (group.value("tabs").toStringList().contains(panel)) return group;
    }
    return {};
}

QRectF RectOf(const QVariantMap &group) {
    return {group.value("x").toDouble(), group.value("y").toDouble(),
            group.value("width").toDouble(), group.value("height").toDouble()};
}

// Inputs over Graphics on the left, Bruteforce + Code tabbed on the right,
// Playback floating.
QString DefaultState() {
    DockTree tree;
    tree.dock(DockGroup{{}, {"inputs"}, "inputs"}, {}, "left", 0.2, 320);
    tree.dock(DockGroup{{}, {"bruteforce", "code"}, "bruteforce"}, {}, "right", 0.25, 400);
    tree.dock(DockGroup{{}, {"graphics"}, "graphics"}, tree.groupOf("inputs"), "bottom", 0.5);
    tree.addFloating(DockGroup{{}, {"playback"}, "playback"}, QRectF(700, 880, 430, 90));
    return QString::fromUtf8(QJsonDocument(tree.toJson()).toJson(QJsonDocument::Compact));
}

void TestTree() {
    DockTree tree;
    tree.dock(DockGroup{{}, {"a"}, "a"}, {}, "left", 0.25);
    tree.dock(DockGroup{{}, {"b", "c"}, "b"}, {}, "right", 0.25);
    const auto all = [](const QString &) { return DockPanelState{true, QSizeF(200, 120)}; };
    auto geometry = tree.compute(QRectF(0, 0, 1000, 600), 10, QSizeF(160, 120), all);
    Check(geometry.groups.size() == 2, "two docked groups expected");
    Check(geometry.center.left() > 200 && geometry.center.right() < 800,
          "race view should sit between the columns");
    Check(geometry.splitters.size() == 2, "each split needs a handle");

    // A closed panel gives its space back.
    tree.setClosed("a", true);
    const auto shown = [&](const QString &panel) {
        return DockPanelState{!tree.isClosed(panel), QSizeF(200, 120)};
    };
    geometry = tree.compute(QRectF(0, 0, 1000, 600), 10, QSizeF(160, 120), shown);
    Check(geometry.center.left() == 0, "closed column should free its side");
    tree.setClosed("a", false);

    // Splitting a group and taking it out again restores the tree.
    tree.dock(DockGroup{{}, {"d"}, "d"}, tree.groupOf("a"), "bottom", 0.5);
    Check(tree.groupOf("d") != tree.groupOf("a"), "d should get its own group");
    Check(tree.placeOf(tree.groupOf("d"))->side == "bottom", "place of d");
    const auto taken = tree.removeGroup(tree.groupOf("d"));
    Check(taken && taken->tabs == QStringList{"d"}, "removed group keeps its tabs");
    Check(tree.groupOf("d").isEmpty(), "d should be gone");

    // Docking as a tab, then removing every panel leaves only the race view.
    Check(tree.dock(DockGroup{{}, {"e"}, "e"}, tree.groupOf("b"), "center", 0), "tab dock");
    Check(tree.group(tree.groupOf("b"))->tabs == (QStringList{"b", "c", "e"}), "tab order");
    for (const QString &panel : QStringList{"a", "b", "c", "e"}) tree.removePanel(panel);
    Check(tree.node("center") != nullptr && tree.panels().isEmpty(), "only the race view is left");

    // Too small an area shares out the minimums instead of overlapping.
    DockTree tight;
    tight.dock(DockGroup{{}, {"x"}, "x"}, {}, "left", 0.5);
    tight.dock(DockGroup{{}, {"y"}, "y"}, {}, "right", 0.5);
    geometry = tight.compute(QRectF(0, 0, 500, 300), 10, QSizeF(160, 120), all);
    for (const auto &group : geometry.groups)
        Check(group.rect.left() >= 0 && group.rect.right() <= 500, "groups stay inside");

    // Saving and loading keeps the tree; unknown panels go, new ones arrive.
    const std::optional<DockTree> fallback = DockTree::FromJson(
            QJsonDocument::fromJson(DefaultState().toUtf8()).object());
    Check(fallback.has_value(), "default layout parses");
    std::optional<DockTree> loaded = DockTree::FromJson(fallback->toJson());
    Check(loaded && loaded->panels() == fallback->panels(), "round trip");
    loaded->removePanel("graphics");
    loaded->dock(DockGroup{{}, {"ghost"}, "ghost"}, {}, "top", 0.2);
    loaded->reconcile({"inputs", "graphics", "bruteforce", "code", "playback"}, *fallback);
    Check(loaded->groupOf("ghost").isEmpty(), "unknown panel removed");
    Check(!loaded->groupOf("graphics").isEmpty(), "missing panel added back");
    Check(!DockTree::FromJson(QJsonDocument::fromJson("{\"version\":1}").object()),
          "broken layout rejected");

    // Bruteforce split into Base, Target, Passes and Search: a saved layout
    // gets them where Bruteforce was, with Results under them.
    DockTree saved;
    saved.dock(DockGroup{{}, {"inputs"}, "inputs"}, {}, "left", 0.25);
    saved.dock(DockGroup{{}, {"bruteforce", "code"}, "bruteforce"}, {}, "right", 0.3);
    const QString place = saved.groupOf("code");
    DockTree defaults;
    defaults.dock(DockGroup{{}, {"inputs"}, "inputs"}, {}, "left", 0.25);
    defaults.dock(DockGroup{{}, {"base", "target", "passes", "search", "code"}, "base"}, {}, "right", 0.3);
    defaults.dock(DockGroup{{}, {"results"}, "results"}, defaults.groupOf("base"), "bottom", 0.4);
    saved.reconcile({"inputs", "base", "target", "passes", "search", "results", "code"}, defaults);
    Check(saved.groupOf("bruteforce").isEmpty(), "Bruteforce gone");
    for (const char *page : {"base", "target", "passes", "search"})
        Check(saved.groupOf(page) == place, "a Bruteforce page where Bruteforce was");
    Check(!saved.groupOf("results").isEmpty() && saved.groupOf("results") != place,
          "Results in a group of its own");
}

void TestSnapping() {
    using forevertas::app::SnapDockMove;
    using forevertas::app::SnapDockResize;
    const QRectF area(0, 0, 1000, 800);
    Check(SnapDockMove(QRectF(213, 100, 100, 50), {QRectF(0, 90, 200, 100)}, area, 10, 8).x() == 210,
          "should stick one gap right of the neighbour");
    const QRectF corner = SnapDockMove(QRectF(950, -30, 100, 50), {}, area, 10, 8);
    Check(corner.x() == 900 && corner.y() == 0, "should stay inside the window");
    Check(SnapDockResize(QRectF(0, 0, 395, 50), Qt::RightEdge, {QRectF(400, 0, 100, 100)}, area, 10, 8)
                  .width() == 390,
          "resize should stop one gap before the neighbour");
}

void TestModel() {
    DockLayoutModel model;
    model.setDefaultState(DefaultState());
    model.setArea(QRectF(0, 0, 1600, 1000));
    for (const QString &panel : QStringList{"inputs", "graphics", "bruteforce", "code", "playback", "splits"})
        model.setPanel(panel, panel != "splits", 220, 140);
    model.setState(QString());
    Check(model.groups().size() == 4, "default layout has four groups");
    Check(GroupWith(model, "splits").isEmpty(), "empty panel stays hidden");
    model.setPanel("splits", true, 220, 140);
    Check(!GroupWith(model, "splits").isEmpty(), "panel shows once it has content");

    // Close and reopen: same place.
    const QRectF before = RectOf(GroupWith(model, "graphics"));
    model.setOpen("graphics", false);
    Check(GroupWith(model, "graphics").isEmpty(), "closed panel hidden");
    Check(RectOf(GroupWith(model, "inputs")).height() > before.height(), "inputs takes the room");
    model.setOpen("graphics", true);
    Check(RectOf(GroupWith(model, "graphics")) == before, "reopened in the same place");

    // Drag Code out of the Bruteforce tabs and drop it under Inputs.
    const QString source = model.groupOf("code");
    const QString dragged = model.beginDrag(source, "code", QRectF(0, 0, 400, 300));
    Check(model.dragging() && dragged != source, "tab drag gets its own group");
    Check(GroupWith(model, "bruteforce").value("tabs").toStringList() == QStringList{"bruteforce"},
          "source group keeps the other tab");
    model.previewDrop(model.groupOf("inputs"), "bottom");
    Check(model.previewRect().isValid(), "ghost shown while hovering a target");
    const QString landed = model.endDrag(QRectF(500, 300, 400, 300));
    Check(!model.dragging() && !model.isFloating(landed), "code docked");
    Check(RectOf(GroupWith(model, "code")).left() < 400, "code is in the left column");

    // Float it with the button, then dock it back to the same spot.
    const QRectF docked = RectOf(GroupWith(model, "code"));
    model.toggleFloating(landed);
    Check(model.isFloating(model.groupOf("code")), "toggle floats");
    model.toggleFloating(model.groupOf("code"));
    Check(RectOf(GroupWith(model, "code")) == docked, "toggle docks back where it was");

    // Floating panels can't leave the window.
    const QString playback = model.groupOf("playback");
    const QRectF moved = model.moveFloating(playback, QRectF(1500, 990, 430, 90));
    Check(moved.right() <= 1600 && moved.bottom() <= 1000, "kept inside");

    // State survives a reload; a broken state falls back to the default.
    const QString saved = model.state();
    DockLayoutModel reloaded;
    reloaded.setDefaultState(DefaultState());
    reloaded.setArea(model.area());
    for (const QString &panel : QStringList{"inputs", "graphics", "bruteforce", "code", "playback", "splits"})
        reloaded.setPanel(panel, true, 220, 140);
    reloaded.setState(saved);
    Check(RectOf(GroupWith(reloaded, "code")) == RectOf(GroupWith(model, "code")), "reload keeps layout");
    reloaded.setState("not json");
    Check(GroupWith(reloaded, "code").value("tabs").toStringList().contains("bruteforce"),
          "broken state gives the default layout");

    // Panels beside the race view keep their width: the race view takes a
    // window resize, and the space of a column that goes away.
    const double rightWidth = RectOf(GroupWith(model, "bruteforce")).width();
    Check(rightWidth == 400, "docked column keeps its size in px");
    model.setArea(QRectF(0, 0, 1900, 1000));
    Check(RectOf(GroupWith(model, "bruteforce")).width() == rightWidth,
          "window resize changed a docked column");
    model.setOpen("inputs", false);
    model.setOpen("graphics", false);
    model.setOpen("code", false);
    Check(RectOf(GroupWith(model, "bruteforce")).width() == rightWidth,
          "hiding the left column widened the right one");
    model.setOpen("inputs", true);
    model.setOpen("graphics", true);
    model.setOpen("code", true);
    model.setArea(QRectF(0, 0, 1600, 1000));

    // Hide all panels: the race view gets the whole area.
    model.setHideAll(true);
    Check(model.groups().isEmpty() && model.center() == model.area(), "hide all");
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        TestTree();
        TestSnapping();
        TestModel();
    } catch (const std::exception &error) {
        std::cerr << "dock layout test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "dock layout tests passed\n";
    return 0;
}
