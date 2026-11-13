#ifndef FOREVERTAS_APP_DOCK_LAYOUT_H
#define FOREVERTAS_APP_DOCK_LAYOUT_H

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QRectF>
#include <QSizeF>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace forevertas::app {

// Panel arrangement, ImGui style. The dock area is a binary tree of splits
// whose leaves are tab groups and one "center" leaf, the race view. Tab
// groups can also float above it. Closed panels keep their place so they
// come back where they were.
struct DockNode {
    enum class Kind { Split, Tabs, Center };

    Kind kind = Kind::Center;
    QString id;
    bool row = true;        // split: children side by side, else stacked
    double ratio = 0.5;     // split: share of the first child
    // split with the race view on one side only: the other side's size in
    // px (0 = use ratio). The race view takes up window size changes, the
    // panels keep their size, as in ImGui.
    double size = 0;
    std::unique_ptr<DockNode> first;
    std::unique_ptr<DockNode> second;
    QStringList tabs;       // tabs: panel ids, in tab order
    QString current;        // tabs: the panel on top
};

struct DockFloatingGroup {
    QString id;
    QStringList tabs;
    QString current;
    QRectF rect;
    bool collapsed = false;
};

// A group that is being moved: the panels and which one is on top.
struct DockGroup {
    QString id;
    QStringList tabs;
    QString current;
};

struct DockPanelState {
    bool shown = true;
    QSizeF minimum;
};

struct DockGeometry {
    struct Group {
        QString id;
        QRectF rect;
        QStringList tabs;   // shown tabs only
        QString current;
        bool floating = false;
        bool collapsed = false;
    };
    struct Splitter {
        QString id;
        bool row = true;
        QRectF rect;        // the gap between the two sides
        double start = 0;   // where the split node begins along its axis
        double total = 0;   // node size along the axis minus the gap
        double minimumFirst = 0;
        double minimumSecond = 0;
    };

    std::vector<Group> groups;
    std::vector<Splitter> splitters;
    QRectF center;
};

// Where a panel goes back to when it is docked again.
struct DockPlace {
    QString target;     // node id, or empty for the dock area edge
    QString side;       // left, right, top, bottom or center
    double share = 0.25;
    double size = 0;    // px, when the race view was on the other side
};

class DockTree {
public:
    using PanelState = std::function<DockPanelState(const QString &)>;

    DockTree();
    DockTree(const DockTree &other);
    DockTree &operator=(const DockTree &other);
    DockTree(DockTree &&) noexcept = default;
    DockTree &operator=(DockTree &&) noexcept = default;
    ~DockTree();

    static std::optional<DockTree> FromJson(const QJsonObject &json);
    QJsonObject toJson() const;

    QStringList panels() const;
    // An id no node uses yet, for a group that is not in the tree.
    QString reserveId();
    QString groupOf(const QString &panel) const;
    bool isFloating(const QString &group) const;
    std::optional<DockGroup> group(const QString &id) const;
    const DockNode *node(const QString &id) const;
    const DockFloatingGroup *floatingGroup(const QString &id) const;

    bool removePanel(const QString &panel);
    std::optional<DockGroup> removeGroup(const QString &id);
    // target: a node id, or empty for the edge of the whole dock area.
    // side "center" adds the tabs to a tab group (docked or floating).
    // size: the new group's size in px, kept when the race view is on the
    // other side (0 = use share).
    bool dock(const DockGroup &group, const QString &target,
              const QString &side, double share, double size = 0);
    QString addFloating(const DockGroup &group, const QRectF &rect);
    void raise(const QString &floatingGroup);
    // total: the split's size along its axis minus the gap.
    bool setRatio(const QString &split, double ratio, double total = 0);
    bool setCurrent(const QString &group, const QString &panel);
    bool setCollapsed(const QString &floatingGroup, bool collapsed);
    // The area grew or shrank: floating panels keep their place from the
    // nearest edges (a panel in the middle third stays centred).
    void followArea(const QRectF &from, const QRectF &to);
    bool setFloatingRect(const QString &floatingGroup, const QRectF &rect);
    // Where a docked group would go back to if it were taken out now.
    std::optional<DockPlace> placeOf(const QString &group) const;

    bool isClosed(const QString &panel) const;
    void setClosed(const QString &panel, bool closed);

    DockGeometry compute(const QRectF &area, double gap,
                         const QSizeF &centerMinimum,
                         const PanelState &state) const;

    // Drop unknown or repeated panels; add missing ones the way the
    // fallback layout has them.
    void reconcile(const QStringList &known, const DockTree &fallback);

private:
    QString newId();
    DockNode *findNode(const QString &id) const;
    DockNode *findParent(const DockNode *child) const;
    DockNode *findTabs(const QString &panel) const;
    void replaceNode(DockNode *oldNode, std::unique_ptr<DockNode> newNode);
    void pruneEmpty(DockNode *tabs);

    int nextId_ = 1;
    std::unique_ptr<DockNode> root_;
    std::vector<DockFloatingGroup> floating_;
    QStringList closed_;
};

QRectF ClampDockRect(const QRectF &rect, const QRectF &area);
// Pull a moving rectangle onto nearby edges: the area border and other
// panels, which end up one gap apart like docked ones.
QRectF SnapDockMove(const QRectF &rect, const std::vector<QRectF> &others,
                    const QRectF &area, double gap, double reach);
// Snap the edges being dragged during a resize.
QRectF SnapDockResize(const QRectF &rect, Qt::Edges edges,
                      const std::vector<QRectF> &others, const QRectF &area,
                      double gap, double reach);

// QML side of the layout. QML registers each panel, reads the computed
// group rectangles and splitters, and calls back while the user drags.
class DockLayoutModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(QRectF area READ area WRITE setArea NOTIFY areaChanged)
    Q_PROPERTY(qreal gap READ gap WRITE setGap NOTIFY gapChanged)
    Q_PROPERTY(qreal headerHeight READ headerHeight WRITE setHeaderHeight
                       NOTIFY headerHeightChanged)
    Q_PROPERTY(bool hideAll READ hideAll WRITE setHideAll NOTIFY
                       hideAllChanged)
    Q_PROPERTY(QString state READ state WRITE setState NOTIFY stateChanged)
    Q_PROPERTY(QString defaultState READ defaultState WRITE setDefaultState
                       NOTIFY defaultStateChanged)
    Q_PROPERTY(QVariantList groups READ groups NOTIFY geometryChanged)
    Q_PROPERTY(QVariantList splitters READ splitters NOTIFY geometryChanged)
    Q_PROPERTY(QRectF center READ center NOTIFY geometryChanged)
    Q_PROPERTY(bool dragging READ dragging NOTIFY draggingChanged)
    Q_PROPERTY(QString dragGroup READ dragGroup NOTIFY draggingChanged)
    Q_PROPERTY(QRectF previewRect READ previewRect NOTIFY geometryChanged)
    Q_PROPERTY(int revision READ revision NOTIFY stateChanged)

public:
    explicit DockLayoutModel(QObject *parent = nullptr);

    QRectF area() const;
    void setArea(const QRectF &value);
    qreal gap() const;
    void setGap(qreal value);
    qreal headerHeight() const;
    void setHeaderHeight(qreal value);
    bool hideAll() const;
    void setHideAll(bool value);
    QString state() const;
    void setState(const QString &value);
    QString defaultState() const;
    void setDefaultState(const QString &value);
    QVariantList groups() const;
    QVariantList splitters() const;
    QRectF center() const;
    bool dragging() const;
    QString dragGroup() const;
    QRectF previewRect() const;
    int revision() const;

    // Panels tell the layout whether they have anything to show and how
    // small they may get (header included).
    Q_INVOKABLE void setPanel(const QString &panel, bool available,
                              qreal minimumWidth, qreal minimumHeight);
    Q_INVOKABLE bool isOpen(const QString &panel) const;
    Q_INVOKABLE void setOpen(const QString &panel, bool open);
    Q_INVOKABLE bool isShown(const QString &panel) const;
    Q_INVOKABLE QString groupOf(const QString &panel) const;
    Q_INVOKABLE bool isFloating(const QString &group) const;
    Q_INVOKABLE void resetLayout();

    Q_INVOKABLE void setCurrent(const QString &group, const QString &panel);
    Q_INVOKABLE void setRatio(const QString &split, qreal ratio);
    Q_INVOKABLE void raise(const QString &group);
    Q_INVOKABLE void setCollapsed(const QString &group, bool collapsed);
    // Floating groups: snapped against the other panels and kept inside
    // the area. Returns the rectangle actually used.
    Q_INVOKABLE QRectF moveFloating(const QString &group, const QRectF &rect);
    Q_INVOKABLE QRectF resizeFloating(const QString &group,
                                      const QRectF &rect, int edges);
    // Docked group: float it next to where it is. Floating: dock it back.
    Q_INVOKABLE void toggleFloating(const QString &group);

    // Dragging. beginDrag takes the panel (or the whole group when panel is
    // empty or alone) out of the layout. previewDrop shows where it would
    // land; endDrag puts it there, or floats it at floatingRect.
    Q_INVOKABLE QString beginDrag(const QString &group, const QString &panel,
                                  const QRectF &rect);
    Q_INVOKABLE void previewDrop(const QString &target, const QString &side);
    Q_INVOKABLE QString endDrag(const QRectF &floatingRect);
    Q_INVOKABLE QRectF snapDrag(const QRectF &rect) const;

signals:
    void areaChanged();
    void gapChanged();
    void headerHeightChanged();
    void hideAllChanged();
    void stateChanged();
    void defaultStateChanged();
    void geometryChanged();
    void draggingChanged();

private:
    DockPanelState panelState(const QString &panel) const;
    void recompute();
    void changed();
    std::vector<QRectF> otherRects(const QString &group) const;
    QRectF targetRect() const;
    double shareFor(const QString &side) const;
    double sizeFor(const QString &side) const;
    void rememberPlace(const QString &group, const QString &panel);
    void restoreFromJson(const QString &json);

    struct Panel {
        bool available = true;
        QSizeF minimum{200, 120};
    };

    QRectF area_;
    qreal gap_ = 8;
    qreal headerHeight_ = 32;
    bool hideAll_ = false;
    QString defaultState_;
    QStringList order_;
    QHash<QString, Panel> panels_;
    QHash<QString, DockPlace> places_;
    DockTree tree_;
    DockGeometry geometry_;
    std::optional<DockGroup> drag_;
    QSizeF dragSize_;
    QString previewTarget_;
    QString previewSide_;
    QRectF previewRect_;
    QString pendingState_;
    int revision_ = 0;
};

}  // namespace forevertas::app

#endif
