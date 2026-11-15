#ifndef FOREVERTAS_VIEWER_CUSTOM_VOLUME_GEOMETRY_H
#define FOREVERTAS_VIEWER_CUSTOM_VOLUME_GEOMETRY_H

#include <QtQuick3D/qquick3dgeometry.h>

#include <QPointF>
#include <QString>
#include <QVector3D>

#include <vector>

namespace forevertas::viewer {

class CustomVolumeGeometry final : public QQuick3DGeometry {
    Q_OBJECT

public:
    CustomVolumeGeometry();
    void setVolume(const QString &plane,
                   const QVector3D &origin,
                   float depth,
                   const std::vector<QPointF> &vertices);

protected:
    // Qt Quick 3D (6.9) gives a geometry a new, empty renderer node when it
    // comes back into a scene (no Model used it for a while, then one does)
    // and never sends it the data it already has: the car (or anything)
    // using it is then not drawn. A new node gets the data again.
    QSSGRenderGraphObject *updateSpatialNode(QSSGRenderGraphObject *node) override;
};

}  // namespace forevertas::viewer

#endif
