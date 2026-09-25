#ifndef FOREVERTAS_APP_CUSTOM_VOLUME_TARGET_MODEL_H
#define FOREVERTAS_APP_CUSTOM_VOLUME_TARGET_MODEL_H

#include <QObject>
#include <QPointF>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QVector3D>

#include <memory>
#include <vector>

namespace forevertas::viewer {
class CustomVolumeGeometry;
}

namespace forevertas::app {

class CustomVolumeTargetModel final : public QObject {
    Q_OBJECT

    Q_PROPERTY(QVariantList targets READ targets NOTIFY targetsChanged)
    Q_PROPERTY(int count READ count NOTIFY targetsChanged)
    Q_PROPERTY(int maximumCount READ maximumCount CONSTANT)
    Q_PROPERTY(int selectedIndex READ selectedIndex NOTIFY
                       selectedTargetChanged)
    Q_PROPERTY(QVariantMap selectedTarget READ selectedTarget NOTIFY
                       selectedTargetChanged)
    Q_PROPERTY(bool editingEnabled READ editingEnabled NOTIFY
                       editingEnabledChanged)
    Q_PROPERTY(bool drawing READ drawing NOTIFY drawingChanged)

public:
    explicit CustomVolumeTargetModel(
            const QVariantMap &legacySettings = {},
            QObject *parent = nullptr);
    ~CustomVolumeTargetModel() override;

    QVariantList targets() const;
    int count() const;
    int maximumCount() const;
    int selectedIndex() const;
    QVariantMap selectedTarget() const;
    bool editingEnabled() const;
    bool drawing() const;

    Q_INVOKABLE int addTarget(const QString &plane,
                              double originX,
                              double originY,
                              double originZ);
    Q_INVOKABLE int duplicateSelected();
    Q_INVOKABLE bool removeTarget(int index);
    Q_INVOKABLE bool selectTarget(int index);
    Q_INVOKABLE bool setName(int index, const QString &name);
    Q_INVOKABLE bool setPlane(int index, const QString &plane);
    Q_INVOKABLE bool setOriginComponent(int index,
                                        const QString &axis,
                                        const QString &value);
    Q_INVOKABLE bool setDepth(int index, const QString &value);
    bool setPolygon(int index, const QString &encoded);
    Q_INVOKABLE bool setVertex(int index,
                               int vertexIndex,
                               const QString &axis,
                               const QString &value);
    Q_INVOKABLE bool setVertexWorld(int vertexIndex,
                                    double x,
                                    double y,
                                    double z);
    Q_INVOKABLE bool addVertexWorld(double x, double y, double z);
    Q_INVOKABLE bool removeVertex(int index, int vertexIndex);
    Q_INVOKABLE bool translateSelected(double x, double y, double z);
    Q_INVOKABLE bool moveSelectedTo(double x, double y, double z);
    Q_INVOKABLE bool resizeDepthSelected(double delta);
    Q_INVOKABLE bool restoreTargets(const QVariantList &snapshot);

    bool beginDrawing();
    bool finishDrawing();
    void cancelDrawing();
    void setEditingEnabled(bool enabled);

signals:
    void targetsChanged();
    void selectedTargetChanged();
    void editingEnabledChanged();
    void drawingChanged();

private:
    struct Target {
        QString id;
        QString name;
        QString plane;
        QVector3D origin;
        float depth = 5.0F;
        std::vector<QPointF> vertices;
        std::unique_ptr<forevertas::viewer::CustomVolumeGeometry> geometry;
        std::unique_ptr<forevertas::viewer::CustomVolumeGeometry>
                stagingGeometry;
    };

    QVariantMap toVariantMap(const Target &target, bool selected) const;
    static bool parseFinite(const QString &value, double *result);
    static int axisIndex(const QString &axis);
    static bool validPlane(const QString &plane);
    static bool validPolygon(const std::vector<QPointF> &vertices);
    static QVector3D worldPoint(const Target &target,
                                const QPointF &point,
                                float normal);
    static QPointF planePoint(const Target &target,
                              const QVector3D &point);
    static QString encodePolygon(const std::vector<QPointF> &vertices);
    static std::vector<QPointF> decodePolygon(const QString &encoded);
    QString nextDefaultName() const;
    void rebuildGeometry(Target *target);
    void load(const QVariantMap &legacySettings);
    void persist() const;
    void notifyTargetChanged(int index);

    std::vector<Target> targets_;
    int selectedIndex_ = 0;
    bool editingEnabled_ = true;
    bool drawing_ = false;
    std::vector<QPointF> drawingBackup_;
};

}  // namespace forevertas::app

#endif
