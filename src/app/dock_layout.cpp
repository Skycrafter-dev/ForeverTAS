#include "app/dock_layout.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>
#include <cmath>

namespace forevertas::app {
namespace {

constexpr int kLayoutVersion = 1;
const QString kCenterId = QStringLiteral("center");

std::unique_ptr<DockNode> CloneNode(const DockNode *node) {
    if (node == nullptr) {
        return nullptr;
    }
    auto copy = std::make_unique<DockNode>();
    copy->kind = node->kind;
    copy->id = node->id;
    copy->row = node->row;
    copy->ratio = node->ratio;
    copy->size = node->size;
    copy->tabs = node->tabs;
    copy->current = node->current;
    copy->first = CloneNode(node->first.get());
    copy->second = CloneNode(node->second.get());
    return copy;
}

std::unique_ptr<DockNode> CenterNode() {
    auto node = std::make_unique<DockNode>();
    node->kind = DockNode::Kind::Center;
    node->id = kCenterId;
    return node;
}

template <typename Visit>
void Walk(DockNode *node, DockNode *parent, const Visit &visit) {
    if (node == nullptr) {
        return;
    }
    visit(node, parent);
    if (node->kind == DockNode::Kind::Split) {
        Walk(node->first.get(), node, visit);
        Walk(node->second.get(), node, visit);
    }
}

QJsonObject RectToJson(const QRectF &rect) {
    return {{QStringLiteral("x"), rect.x()},
            {QStringLiteral("y"), rect.y()},
            {QStringLiteral("w"), rect.width()},
            {QStringLiteral("h"), rect.height()}};
}

QRectF RectFromJson(const QJsonObject &json) {
    return {json.value(QStringLiteral("x")).toDouble(),
            json.value(QStringLiteral("y")).toDouble(),
            json.value(QStringLiteral("w")).toDouble(),
            json.value(QStringLiteral("h")).toDouble()};
}

QStringList StringsFromJson(const QJsonValue &value) {
    QStringList strings;
    for (const QJsonValue &item : value.toArray()) {
        if (item.isString()) {
            strings.append(item.toString());
        }
    }
    return strings;
}

QJsonObject NodeToJson(const DockNode &node) {
    switch (node.kind) {
    case DockNode::Kind::Center:
        return {{QStringLiteral("type"), QStringLiteral("center")},
                {QStringLiteral("id"), node.id}};
    case DockNode::Kind::Tabs:
        return {{QStringLiteral("type"), QStringLiteral("tabs")},
                {QStringLiteral("id"), node.id},
                {QStringLiteral("tabs"), QJsonArray::fromStringList(node.tabs)},
                {QStringLiteral("current"), node.current}};
    case DockNode::Kind::Split:
        break;
    }
    return {{QStringLiteral("type"), QStringLiteral("split")},
            {QStringLiteral("id"), node.id},
            {QStringLiteral("dir"),
             node.row ? QStringLiteral("row") : QStringLiteral("column")},
            {QStringLiteral("ratio"), node.ratio},
            {QStringLiteral("size"), node.size},
            {QStringLiteral("a"), NodeToJson(*node.first)},
            {QStringLiteral("b"), NodeToJson(*node.second)}};
}

std::unique_ptr<DockNode> NodeFromJson(const QJsonObject &json, int depth) {
    if (depth > 64) {
        return nullptr;
    }
    const QString type = json.value(QStringLiteral("type")).toString();
    auto node = std::make_unique<DockNode>();
    node->id = json.value(QStringLiteral("id")).toString();
    if (type == QStringLiteral("center")) {
        node->kind = DockNode::Kind::Center;
        node->id = kCenterId;
        return node;
    }
    if (node->id.isEmpty() || node->id == kCenterId) {
        return nullptr;
    }
    if (type == QStringLiteral("tabs")) {
        node->kind = DockNode::Kind::Tabs;
        node->tabs = StringsFromJson(json.value(QStringLiteral("tabs")));
        node->current = json.value(QStringLiteral("current")).toString();
        return node;
    }
    if (type != QStringLiteral("split")) {
        return nullptr;
    }
    node->kind = DockNode::Kind::Split;
    node->row = json.value(QStringLiteral("dir")).toString() !=
                QStringLiteral("column");
    node->ratio = std::clamp(
            json.value(QStringLiteral("ratio")).toDouble(0.5), 0.02, 0.98);
    node->size = std::max(0.0, json.value(QStringLiteral("size")).toDouble(0));
    node->first = NodeFromJson(json.value(QStringLiteral("a")).toObject(),
                               depth + 1);
    node->second = NodeFromJson(json.value(QStringLiteral("b")).toObject(),
                                depth + 1);
    if (!node->first || !node->second) {
        return nullptr;
    }
    return node;
}

bool ContainsCenter(const DockNode *node) {
    if (node == nullptr) {
        return false;
    }
    if (node->kind == DockNode::Kind::Center) {
        return true;
    }
    return node->kind == DockNode::Kind::Split &&
           (ContainsCenter(node->first.get()) ||
            ContainsCenter(node->second.get()));
}

int IdNumber(const QString &id) {
    bool ok = false;
    const int value = id.mid(1).toInt(&ok);
    return ok ? value : 0;
}

struct Live {
    const DockTree::PanelState &state;

    bool operator()(const DockNode *node) const {
        switch (node->kind) {
        case DockNode::Kind::Center:
            return true;
        case DockNode::Kind::Tabs:
            return std::any_of(node->tabs.cbegin(), node->tabs.cend(),
                               [this](const QString &panel) {
                                   return state(panel).shown;
                               });
        case DockNode::Kind::Split:
            break;
        }
        return (*this)(node->first.get()) || (*this)(node->second.get());
    }
};

QSizeF MinimumOf(const DockNode *node, const Live &live, double gap,
                 const QSizeF &centerMinimum) {
    if (node->kind == DockNode::Kind::Center) {
        return centerMinimum;
    }
    if (node->kind == DockNode::Kind::Tabs) {
        QSizeF size;
        for (const QString &panel : node->tabs) {
            const DockPanelState state = live.state(panel);
            if (state.shown) {
                size = size.expandedTo(state.minimum);
            }
        }
        return size;
    }
    const bool firstLive = live(node->first.get());
    const bool secondLive = live(node->second.get());
    if (!firstLive) {
        return MinimumOf(node->second.get(), live, gap, centerMinimum);
    }
    if (!secondLive) {
        return MinimumOf(node->first.get(), live, gap, centerMinimum);
    }
    const QSizeF a = MinimumOf(node->first.get(), live, gap, centerMinimum);
    const QSizeF b = MinimumOf(node->second.get(), live, gap, centerMinimum);
    if (node->row) {
        return {a.width() + b.width() + gap,
                std::max(a.height(), b.height())};
    }
    return {std::max(a.width(), b.width()),
            a.height() + b.height() + gap};
}

QStringList ShownTabs(const QStringList &tabs,
                      const DockTree::PanelState &state) {
    QStringList shown;
    for (const QString &panel : tabs) {
        if (state(panel).shown) {
            shown.append(panel);
        }
    }
    return shown;
}

// Keep the closest candidate within reach of value.
void Nearest(double value, double candidate, double reach,
             std::optional<double> &best) {
    const double distance = std::abs(candidate - value);
    if (distance <= reach &&
        (!best || distance < std::abs(*best - value))) {
        best = candidate;
    }
}

}  // namespace

// ---- DockTree ------------------------------------------------------------

DockTree::DockTree() : root_(CenterNode()) {}

DockTree::DockTree(const DockTree &other)
    : nextId_(other.nextId_),
      root_(CloneNode(other.root_.get())),
      floating_(other.floating_),
      closed_(other.closed_) {}

DockTree &DockTree::operator=(const DockTree &other) {
    if (this != &other) {
        nextId_ = other.nextId_;
        root_ = CloneNode(other.root_.get());
        floating_ = other.floating_;
        closed_ = other.closed_;
    }
    return *this;
}

DockTree::~DockTree() = default;

std::optional<DockTree> DockTree::FromJson(const QJsonObject &json) {
    if (json.value(QStringLiteral("version")).toInt() != kLayoutVersion) {
        return std::nullopt;
    }
    DockTree tree;
    tree.root_ = NodeFromJson(json.value(QStringLiteral("root")).toObject(), 0);
    if (!tree.root_) {
        return std::nullopt;
    }
    int centers = 0;
    int maxId = 0;
    Walk(tree.root_.get(), nullptr, [&](DockNode *node, DockNode *) {
        if (node->kind == DockNode::Kind::Center) {
            ++centers;
        } else {
            maxId = std::max(maxId, IdNumber(node->id));
        }
    });
    if (centers != 1) {
        return std::nullopt;
    }
    for (const QJsonValue &value :
         json.value(QStringLiteral("floating")).toArray()) {
        const QJsonObject object = value.toObject();
        DockFloatingGroup group;
        group.id = object.value(QStringLiteral("id")).toString();
        group.tabs = StringsFromJson(object.value(QStringLiteral("tabs")));
        group.current = object.value(QStringLiteral("current")).toString();
        group.rect = RectFromJson(
                object.value(QStringLiteral("rect")).toObject());
        group.collapsed =
                object.value(QStringLiteral("collapsed")).toBool();
        if (group.id.isEmpty() || group.tabs.isEmpty() ||
            !group.rect.isValid()) {
            continue;
        }
        maxId = std::max(maxId, IdNumber(group.id));
        tree.floating_.push_back(group);
    }
    tree.closed_ = StringsFromJson(json.value(QStringLiteral("closed")));
    tree.nextId_ = std::max(json.value(QStringLiteral("nextId")).toInt(1),
                            maxId + 1);
    // Empty tab groups can only come from a hand-edited file.
    std::vector<DockNode *> empty;
    Walk(tree.root_.get(), nullptr, [&](DockNode *node, DockNode *) {
        if (node->kind == DockNode::Kind::Tabs && node->tabs.isEmpty()) {
            empty.push_back(node);
        }
    });
    for (DockNode *node : empty) {
        tree.pruneEmpty(node);
    }
    return tree;
}

QJsonObject DockTree::toJson() const {
    QJsonArray floating;
    for (const DockFloatingGroup &group : floating_) {
        floating.append(QJsonObject{
                {QStringLiteral("id"), group.id},
                {QStringLiteral("tabs"), QJsonArray::fromStringList(group.tabs)},
                {QStringLiteral("current"), group.current},
                {QStringLiteral("rect"), RectToJson(group.rect)},
                {QStringLiteral("collapsed"), group.collapsed}});
    }
    return {{QStringLiteral("version"), kLayoutVersion},
            {QStringLiteral("nextId"), nextId_},
            {QStringLiteral("root"), NodeToJson(*root_)},
            {QStringLiteral("floating"), floating},
            {QStringLiteral("closed"), QJsonArray::fromStringList(closed_)}};
}

QString DockTree::newId() {
    return QStringLiteral("g%1").arg(nextId_++);
}

QString DockTree::reserveId() {
    return newId();
}

QStringList DockTree::panels() const {
    QStringList ids;
    Walk(root_.get(), nullptr, [&](DockNode *node, DockNode *) {
        if (node->kind == DockNode::Kind::Tabs) {
            ids.append(node->tabs);
        }
    });
    for (const DockFloatingGroup &group : floating_) {
        ids.append(group.tabs);
    }
    return ids;
}

DockNode *DockTree::findNode(const QString &id) const {
    DockNode *found = nullptr;
    Walk(root_.get(), nullptr, [&](DockNode *node, DockNode *) {
        if (node->id == id) {
            found = node;
        }
    });
    return found;
}

DockNode *DockTree::findParent(const DockNode *child) const {
    DockNode *found = nullptr;
    Walk(root_.get(), nullptr, [&](DockNode *node, DockNode *parent) {
        if (node == child) {
            found = parent;
        }
    });
    return found;
}

DockNode *DockTree::findTabs(const QString &panel) const {
    DockNode *found = nullptr;
    Walk(root_.get(), nullptr, [&](DockNode *node, DockNode *) {
        if (node->kind == DockNode::Kind::Tabs && node->tabs.contains(panel)) {
            found = node;
        }
    });
    return found;
}

const DockNode *DockTree::node(const QString &id) const {
    return findNode(id);
}

const DockFloatingGroup *DockTree::floatingGroup(const QString &id) const {
    for (const DockFloatingGroup &group : floating_) {
        if (group.id == id) {
            return &group;
        }
    }
    return nullptr;
}

QString DockTree::groupOf(const QString &panel) const {
    if (const DockNode *tabs = findTabs(panel)) {
        return tabs->id;
    }
    for (const DockFloatingGroup &group : floating_) {
        if (group.tabs.contains(panel)) {
            return group.id;
        }
    }
    return {};
}

bool DockTree::isFloating(const QString &group) const {
    return floatingGroup(group) != nullptr;
}

std::optional<DockGroup> DockTree::group(const QString &id) const {
    if (const DockNode *tabs = findNode(id);
        tabs != nullptr && tabs->kind == DockNode::Kind::Tabs) {
        return DockGroup{tabs->id, tabs->tabs, tabs->current};
    }
    if (const DockFloatingGroup *floating = floatingGroup(id)) {
        return DockGroup{floating->id, floating->tabs, floating->current};
    }
    return std::nullopt;
}

void DockTree::replaceNode(DockNode *oldNode,
                           std::unique_ptr<DockNode> newNode) {
    DockNode *const parent = findParent(oldNode);
    if (parent == nullptr) {
        root_ = std::move(newNode);
    } else if (parent->first.get() == oldNode) {
        parent->first = std::move(newNode);
    } else {
        parent->second = std::move(newNode);
    }
}

// An empty tab group leaves the tree; its sibling takes the split's place.
void DockTree::pruneEmpty(DockNode *tabs) {
    if (!tabs->tabs.isEmpty()) {
        return;
    }
    DockNode *const parent = findParent(tabs);
    if (parent == nullptr) {
        root_ = CenterNode();
        return;
    }
    std::unique_ptr<DockNode> sibling = std::move(
            parent->first.get() == tabs ? parent->second : parent->first);
    replaceNode(parent, std::move(sibling));
}

bool DockTree::removePanel(const QString &panel) {
    if (DockNode *tabs = findTabs(panel)) {
        tabs->tabs.removeAll(panel);
        if (tabs->current == panel) {
            tabs->current = tabs->tabs.value(0);
        }
        pruneEmpty(tabs);
        return true;
    }
    for (auto it = floating_.begin(); it != floating_.end(); ++it) {
        if (it->tabs.contains(panel)) {
            it->tabs.removeAll(panel);
            if (it->current == panel) {
                it->current = it->tabs.value(0);
            }
            if (it->tabs.isEmpty()) {
                floating_.erase(it);
            }
            return true;
        }
    }
    return false;
}

std::optional<DockGroup> DockTree::removeGroup(const QString &id) {
    if (DockNode *tabs = findNode(id);
        tabs != nullptr && tabs->kind == DockNode::Kind::Tabs) {
        DockGroup taken{tabs->id, tabs->tabs, tabs->current};
        tabs->tabs.clear();
        pruneEmpty(tabs);
        return taken;
    }
    for (auto it = floating_.begin(); it != floating_.end(); ++it) {
        if (it->id == id) {
            DockGroup taken{it->id, it->tabs, it->current};
            floating_.erase(it);
            return taken;
        }
    }
    return std::nullopt;
}

bool DockTree::dock(const DockGroup &group, const QString &target,
                    const QString &side, double share, double size) {
    if (group.tabs.isEmpty()) {
        return false;
    }
    const QString current =
            group.tabs.contains(group.current) ? group.current
                                               : group.tabs.first();
    if (side == QStringLiteral("center")) {
        if (DockNode *tabs = findNode(target);
            tabs != nullptr && tabs->kind == DockNode::Kind::Tabs) {
            tabs->tabs.append(group.tabs);
            tabs->current = current;
            return true;
        }
        for (DockFloatingGroup &floating : floating_) {
            if (floating.id == target) {
                floating.tabs.append(group.tabs);
                floating.current = current;
                return true;
            }
        }
        return false;
    }
    const bool horizontal = side == QStringLiteral("left") ||
                            side == QStringLiteral("right");
    const bool first = side == QStringLiteral("left") ||
                       side == QStringLiteral("top");
    if (!horizontal && side != QStringLiteral("top") &&
        side != QStringLiteral("bottom")) {
        return false;
    }
    DockNode *const targetNode = target.isEmpty() ? root_.get()
                                                  : findNode(target);
    if (targetNode == nullptr) {
        return false;
    }
    DockNode *const parent = findParent(targetNode);
    std::unique_ptr<DockNode> &slot =
            parent == nullptr ? root_
            : parent->first.get() == targetNode ? parent->first
                                                : parent->second;
    auto tabs = std::make_unique<DockNode>();
    tabs->kind = DockNode::Kind::Tabs;
    tabs->id = !group.id.isEmpty() && findNode(group.id) == nullptr &&
                               floatingGroup(group.id) == nullptr
                       ? group.id
                       : newId();
    tabs->tabs = group.tabs;
    tabs->current = current;

    auto split = std::make_unique<DockNode>();
    split->kind = DockNode::Kind::Split;
    split->id = newId();
    split->row = horizontal;
    split->ratio = std::clamp(first ? share : 1.0 - share, 0.08, 0.92);
    if (ContainsCenter(targetNode) && size > 0) {
        split->size = size;
    }
    std::unique_ptr<DockNode> taken = std::move(slot);
    split->first = first ? std::move(tabs) : std::move(taken);
    split->second = first ? std::move(taken) : std::move(tabs);
    slot = std::move(split);
    return true;
}

QString DockTree::addFloating(const DockGroup &group, const QRectF &rect) {
    DockFloatingGroup floating;
    floating.id = !group.id.isEmpty() && findNode(group.id) == nullptr &&
                                  floatingGroup(group.id) == nullptr
                          ? group.id
                          : newId();
    floating.tabs = group.tabs;
    floating.current = group.tabs.contains(group.current)
                               ? group.current
                               : group.tabs.value(0);
    floating.rect = rect;
    floating_.push_back(floating);
    return floating.id;
}

void DockTree::raise(const QString &floatingGroup) {
    const auto it = std::find_if(floating_.begin(), floating_.end(),
                                 [&](const DockFloatingGroup &group) {
                                     return group.id == floatingGroup;
                                 });
    if (it != floating_.end() && it + 1 != floating_.end()) {
        std::rotate(it, it + 1, floating_.end());
    }
}

bool DockTree::setRatio(const QString &split, double ratio, double total) {
    DockNode *const node = findNode(split);
    if (node == nullptr || node->kind != DockNode::Kind::Split) {
        return false;
    }
    node->ratio = std::clamp(ratio, 0.02, 0.98);
    const bool centerFirst = ContainsCenter(node->first.get());
    const bool centerSecond = ContainsCenter(node->second.get());
    if (centerFirst != centerSecond && total > 0) {
        node->size = (centerFirst ? 1.0 - node->ratio : node->ratio) * total;
    }
    return true;
}

bool DockTree::setCurrent(const QString &group, const QString &panel) {
    if (DockNode *tabs = findNode(group);
        tabs != nullptr && tabs->kind == DockNode::Kind::Tabs &&
        tabs->tabs.contains(panel)) {
        tabs->current = panel;
        return true;
    }
    for (DockFloatingGroup &floating : floating_) {
        if (floating.id == group && floating.tabs.contains(panel)) {
            floating.current = panel;
            return true;
        }
    }
    return false;
}

void DockTree::followArea(const QRectF &from, const QRectF &to) {
    if (from.isEmpty() || to.isEmpty()) {
        return;
    }
    // Along one axis: the start of a span of length size, kept from the
    // nearer edge, or centred when it sits in the middle third.
    const auto follow = [](qreal start, qreal size, qreal fromStart,
                           qreal fromLength, qreal toStart, qreal toLength) {
        const qreal centre = (start + size / 2 - fromStart) / fromLength;
        qreal moved = start - fromStart + toStart;
        if (centre > 2.0 / 3.0) {
            moved += toLength - fromLength;
        } else if (centre >= 1.0 / 3.0) {
            moved += (toLength - fromLength) / 2;
        }
        return std::clamp(moved, toStart,
                          std::max(toStart, toStart + toLength - size));
    };
    for (DockFloatingGroup &floating : floating_) {
        QRectF &rect = floating.rect;
        rect.moveTo(follow(rect.x(), rect.width(), from.x(), from.width(),
                           to.x(), to.width()),
                    follow(rect.y(), rect.height(), from.y(), from.height(),
                           to.y(), to.height()));
    }
}

bool DockTree::setCollapsed(const QString &floatingGroup, bool collapsed) {
    for (DockFloatingGroup &floating : floating_) {
        if (floating.id == floatingGroup) {
            floating.collapsed = collapsed;
            return true;
        }
    }
    return false;
}

bool DockTree::setFloatingRect(const QString &floatingGroup,
                               const QRectF &rect) {
    for (DockFloatingGroup &floating : floating_) {
        if (floating.id == floatingGroup) {
            floating.rect = rect;
            return true;
        }
    }
    return false;
}

std::optional<DockPlace> DockTree::placeOf(const QString &group) const {
    DockNode *const node = findNode(group);
    if (node == nullptr || node->kind != DockNode::Kind::Tabs) {
        return std::nullopt;
    }
    DockNode *const parent = findParent(node);
    if (parent == nullptr) {
        return std::nullopt;
    }
    const bool first = parent->first.get() == node;
    const DockNode *const sibling =
            first ? parent->second.get() : parent->first.get();
    DockPlace place;
    place.target = sibling->id;
    place.side = parent->row
                         ? (first ? QStringLiteral("left")
                                  : QStringLiteral("right"))
                         : (first ? QStringLiteral("top")
                                  : QStringLiteral("bottom"));
    place.share = first ? parent->ratio : 1.0 - parent->ratio;
    place.size = parent->size;
    return place;
}

bool DockTree::isClosed(const QString &panel) const {
    return closed_.contains(panel);
}

void DockTree::setClosed(const QString &panel, bool closed) {
    closed_.removeAll(panel);
    if (closed) {
        closed_.append(panel);
    }
}

DockGeometry DockTree::compute(const QRectF &area, double gap,
                               const QSizeF &centerMinimum,
                               const PanelState &state) const {
    DockGeometry out;
    out.center = area;
    const Live live{state};

    std::function<void(const DockNode *, const QRectF &)> place =
            [&](const DockNode *node, const QRectF &rect) {
        if (!live(node)) {
            return;
        }
        if (node->kind == DockNode::Kind::Center) {
            out.center = rect;
            return;
        }
        if (node->kind == DockNode::Kind::Tabs) {
            DockGeometry::Group group;
            group.id = node->id;
            group.rect = rect;
            group.tabs = ShownTabs(node->tabs, state);
            group.current = group.tabs.contains(node->current)
                                    ? node->current
                                    : group.tabs.value(0);
            out.groups.push_back(group);
            return;
        }
        const bool firstLive = live(node->first.get());
        const bool secondLive = live(node->second.get());
        if (!firstLive || !secondLive) {
            place(firstLive ? node->first.get() : node->second.get(), rect);
            return;
        }
        const double total =
                std::max(0.0, (node->row ? rect.width() : rect.height()) - gap);
        const QSizeF minimumFirst =
                MinimumOf(node->first.get(), live, gap, centerMinimum);
        const QSizeF minimumSecond =
                MinimumOf(node->second.get(), live, gap, centerMinimum);
        const double minFirst =
                node->row ? minimumFirst.width() : minimumFirst.height();
        const double minSecond =
                node->row ? minimumSecond.width() : minimumSecond.height();
        double sizeFirst = total * node->ratio;
        const bool centerFirst = ContainsCenter(node->first.get());
        const bool centerSecond = ContainsCenter(node->second.get());
        if (centerFirst != centerSecond && node->size > 0) {
            sizeFirst = centerFirst ? total - node->size : node->size;
        }
        if (minFirst + minSecond >= total) {
            sizeFirst = total * minFirst / std::max(1.0, minFirst + minSecond);
        } else {
            sizeFirst = std::clamp(sizeFirst, minFirst, total - minSecond);
        }
        sizeFirst = std::round(sizeFirst);
        const double sizeSecond = total - sizeFirst;
        DockGeometry::Splitter splitter;
        splitter.id = node->id;
        splitter.row = node->row;
        splitter.total = total;
        splitter.minimumFirst = minFirst;
        splitter.minimumSecond = minSecond;
        if (node->row) {
            place(node->first.get(),
                  QRectF(rect.x(), rect.y(), sizeFirst, rect.height()));
            place(node->second.get(),
                  QRectF(rect.x() + sizeFirst + gap, rect.y(), sizeSecond,
                         rect.height()));
            splitter.rect = QRectF(rect.x() + sizeFirst, rect.y(), gap,
                                   rect.height());
            splitter.start = rect.x();
        } else {
            place(node->first.get(),
                  QRectF(rect.x(), rect.y(), rect.width(), sizeFirst));
            place(node->second.get(),
                  QRectF(rect.x(), rect.y() + sizeFirst + gap, rect.width(),
                         sizeSecond));
            splitter.rect = QRectF(rect.x(), rect.y() + sizeFirst,
                                   rect.width(), gap);
            splitter.start = rect.y();
        }
        out.splitters.push_back(splitter);
    };
    place(root_.get(), area);

    for (const DockFloatingGroup &floating : floating_) {
        const QStringList tabs = ShownTabs(floating.tabs, state);
        if (tabs.isEmpty()) {
            continue;
        }
        // A floating group grows when its content needs more room.
        QSizeF minimum;
        for (const QString &panel : tabs) {
            minimum = minimum.expandedTo(state(panel).minimum);
        }
        QRectF rect = floating.rect;
        rect.setWidth(std::max(rect.width(), minimum.width()));
        rect.setHeight(std::max(rect.height(), minimum.height()));
        DockGeometry::Group group;
        group.id = floating.id;
        group.rect = ClampDockRect(rect, area);
        group.tabs = tabs;
        group.current = tabs.contains(floating.current) ? floating.current
                                                        : tabs.first();
        group.floating = true;
        group.collapsed = floating.collapsed;
        out.groups.push_back(group);
    }
    return out;
}

void DockTree::reconcile(const QStringList &known, const DockTree &fallback) {
    // A repeated panel keeps its last place.
    QStringList seen;
    for (const QString &panel : panels()) {
        if (known.contains(panel) && !seen.contains(panel)) {
            seen.append(panel);
            continue;
        }
        removePanel(panel);
    }
    QStringList closed;
    for (const QString &panel : closed_) {
        if (known.contains(panel)) {
            closed.append(panel);
        }
    }
    closed_ = closed;
    for (const QString &panel : known) {
        if (!groupOf(panel).isEmpty()) {
            continue;
        }
        const QString fallbackGroup = fallback.groupOf(panel);
        if (const DockFloatingGroup *floating =
                    fallback.floatingGroup(fallbackGroup)) {
            addFloating(DockGroup{{}, {panel}, panel}, floating->rect);
        } else {
            dock(DockGroup{{}, {panel}, panel}, {}, QStringLiteral("right"),
                 0.2);
        }
        if (fallback.isClosed(panel)) {
            setClosed(panel, true);
        }
    }
}

// ---- geometry helpers ---------------------------------------------------

QRectF ClampDockRect(const QRectF &rect, const QRectF &area) {
    const double width = std::min(rect.width(), area.width());
    const double height = std::min(rect.height(), area.height());
    return {std::clamp(rect.x(), area.x(), area.right() - width),
            std::clamp(rect.y(), area.y(), area.bottom() - height), width,
            height};
}

QRectF SnapDockMove(const QRectF &rect, const std::vector<QRectF> &others,
                    const QRectF &area, double gap, double reach) {
    std::optional<double> x;
    std::optional<double> y;
    Nearest(rect.x(), area.x(), reach, x);
    Nearest(rect.x(), area.right() - rect.width(), reach, x);
    Nearest(rect.y(), area.y(), reach, y);
    Nearest(rect.y(), area.bottom() - rect.height(), reach, y);
    for (const QRectF &other : others) {
        const bool overlapY = rect.top() < other.bottom() + reach &&
                              rect.bottom() > other.top() - reach;
        const bool overlapX = rect.left() < other.right() + reach &&
                              rect.right() > other.left() - reach;
        if (overlapY) {
            Nearest(rect.x(), other.right() + gap, reach, x);
            Nearest(rect.x(), other.left() - rect.width() - gap, reach, x);
            Nearest(rect.x(), other.left(), reach, x);
            Nearest(rect.x(), other.right() - rect.width(), reach, x);
        }
        if (overlapX) {
            Nearest(rect.y(), other.bottom() + gap, reach, y);
            Nearest(rect.y(), other.top() - rect.height() - gap, reach, y);
            Nearest(rect.y(), other.top(), reach, y);
            Nearest(rect.y(), other.bottom() - rect.height(), reach, y);
        }
    }
    return ClampDockRect(QRectF(x.value_or(rect.x()), y.value_or(rect.y()),
                                rect.width(), rect.height()),
                         area);
}

QRectF SnapDockResize(const QRectF &rect, Qt::Edges edges,
                      const std::vector<QRectF> &others, const QRectF &area,
                      double gap, double reach) {
    std::optional<double> left;
    std::optional<double> right;
    std::optional<double> top;
    std::optional<double> bottom;
    Nearest(rect.left(), area.left(), reach, left);
    Nearest(rect.right(), area.right(), reach, right);
    Nearest(rect.top(), area.top(), reach, top);
    Nearest(rect.bottom(), area.bottom(), reach, bottom);
    for (const QRectF &other : others) {
        if (rect.top() < other.bottom() && rect.bottom() > other.top()) {
            Nearest(rect.left(), other.right() + gap, reach, left);
            Nearest(rect.left(), other.left(), reach, left);
            Nearest(rect.right(), other.left() - gap, reach, right);
            Nearest(rect.right(), other.right(), reach, right);
        }
        if (rect.left() < other.right() && rect.right() > other.left()) {
            Nearest(rect.top(), other.bottom() + gap, reach, top);
            Nearest(rect.top(), other.top(), reach, top);
            Nearest(rect.bottom(), other.top() - gap, reach, bottom);
            Nearest(rect.bottom(), other.bottom(), reach, bottom);
        }
    }
    QRectF snapped = rect;
    if (edges.testFlag(Qt::LeftEdge)) {
        snapped.setLeft(std::max(area.left(), left.value_or(rect.left())));
    }
    if (edges.testFlag(Qt::RightEdge)) {
        snapped.setRight(std::min(area.right(), right.value_or(rect.right())));
    }
    if (edges.testFlag(Qt::TopEdge)) {
        snapped.setTop(std::max(area.top(), top.value_or(rect.top())));
    }
    if (edges.testFlag(Qt::BottomEdge)) {
        snapped.setBottom(
                std::min(area.bottom(), bottom.value_or(rect.bottom())));
    }
    return snapped;
}

// ---- DockLayoutModel ----------------------------------------------------

DockLayoutModel::DockLayoutModel(QObject *parent) : QObject(parent) {}

QRectF DockLayoutModel::area() const { return area_; }

void DockLayoutModel::setArea(const QRectF &value) {
    if (area_ == value) {
        return;
    }
    tree_.followArea(area_, value);
    area_ = value;
    emit areaChanged();
    recompute();
}

qreal DockLayoutModel::gap() const { return gap_; }

void DockLayoutModel::setGap(qreal value) {
    if (qFuzzyCompare(gap_, value)) {
        return;
    }
    gap_ = value;
    emit gapChanged();
    recompute();
}

qreal DockLayoutModel::headerHeight() const { return headerHeight_; }

void DockLayoutModel::setHeaderHeight(qreal value) {
    if (qFuzzyCompare(headerHeight_, value)) {
        return;
    }
    headerHeight_ = value;
    emit headerHeightChanged();
    recompute();
}

bool DockLayoutModel::hideAll() const { return hideAll_; }

void DockLayoutModel::setHideAll(bool value) {
    if (hideAll_ == value) {
        return;
    }
    hideAll_ = value;
    emit hideAllChanged();
    recompute();
}

QString DockLayoutModel::state() const {
    QJsonObject json = tree_.toJson();
    QJsonObject places;
    for (auto it = places_.cbegin(); it != places_.cend(); ++it) {
        places.insert(it.key(),
                      QJsonObject{{QStringLiteral("target"), it->target},
                                  {QStringLiteral("side"), it->side},
                                  {QStringLiteral("share"), it->share},
                                  {QStringLiteral("size"), it->size}});
    }
    json.insert(QStringLiteral("places"), places);
    return QString::fromUtf8(
            QJsonDocument(json).toJson(QJsonDocument::Compact));
}

void DockLayoutModel::restoreFromJson(const QString &json) {
    const QJsonObject object =
            QJsonDocument::fromJson(json.toUtf8()).object();
    const std::optional<DockTree> fallback = DockTree::FromJson(
            QJsonDocument::fromJson(defaultState_.toUtf8()).object());
    std::optional<DockTree> tree = DockTree::FromJson(object);
    if (!tree) {
        tree = fallback.value_or(DockTree());
    }
    tree->reconcile(order_, fallback.value_or(DockTree()));
    tree_ = std::move(*tree);
    places_.clear();
    const QJsonObject places = object.value(QStringLiteral("places")).toObject();
    for (auto it = places.constBegin(); it != places.constEnd(); ++it) {
        const QJsonObject place = it.value().toObject();
        places_.insert(it.key(),
                       DockPlace{place.value(QStringLiteral("target")).toString(),
                                 place.value(QStringLiteral("side")).toString(),
                                 place.value(QStringLiteral("share")).toDouble(0.25),
                                 place.value(QStringLiteral("size")).toDouble(0)});
    }
}

void DockLayoutModel::setState(const QString &value) {
    if (drag_) {
        return;
    }
    restoreFromJson(value);
    changed();
}

QString DockLayoutModel::defaultState() const { return defaultState_; }

void DockLayoutModel::setDefaultState(const QString &value) {
    if (defaultState_ == value) {
        return;
    }
    defaultState_ = value;
    emit defaultStateChanged();
}

void DockLayoutModel::resetLayout() {
    if (drag_) {
        return;
    }
    restoreFromJson(defaultState_);
    changed();
}

QVariantList DockLayoutModel::groups() const {
    QVariantList list;
    int order = 0;
    for (const DockGeometry::Group &group : geometry_.groups) {
        QRectF rect = group.rect;
        if (group.collapsed) {
            rect.setHeight(std::min(rect.height(), headerHeight_));
        }
        list.append(QVariantMap{{QStringLiteral("id"), group.id},
                                {QStringLiteral("x"), rect.x()},
                                {QStringLiteral("y"), rect.y()},
                                {QStringLiteral("width"), rect.width()},
                                {QStringLiteral("height"), rect.height()},
                                {QStringLiteral("tabs"), group.tabs},
                                {QStringLiteral("current"), group.current},
                                {QStringLiteral("floating"), group.floating},
                                {QStringLiteral("collapsed"), group.collapsed},
                                {QStringLiteral("order"), order++}});
    }
    return list;
}

QVariantList DockLayoutModel::splitters() const {
    QVariantList list;
    for (const DockGeometry::Splitter &splitter : geometry_.splitters) {
        list.append(QVariantMap{
                {QStringLiteral("id"), splitter.id},
                {QStringLiteral("row"), splitter.row},
                {QStringLiteral("x"), splitter.rect.x()},
                {QStringLiteral("y"), splitter.rect.y()},
                {QStringLiteral("width"), splitter.rect.width()},
                {QStringLiteral("height"), splitter.rect.height()},
                {QStringLiteral("start"), splitter.start},
                {QStringLiteral("total"), splitter.total},
                {QStringLiteral("minimumFirst"), splitter.minimumFirst},
                {QStringLiteral("minimumSecond"), splitter.minimumSecond}});
    }
    return list;
}

QRectF DockLayoutModel::center() const { return geometry_.center; }

bool DockLayoutModel::dragging() const { return drag_.has_value(); }

QString DockLayoutModel::dragGroup() const {
    return drag_ ? drag_->id : QString();
}

QRectF DockLayoutModel::previewRect() const { return previewRect_; }

int DockLayoutModel::revision() const { return revision_; }

void DockLayoutModel::setPanel(const QString &panel, bool available,
                               qreal minimumWidth, qreal minimumHeight) {
    Panel &entry = panels_[panel];
    const bool known = order_.contains(panel);
    if (known && entry.available == available &&
        entry.minimum == QSizeF(minimumWidth, minimumHeight)) {
        return;
    }
    entry.available = available;
    entry.minimum = QSizeF(minimumWidth, minimumHeight);
    if (!known) {
        order_.append(panel);
        if (tree_.groupOf(panel).isEmpty() &&
            !(drag_ && drag_->tabs.contains(panel))) {
            const std::optional<DockTree> fallback = DockTree::FromJson(
                    QJsonDocument::fromJson(defaultState_.toUtf8()).object());
            QStringList known = tree_.panels();
            known.append(panel);
            tree_.reconcile(known, fallback.value_or(DockTree()));
            changed();
            return;
        }
    }
    recompute();
}

bool DockLayoutModel::isOpen(const QString &panel) const {
    return !tree_.isClosed(panel);
}

void DockLayoutModel::setOpen(const QString &panel, bool open) {
    if (isOpen(panel) == open) {
        return;
    }
    tree_.setClosed(panel, !open);
    if (open) {
        // Reopened panels come to the front of their group.
        const QString group = tree_.groupOf(panel);
        tree_.setCurrent(group, panel);
        tree_.raise(group);
    }
    changed();
}

bool DockLayoutModel::isShown(const QString &panel) const {
    return panelState(panel).shown;
}

QString DockLayoutModel::groupOf(const QString &panel) const {
    if (drag_ && drag_->tabs.contains(panel)) {
        return drag_->id;
    }
    return tree_.groupOf(panel);
}

bool DockLayoutModel::isFloating(const QString &group) const {
    return tree_.isFloating(group);
}

DockPanelState DockLayoutModel::panelState(const QString &panel) const {
    const auto it = panels_.constFind(panel);
    DockPanelState state;
    state.shown = !hideAll_ && !tree_.isClosed(panel) &&
                  (it == panels_.cend() || it->available);
    state.minimum = it == panels_.cend() ? QSizeF(200, 120) : it->minimum;
    return state;
}

void DockLayoutModel::recompute() {
    const auto state = [this](const QString &panel) {
        return panelState(panel);
    };
    // The race view fills the window behind the panels; the free area
    // between them may close entirely.
    const QSizeF centerMinimum(0, 0);
    previewRect_ = QRectF();
    // While a drop is previewed only the ghost shows the result; the other
    // panels stay put so the drop targets do not move under the pointer.
    if (drag_ && !previewSide_.isEmpty() &&
        previewSide_ != QStringLiteral("center")) {
        DockTree preview = tree_;
        if (preview.dock(*drag_, previewTarget_, previewSide_,
                         shareFor(previewSide_), sizeFor(previewSide_))) {
            const DockGeometry landedGeometry =
                    preview.compute(area_, gap_, centerMinimum, state);
            const QString landed = preview.groupOf(drag_->tabs.first());
            for (const DockGeometry::Group &group : landedGeometry.groups) {
                if (group.id == landed) {
                    previewRect_ = group.rect;
                }
            }
        }
    }
    geometry_ = tree_.compute(area_, gap_, centerMinimum, state);
    if (drag_ && previewSide_ == QStringLiteral("center")) {
        for (const DockGeometry::Group &group : geometry_.groups) {
            if (group.id == previewTarget_) {
                previewRect_ = group.rect;
            }
        }
    }
    if (drag_) {
        geometry_.splitters.clear();
    }
    emit geometryChanged();
}

void DockLayoutModel::changed() {
    ++revision_;
    recompute();
    emit stateChanged();
}

std::vector<QRectF> DockLayoutModel::otherRects(const QString &group) const {
    std::vector<QRectF> rects;
    for (const DockGeometry::Group &other : geometry_.groups) {
        if (other.id != group) {
            QRectF rect = other.rect;
            if (other.collapsed) {
                rect.setHeight(std::min(rect.height(), headerHeight_));
            }
            rects.push_back(rect);
        }
    }
    return rects;
}

// The rectangle of the hovered drop target.
QRectF DockLayoutModel::targetRect() const {
    if (previewTarget_.isEmpty()) {
        return area_;
    }
    const DockGeometry current = tree_.compute(
            area_, gap_, QSizeF(160, 120),
            [this](const QString &panel) { return panelState(panel); });
    if (previewTarget_ == kCenterId) {
        return current.center;
    }
    for (const DockGeometry::Group &group : current.groups) {
        if (group.id == previewTarget_) {
            return group.rect;
        }
    }
    return area_;
}

// How much of the target a dropped group takes: about its own size, never
// more than half.
double DockLayoutModel::shareFor(const QString &side) const {
    const bool horizontal = side == QStringLiteral("left") ||
                            side == QStringLiteral("right");
    const QRectF target = targetRect();
    const double size = horizontal ? dragSize_.width() : dragSize_.height();
    const double available = horizontal ? target.width() : target.height();
    if (available <= 0) {
        return 0.25;
    }
    return std::clamp(size / available, 0.15, 0.5);
}

double DockLayoutModel::sizeFor(const QString &side) const {
    const bool horizontal = side == QStringLiteral("left") ||
                            side == QStringLiteral("right");
    const QRectF target = targetRect();
    return shareFor(side) * (horizontal ? target.width() : target.height());
}

void DockLayoutModel::setCurrent(const QString &group, const QString &panel) {
    const std::optional<DockGroup> current = tree_.group(group);
    if (!current || current->current == panel) {
        return;
    }
    if (tree_.setCurrent(group, panel)) {
        changed();
    }
}

void DockLayoutModel::setRatio(const QString &split, qreal ratio) {
    double total = 0;
    for (const DockGeometry::Splitter &splitter : geometry_.splitters) {
        if (splitter.id == split) {
            total = splitter.total;
        }
    }
    if (tree_.setRatio(split, ratio, total)) {
        changed();
    }
}

void DockLayoutModel::raise(const QString &group) {
    if (!tree_.isFloating(group)) {
        return;
    }
    const DockGeometry::Group *last =
            geometry_.groups.empty() ? nullptr : &geometry_.groups.back();
    if (last != nullptr && last->id == group) {
        return;
    }
    tree_.raise(group);
    changed();
}

void DockLayoutModel::setCollapsed(const QString &group, bool collapsed) {
    if (tree_.setCollapsed(group, collapsed)) {
        changed();
    }
}

QRectF DockLayoutModel::moveFloating(const QString &group,
                                     const QRectF &rect) {
    const QRectF snapped =
            SnapDockMove(rect, otherRects(group), area_, gap_, 10);
    if (tree_.setFloatingRect(group, snapped)) {
        changed();
    }
    return snapped;
}

QRectF DockLayoutModel::resizeFloating(const QString &group,
                                       const QRectF &rect, int edges) {
    const DockFloatingGroup *floating = tree_.floatingGroup(group);
    if (floating == nullptr) {
        return rect;
    }
    // Just enough to keep the header in reach.
    QSizeF minimum(48, headerHeight_);
    for (const QString &panel : floating->tabs) {
        minimum = minimum.expandedTo(panelState(panel).minimum);
    }
    const Qt::Edges dragged(edges);
    QRectF snapped = SnapDockResize(rect, dragged, otherRects(group), area_,
                                    gap_, 10);
    // Grow back to the minimum from the edge being dragged.
    if (snapped.width() < minimum.width()) {
        if (dragged.testFlag(Qt::LeftEdge)) {
            snapped.setLeft(snapped.right() - minimum.width());
        } else {
            snapped.setWidth(minimum.width());
        }
    }
    if (snapped.height() < minimum.height()) {
        if (dragged.testFlag(Qt::TopEdge)) {
            snapped.setTop(snapped.bottom() - minimum.height());
        } else {
            snapped.setHeight(minimum.height());
        }
    }
    snapped = ClampDockRect(snapped, area_);
    tree_.setFloatingRect(group, snapped);
    changed();
    return snapped;
}

void DockLayoutModel::rememberPlace(const QString &group,
                                    const QString &panel) {
    const std::optional<DockGroup> current = tree_.group(group);
    if (!current || tree_.isFloating(group)) {
        return;
    }
    if (!panel.isEmpty() && current->tabs.size() > 1) {
        places_.insert(panel, DockPlace{group, QStringLiteral("center"), 0});
        return;
    }
    if (const std::optional<DockPlace> place = tree_.placeOf(group)) {
        for (const QString &tab : current->tabs) {
            places_.insert(tab, *place);
        }
    }
}

void DockLayoutModel::toggleFloating(const QString &group) {
    if (drag_) {
        return;
    }
    if (!tree_.isFloating(group)) {
        QRectF rect;
        for (const DockGeometry::Group &candidate : geometry_.groups) {
            if (candidate.id == group) {
                rect = candidate.rect;
            }
        }
        rememberPlace(group, {});
        const std::optional<DockGroup> taken = tree_.removeGroup(group);
        if (!taken) {
            return;
        }
        // Pop out a little so it is clear the panel now floats.
        rect = QRectF(rect.x() + 28, rect.y() + 28,
                      std::min(rect.width(), area_.width() * 0.5),
                      std::min(rect.height(), area_.height() * 0.7));
        tree_.addFloating(*taken, ClampDockRect(rect, area_));
        changed();
        return;
    }
    const std::optional<DockGroup> taken = tree_.removeGroup(group);
    if (!taken) {
        return;
    }
    const DockPlace place = places_.value(
            taken->current,
            DockPlace{{}, QStringLiteral("right"), 0.25});
    const bool targetExists =
            place.target.isEmpty() || tree_.node(place.target) != nullptr ||
            (place.side == QStringLiteral("center") &&
             tree_.isFloating(place.target));
    if (!targetExists ||
        !tree_.dock(*taken, place.target, place.side, place.share,
                    place.size)) {
        tree_.dock(*taken, {}, QStringLiteral("right"), 0.25);
    }
    changed();
}

QString DockLayoutModel::beginDrag(const QString &group, const QString &panel,
                                   const QRectF &rect) {
    if (drag_) {
        return drag_->id;
    }
    const std::optional<DockGroup> current = tree_.group(group);
    if (!current) {
        return {};
    }
    if (!panel.isEmpty() && current->tabs.size() > 1 &&
        current->tabs.contains(panel)) {
        rememberPlace(group, panel);
        tree_.removePanel(panel);
        // A fresh id: the group it came from keeps its own.
        drag_ = DockGroup{tree_.reserveId(), {panel}, panel};
    } else {
        rememberPlace(group, {});
        drag_ = tree_.removeGroup(group);
    }
    dragSize_ = rect.size();
    previewTarget_.clear();
    previewSide_.clear();
    emit draggingChanged();
    recompute();
    return drag_ ? drag_->id : QString();
}

void DockLayoutModel::previewDrop(const QString &target,
                                  const QString &side) {
    if (!drag_ || (target == previewTarget_ && side == previewSide_)) {
        return;
    }
    previewTarget_ = side.isEmpty() ? QString() : target;
    previewSide_ = side;
    recompute();
}

QString DockLayoutModel::endDrag(const QRectF &floatingRect) {
    if (!drag_) {
        return {};
    }
    const DockGroup group = *drag_;
    bool docked = false;
    if (!previewSide_.isEmpty()) {
        docked = tree_.dock(group, previewTarget_, previewSide_,
                            shareFor(previewSide_), sizeFor(previewSide_));
    }
    if (!docked) {
        tree_.addFloating(group, ClampDockRect(floatingRect, area_));
    }
    drag_.reset();
    previewTarget_.clear();
    previewSide_.clear();
    emit draggingChanged();
    changed();
    return tree_.groupOf(group.tabs.first());
}

QRectF DockLayoutModel::snapDrag(const QRectF &rect) const {
    return SnapDockMove(rect, otherRects(drag_ ? drag_->id : QString()),
                        area_, gap_, 10);
}

}  // namespace forevertas::app
