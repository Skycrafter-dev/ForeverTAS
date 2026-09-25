#include "viewer/whiteboard_model.h"

#include "viewer/whiteboard_canvas_item.h"

#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QSaveFile>
#include <QSettings>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace forevertas::viewer {
namespace {

constexpr int kMaximumItems = 512;
constexpr int kMaximumBoards = 128;
constexpr std::size_t kMaximumPointsPerItem = 16384u;
constexpr std::size_t kMaximumErasuresPerItem = 4096u;
constexpr qint64 kMaximumImportBytes = 16 * 1024 * 1024;
constexpr double kMinimumExtent = 0.01;
constexpr double kMinimumGestureExtent = 0.002;
constexpr double kMinimumSize = 1.0;
constexpr int kMaximumMapNameLength = 512;
constexpr char kPersistedBoardsKey[] = "whiteboards/boardsV2";
constexpr char kFileFormat[] = "ForeverTAS whiteboard set";
constexpr int kFileVersion = 3;
constexpr int kLegacyFileVersion = 2;
constexpr int kProjectionVersion = 1;
constexpr char kPerspectiveVerticalProjection[] =
        "perspective-vertical";
constexpr double kMaximumProjectionCanvasDimension = 8192.0;
constexpr int kExportImageLongEdge = 2048;

bool IsDrawingTool(const QString &tool) {
    return tool == QStringLiteral("pen") ||
            tool == QStringLiteral("line") ||
            tool == QStringLiteral("rectangle") ||
            tool == QStringLiteral("ellipse");
}

bool IsKnownTool(const QString &tool) {
    return tool == QStringLiteral("select") ||
            tool == QStringLiteral("eraser") ||
            tool == QStringLiteral("text") ||
            IsDrawingTool(tool);
}

bool IsHexDigit(QChar character) {
    const ushort code = character.toLower().unicode();
    return (code >= '0' && code <= '9') ||
            (code >= 'a' && code <= 'f');
}

QString StripTrackmaniaFormatting(const QString &name) {
    QString result;
    result.reserve(name.size());
    for (qsizetype index = 0; index < name.size(); ++index) {
        const QChar character = name.at(index);
        if (character != QLatin1Char('$') ||
            index + 1 >= name.size()) {
            result.append(character);
            continue;
        }
        const QChar marker = name.at(index + 1);
        if (marker == QLatin1Char('$')) {
            result.append(QLatin1Char('$'));
            ++index;
            continue;
        }
        if (index + 3 < name.size() &&
            IsHexDigit(marker) &&
            IsHexDigit(name.at(index + 2)) &&
            IsHexDigit(name.at(index + 3))) {
            index += 3;
            continue;
        }
        const QChar lower = marker.toLower();
        if (QStringLiteral("iswnmgzot").contains(lower)) {
            ++index;
            continue;
        }
        if (lower == QLatin1Char('l') ||
            lower == QLatin1Char('h')) {
            ++index;
            if (index + 1 < name.size() &&
                name.at(index + 1) == QLatin1Char('[')) {
                const qsizetype end =
                        name.indexOf(QLatin1Char(']'), index + 2);
                index = end < 0 ? name.size() - 1 : end;
            }
            continue;
        }
        result.append(character);
    }
    return result;
}

bool SavePngAtomically(const QImage &image, const QString &path) {
    if (image.isNull() || path.isEmpty()) {
        return false;
    }
    QByteArray encoded;
    QBuffer buffer(&encoded);
    if (!buffer.open(QIODevice::WriteOnly) ||
        !image.save(&buffer, "PNG")) {
        return false;
    }
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) &&
            file.write(encoded) == encoded.size() &&
            file.commit();
}

QRectF ClampedBounds(const QPointF &first, const QPointF &second) {
    const double left = std::clamp(
            std::min(first.x(), second.x()), 0.0, 1.0);
    const double top = std::clamp(
            std::min(first.y(), second.y()), 0.0, 1.0);
    const double right = std::clamp(
            std::max(first.x(), second.x()), 0.0, 1.0);
    const double bottom = std::clamp(
            std::max(first.y(), second.y()), 0.0, 1.0);
    return QRectF(QPointF(left, top), QPointF(right, bottom));
}

}  // namespace

WhiteboardModel::WhiteboardModel(QObject *parent)
    : QObject(parent) {
    loadPersistedBoards();
    const auto changed = [this]() {
        ++historyRevision_;
        currentHistoryToken_ = 0;
    };
    connect(this, &WhiteboardModel::itemsChanged, this, changed);
    connect(this, &WhiteboardModel::selectionChanged, this, changed);
    connect(this, &WhiteboardModel::boardsChanged, this, changed);
    connect(this, &WhiteboardModel::boardSelectionChanged, this, changed);
    connect(this, &WhiteboardModel::activeChanged, this, changed);
    connect(this, &WhiteboardModel::toolChanged, this, changed);
    connect(this, &WhiteboardModel::colorChanged, this, changed);
    connect(this, &WhiteboardModel::sizeChanged, this, changed);
}

quint64 WhiteboardModel::captureHistorySnapshot() {
    if (capturedRevision_ == historyRevision_ &&
        historySnapshots_.find(capturedToken_) != historySnapshots_.end()) {
        currentHistoryToken_ = capturedToken_;
        return capturedToken_;
    }
    const quint64 token = nextHistoryToken_++;
    historySnapshots_.emplace(token, HistorySnapshot{
            items_, boards_, selectedIndex_, selectedBoardIndex_, active_,
            tool_, color_, size_});
    capturedRevision_ = historyRevision_;
    capturedToken_ = token;
    currentHistoryToken_ = token;
    return token;
}

bool WhiteboardModel::restoreHistorySnapshot(quint64 token) {
    const auto found = historySnapshots_.find(token);
    if (found == historySnapshots_.end()) return false;
    if (currentHistoryToken_ == token) return true;
    const HistorySnapshot snapshot = found->second;
    items_ = snapshot.items;
    boards_ = snapshot.boards;
    selectedIndex_ = snapshot.selectedIndex;
    selectedBoardIndex_ = snapshot.selectedBoardIndex;
    active_ = snapshot.active;
    tool_ = snapshot.tool;
    color_ = snapshot.color;
    size_ = snapshot.size;
    drawing_ = false;
    draftIndex_ = -1;
    persistBoards();
    emit itemsChanged();
    emit selectionChanged();
    emit boardsChanged();
    emit boardSelectionChanged();
    emit activeChanged();
    emit toolChanged();
    emit colorChanged();
    emit sizeChanged();
    emit drawingChanged();
    currentHistoryToken_ = token;
    capturedToken_ = token;
    capturedRevision_ = historyRevision_;
    return true;
}

void WhiteboardModel::pruneHistorySnapshots(const QVariantList &tokens) {
    std::set<quint64> retained;
    for (const QVariant &token : tokens)
        retained.insert(token.toULongLong());
    for (auto entry = historySnapshots_.begin();
         entry != historySnapshots_.end();) {
        if (retained.find(entry->first) == retained.end())
            entry = historySnapshots_.erase(entry);
        else
            ++entry;
    }
}

bool WhiteboardModel::active() const {
    return active_;
}

QVariantList WhiteboardModel::items() const {
    QVariantList result;
    result.reserve(static_cast<qsizetype>(items_.size()));
    for (int index = 0; index < count(); ++index) {
        result.push_back(ToVariantMap(
                items_[static_cast<std::size_t>(index)],
                index == selectedIndex_));
    }
    return result;
}

int WhiteboardModel::count() const {
    return static_cast<int>(items_.size());
}

int WhiteboardModel::maximumCount() const {
    return kMaximumItems;
}

int WhiteboardModel::selectedIndex() const {
    return selectedIndex_;
}

QVariantMap WhiteboardModel::selectedItem() const {
    if (selectedIndex_ < 0 || selectedIndex_ >= count()) {
        return {};
    }
    return ToVariantMap(
            items_[static_cast<std::size_t>(selectedIndex_)], true);
}

QString WhiteboardModel::tool() const {
    return tool_;
}

QColor WhiteboardModel::color() const {
    return color_;
}

double WhiteboardModel::size() const {
    return size_;
}

bool WhiteboardModel::drawing() const {
    return drawing_;
}

QVariantList WhiteboardModel::boards() const {
    QVariantList result;
    result.reserve(static_cast<qsizetype>(boards_.size()));
    for (int index = 0; index < boardCount(); ++index) {
        result.push_back(boardToVariantMap(
                boards_[static_cast<std::size_t>(index)], index));
    }
    return result;
}

QVariantList WhiteboardModel::visibleBoards() const {
    QVariantList result;
    for (int index = 0; index < boardCount(); ++index) {
        const Board &board =
                boards_[static_cast<std::size_t>(index)];
        if (board.visible && board.mapKey == mapKey_) {
            result.push_back(boardToVariantMap(board, index));
        }
    }
    return result;
}

int WhiteboardModel::boardCount() const {
    return static_cast<int>(boards_.size());
}

int WhiteboardModel::maximumBoardCount() const {
    return kMaximumBoards;
}

int WhiteboardModel::selectedBoardIndex() const {
    return selectedBoardIndex_;
}

QString WhiteboardModel::mapKey() const {
    return mapKey_;
}

QString WhiteboardModel::mapName() const {
    return mapName_;
}

QString WhiteboardModel::operationMessage() const {
    return operationMessage_;
}

void WhiteboardModel::setActive(bool value) {
    if (active_ == value) {
        return;
    }
    if (!value) {
        cancelItem();
    }
    active_ = value;
    emit activeChanged();
}

void WhiteboardModel::setTool(const QString &value) {
    if (!IsKnownTool(value) || tool_ == value) {
        return;
    }
    cancelItem();
    tool_ = value;
    emit toolChanged();
}

void WhiteboardModel::setColor(const QColor &value) {
    if (!value.isValid() || color_ == value) {
        return;
    }
    color_ = value;
    emit colorChanged();
}

void WhiteboardModel::setSize(double value) {
    if (!IsFinite(value)) {
        return;
    }
    const double clamped = std::max(value, kMinimumSize);
    if (qFuzzyCompare(size_ + 1.0, clamped + 1.0)) {
        return;
    }
    size_ = clamped;
    emit sizeChanged();
}

void WhiteboardModel::setMapIdentity(const QString &key,
                                     const QString &name) {
    const QString normalizedKey = key.trimmed();
    const QString normalizedName = normalizedKey.isEmpty()
            ? QString()
            : NormalizeMapName(name);
    const bool keyChanged = mapKey_ != normalizedKey;
    const bool nameChanged = mapName_ != normalizedName;
    if (!keyChanged && !nameChanged) {
        return;
    }
    mapKey_ = normalizedKey;
    mapName_ = normalizedName;
    if (keyChanged) {
        selectedBoardIndex_ = -1;
    }

    if (keyChanged) {
        emit mapKeyChanged();
    }
    if (nameChanged) {
        emit mapNameChanged();
    }
    emit boardsChanged();
    if (keyChanged) {
        emit boardSelectionChanged();
    }
}

bool WhiteboardModel::beginItem(double x, double y) {
    if (!active_ || !IsDrawingTool(tool_) ||
        !IsFinite(x) || !IsFinite(y) ||
        drawing_ || count() >= maximumCount()) {
        return false;
    }
    const QPointF point(ClampUnit(x), ClampUnit(y));
    Item item;
    item.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    item.type = tool_;
    item.bounds = QRectF(point, QSizeF());
    item.color = color_;
    item.strokeWidth = size_;
    if (tool_ == QStringLiteral("pen") ||
        tool_ == QStringLiteral("line")) {
        item.points.push_back(point);
    }
    if (tool_ == QStringLiteral("line")) {
        item.points.push_back(point);
    }
    items_.push_back(std::move(item));
    draftIndex_ = count() - 1;
    selectedIndex_ = draftIndex_;
    draftOrigin_ = point;
    drawing_ = true;
    notifyItemsChanged(true);
    emit drawingChanged();
    return true;
}

bool WhiteboardModel::updateItem(double x, double y) {
    if (!active_ || !drawing_ || draftIndex_ < 0 ||
        draftIndex_ >= count() || !IsFinite(x) || !IsFinite(y)) {
        return false;
    }
    const QPointF point(ClampUnit(x), ClampUnit(y));
    Item &item = items_[static_cast<std::size_t>(draftIndex_)];
    if (item.type == QStringLiteral("pen")) {
        const QPointF previous = item.points.back();
        const double distance =
                std::hypot(point.x() - previous.x(),
                           point.y() - previous.y());
        if (distance < 0.001 ||
            item.points.size() >= kMaximumPointsPerItem) {
            return false;
        }
        item.points.push_back(point);
    } else if (item.type == QStringLiteral("line")) {
        item.points.back() = point;
    }
    if (item.type == QStringLiteral("rectangle") ||
        item.type == QStringLiteral("ellipse")) {
        item.bounds = ClampedBounds(draftOrigin_, point);
    } else {
        updateDraftBounds(&item);
    }
    emit itemsChanged();
    emit selectionChanged();
    return true;
}

bool WhiteboardModel::finishItem() {
    if (!drawing_ || draftIndex_ < 0 || draftIndex_ >= count()) {
        return false;
    }
    Item &item = items_[static_cast<std::size_t>(draftIndex_)];
    const bool lineValid =
            item.type == QStringLiteral("line") &&
            item.points.size() == 2u &&
            std::hypot(item.points[1].x() - item.points[0].x(),
                       item.points[1].y() - item.points[0].y()) >=
                    kMinimumGestureExtent;
    const bool shapeValid =
            (item.type == QStringLiteral("rectangle") ||
             item.type == QStringLiteral("ellipse")) &&
            item.bounds.width() >= kMinimumGestureExtent &&
            item.bounds.height() >= kMinimumGestureExtent;
    const bool valid = item.type == QStringLiteral("pen")
            ? !item.points.empty()
            : lineValid || shapeValid;
    if (!valid) {
        removeDraft();
        return false;
    }
    updateDraftBounds(&item);
    drawing_ = false;
    draftIndex_ = -1;
    emit itemsChanged();
    emit selectionChanged();
    emit drawingChanged();
    return true;
}

void WhiteboardModel::cancelItem() {
    if (!drawing_) {
        return;
    }
    removeDraft();
}

int WhiteboardModel::addText(double x,
                             double y,
                             const QString &text) {
    const QString normalized = NormalizeText(text);
    if (!active_ || !IsFinite(x) || !IsFinite(y) ||
        normalized.isEmpty() || count() >= maximumCount()) {
        return -1;
    }
    const double width = 0.28;
    const double height =
            std::clamp(0.065 + size_ * 0.0015, 0.07, 0.12);
    const double left =
            std::clamp(ClampUnit(x), 0.0, 1.0 - width);
    const double top =
            std::clamp(ClampUnit(y), 0.0, 1.0 - height);
    Item item;
    item.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    item.type = QStringLiteral("text");
    item.bounds = QRectF(left, top, width, height);
    item.color = color_;
    item.strokeWidth = size_;
    item.fontSize = std::clamp(
            0.035 + size_ * 0.0015, 0.035, 0.075);
    item.text = normalized;
    items_.push_back(std::move(item));
    selectedIndex_ = count() - 1;
    notifyItemsChanged(true);
    return selectedIndex_;
}

bool WhiteboardModel::setText(int index, const QString &text) {
    const QString normalized = NormalizeText(text);
    if (!active_ || index < 0 || index >= count() ||
        normalized.isEmpty()) {
        return false;
    }
    Item &item = items_[static_cast<std::size_t>(index)];
    if (item.type != QStringLiteral("text") ||
        item.text == normalized) {
        return false;
    }
    item.text = normalized;
    emit itemsChanged();
    if (index == selectedIndex_) {
        emit selectionChanged();
    }
    return true;
}

int WhiteboardModel::itemAt(double x, double y) const {
    if (!active_ || drawing_ || !IsFinite(x) || !IsFinite(y)) {
        return -1;
    }
    const QPointF point(ClampUnit(x), ClampUnit(y));
    for (int index = count() - 1; index >= 0; --index) {
        const Item &item = items_[static_cast<std::size_t>(index)];
        const double padding = std::max(0.003, item.strokeWidth / 2000.0);
        if (item.bounds.adjusted(-padding, -padding, padding, padding)
                    .contains(point)) {
            return index;
        }
    }
    return -1;
}

bool WhiteboardModel::selectItem(int index) {
    if (!active_ || drawing_ || index < 0 || index >= count() ||
        selectedIndex_ == index) {
        return false;
    }
    selectedIndex_ = index;
    emit itemsChanged();
    emit selectionChanged();
    return true;
}

void WhiteboardModel::clearSelection() {
    if (!active_ || drawing_ || selectedIndex_ < 0) {
        return;
    }
    selectedIndex_ = -1;
    emit itemsChanged();
    emit selectionChanged();
}

bool WhiteboardModel::moveSelected(double deltaX, double deltaY) {
    if (!active_ || drawing_ || selectedIndex_ < 0 ||
        selectedIndex_ >= count() || !IsFinite(deltaX) ||
        !IsFinite(deltaY)) {
        return false;
    }
    Item &item = items_[static_cast<std::size_t>(selectedIndex_)];
    const double clampedX = std::clamp(
            deltaX, -item.bounds.left(), 1.0 - item.bounds.right());
    const double clampedY = std::clamp(
            deltaY, -item.bounds.top(), 1.0 - item.bounds.bottom());
    if (qFuzzyIsNull(clampedX) && qFuzzyIsNull(clampedY)) {
        return false;
    }
    TranslateItem(&item, QPointF(clampedX, clampedY));
    emit itemsChanged();
    emit selectionChanged();
    return true;
}

bool WhiteboardModel::resizeSelected(double deltaWidth,
                                     double deltaHeight) {
    if (!active_ || drawing_ || selectedIndex_ < 0 ||
        selectedIndex_ >= count() || !IsFinite(deltaWidth) ||
        !IsFinite(deltaHeight)) {
        return false;
    }
    Item &item = items_[static_cast<std::size_t>(selectedIndex_)];
    const QRectF oldBounds = item.bounds;
    const double width = std::clamp(
            oldBounds.width() + deltaWidth,
            kMinimumExtent,
            1.0 - oldBounds.left());
    const double height = std::clamp(
            oldBounds.height() + deltaHeight,
            kMinimumExtent,
            1.0 - oldBounds.top());
    if (qFuzzyCompare(width + 1.0, oldBounds.width() + 1.0) &&
        qFuzzyCompare(height + 1.0, oldBounds.height() + 1.0)) {
        return false;
    }
    const double scaleX = width / oldBounds.width();
    const double scaleY = height / oldBounds.height();
    const QPointF origin = oldBounds.topLeft();
    for (QPointF &point : item.points) {
        point = QPointF(
                origin.x() + (point.x() - origin.x()) * scaleX,
                origin.y() + (point.y() - origin.y()) * scaleY);
    }
    for (Erasure &erasure : item.erasures) {
        erasure.point = QPointF(
                origin.x() +
                        (erasure.point.x() - origin.x()) * scaleX,
                origin.y() +
                        (erasure.point.y() - origin.y()) * scaleY);
        erasure.radius *= std::sqrt(scaleX * scaleY);
    }
    item.fontSize *= scaleY;
    item.bounds.setSize(QSizeF(width, height));
    emit itemsChanged();
    emit selectionChanged();
    return true;
}

bool WhiteboardModel::eraseSelected(double x,
                                    double y,
                                    double radius) {
    if (!active_ || drawing_ || selectedIndex_ < 0 ||
        selectedIndex_ >= count() || !IsFinite(x) || !IsFinite(y) ||
        !IsFinite(radius) || radius <= 0.0) {
        return false;
    }
    Item &item = items_[static_cast<std::size_t>(selectedIndex_)];
    const QPointF point(ClampUnit(x), ClampUnit(y));
    const double clampedRadius = std::clamp(radius, 0.002, 0.2);
    const QRectF hitBounds =
            item.bounds.adjusted(-clampedRadius,
                                 -clampedRadius,
                                 clampedRadius,
                                 clampedRadius);
    if (!hitBounds.contains(point) ||
        item.erasures.size() >= kMaximumErasuresPerItem) {
        return false;
    }
    if (!item.erasures.empty()) {
        const Erasure &last = item.erasures.back();
        if (std::hypot(last.point.x() - point.x(),
                       last.point.y() - point.y()) <
            clampedRadius * 0.25) {
            return false;
        }
    }
    item.erasures.push_back({point, clampedRadius});
    emit itemsChanged();
    emit selectionChanged();
    return true;
}

bool WhiteboardModel::removeSelected() {
    if (!active_ || drawing_ || selectedIndex_ < 0 ||
        selectedIndex_ >= count()) {
        return false;
    }
    items_.erase(items_.begin() + selectedIndex_);
    selectedIndex_ = items_.empty()
            ? -1
            : std::min(selectedIndex_, count() - 1);
    notifyItemsChanged(true);
    return true;
}

int WhiteboardModel::captureCurrentBoard(
        const QString &name,
        const QVariantMap &capture) {
    if (!active_ || drawing_ || items_.empty() ||
        mapKey_.isEmpty() || mapName_.isEmpty() ||
        boardCount() >= maximumBoardCount()) {
        setOperationMessage(QStringLiteral(
                "Draw something on a loaded map before placing it."));
        return -1;
    }
    const QString normalizedName = NormalizeBoardName(name);
    if (normalizedName.isEmpty()) {
        setOperationMessage(QStringLiteral(
                "Enter a name for the drawing."));
        return -1;
    }
    const auto number = [&capture](const QString &key,
                                    double *result) {
        bool okay = false;
        const double value = capture.value(key).toDouble(&okay);
        if (!okay || !IsFinite(value)) {
            return false;
        }
        *result = value;
        return true;
    };
    Board board;
    board.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    board.name = normalizedName;
    board.mapKey = mapKey_;
    board.mapName = mapName_;
    double targetX = 0.0;
    double targetY = 0.0;
    double targetZ = 0.0;
    double planeX = 0.0;
    double planeY = 0.0;
    double planeZ = 0.0;
    board.projectionVersion = kProjectionVersion;
    board.projection =
            QString::fromLatin1(kPerspectiveVerticalProjection);
    if (!number(QStringLiteral("targetX"), &targetX) ||
        !number(QStringLiteral("targetY"), &targetY) ||
        !number(QStringLiteral("targetZ"), &targetZ) ||
        !number(QStringLiteral("yaw"), &board.yaw) ||
        !number(QStringLiteral("pitch"), &board.pitch) ||
        !number(QStringLiteral("distance"), &board.distance) ||
        !number(QStringLiteral("planeX"), &planeX) ||
        !number(QStringLiteral("planeY"), &planeY) ||
        !number(QStringLiteral("planeZ"), &planeZ) ||
        !number(QStringLiteral("planeWidth"), &board.planeWidth) ||
        !number(QStringLiteral("planeHeight"), &board.planeHeight) ||
        !number(QStringLiteral("fieldOfView"), &board.fieldOfView) ||
        !number(QStringLiteral("planeDistance"),
                &board.planeDistance) ||
        !number(QStringLiteral("viewportWidth"),
                &board.viewportWidth) ||
        !number(QStringLiteral("viewportHeight"),
                &board.viewportHeight) ||
        !number(QStringLiteral("contentX"), &board.contentX) ||
        !number(QStringLiteral("contentY"), &board.contentY) ||
        !number(QStringLiteral("contentWidth"),
                &board.contentWidth) ||
        !number(QStringLiteral("contentHeight"),
                &board.contentHeight) ||
        !number(QStringLiteral("canvasWidth"), &board.canvasWidth) ||
        !number(QStringLiteral("canvasHeight"), &board.canvasHeight) ||
        capture.value(QStringLiteral("projection")).toString() !=
                board.projection ||
        std::abs(targetX) > 10000000.0 ||
        std::abs(targetY) > 10000000.0 ||
        std::abs(targetZ) > 10000000.0 ||
        std::abs(planeX) > 10000000.0 ||
        std::abs(planeY) > 10000000.0 ||
        std::abs(planeZ) > 10000000.0 ||
        board.pitch < -89.0 || board.pitch > 89.0 ||
        board.distance < 0.01 || board.distance > 1000000.0 ||
        board.fieldOfView < 1.0 || board.fieldOfView > 179.0 ||
        board.planeDistance < 0.01 ||
        board.planeDistance > 1000000.0 ||
        board.viewportWidth < 1.0 ||
        board.viewportWidth > kMaximumProjectionCanvasDimension ||
        board.viewportHeight < 1.0 ||
        board.viewportHeight > kMaximumProjectionCanvasDimension ||
        board.contentX < 0.0 || board.contentX > 1.0 ||
        board.contentY < 0.0 || board.contentY > 1.0 ||
        board.contentWidth <= 0.0 ||
        board.contentHeight <= 0.0 ||
        board.contentX + board.contentWidth > 1.000001 ||
        board.contentY + board.contentHeight > 1.000001 ||
        board.canvasWidth < 1.0 ||
        board.canvasWidth > kMaximumProjectionCanvasDimension ||
        board.canvasHeight < 1.0 ||
        board.canvasHeight > kMaximumProjectionCanvasDimension ||
        board.planeWidth < 0.01 || board.planeWidth > 1000000.0 ||
        board.planeHeight < 0.01 ||
        board.planeHeight > 1000000.0) {
        setOperationMessage(QStringLiteral(
                "The current camera view cannot be captured."));
        return -1;
    }
    board.target = QVector3D(
            static_cast<float>(targetX),
            static_cast<float>(targetY),
            static_cast<float>(targetZ));
    board.planePosition = QVector3D(
            static_cast<float>(planeX),
            static_cast<float>(planeY),
            static_cast<float>(planeZ));
    board.items = items_;
    boards_.push_back(std::move(board));
    if (!persistBoards()) {
        boards_.pop_back();
        return -1;
    }
    items_.clear();
    selectedIndex_ = -1;
    selectedBoardIndex_ = boardCount() - 1;
    notifyItemsChanged(true);
    emit boardsChanged();
    emit boardSelectionChanged();
    setOperationMessage(QStringLiteral(
            "Drawing placed in the 3D view."));
    return selectedBoardIndex_;
}

bool WhiteboardModel::pickUpBoard(int index) {
    if (!active_ || drawing_ || !items_.empty() || index < 0 ||
        index >= boardCount()) {
        setOperationMessage(QStringLiteral(
                "Finish or place the current drawing before picking one up."));
        return false;
    }
    const Board board = boards_[static_cast<std::size_t>(index)];
    if (board.mapKey != mapKey_) {
        setOperationMessage(QStringLiteral(
                "Load this drawing's map before picking it up."));
        return false;
    }

    const int previousSelection = selectedBoardIndex_;
    boards_.erase(boards_.begin() + index);
    selectedBoardIndex_ = -1;
    if (!persistBoards()) {
        boards_.insert(boards_.begin() + index, board);
        selectedBoardIndex_ = previousSelection;
        return false;
    }

    items_ = board.items;
    selectedIndex_ = items_.empty() ? -1 : 0;
    notifyItemsChanged(true);
    emit boardsChanged();
    emit boardSelectionChanged();
    setOperationMessage(QStringLiteral(
            "Drawing picked up and ready to place again."));
    return true;
}

bool WhiteboardModel::selectBoard(int index) {
    if (index < 0 || index >= boardCount()) {
        return false;
    }
    const Board &board = boards_[static_cast<std::size_t>(index)];
    if (board.mapKey != mapKey_) {
        setOperationMessage(QStringLiteral(
                "Load this drawing's map before restoring its view."));
        return false;
    }
    if (selectedBoardIndex_ == index) {
        return true;
    }
    selectedBoardIndex_ = index;
    emit boardsChanged();
    emit boardSelectionChanged();
    return true;
}

bool WhiteboardModel::setBoardVisible(int index, bool visible) {
    if (index < 0 || index >= boardCount()) {
        return false;
    }
    Board &board = boards_[static_cast<std::size_t>(index)];
    if (board.mapKey != mapKey_) {
        setOperationMessage(QStringLiteral(
                "Load this drawing's map to change its visibility."));
        return false;
    }
    if (board.visible == visible) {
        return true;
    }
    board.visible = visible;
    if (!persistBoards()) {
        board.visible = !visible;
        return false;
    }
    emit boardsChanged();
    setOperationMessage(visible
            ? QStringLiteral("Drawing shown in the current map.")
            : QStringLiteral("Drawing hidden but kept in the list."));
    return true;
}

bool WhiteboardModel::removeBoard(int index) {
    if (index < 0 || index >= boardCount()) {
        return false;
    }
    const Board removed =
            boards_[static_cast<std::size_t>(index)];
    boards_.erase(boards_.begin() + index);
    const int previousSelection = selectedBoardIndex_;
    if (selectedBoardIndex_ == index) {
        selectedBoardIndex_ = -1;
    } else if (selectedBoardIndex_ > index) {
        --selectedBoardIndex_;
    }
    if (!persistBoards()) {
        boards_.insert(boards_.begin() + index, removed);
        selectedBoardIndex_ = previousSelection;
        return false;
    }
    emit boardsChanged();
    emit boardSelectionChanged();
    setOperationMessage(QStringLiteral("Drawing removed."));
    return true;
}

bool WhiteboardModel::exportBoardSet(const QUrl &fileUrl) {
    const QString path = LocalPath(fileUrl);
    if (path.isEmpty() || boards_.empty()) {
        setOperationMessage(QStringLiteral(
                "Choose a file after placing at least one drawing."));
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        setOperationMessage(QStringLiteral(
                "The whiteboard set could not be opened for writing."));
        return false;
    }
    const QString setName =
            NormalizeBoardName(QFileInfo(path).completeBaseName());
    const QByteArray data = serializeBoards(
            boards_,
            setName.isEmpty()
                    ? QStringLiteral("Whiteboard set")
                    : setName);
    if (file.write(data) != data.size() || !file.commit()) {
        setOperationMessage(QStringLiteral(
                "The whiteboard set could not be saved."));
        return false;
    }
    setOperationMessage(QStringLiteral(
            "Whiteboard set exported."));
    return true;
}

bool WhiteboardModel::importBoardSet(const QUrl &fileUrl) {
    const QString path = LocalPath(fileUrl);
    QFile file(path);
    if (path.isEmpty() || !file.open(QIODevice::ReadOnly) ||
        file.size() < 1 || file.size() > kMaximumImportBytes) {
        setOperationMessage(QStringLiteral(
                "Choose a valid whiteboard set smaller than 16 MB."));
        return false;
    }
    const QByteArray data = file.readAll();
    std::vector<Board> imported;
    QString error;
    if (!deserializeBoards(data, &imported, &error) ||
        imported.empty() ||
        imported.size() >
                static_cast<std::size_t>(
                        maximumBoardCount() - boardCount())) {
        setOperationMessage(error.isEmpty()
                ? QStringLiteral(
                          "The imported set exceeds the drawing limit.")
                : error);
        return false;
    }
    const std::size_t previousSize = boards_.size();
    if (mapKey_.isEmpty() &&
        std::any_of(imported.cbegin(), imported.cend(),
                    [](const Board &board) {
                        return board.mapKey.isEmpty();
                    })) {
        setOperationMessage(QStringLiteral(
                "Load a map before importing map-neutral drawings."));
        return false;
    }
    for (Board &board : imported) {
        board.id =
                QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (board.mapKey.isEmpty()) {
            board.mapKey = mapKey_;
        }
        boards_.push_back(std::move(board));
    }
    if (!persistBoards()) {
        boards_.resize(previousSize);
        return false;
    }
    emit boardsChanged();
    setOperationMessage(QStringLiteral(
            "Imported %1 drawing(s).")
                                .arg(imported.size()));
    return true;
}

bool WhiteboardModel::exportBoardContentImage(
        int index,
        const QUrl &fileUrl) {
    const QString path = LocalPath(fileUrl);
    if (index < 0 || index >= boardCount() || path.isEmpty()) {
        setOperationMessage(QStringLiteral(
                "Choose a valid drawing and local image file."));
        return false;
    }
    const Board &board = boards_[static_cast<std::size_t>(index)];
    const double aspect = board.planeWidth / board.planeHeight;
    if (!IsFinite(aspect) || aspect <= 0.0) {
        setOperationMessage(QStringLiteral(
                "The drawing has invalid image dimensions."));
        return false;
    }
    const int width = aspect >= 1.0
            ? kExportImageLongEdge
            : std::max(1, qRound(kExportImageLongEdge * aspect));
    const int height = aspect >= 1.0
            ? std::max(1, qRound(kExportImageLongEdge / aspect))
            : kExportImageLongEdge;
    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) {
        setOperationMessage(QStringLiteral(
                "The transparent drawing image could not be created."));
        return false;
    }
    image.fill(Qt::transparent);
    QPainter painter(&image);
    if (!painter.isActive()) {
        setOperationMessage(QStringLiteral(
                "The transparent drawing image could not be rendered."));
        return false;
    }
    const double strokeScale = std::max(
            0.01,
            std::min(width / board.canvasWidth,
                     height / board.canvasHeight));
    for (const Item &item : board.items) {
        const QRectF pixelBounds(
                item.bounds.x() * width,
                item.bounds.y() * height,
                std::max(1.0, item.bounds.width() * width),
                std::max(1.0, item.bounds.height() * height));
        QVariantMap drawing = ToVariantMap(item, false);
        drawing.insert(
                QStringLiteral("strokeWidth"),
                item.strokeWidth * strokeScale);
        WhiteboardCanvasItem canvas;
        canvas.setWidth(pixelBounds.width());
        canvas.setHeight(pixelBounds.height());
        canvas.setDrawing(drawing);
        painter.save();
        painter.translate(pixelBounds.topLeft());
        canvas.paint(&painter);
        painter.restore();
    }
    painter.end();

    if (!SavePngAtomically(image, path)) {
        setOperationMessage(QStringLiteral(
                "The transparent drawing image could not be saved."));
        return false;
    }
    setOperationMessage(QStringLiteral(
            "Transparent drawing image exported."));
    return true;
}

QString WhiteboardModel::imageExportPath(
        const QUrl &fileUrl) const {
    return LocalPath(fileUrl);
}

bool WhiteboardModel::saveBoardBackgroundImage(
        const QVariant &imageValue,
        const QUrl &fileUrl) {
    return imageValue.canConvert<QImage>() &&
            SavePngAtomically(
                    imageValue.value<QImage>(),
                    LocalPath(fileUrl));
}

void WhiteboardModel::finishBoardImageExport(
        bool success,
        bool fullBackground) {
    if (success) {
        setOperationMessage(fullBackground
                ? QStringLiteral(
                          "Drawing image with background exported.")
                : QStringLiteral(
                          "Transparent drawing image exported."));
    } else {
        setOperationMessage(fullBackground
                ? QStringLiteral(
                          "The drawing image with background could not be saved.")
                : QStringLiteral(
                          "The transparent drawing image could not be saved."));
    }
}

bool WhiteboardModel::IsFinite(double value) {
    return std::isfinite(value);
}

bool WhiteboardModel::IsPointFinite(const QPointF &point) {
    return IsFinite(point.x()) && IsFinite(point.y());
}

double WhiteboardModel::ClampUnit(double value) {
    return std::clamp(value, 0.0, 1.0);
}

QString WhiteboardModel::NormalizeText(const QString &text) {
    QString normalized = text;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return normalized.trimmed().left(500);
}

QVariantMap WhiteboardModel::ToVariantMap(const Item &item,
                                          bool selected) {
    QVariantList points;
    points.reserve(static_cast<qsizetype>(item.points.size()));
    for (const QPointF &point : item.points) {
        points.push_back(point);
    }
    QVariantList erasures;
    erasures.reserve(static_cast<qsizetype>(item.erasures.size()));
    for (const Erasure &erasure : item.erasures) {
        erasures.push_back(QVariantMap{
                {QStringLiteral("x"), erasure.point.x()},
                {QStringLiteral("y"), erasure.point.y()},
                {QStringLiteral("radius"), erasure.radius}});
    }
    return {
            {QStringLiteral("id"), item.id},
            {QStringLiteral("type"), item.type},
            {QStringLiteral("x"), item.bounds.x()},
            {QStringLiteral("y"), item.bounds.y()},
            {QStringLiteral("width"), item.bounds.width()},
            {QStringLiteral("height"), item.bounds.height()},
            {QStringLiteral("color"), item.color},
            {QStringLiteral("strokeWidth"), item.strokeWidth},
            {QStringLiteral("fontSize"), item.fontSize},
            {QStringLiteral("text"), item.text},
            {QStringLiteral("points"), points},
            {QStringLiteral("erasures"), erasures},
            {QStringLiteral("selected"), selected}};
}

QVariantMap WhiteboardModel::boardToVariantMap(
        const Board &board,
        int index) const {
    QVariantList boardItems;
    boardItems.reserve(
            static_cast<qsizetype>(board.items.size()));
    for (const Item &item : board.items) {
        boardItems.push_back(ToVariantMap(item, false));
    }
    return {
            {QStringLiteral("id"), board.id},
            {QStringLiteral("name"), board.name},
            {QStringLiteral("mapKey"), board.mapKey},
            {QStringLiteral("mapName"), board.mapName},
            {QStringLiteral("visible"), board.visible},
            {QStringLiteral("isCurrentMap"), board.mapKey == mapKey_},
            {QStringLiteral("boardIndex"), index},
            {QStringLiteral("selected"),
             index == selectedBoardIndex_},
            {QStringLiteral("projectionVersion"),
             board.projectionVersion},
            {QStringLiteral("projection"), board.projection},
            {QStringLiteral("fieldOfView"), board.fieldOfView},
            {QStringLiteral("planeDistance"),
             board.planeDistance},
            {QStringLiteral("viewportWidth"),
             board.viewportWidth},
            {QStringLiteral("viewportHeight"),
             board.viewportHeight},
            {QStringLiteral("contentX"), board.contentX},
            {QStringLiteral("contentY"), board.contentY},
            {QStringLiteral("contentWidth"),
             board.contentWidth},
            {QStringLiteral("contentHeight"),
             board.contentHeight},
            {QStringLiteral("canvasWidth"), board.canvasWidth},
            {QStringLiteral("canvasHeight"), board.canvasHeight},
            {QStringLiteral("targetX"), board.target.x()},
            {QStringLiteral("targetY"), board.target.y()},
            {QStringLiteral("targetZ"), board.target.z()},
            {QStringLiteral("yaw"), board.yaw},
            {QStringLiteral("pitch"), board.pitch},
            {QStringLiteral("distance"), board.distance},
            {QStringLiteral("planeX"), board.planePosition.x()},
            {QStringLiteral("planeY"), board.planePosition.y()},
            {QStringLiteral("planeZ"), board.planePosition.z()},
            {QStringLiteral("planeWidth"), board.planeWidth},
            {QStringLiteral("planeHeight"), board.planeHeight},
            {QStringLiteral("items"), boardItems}};
}

QString WhiteboardModel::LocalPath(const QUrl &fileUrl) {
    if (fileUrl.isLocalFile()) {
        return QFileInfo(fileUrl.toLocalFile()).absoluteFilePath();
    }
    if (fileUrl.scheme().isEmpty()) {
        return QFileInfo(fileUrl.toString()).absoluteFilePath();
    }
    return {};
}

QString WhiteboardModel::NormalizeBoardName(
        const QString &name) {
    QString normalized = name.simplified();
    return normalized.left(120);
}

QString WhiteboardModel::NormalizeMapName(
        const QString &name) {
    QString normalized = StripTrackmaniaFormatting(name);
    for (qsizetype index = 0; index < normalized.size(); ++index) {
        QChar character = normalized.at(index);
        if (character.category() == QChar::Other_Control ||
            character == QChar::LineSeparator ||
            character == QChar::ParagraphSeparator) {
            normalized[index] = QLatin1Char(' ');
        }
    }
    return normalized.simplified().left(kMaximumMapNameLength);
}

QByteArray WhiteboardModel::serializeBoards(
        const std::vector<Board> &boards,
        const QString &setName) {
    QJsonArray serializedBoards;
    for (const Board &board : boards) {
        QJsonArray serializedItems;
        for (const Item &item : board.items) {
            QJsonArray points;
            for (const QPointF &point : item.points) {
                points.push_back(QJsonArray{
                        point.x(), point.y()});
            }
            QJsonArray erasures;
            for (const Erasure &erasure : item.erasures) {
                erasures.push_back(QJsonObject{
                        {QStringLiteral("x"), erasure.point.x()},
                        {QStringLiteral("y"), erasure.point.y()},
                        {QStringLiteral("radius"), erasure.radius}});
            }
            serializedItems.push_back(QJsonObject{
                    {QStringLiteral("type"), item.type},
                    {QStringLiteral("x"), item.bounds.x()},
                    {QStringLiteral("y"), item.bounds.y()},
                    {QStringLiteral("width"), item.bounds.width()},
                    {QStringLiteral("height"), item.bounds.height()},
                    {QStringLiteral("color"), item.color.name(
                                                       QColor::HexArgb)},
                    {QStringLiteral("strokeWidth"), item.strokeWidth},
                    {QStringLiteral("fontSize"), item.fontSize},
                    {QStringLiteral("text"), item.text},
                    {QStringLiteral("points"), points},
                    {QStringLiteral("erasures"), erasures}});
        }
        serializedBoards.push_back(QJsonObject{
                {QStringLiteral("id"), board.id},
                {QStringLiteral("name"), board.name},
                {QStringLiteral("mapKey"), board.mapKey},
                {QStringLiteral("mapName"), board.mapName},
                {QStringLiteral("visible"), board.visible},
                {QStringLiteral("projectionVersion"),
                 board.projectionVersion},
                {QStringLiteral("projection"), board.projection},
                {QStringLiteral("fieldOfView"),
                 board.fieldOfView},
                {QStringLiteral("planeDistance"),
                 board.planeDistance},
                {QStringLiteral("viewportWidth"),
                 board.viewportWidth},
                {QStringLiteral("viewportHeight"),
                 board.viewportHeight},
                {QStringLiteral("contentX"), board.contentX},
                {QStringLiteral("contentY"), board.contentY},
                {QStringLiteral("contentWidth"),
                 board.contentWidth},
                {QStringLiteral("contentHeight"),
                 board.contentHeight},
                {QStringLiteral("canvasWidth"),
                 board.canvasWidth},
                {QStringLiteral("canvasHeight"),
                 board.canvasHeight},
                {QStringLiteral("target"),
                 QJsonArray{board.target.x(),
                            board.target.y(),
                            board.target.z()}},
                {QStringLiteral("yaw"), board.yaw},
                {QStringLiteral("pitch"), board.pitch},
                {QStringLiteral("distance"), board.distance},
                {QStringLiteral("planePosition"),
                 QJsonArray{board.planePosition.x(),
                            board.planePosition.y(),
                            board.planePosition.z()}},
                {QStringLiteral("planeWidth"), board.planeWidth},
                {QStringLiteral("planeHeight"), board.planeHeight},
                {QStringLiteral("items"), serializedItems}});
    }
    const QJsonObject root{
            {QStringLiteral("format"),
             QString::fromLatin1(kFileFormat)},
            {QStringLiteral("version"), kFileVersion},
            {QStringLiteral("name"), setName},
            {QStringLiteral("boards"), serializedBoards}};
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

bool WhiteboardModel::deserializeBoards(
        const QByteArray &data,
        std::vector<Board> *boards,
        QString *error) {
    if (boards == nullptr || error == nullptr) {
        return false;
    }
    boards->clear();
    QJsonParseError parseError;
    const QJsonDocument document =
            QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError ||
        !document.isObject()) {
        *error = QStringLiteral(
                "The selected file is not valid JSON.");
        return false;
    }
    const QJsonObject root = document.object();
    const int fileVersion =
            root.value(QStringLiteral("version")).toInt(-1);
    if (root.value(QStringLiteral("format")).toString() !=
                QString::fromLatin1(kFileFormat) ||
        (fileVersion != kFileVersion &&
         fileVersion != kLegacyFileVersion) ||
        !root.value(QStringLiteral("boards")).isArray()) {
        *error = QStringLiteral(
                "The selected file is not a supported whiteboard set.");
        return false;
    }
    const QJsonArray serializedBoards =
            root.value(QStringLiteral("boards")).toArray();
    if (serializedBoards.isEmpty() ||
        serializedBoards.size() > kMaximumBoards) {
        *error = QStringLiteral(
                "The whiteboard set has an invalid drawing count.");
        return false;
    }
    const auto finiteNumber = [](const QJsonObject &object,
                                 const QString &key,
                                 double *result) {
        const QJsonValue value = object.value(key);
        if (!value.isDouble() || !IsFinite(value.toDouble())) {
            return false;
        }
        *result = value.toDouble();
        return true;
    };
    const auto vector = [](const QJsonValue &value,
                           QVector3D *result) {
        if (!value.isArray()) {
            return false;
        }
        const QJsonArray values = value.toArray();
        if (values.size() != 3 ||
            !values[0].isDouble() ||
            !values[1].isDouble() ||
            !values[2].isDouble() ||
            !IsFinite(values[0].toDouble()) ||
            !IsFinite(values[1].toDouble()) ||
            !IsFinite(values[2].toDouble())) {
            return false;
        }
        *result = QVector3D(
                static_cast<float>(values[0].toDouble()),
                static_cast<float>(values[1].toDouble()),
                static_cast<float>(values[2].toDouble()));
        return true;
    };

    std::vector<Board> parsedBoards;
    parsedBoards.reserve(
            static_cast<std::size_t>(serializedBoards.size()));
    for (const QJsonValue &boardValue : serializedBoards) {
        if (!boardValue.isObject()) {
            *error = QStringLiteral(
                    "A drawing entry is malformed.");
            return false;
        }
        const QJsonObject object = boardValue.toObject();
        Board board;
        board.id = object.value(QStringLiteral("id")).toString();
        board.name = NormalizeBoardName(
                object.value(QStringLiteral("name")).toString());
        board.mapKey =
                object.value(QStringLiteral("mapKey")).toString().trimmed();
        const QJsonValue mapNameValue =
                object.value(QStringLiteral("mapName"));
        const QString rawMapName = mapNameValue.toString();
        board.mapName = NormalizeMapName(rawMapName);
        board.visible =
                object.value(QStringLiteral("visible")).toBool(true);
        if (!object.value(QStringLiteral("id")).isString() ||
            !object.value(QStringLiteral("name")).isString() ||
            !object.value(QStringLiteral("mapKey")).isString() ||
            !mapNameValue.isString() ||
            !object.value(QStringLiteral("visible")).isBool() ||
            board.id.isEmpty() || board.id.size() > 120 ||
            board.name.isEmpty() ||
            board.mapKey.size() > 4096 ||
            rawMapName.size() > 4096 ||
            board.mapName.isEmpty()) {
            *error = QStringLiteral(
                    "A drawing has invalid identity or map data.");
            return false;
        }
        if (fileVersion == kFileVersion) {
            board.projectionVersion =
                    object.value(
                              QStringLiteral("projectionVersion"))
                            .toInt(-1);
            board.projection =
                    object.value(QStringLiteral("projection"))
                            .toString();
            if (!object.value(
                        QStringLiteral("projectionVersion")).isDouble() ||
                (board.projectionVersion != 0 &&
                 board.projectionVersion != kProjectionVersion) ||
                !object.value(
                        QStringLiteral("projection")).isString() ||
                board.projection !=
                        QString::fromLatin1(
                                kPerspectiveVerticalProjection) ||
                !finiteNumber(object,
                              QStringLiteral("fieldOfView"),
                              &board.fieldOfView) ||
                !finiteNumber(object,
                              QStringLiteral("planeDistance"),
                              &board.planeDistance) ||
                !finiteNumber(object,
                              QStringLiteral("viewportWidth"),
                              &board.viewportWidth) ||
                !finiteNumber(object,
                              QStringLiteral("viewportHeight"),
                              &board.viewportHeight) ||
                !finiteNumber(object,
                              QStringLiteral("contentX"),
                              &board.contentX) ||
                !finiteNumber(object,
                              QStringLiteral("contentY"),
                              &board.contentY) ||
                !finiteNumber(object,
                              QStringLiteral("contentWidth"),
                              &board.contentWidth) ||
                !finiteNumber(object,
                              QStringLiteral("contentHeight"),
                              &board.contentHeight) ||
                !finiteNumber(object,
                              QStringLiteral("canvasWidth"),
                              &board.canvasWidth) ||
                !finiteNumber(object,
                              QStringLiteral("canvasHeight"),
                              &board.canvasHeight) ||
                board.fieldOfView < 1.0 ||
                board.fieldOfView > 179.0 ||
                board.planeDistance < 0.01 ||
                board.planeDistance > 1000000.0 ||
                board.viewportWidth < 1.0 ||
                board.viewportWidth >
                        kMaximumProjectionCanvasDimension ||
                board.viewportHeight < 1.0 ||
                board.viewportHeight >
                        kMaximumProjectionCanvasDimension ||
                board.contentX < 0.0 ||
                board.contentX > 1.0 ||
                board.contentY < 0.0 ||
                board.contentY > 1.0 ||
                board.contentWidth <= 0.0 ||
                board.contentHeight <= 0.0 ||
                board.contentX + board.contentWidth > 1.000001 ||
                board.contentY + board.contentHeight > 1.000001 ||
                board.canvasWidth < 1.0 ||
                board.canvasWidth >
                        kMaximumProjectionCanvasDimension ||
                board.canvasHeight < 1.0 ||
                board.canvasHeight >
                        kMaximumProjectionCanvasDimension) {
                *error = QStringLiteral(
                        "A drawing has invalid projection data.");
                return false;
            }
        } else {
            board.projectionVersion = 0;
            board.projection =
                    QString::fromLatin1(
                            kPerspectiveVerticalProjection);
        }
        if (!vector(object.value(QStringLiteral("target")),
                    &board.target) ||
            !vector(object.value(QStringLiteral("planePosition")),
                    &board.planePosition) ||
            !finiteNumber(object, QStringLiteral("yaw"),
                          &board.yaw) ||
            !finiteNumber(object, QStringLiteral("pitch"),
                          &board.pitch) ||
            !finiteNumber(object, QStringLiteral("distance"),
                          &board.distance) ||
            !finiteNumber(object, QStringLiteral("planeWidth"),
                          &board.planeWidth) ||
            !finiteNumber(object, QStringLiteral("planeHeight"),
                          &board.planeHeight) ||
            board.pitch < -89.0 || board.pitch > 89.0 ||
            board.distance < 0.01 ||
            board.distance > 1000000.0 ||
            board.planeWidth < 0.01 ||
            board.planeWidth > 1000000.0 ||
            board.planeHeight < 0.01 ||
            board.planeHeight > 1000000.0 ||
            board.target.length() > 10000000.0f ||
            board.planePosition.length() > 10000000.0f ||
            !object.value(QStringLiteral("items")).isArray()) {
            *error = QStringLiteral(
                    "A drawing has invalid camera or plane data.");
            return false;
        }
        const QJsonArray serializedItems =
                object.value(QStringLiteral("items")).toArray();
        if (serializedItems.isEmpty() ||
            serializedItems.size() > kMaximumItems) {
            *error = QStringLiteral(
                    "A drawing has an invalid number of items.");
            return false;
        }
        board.items.reserve(
                static_cast<std::size_t>(serializedItems.size()));
        for (const QJsonValue &itemValue : serializedItems) {
            if (!itemValue.isObject()) {
                *error = QStringLiteral(
                        "A whiteboard item is malformed.");
                return false;
            }
            const QJsonObject itemObject = itemValue.toObject();
            Item item;
            item.id = QUuid::createUuid().toString(
                    QUuid::WithoutBraces);
            item.type =
                    itemObject.value(QStringLiteral("type")).toString();
            double x = 0.0;
            double y = 0.0;
            double width = 0.0;
            double height = 0.0;
            if (!IsKnownTool(item.type) ||
                item.type == QStringLiteral("select") ||
                item.type == QStringLiteral("eraser") ||
                !finiteNumber(itemObject, QStringLiteral("x"), &x) ||
                !finiteNumber(itemObject, QStringLiteral("y"), &y) ||
                !finiteNumber(
                        itemObject, QStringLiteral("width"), &width) ||
                !finiteNumber(
                        itemObject, QStringLiteral("height"), &height) ||
                !finiteNumber(itemObject,
                              QStringLiteral("strokeWidth"),
                              &item.strokeWidth) ||
                !finiteNumber(itemObject,
                              QStringLiteral("fontSize"),
                              &item.fontSize) ||
                x < 0.0 || y < 0.0 || width < kMinimumExtent ||
                height < kMinimumExtent ||
                x + width > 1.000001 ||
                y + height > 1.000001 ||
                item.strokeWidth < kMinimumSize ||
                item.fontSize < 0.001 || item.fontSize > 10.0) {
                *error = QStringLiteral(
                        "A whiteboard item has invalid geometry.");
                return false;
            }
            item.bounds = QRectF(x, y, width, height);
            item.color = QColor(
                    itemObject.value(
                                      QStringLiteral("color"))
                            .toString());
            item.text = NormalizeText(
                    itemObject.value(QStringLiteral("text")).toString());
            if (!item.color.isValid() ||
                (item.type == QStringLiteral("text") &&
                 item.text.isEmpty()) ||
                !itemObject.value(QStringLiteral("points")).isArray() ||
                !itemObject.value(QStringLiteral("erasures")).isArray()) {
                *error = QStringLiteral(
                        "A whiteboard item has invalid styling.");
                return false;
            }
            const QJsonArray points =
                    itemObject.value(QStringLiteral("points")).toArray();
            if (points.size() >
                static_cast<qsizetype>(kMaximumPointsPerItem)) {
                *error = QStringLiteral(
                        "A whiteboard stroke has too many points.");
                return false;
            }
            for (const QJsonValue &pointValue : points) {
                if (!pointValue.isArray()) {
                    *error = QStringLiteral(
                            "A whiteboard point is malformed.");
                    return false;
                }
                const QJsonArray values = pointValue.toArray();
                if (values.size() != 2 ||
                    !values[0].isDouble() ||
                    !values[1].isDouble() ||
                    !IsFinite(values[0].toDouble()) ||
                    !IsFinite(values[1].toDouble()) ||
                    values[0].toDouble() < 0.0 ||
                    values[0].toDouble() > 1.0 ||
                    values[1].toDouble() < 0.0 ||
                    values[1].toDouble() > 1.0) {
                    *error = QStringLiteral(
                            "A whiteboard point is invalid.");
                    return false;
                }
                item.points.emplace_back(
                        values[0].toDouble(),
                        values[1].toDouble());
            }
            if ((item.type == QStringLiteral("line") &&
                 item.points.size() != 2u) ||
                (item.type == QStringLiteral("pen") &&
                 item.points.empty()) ||
                ((item.type == QStringLiteral("rectangle") ||
                  item.type == QStringLiteral("ellipse") ||
                  item.type == QStringLiteral("text")) &&
                 !item.points.empty())) {
                *error = QStringLiteral(
                        "A whiteboard item has inconsistent vector data.");
                return false;
            }
            const QJsonArray erasures = itemObject
                    .value(QStringLiteral("erasures"))
                    .toArray();
            if (erasures.size() >
                static_cast<qsizetype>(kMaximumErasuresPerItem)) {
                *error = QStringLiteral(
                        "A whiteboard item has too many erasures.");
                return false;
            }
            for (const QJsonValue &erasureValue : erasures) {
                if (!erasureValue.isObject()) {
                    *error = QStringLiteral(
                            "A whiteboard erasure is malformed.");
                    return false;
                }
                const QJsonObject erasureObject =
                        erasureValue.toObject();
                Erasure erasure;
                double erasureX = 0.0;
                double erasureY = 0.0;
                if (!finiteNumber(erasureObject,
                                  QStringLiteral("x"),
                                  &erasureX) ||
                    !finiteNumber(erasureObject,
                                  QStringLiteral("y"),
                                  &erasureY) ||
                    !finiteNumber(erasureObject,
                                  QStringLiteral("radius"),
                                  &erasure.radius) ||
                    erasureX < 0.0 || erasureX > 1.0 ||
                    erasureY < 0.0 || erasureY > 1.0 ||
                    erasure.radius < 0.002 ||
                    erasure.radius > 0.2) {
                    *error = QStringLiteral(
                            "A whiteboard erasure is invalid.");
                    return false;
                }
                erasure.point = QPointF(erasureX, erasureY);
                item.erasures.push_back(erasure);
            }
            board.items.push_back(std::move(item));
        }
        parsedBoards.push_back(std::move(board));
    }
    *boards = std::move(parsedBoards);
    error->clear();
    return true;
}

void WhiteboardModel::TranslateItem(Item *item,
                                    const QPointF &delta) {
    item->bounds.translate(delta);
    for (QPointF &point : item->points) {
        point += delta;
    }
    for (Erasure &erasure : item->erasures) {
        erasure.point += delta;
    }
}

void WhiteboardModel::updateDraftBounds(Item *item) {
    if (item == nullptr || item->points.empty()) {
        return;
    }
    double left = item->points.front().x();
    double top = item->points.front().y();
    double right = left;
    double bottom = top;
    for (const QPointF &point : item->points) {
        if (!IsPointFinite(point)) {
            continue;
        }
        left = std::min(left, point.x());
        top = std::min(top, point.y());
        right = std::max(right, point.x());
        bottom = std::max(bottom, point.y());
    }
    const double padding =
            std::clamp(item->strokeWidth * 0.0015, 0.003, 0.03);
    left = std::clamp(left - padding, 0.0, 1.0);
    top = std::clamp(top - padding, 0.0, 1.0);
    right = std::clamp(right + padding, 0.0, 1.0);
    bottom = std::clamp(bottom + padding, 0.0, 1.0);
    item->bounds = QRectF(
            left,
            top,
            std::max(kMinimumExtent, right - left),
            std::max(kMinimumExtent, bottom - top));
    if (item->bounds.right() > 1.0) {
        item->bounds.moveRight(1.0);
    }
    if (item->bounds.bottom() > 1.0) {
        item->bounds.moveBottom(1.0);
    }
}

void WhiteboardModel::removeDraft() {
    if (draftIndex_ >= 0 && draftIndex_ < count()) {
        items_.erase(items_.begin() + draftIndex_);
    }
    drawing_ = false;
    draftIndex_ = -1;
    selectedIndex_ = items_.empty()
            ? -1
            : std::min(selectedIndex_, count() - 1);
    notifyItemsChanged(true);
    emit drawingChanged();
}

void WhiteboardModel::notifyItemsChanged(
        bool selectionMayHaveChanged) {
    emit itemsChanged();
    if (selectionMayHaveChanged) {
        emit selectionChanged();
    }
}

void WhiteboardModel::loadPersistedBoards() {
    const QByteArray data =
            QSettings().value(QString::fromLatin1(
                                      kPersistedBoardsKey))
                    .toByteArray();
    if (data.isEmpty()) {
        return;
    }
    std::vector<Board> restored;
    QString error;
    if (!deserializeBoards(data, &restored, &error)) {
        setOperationMessage(QStringLiteral(
                "Saved whiteboards could not be restored: %1")
                                    .arg(error));
        return;
    }
    boards_ = std::move(restored);
}

bool WhiteboardModel::persistBoards() {
    QSettings settings;
    settings.setValue(
            QString::fromLatin1(kPersistedBoardsKey),
            serializeBoards(
                    boards_, QStringLiteral("Persistent whiteboards")));
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        setOperationMessage(QStringLiteral(
                "Whiteboards could not be persisted."));
        return false;
    }
    return true;
}

void WhiteboardModel::setOperationMessage(
        const QString &value) {
    if (operationMessage_ == value) {
        return;
    }
    operationMessage_ = value;
    emit operationMessageChanged();
}

}  // namespace forevertas::viewer
