#include "viewer/custom_volume_geometry.h"

#include <QByteArray>

#include <algorithm>
#include <array>
#include <cstring>
#include <numeric>

namespace forevertas::viewer {
namespace {

float Cross(const QPointF &a, const QPointF &b, const QPointF &c) {
    return static_cast<float>(
            (b.x() - a.x()) * (c.y() - a.y()) -
            (b.y() - a.y()) * (c.x() - a.x()));
}

bool PointInTriangle(const QPointF &point,
                     const QPointF &a,
                     const QPointF &b,
                     const QPointF &c) {
    const float first = Cross(a, b, point);
    const float second = Cross(b, c, point);
    const float third = Cross(c, a, point);
    const bool negative = first < 0.0F || second < 0.0F || third < 0.0F;
    const bool positive = first > 0.0F || second > 0.0F || third > 0.0F;
    return !(negative && positive);
}

std::vector<std::array<int, 3>> Triangulate(
        const std::vector<QPointF> &vertices) {
    std::vector<std::array<int, 3>> triangles;
    if (vertices.size() < 3u) return triangles;
    double area = 0.0;
    for (std::size_t index = 0u; index < vertices.size(); ++index) {
        const QPointF &a = vertices[index];
        const QPointF &b = vertices[(index + 1u) % vertices.size()];
        area += a.x() * b.y() - a.y() * b.x();
    }
    std::vector<int> remaining(vertices.size());
    std::iota(remaining.begin(), remaining.end(), 0);
    if (area < 0.0) std::reverse(remaining.begin(), remaining.end());
    while (remaining.size() > 3u) {
        bool clipped = false;
        for (std::size_t index = 0u; index < remaining.size(); ++index) {
            const int previous = remaining[
                    (index + remaining.size() - 1u) % remaining.size()];
            const int current = remaining[index];
            const int next = remaining[(index + 1u) % remaining.size()];
            if (Cross(vertices[previous],
                      vertices[current],
                      vertices[next]) <= 1e-7F) {
                continue;
            }
            bool contains = false;
            for (const int candidate : remaining) {
                if (candidate == previous || candidate == current ||
                    candidate == next) {
                    continue;
                }
                contains |= PointInTriangle(
                        vertices[candidate],
                        vertices[previous],
                        vertices[current],
                        vertices[next]);
            }
            if (contains) continue;
            triangles.push_back({previous, current, next});
            remaining.erase(remaining.begin() +
                            static_cast<std::ptrdiff_t>(index));
            clipped = true;
            break;
        }
        if (!clipped) return {};
    }
    triangles.push_back(
            {remaining[0], remaining[1], remaining[2]});
    return triangles;
}

QVector3D World(const QString &plane,
                const QVector3D &origin,
                const QPointF &point,
                float normal) {
    if (plane == QStringLiteral("xy")) {
        return origin + QVector3D(
                static_cast<float>(point.x()),
                static_cast<float>(point.y()),
                normal);
    }
    if (plane == QStringLiteral("yz")) {
        return origin + QVector3D(
                normal,
                static_cast<float>(point.x()),
                static_cast<float>(point.y()));
    }
    return origin + QVector3D(
            static_cast<float>(point.x()),
            normal,
            static_cast<float>(point.y()));
}

}  // namespace

CustomVolumeGeometry::CustomVolumeGeometry()
    : QQuick3DGeometry(nullptr) {}

void CustomVolumeGeometry::setVolume(
        const QString &plane,
        const QVector3D &origin,
        float depth,
        const std::vector<QPointF> &vertices) {
    clear();
    if (vertices.size() < 3u) {
        setVertexData({});
        setBounds(origin, origin);
        return;
    }
    const std::vector<std::array<int, 3>> capTriangles =
            Triangulate(vertices);
    if (capTriangles.empty()) {
        setVertexData({});
        setBounds(origin, origin);
        return;
    }
    std::vector<QVector3D> positions;
    positions.reserve(capTriangles.size() * 6u + vertices.size() * 6u);
    const auto add = [&positions](
                             const QVector3D &a,
                             const QVector3D &b,
                             const QVector3D &c) {
        positions.push_back(a);
        positions.push_back(b);
        positions.push_back(c);
    };
    for (const auto &triangle : capTriangles) {
        const QVector3D a = World(
                plane, origin, vertices[triangle[0]], 0.0F);
        const QVector3D b = World(
                plane, origin, vertices[triangle[1]], 0.0F);
        const QVector3D c = World(
                plane, origin, vertices[triangle[2]], 0.0F);
        add(c, b, a);
        add(World(plane, origin, vertices[triangle[0]], depth),
            World(plane, origin, vertices[triangle[1]], depth),
            World(plane, origin, vertices[triangle[2]], depth));
    }
    for (std::size_t index = 0u; index < vertices.size(); ++index) {
        const std::size_t next = (index + 1u) % vertices.size();
        const QVector3D a = World(plane, origin, vertices[index], 0.0F);
        const QVector3D b = World(plane, origin, vertices[next], 0.0F);
        const QVector3D c = World(plane, origin, vertices[next], depth);
        const QVector3D d = World(plane, origin, vertices[index], depth);
        add(a, b, c);
        add(a, c, d);
    }

    QVector3D minimum = positions.front();
    QVector3D maximum = positions.front();
    QByteArray bytes(
            static_cast<qsizetype>(
                    positions.size() * 3u * sizeof(float)),
            Qt::Uninitialized);
    char *output = bytes.data();
    const auto write = [&output](float value) {
        std::memcpy(output, &value, sizeof(value));
        output += sizeof(value);
    };
    for (const QVector3D &position : positions) {
        write(position.x());
        write(position.y());
        write(position.z());
        minimum.setX(std::min(minimum.x(), position.x()));
        minimum.setY(std::min(minimum.y(), position.y()));
        minimum.setZ(std::min(minimum.z(), position.z()));
        maximum.setX(std::max(maximum.x(), position.x()));
        maximum.setY(std::max(maximum.y(), position.y()));
        maximum.setZ(std::max(maximum.z(), position.z()));
    }
    setPrimitiveType(PrimitiveType::Triangles);
    setStride(3 * static_cast<int>(sizeof(float)));
    setVertexData(bytes);
    setBounds(minimum, maximum);
    addAttribute(Attribute::PositionSemantic, 0, Attribute::F32Type);
}

QSSGRenderGraphObject *CustomVolumeGeometry::updateSpatialNode(QSSGRenderGraphObject *node) {
    if (node == nullptr) setVertexData(vertexData());
    return QQuick3DGeometry::updateSpatialNode(node);
}

}  // namespace forevertas::viewer
