#include "app/custom_volume_target_model.h"

#include "viewer/custom_volume_geometry.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineF>
#include <QSet>
#include <QSettings>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <limits>

namespace forevertas::app {
namespace {

constexpr char kCustomVolumesKey[] = "targets/customVolumes";
constexpr int kVersion = 1;
constexpr qsizetype kMaximumTargets = 256;
constexpr qsizetype kMaximumVertices = 256;
constexpr float kMinimumDepth = 0.001F;
constexpr double kMaximumCoordinate = 10000000.0;

QString Number(double value) {
    return QString::number(value, 'g', 15);
}

QString DisplayNumber(double value) {
    QString result = QString::number(value, 'f', 3);
    while (result.contains(QLatin1Char('.')) &&
           result.endsWith(QLatin1Char('0'))) {
        result.chop(1);
    }
    if (result.endsWith(QLatin1Char('.'))) result.chop(1);
    return result == QStringLiteral("-0") ? QStringLiteral("0") : result;
}

double Cross(const QPointF &a, const QPointF &b, const QPointF &c) {
    return (b.x() - a.x()) * (c.y() - a.y()) -
            (b.y() - a.y()) * (c.x() - a.x());
}

bool OnSegment(const QPointF &point,
               const QPointF &from,
               const QPointF &to) {
    if (std::abs(Cross(from, to, point)) > 1e-8) return false;
    return point.x() >= std::min(from.x(), to.x()) - 1e-8 &&
            point.x() <= std::max(from.x(), to.x()) + 1e-8 &&
            point.y() >= std::min(from.y(), to.y()) - 1e-8 &&
            point.y() <= std::max(from.y(), to.y()) + 1e-8;
}

int Orientation(const QPointF &a, const QPointF &b, const QPointF &c) {
    const double value = Cross(a, b, c);
    if (std::abs(value) <= 1e-8) return 0;
    return value > 0.0 ? 1 : -1;
}

bool Intersects(const QPointF &a,
                const QPointF &b,
                const QPointF &c,
                const QPointF &d) {
    const int first = Orientation(a, b, c);
    const int second = Orientation(a, b, d);
    const int third = Orientation(c, d, a);
    const int fourth = Orientation(c, d, b);
    if (first == 0 && OnSegment(c, a, b)) return true;
    if (second == 0 && OnSegment(d, a, b)) return true;
    if (third == 0 && OnSegment(a, c, d)) return true;
    if (fourth == 0 && OnSegment(b, c, d)) return true;
    return first != second && third != fourth;
}

std::unique_ptr<viewer::CustomVolumeGeometry> NewGeometry() {
    return std::make_unique<viewer::CustomVolumeGeometry>();
}

}  // namespace

CustomVolumeTargetModel::CustomVolumeTargetModel(
        const QVariantMap &legacySettings,
        QObject *parent)
    : QObject(parent) {
    load(legacySettings);
}

CustomVolumeTargetModel::~CustomVolumeTargetModel() = default;

QVariantList CustomVolumeTargetModel::targets() const {
    QVariantList result;
    result.reserve(count());
    for (int index = 0; index < count(); ++index) {
        result.push_back(toVariantMap(
                targets_[static_cast<std::size_t>(index)],
                index == selectedIndex_));
    }
    return result;
}

int CustomVolumeTargetModel::count() const {
    return static_cast<int>(targets_.size());
}

int CustomVolumeTargetModel::maximumCount() const {
    return static_cast<int>(kMaximumTargets);
}

int CustomVolumeTargetModel::selectedIndex() const {
    return selectedIndex_;
}

QVariantMap CustomVolumeTargetModel::selectedTarget() const {
    if (selectedIndex_ < 0 || selectedIndex_ >= count()) return {};
    return toVariantMap(
            targets_[static_cast<std::size_t>(selectedIndex_)], true);
}

bool CustomVolumeTargetModel::editingEnabled() const {
    return editingEnabled_;
}

bool CustomVolumeTargetModel::drawing() const {
    return drawing_;
}

int CustomVolumeTargetModel::addTarget(const QString &plane,
                                       double originX,
                                       double originY,
                                       double originZ) {
    const QVector3D origin(
            static_cast<float>(originX),
            static_cast<float>(originY),
            static_cast<float>(originZ));
    if (!editingEnabled_ || drawing_ || !validPlane(plane) ||
        !std::isfinite(origin.x()) || !std::isfinite(origin.y()) ||
        !std::isfinite(origin.z()) ||
        std::abs(origin.x()) > kMaximumCoordinate ||
        std::abs(origin.y()) > kMaximumCoordinate ||
        std::abs(origin.z()) > kMaximumCoordinate ||
        targets_.size() >=
                static_cast<std::size_t>(kMaximumTargets)) {
        return -1;
    }
    Target target{
            QUuid::createUuid().toString(QUuid::WithoutBraces),
            nextDefaultName(),
            plane,
            origin,
            5.0F,
            {QPointF(-5.0, -5.0),
             QPointF(5.0, -5.0),
             QPointF(0.0, 5.0)},
            NewGeometry(),
            NewGeometry()};
    rebuildGeometry(&target);
    targets_.push_back(std::move(target));
    selectedIndex_ = count() - 1;
    persist();
    emit targetsChanged();
    emit selectedTargetChanged();
    return selectedIndex_;
}

int CustomVolumeTargetModel::duplicateSelected() {
    if (!editingEnabled_ || drawing_ || selectedIndex_ < 0 ||
        selectedIndex_ >= count() ||
        targets_.size() >= static_cast<std::size_t>(kMaximumTargets)) {
        return -1;
    }
    const Target &source =
            targets_[static_cast<std::size_t>(selectedIndex_)];
    QVector3D duplicateOrigin = source.origin;
    duplicateOrigin.setX(
            source.origin.x() + 1.0F <= kMaximumCoordinate
                    ? source.origin.x() + 1.0F
                    : source.origin.x() - 1.0F);
    duplicateOrigin.setZ(
            source.origin.z() + 1.0F <= kMaximumCoordinate
                    ? source.origin.z() + 1.0F
                    : source.origin.z() - 1.0F);
    Target target{
            QUuid::createUuid().toString(QUuid::WithoutBraces),
            nextDefaultName(),
            source.plane,
            duplicateOrigin,
            source.depth,
            source.vertices,
            NewGeometry(),
            NewGeometry()};
    rebuildGeometry(&target);
    targets_.push_back(std::move(target));
    selectedIndex_ = count() - 1;
    persist();
    emit targetsChanged();
    emit selectedTargetChanged();
    return selectedIndex_;
}

bool CustomVolumeTargetModel::removeTarget(int index) {
    if (!editingEnabled_ || drawing_ || index < 0 || index >= count() ||
        count() <= 1) {
        return false;
    }
    const int previous = selectedIndex_;
    targets_.erase(targets_.begin() + index);
    if (selectedIndex_ > index) --selectedIndex_;
    else if (selectedIndex_ == index) {
        selectedIndex_ = std::min(index, count() - 1);
    }
    persist();
    emit targetsChanged();
    if (previous != selectedIndex_ || previous == index) {
        emit selectedTargetChanged();
    }
    return true;
}

bool CustomVolumeTargetModel::selectTarget(int index) {
    if (!editingEnabled_ || drawing_ || index < 0 || index >= count() ||
        selectedIndex_ == index) {
        return false;
    }
    selectedIndex_ = index;
    persist();
    emit targetsChanged();
    emit selectedTargetChanged();
    return true;
}

bool CustomVolumeTargetModel::setName(int index, const QString &name) {
    if (!editingEnabled_ || drawing_ || index < 0 || index >= count()) {
        return false;
    }
    const QString normalized = name.trimmed().left(80);
    Target &target = targets_[static_cast<std::size_t>(index)];
    if (normalized.isEmpty() || target.name == normalized) return false;
    target.name = normalized;
    persist();
    notifyTargetChanged(index);
    return true;
}

bool CustomVolumeTargetModel::setPlane(int index, const QString &plane) {
    if (!editingEnabled_ || drawing_ || index < 0 || index >= count() ||
        !validPlane(plane)) {
        return false;
    }
    Target &target = targets_[static_cast<std::size_t>(index)];
    if (target.plane == plane) return false;
    target.plane = plane;
    rebuildGeometry(&target);
    persist();
    notifyTargetChanged(index);
    return true;
}

bool CustomVolumeTargetModel::setOriginComponent(
        int index,
        const QString &axis,
        const QString &value) {
    double parsed = 0.0;
    const int component = axisIndex(axis);
    if (!editingEnabled_ || drawing_ || index < 0 || index >= count() ||
        component < 0 || !parseFinite(value, &parsed)) {
        return false;
    }
    Target &target = targets_[static_cast<std::size_t>(index)];
    if (qFuzzyCompare(
                static_cast<double>(target.origin[component]) + 1.0,
                parsed + 1.0)) {
        return false;
    }
    target.origin[component] = static_cast<float>(parsed);
    rebuildGeometry(&target);
    persist();
    notifyTargetChanged(index);
    return true;
}

bool CustomVolumeTargetModel::setDepth(int index,
                                       const QString &value) {
    double parsed = 0.0;
    if (!editingEnabled_ || drawing_ || index < 0 || index >= count() ||
        !parseFinite(value, &parsed) || parsed < kMinimumDepth) {
        return false;
    }
    Target &target = targets_[static_cast<std::size_t>(index)];
    if (qFuzzyCompare(
                static_cast<double>(target.depth) + 1.0, parsed + 1.0)) {
        return false;
    }
    target.depth = static_cast<float>(parsed);
    rebuildGeometry(&target);
    persist();
    notifyTargetChanged(index);
    return true;
}

bool CustomVolumeTargetModel::setPolygon(
        int index,
        const QString &encoded) {
    if (!editingEnabled_ || drawing_ || index < 0 || index >= count()) {
        return false;
    }
    std::vector<QPointF> vertices = decodePolygon(encoded);
    if (!validPolygon(vertices)) return false;
    Target &target = targets_[static_cast<std::size_t>(index)];
    if (encodePolygon(target.vertices) == encodePolygon(vertices)) {
        return false;
    }
    target.vertices = std::move(vertices);
    rebuildGeometry(&target);
    persist();
    notifyTargetChanged(index);
    return true;
}

bool CustomVolumeTargetModel::setVertex(
        int index,
        int vertexIndex,
        const QString &axis,
        const QString &value) {
    double parsed = 0.0;
    if (!editingEnabled_ || drawing_ || index < 0 || index >= count() ||
        vertexIndex < 0 ||
        vertexIndex >= static_cast<int>(
                               targets_[static_cast<std::size_t>(index)]
                                       .vertices.size()) ||
        !parseFinite(value, &parsed) ||
        (axis != QStringLiteral("u") && axis != QStringLiteral("v"))) {
        return false;
    }
    Target &target = targets_[static_cast<std::size_t>(index)];
    std::vector<QPointF> proposed = target.vertices;
    QPointF &point = proposed[static_cast<std::size_t>(vertexIndex)];
    if (axis == QStringLiteral("u")) point.setX(parsed);
    else point.setY(parsed);
    if (!validPolygon(proposed)) return false;
    target.vertices = std::move(proposed);
    rebuildGeometry(&target);
    persist();
    notifyTargetChanged(index);
    return true;
}

bool CustomVolumeTargetModel::setVertexWorld(
        int vertexIndex,
        double x,
        double y,
        double z) {
    if (!editingEnabled_ || drawing_ || selectedIndex_ < 0 ||
        selectedIndex_ >= count() || vertexIndex < 0) {
        return false;
    }
    const QVector3D world(
            static_cast<float>(x),
            static_cast<float>(y),
            static_cast<float>(z));
    if (!std::isfinite(world.x()) || !std::isfinite(world.y()) ||
        !std::isfinite(world.z()) ||
        std::abs(world.x()) > kMaximumCoordinate ||
        std::abs(world.y()) > kMaximumCoordinate ||
        std::abs(world.z()) > kMaximumCoordinate) {
        return false;
    }
    Target &target =
            targets_[static_cast<std::size_t>(selectedIndex_)];
    if (vertexIndex >= static_cast<int>(target.vertices.size())) return false;
    std::vector<QPointF> proposed = target.vertices;
    proposed[static_cast<std::size_t>(vertexIndex)] =
            planePoint(target, world);
    if (!validPolygon(proposed)) return false;
    target.vertices = std::move(proposed);
    rebuildGeometry(&target);
    persist();
    notifyTargetChanged(selectedIndex_);
    return true;
}

bool CustomVolumeTargetModel::addVertexWorld(
        double x,
        double y,
        double z) {
    if (!editingEnabled_ || !drawing_ || selectedIndex_ < 0 ||
        selectedIndex_ >= count()) {
        return false;
    }
    Target &target =
            targets_[static_cast<std::size_t>(selectedIndex_)];
    if (target.vertices.size() >=
        static_cast<std::size_t>(kMaximumVertices)) {
        return false;
    }
    const QVector3D world(
            static_cast<float>(x),
            static_cast<float>(y),
            static_cast<float>(z));
    if (!std::isfinite(world.x()) || !std::isfinite(world.y()) ||
        !std::isfinite(world.z()) ||
        std::abs(world.x()) > kMaximumCoordinate ||
        std::abs(world.y()) > kMaximumCoordinate ||
        std::abs(world.z()) > kMaximumCoordinate) {
        return false;
    }
    const QPointF point = planePoint(target, world);
    if (!target.vertices.empty() &&
        QLineF(target.vertices.back(), point).length() <= 0.001) {
        return false;
    }
    target.vertices.push_back(point);
    rebuildGeometry(&target);
    notifyTargetChanged(selectedIndex_);
    return true;
}

bool CustomVolumeTargetModel::removeVertex(int index, int vertexIndex) {
    if (!editingEnabled_ || drawing_ || index < 0 || index >= count()) {
        return false;
    }
    Target &target = targets_[static_cast<std::size_t>(index)];
    if (target.vertices.size() <= 3u || vertexIndex < 0 ||
        vertexIndex >= static_cast<int>(target.vertices.size())) {
        return false;
    }
    std::vector<QPointF> proposed = target.vertices;
    proposed.erase(proposed.begin() + vertexIndex);
    if (!validPolygon(proposed)) return false;
    target.vertices = std::move(proposed);
    rebuildGeometry(&target);
    persist();
    notifyTargetChanged(index);
    return true;
}

bool CustomVolumeTargetModel::translateSelected(
        double x,
        double y,
        double z) {
    if (!editingEnabled_ || drawing_ || selectedIndex_ < 0 ||
        selectedIndex_ >= count()) {
        return false;
    }
    const QVector3D delta(
            static_cast<float>(x),
            static_cast<float>(y),
            static_cast<float>(z));
    Target &target =
            targets_[static_cast<std::size_t>(selectedIndex_)];
    const QVector3D proposed = target.origin + delta;
    if (!std::isfinite(proposed.x()) || !std::isfinite(proposed.y()) ||
        !std::isfinite(proposed.z()) ||
        std::abs(proposed.x()) > kMaximumCoordinate ||
        std::abs(proposed.y()) > kMaximumCoordinate ||
        std::abs(proposed.z()) > kMaximumCoordinate ||
        delta.isNull()) {
        return false;
    }
    target.origin = proposed;
    rebuildGeometry(&target);
    persist();
    notifyTargetChanged(selectedIndex_);
    return true;
}

bool CustomVolumeTargetModel::moveSelectedTo(
        double x,
        double y,
        double z) {
    if (!editingEnabled_ || drawing_ || selectedIndex_ < 0 ||
        selectedIndex_ >= count()) {
        return false;
    }
    const QVector3D origin(
            static_cast<float>(x),
            static_cast<float>(y),
            static_cast<float>(z));
    if (!std::isfinite(origin.x()) || !std::isfinite(origin.y()) ||
        !std::isfinite(origin.z()) ||
        std::abs(origin.x()) > kMaximumCoordinate ||
        std::abs(origin.y()) > kMaximumCoordinate ||
        std::abs(origin.z()) > kMaximumCoordinate) {
        return false;
    }
    Target &target = targets_[static_cast<std::size_t>(selectedIndex_)];
    if (target.origin == origin) return false;
    target.origin = origin;
    rebuildGeometry(&target);
    persist();
    notifyTargetChanged(selectedIndex_);
    return true;
}

bool CustomVolumeTargetModel::resizeDepthSelected(double delta) {
    if (!editingEnabled_ || drawing_ || !std::isfinite(delta) ||
        selectedIndex_ < 0 || selectedIndex_ >= count()) {
        return false;
    }
    Target &target =
            targets_[static_cast<std::size_t>(selectedIndex_)];
    const double proposed = static_cast<double>(target.depth) + delta;
    if (proposed < kMinimumDepth ||
        proposed > kMaximumCoordinate) {
        return false;
    }
    target.depth = static_cast<float>(proposed);
    rebuildGeometry(&target);
    persist();
    notifyTargetChanged(selectedIndex_);
    return true;
}

bool CustomVolumeTargetModel::beginDrawing() {
    if (!editingEnabled_ || drawing_ || selectedIndex_ < 0 ||
        selectedIndex_ >= count()) {
        return false;
    }
    Target &target =
            targets_[static_cast<std::size_t>(selectedIndex_)];
    drawingBackup_ = target.vertices;
    target.vertices.clear();
    drawing_ = true;
    rebuildGeometry(&target);
    emit drawingChanged();
    notifyTargetChanged(selectedIndex_);
    return true;
}

bool CustomVolumeTargetModel::finishDrawing() {
    if (!drawing_ || selectedIndex_ < 0 || selectedIndex_ >= count()) {
        return false;
    }
    Target &target =
            targets_[static_cast<std::size_t>(selectedIndex_)];
    if (!validPolygon(target.vertices)) return false;
    drawingBackup_.clear();
    drawing_ = false;
    persist();
    emit drawingChanged();
    notifyTargetChanged(selectedIndex_);
    return true;
}

void CustomVolumeTargetModel::cancelDrawing() {
    if (!drawing_ || selectedIndex_ < 0 || selectedIndex_ >= count()) {
        return;
    }
    Target &target =
            targets_[static_cast<std::size_t>(selectedIndex_)];
    target.vertices = std::move(drawingBackup_);
    drawing_ = false;
    rebuildGeometry(&target);
    emit drawingChanged();
    notifyTargetChanged(selectedIndex_);
}

void CustomVolumeTargetModel::setEditingEnabled(bool enabled) {
    if (editingEnabled_ == enabled) return;
    if (!enabled) cancelDrawing();
    editingEnabled_ = enabled;
    emit editingEnabledChanged();
}

QVariantMap CustomVolumeTargetModel::toVariantMap(
        const Target &target,
        bool selected) const {
    QVariantList vertices;
    vertices.reserve(static_cast<qsizetype>(target.vertices.size()));
    QVector3D centroid;
    for (std::size_t index = 0u; index < target.vertices.size(); ++index) {
        const QPointF &point = target.vertices[index];
        const QVector3D world = worldPoint(target, point, 0.0F);
        centroid += world;
        vertices.push_back(QVariantMap{
                {QStringLiteral("index"), static_cast<int>(index)},
                {QStringLiteral("u"), DisplayNumber(point.x())},
                {QStringLiteral("v"), DisplayNumber(point.y())},
                {QStringLiteral("world"), world}});
    }
    if (!target.vertices.empty()) {
        centroid /= static_cast<float>(target.vertices.size());
    } else {
        centroid = target.origin;
    }
    const QVector3D depthHandle = worldPoint(
            target,
            planePoint(target, centroid),
            target.depth);
    float extent = target.depth;
    for (const QPointF &point : target.vertices) {
        extent = std::max(
                extent,
                static_cast<float>(
                        std::hypot(point.x(), point.y()) * 2.0));
    }
    const QVector3D focusCenter =
            (centroid + depthHandle) * 0.5F;
    return QVariantMap{
            {QStringLiteral("id"), target.id},
            {QStringLiteral("name"), target.name},
            {QStringLiteral("kind"), QStringLiteral("custom")},
            {QStringLiteral("plane"), target.plane},
            {QStringLiteral("origin"), target.origin},
            {QStringLiteral("originX"), Number(target.origin.x())},
            {QStringLiteral("originY"), Number(target.origin.y())},
            {QStringLiteral("originZ"), Number(target.origin.z())},
            {QStringLiteral("depth"), Number(target.depth)},
            {QStringLiteral("polygon"), encodePolygon(target.vertices)},
            {QStringLiteral("vertices"), vertices},
            {QStringLiteral("vertexCount"),
             static_cast<int>(target.vertices.size())},
            {QStringLiteral("centroid"), centroid},
            {QStringLiteral("depthHandle"), depthHandle},
            {QStringLiteral("focusCenter"), focusCenter},
            {QStringLiteral("focusSize"),
             QVector3D(extent, extent, extent)},
            {QStringLiteral("geometry"),
             QVariant::fromValue(
                     static_cast<QObject *>(target.geometry.get()))},
            {QStringLiteral("selected"), selected},
            {QStringLiteral("valid"), validPolygon(target.vertices)}};
}

bool CustomVolumeTargetModel::parseFinite(
        const QString &value,
        double *result) {
    bool okay = false;
    const double parsed = value.toDouble(&okay);
    if (!okay || !std::isfinite(parsed) ||
        std::abs(parsed) > kMaximumCoordinate) {
        return false;
    }
    *result = parsed;
    return true;
}

int CustomVolumeTargetModel::axisIndex(const QString &axis) {
    if (axis == QStringLiteral("x")) return 0;
    if (axis == QStringLiteral("y")) return 1;
    if (axis == QStringLiteral("z")) return 2;
    return -1;
}

bool CustomVolumeTargetModel::validPlane(const QString &plane) {
    return plane == QStringLiteral("xy") ||
            plane == QStringLiteral("xz") ||
            plane == QStringLiteral("yz");
}

bool CustomVolumeTargetModel::validPolygon(
        const std::vector<QPointF> &vertices) {
    if (vertices.size() < 3u ||
        vertices.size() >
                static_cast<std::size_t>(kMaximumVertices)) {
        return false;
    }
    double area = 0.0;
    for (std::size_t index = 0u; index < vertices.size(); ++index) {
        const QPointF &a = vertices[index];
        const QPointF &b = vertices[(index + 1u) % vertices.size()];
        if (!std::isfinite(a.x()) || !std::isfinite(a.y()) ||
            std::abs(a.x()) > kMaximumCoordinate ||
            std::abs(a.y()) > kMaximumCoordinate ||
            QLineF(a, b).length() <= 1e-8) {
            return false;
        }
        const QPointF &previous = vertices[
                (index + vertices.size() - 1u) % vertices.size()];
        if (std::abs(Cross(previous, a, b)) <= 1e-8) return false;
        area += a.x() * b.y() - a.y() * b.x();
        for (std::size_t other = index + 1u;
             other < vertices.size();
             ++other) {
            if (other == index ||
                other == (index + 1u) % vertices.size() ||
                (other + 1u) % vertices.size() == index) {
                continue;
            }
            if (Intersects(
                        a,
                        b,
                        vertices[other],
                        vertices[(other + 1u) % vertices.size()])) {
                return false;
            }
        }
    }
    return std::abs(area) > 1e-8;
}

QVector3D CustomVolumeTargetModel::worldPoint(
        const Target &target,
        const QPointF &point,
        float normal) {
    if (target.plane == QStringLiteral("xy")) {
        return target.origin + QVector3D(
                static_cast<float>(point.x()),
                static_cast<float>(point.y()),
                normal);
    }
    if (target.plane == QStringLiteral("yz")) {
        return target.origin + QVector3D(
                normal,
                static_cast<float>(point.x()),
                static_cast<float>(point.y()));
    }
    return target.origin + QVector3D(
            static_cast<float>(point.x()),
            normal,
            static_cast<float>(point.y()));
}

QPointF CustomVolumeTargetModel::planePoint(
        const Target &target,
        const QVector3D &point) {
    const QVector3D relative = point - target.origin;
    if (target.plane == QStringLiteral("xy")) {
        return QPointF(relative.x(), relative.y());
    }
    if (target.plane == QStringLiteral("yz")) {
        return QPointF(relative.y(), relative.z());
    }
    return QPointF(relative.x(), relative.z());
}

QString CustomVolumeTargetModel::encodePolygon(
        const std::vector<QPointF> &vertices) {
    QStringList points;
    points.reserve(static_cast<qsizetype>(vertices.size()));
    for (const QPointF &point : vertices) {
        points.push_back(
                Number(point.x()) + QLatin1Char(',') + Number(point.y()));
    }
    return points.join(QLatin1Char(';'));
}

std::vector<QPointF> CustomVolumeTargetModel::decodePolygon(
        const QString &encoded) {
    std::vector<QPointF> vertices;
    const QStringList points = encoded.split(
            QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString &point : points) {
        const QStringList components = point.split(QLatin1Char(','));
        if (components.size() != 2) return {};
        double u = 0.0;
        double v = 0.0;
        if (!parseFinite(components[0], &u) ||
            !parseFinite(components[1], &v)) {
            return {};
        }
        vertices.emplace_back(u, v);
    }
    return vertices;
}

QString CustomVolumeTargetModel::nextDefaultName() const {
    int suffix = 1;
    for (;;) {
        const QString candidate =
                QStringLiteral("Custom volume %1").arg(suffix++);
        const bool exists = std::any_of(
                targets_.cbegin(),
                targets_.cend(),
                [&candidate](const Target &target) {
                    return target.name.compare(
                                   candidate, Qt::CaseInsensitive) == 0;
                });
        if (!exists) return candidate;
    }
}

void CustomVolumeTargetModel::rebuildGeometry(Target *target) {
    target->stagingGeometry->setVolume(
            target->plane,
            target->origin,
            target->depth,
            target->vertices);
    std::swap(target->geometry, target->stagingGeometry);
}

void CustomVolumeTargetModel::load(const QVariantMap &legacySettings) {
    const QJsonDocument document = QJsonDocument::fromJson(
            QSettings().value(QLatin1String(kCustomVolumesKey)).toByteArray());
    QString selectedId;
    QSet<QString> loadedIds;
    int skippedTargets = 0;
    if (document.isObject()) {
        const QJsonObject root = document.object();
        if (root.value(QStringLiteral("version")).toInt() == kVersion) {
            selectedId = root.value(QStringLiteral("selectedId")).toString();
            const QJsonArray array =
                    root.value(QStringLiteral("targets")).toArray();
            const qsizetype limit =
                    std::min(array.size(), kMaximumTargets);
            for (qsizetype index = 0; index < limit; ++index) {
                const QJsonObject object = array.at(index).toObject();
                const QJsonArray originArray =
                        object.value(QStringLiteral("origin")).toArray();
                const QJsonArray vertexArray =
                        object.value(QStringLiteral("vertices")).toArray();
                if (vertexArray.size() < 3 ||
                    vertexArray.size() > kMaximumVertices) {
                    ++skippedTargets;
                    continue;
                }
                std::vector<QPointF> vertices;
                vertices.reserve(
                        static_cast<std::size_t>(vertexArray.size()));
                for (const QJsonValue &value : vertexArray) {
                    const QJsonArray pair = value.toArray();
                    if (pair.size() != 2) {
                        vertices.clear();
                        break;
                    }
                    vertices.emplace_back(
                            pair[0].toDouble(
                                    std::numeric_limits<double>::quiet_NaN()),
                            pair[1].toDouble(
                                    std::numeric_limits<double>::quiet_NaN()));
                }
                const QString id = object.value(QStringLiteral("id"))
                                           .toString()
                                           .trimmed();
                const QString name = object.value(QStringLiteral("name"))
                                             .toString()
                                             .trimmed()
                                             .left(80);
                const QString plane =
                        object.value(QStringLiteral("plane")).toString();
                const double depth =
                        object.value(QStringLiteral("depth")).toDouble(-1.0);
                if (id.isEmpty() || loadedIds.contains(id) ||
                    name.isEmpty() ||
                    originArray.size() != 3 || !validPlane(plane) ||
                    !std::isfinite(depth) || depth < kMinimumDepth ||
                    depth > kMaximumCoordinate ||
                    !validPolygon(vertices)) {
                    ++skippedTargets;
                    continue;
                }
                const QVector3D origin(
                        static_cast<float>(originArray[0].toDouble(
                                std::numeric_limits<double>::quiet_NaN())),
                        static_cast<float>(originArray[1].toDouble(
                                std::numeric_limits<double>::quiet_NaN())),
                        static_cast<float>(originArray[2].toDouble(
                                std::numeric_limits<double>::quiet_NaN())));
                if (!std::isfinite(origin.x()) ||
                    !std::isfinite(origin.y()) ||
                    !std::isfinite(origin.z()) ||
                    std::abs(origin.x()) > kMaximumCoordinate ||
                    std::abs(origin.y()) > kMaximumCoordinate ||
                    std::abs(origin.z()) > kMaximumCoordinate) {
                    ++skippedTargets;
                    continue;
                }
                Target target{
                        id,
                        name,
                        plane,
                        origin,
                        static_cast<float>(depth),
                        std::move(vertices),
                        NewGeometry(),
                        NewGeometry()};
                rebuildGeometry(&target);
                targets_.push_back(std::move(target));
                loadedIds.insert(id);
            }
        }
    }
    if (skippedTargets > 0) {
        qWarning("ForeverTAS: dropped %d invalid persisted custom "
                 "volume(s)",
                 skippedTargets);
    }
    if (targets_.empty()) {
        const QString plane =
                legacySettings.value(QStringLiteral("plane"), "xz")
                        .toString();
        const std::vector<QPointF> polygon = decodePolygon(
                legacySettings
                        .value(QStringLiteral("polygon"),
                               QStringLiteral("-5,-5;5,-5;0,5"))
                        .toString());
        const auto number = [&legacySettings](
                                    const QString &key,
                                    double fallback) {
            double value = 0.0;
            return parseFinite(legacySettings.value(key).toString(), &value)
                    ? value
                    : fallback;
        };
        Target target{
                QUuid::createUuid().toString(QUuid::WithoutBraces),
                QStringLiteral("Custom volume 1"),
                validPlane(plane) ? plane : QStringLiteral("xz"),
                QVector3D(
                        static_cast<float>(number(
                                QStringLiteral("originX"), 0.0)),
                        static_cast<float>(number(
                                QStringLiteral("originY"), 0.0)),
                        static_cast<float>(number(
                                QStringLiteral("originZ"), 0.0))),
                static_cast<float>(std::clamp(
                        number(QStringLiteral("depth"), 5.0),
                        static_cast<double>(kMinimumDepth),
                        kMaximumCoordinate)),
                validPolygon(polygon)
                        ? polygon
                        : std::vector<QPointF>{
                                  QPointF(-5.0, -5.0),
                                  QPointF(5.0, -5.0),
                                  QPointF(0.0, 5.0)},
                NewGeometry(),
                NewGeometry()};
        rebuildGeometry(&target);
        targets_.push_back(std::move(target));
    }
    const auto selected = std::find_if(
            targets_.cbegin(),
            targets_.cend(),
            [&selectedId](const Target &target) {
                return target.id == selectedId;
            });
    selectedIndex_ = selected == targets_.cend()
            ? 0
            : static_cast<int>(selected - targets_.cbegin());
    persist();
}

void CustomVolumeTargetModel::persist() const {
    QJsonArray values;
    for (const Target &target : targets_) {
        QJsonArray vertices;
        for (const QPointF &point : target.vertices) {
            vertices.push_back(QJsonArray{point.x(), point.y()});
        }
        values.push_back(QJsonObject{
                {QStringLiteral("id"), target.id},
                {QStringLiteral("name"), target.name},
                {QStringLiteral("plane"), target.plane},
                {QStringLiteral("origin"),
                 QJsonArray{
                         target.origin.x(),
                         target.origin.y(),
                         target.origin.z()}},
                {QStringLiteral("depth"), target.depth},
                {QStringLiteral("vertices"), vertices}});
    }
    const QString selectedId =
            selectedIndex_ >= 0
                    && selectedIndex_ < static_cast<int>(targets_.size())
            ? targets_[static_cast<std::size_t>(selectedIndex_)].id
            : QString{};
    QSettings().setValue(
            QLatin1String(kCustomVolumesKey),
            QJsonDocument(QJsonObject{
                    {QStringLiteral("version"), kVersion},
                    {QStringLiteral("selectedId"), selectedId},
                    {QStringLiteral("targets"), values}})
                    .toJson(QJsonDocument::Compact));
}

void CustomVolumeTargetModel::notifyTargetChanged(int index) {
    emit targetsChanged();
    if (index == selectedIndex_) emit selectedTargetChanged();
}

}  // namespace forevertas::app
