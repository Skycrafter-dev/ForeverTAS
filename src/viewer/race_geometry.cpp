#include "viewer/race_geometry.h"

namespace forevertas::viewer {

RaceGeometry::RaceGeometry()
    : QQuick3DGeometry(nullptr) {}

void RaceGeometry::setMesh(QByteArray vertexData,
                           int stride,
                           PrimitiveType primitiveType,
                           bool hasVertexColors,
                           const QVector3D &boundsMin,
                           const QVector3D &boundsMax) {
    clear();
    setPrimitiveType(primitiveType);
    setStride(stride);
    setVertexData(vertexData);
    setBounds(boundsMin, boundsMax);
    addAttribute(Attribute::PositionSemantic, 0, Attribute::F32Type);
    if (hasVertexColors) {
        addAttribute(Attribute::ColorSemantic,
                     static_cast<int>(3u * sizeof(float)),
                     Attribute::F32Type);
    }
    update();
}

void RaceGeometry::setIndexedMesh(
        QByteArray vertexData,
        QByteArray indexData,
        int stride,
        bool hasNormals,
        bool hasTangents,
        bool hasUv0,
        bool hasUv1,
        bool hasVertexColors,
        const QVector3D &boundsMin,
        const QVector3D &boundsMax,
        const std::vector<std::pair<int, int>> &subsets) {
    constexpr int PositionOffset = 0;
    constexpr int NormalOffset = 3 * static_cast<int>(sizeof(float));
    constexpr int TangentOffset = 6 * static_cast<int>(sizeof(float));
    constexpr int Uv0Offset = 9 * static_cast<int>(sizeof(float));
    constexpr int Uv1Offset = 11 * static_cast<int>(sizeof(float));
    constexpr int ColorOffset = 13 * static_cast<int>(sizeof(float));

    clear();
    setPrimitiveType(PrimitiveType::Triangles);
    setStride(stride);
    setVertexData(vertexData);
    setIndexData(indexData);
    setBounds(boundsMin, boundsMax);
    addAttribute(Attribute::IndexSemantic, 0, Attribute::U32Type);
    addAttribute(Attribute::PositionSemantic,
                 PositionOffset,
                 Attribute::F32Type);
    if (hasNormals) {
        addAttribute(Attribute::NormalSemantic,
                     NormalOffset,
                     Attribute::F32Type);
    }
    if (hasTangents) {
        addAttribute(Attribute::TangentSemantic,
                     TangentOffset,
                     Attribute::F32Type);
    }
    if (hasUv0) {
        addAttribute(Attribute::TexCoord0Semantic,
                     Uv0Offset,
                     Attribute::F32Type);
    }
    if (hasUv1) {
        addAttribute(Attribute::TexCoord1Semantic,
                     Uv1Offset,
                     Attribute::F32Type);
    }
    if (hasVertexColors) {
        addAttribute(Attribute::ColorSemantic,
                     ColorOffset,
                     Attribute::F32Type);
    }
    for (std::size_t index = 0u; index < subsets.size(); ++index) {
        addSubset(subsets[index].first,
                  subsets[index].second,
                  boundsMin,
                  boundsMax,
                  QStringLiteral("material_%1").arg(index));
    }
    update();
}

void RaceGeometry::clearMesh() {
    clear();
    setVertexData({});
    setIndexData({});
    setBounds({}, {});
    update();
}

QSSGRenderGraphObject *RaceGeometry::updateSpatialNode(QSSGRenderGraphObject *node) {
    if (node == nullptr) setVertexData(vertexData());
    return QQuick3DGeometry::updateSpatialNode(node);
}

}  // namespace forevertas::viewer
