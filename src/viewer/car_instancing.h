#ifndef FOREVERTAS_VIEWER_CAR_INSTANCING_H
#define FOREVERTAS_VIEWER_CAR_INSTANCING_H

#include <QByteArray>
#include <QColor>
#include <QMatrix4x4>
#include <QtQuick3D/QQuick3DInstancing>

#include <vector>

namespace forevertas::viewer {

// Draws many cars with one instanced draw call per mesh: each instance is one
// car ellipsoid with its full transform (car pose times ellipsoid placement)
// and its car's color.
class CarInstancing final : public QQuick3DInstancing {
    Q_OBJECT

public:
    struct Instance {
        QMatrix4x4 transform;
        QColor color;
    };

    explicit CarInstancing(QQuick3DObject *parent = nullptr);

    void setInstances(const std::vector<Instance> &instances);
    int count() const { return count_; }

protected:
    QByteArray getInstanceBuffer(int *instanceCount) override;

private:
    QByteArray table_;
    int count_ = 0;
};

}  // namespace forevertas::viewer

#endif
