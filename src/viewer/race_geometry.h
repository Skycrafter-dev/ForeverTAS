#ifndef FOREVERTAS_VIEWER_RACE_GEOMETRY_H
#define FOREVERTAS_VIEWER_RACE_GEOMETRY_H

#include <QtQuick3D/qquick3dgeometry.h>

#include <QByteArray>
#include <QVector3D>
#include <utility>
#include <vector>

namespace forevertas::viewer {

class RaceGeometry final : public QQuick3DGeometry {
    Q_OBJECT

public:
    RaceGeometry();

    void setMesh(QByteArray vertexData,
                 int stride,
                 PrimitiveType primitiveType,
                 bool hasVertexColors,
                 const QVector3D &boundsMin,
                 const QVector3D &boundsMax);
    void setIndexedMesh(
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
            const std::vector<std::pair<int, int>> &subsets);
    void clearMesh();

protected:
    // Qt Quick 3D (6.9) gives a geometry a new, empty renderer node when it
    // comes back into a scene (no Model used it for a while, then one does)
    // and never sends it the data it already has: the car (or anything)
    // using it is then not drawn. A new node gets the data again.
    QSSGRenderGraphObject *updateSpatialNode(QSSGRenderGraphObject *node) override;
};

}  // namespace forevertas::viewer

#endif
