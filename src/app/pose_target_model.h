#ifndef FOREVERTAS_APP_POSE_TARGET_MODEL_H
#define FOREVERTAS_APP_POSE_TARGET_MODEL_H

#include <QObject>
#include <QQuaternion>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QVector3D>

#include <vector>

namespace forevertas::app {

class PoseTargetModel final : public QObject {
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

public:
    explicit PoseTargetModel(
            const QVariantMap &releasedSettings = {},
            QObject *parent = nullptr);

    QVariantList targets() const;
    int count() const;
    int maximumCount() const;
    int selectedIndex() const;
    QVariantMap selectedTarget() const;
    bool editingEnabled() const;

    Q_INVOKABLE int addTarget(double x,
                              double y,
                              double z,
                              const QQuaternion &rotation);
    Q_INVOKABLE int duplicateSelected();
    Q_INVOKABLE bool removeTarget(int index);
    Q_INVOKABLE bool selectTarget(int index);
    Q_INVOKABLE bool setName(int index, const QString &name);
    Q_INVOKABLE bool setPositionComponent(int index,
                                          const QString &axis,
                                          const QString &value);
    Q_INVOKABLE bool setRotationComponent(int index,
                                          const QString &axis,
                                          const QString &value);
    Q_INVOKABLE bool translateSelected(double x, double y, double z);
    Q_INVOKABLE bool moveSelectedTo(double x,
                                    double y,
                                    double z,
                                    const QQuaternion &rotation);
    Q_INVOKABLE bool rotateSelected(const QString &axis,
                                    double degrees);
    void setEditingEnabled(bool enabled);

signals:
    void targetsChanged();
    void selectedTargetChanged();
    void editingEnabledChanged();

private:
    struct Target {
        QString id;
        QString name;
        QVector3D position;
        float yawDegrees = 0.0F;
        float pitchDegrees = 0.0F;
        float rollDegrees = 0.0F;
    };

    static QVariantMap ToVariantMap(const Target &target, bool selected);
    static bool ParseFinite(const QString &value, double *result);
    static int PositionAxisIndex(const QString &axis);
    static float NormalizeDegrees(double degrees);
    static QQuaternion Rotation(const Target &target);
    QString nextDefaultName() const;
    void load(const QVariantMap &releasedSettings);
    void persist() const;
    void notifyTargetChanged(int index);

    std::vector<Target> targets_;
    int selectedIndex_ = 0;
    bool editingEnabled_ = true;
};

}  // namespace forevertas::app

#endif
