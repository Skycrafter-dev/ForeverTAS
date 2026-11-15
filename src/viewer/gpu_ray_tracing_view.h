#ifndef FOREVERTAS_VIEWER_GPU_RAY_TRACING_VIEW_H
#define FOREVERTAS_VIEWER_GPU_RAY_TRACING_VIEW_H

#include <QPointer>
#include <QString>
#include <QRectF>
#include <QVector3D>

#if FOREVERTAS_GPU_RAY_TRACING
#include <QQuickRhiItem>
#else
#include <QQuickItem>
#endif

namespace forevertas::viewer {

class RaceViewerController;

#if FOREVERTAS_GPU_RAY_TRACING
class GpuRayTracingView : public QQuickRhiItem {
#else
class GpuRayTracingView : public QQuickItem {
#endif
    Q_OBJECT
    Q_PROPERTY(QObject *viewer READ viewer WRITE setViewer NOTIFY viewerChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(bool supported READ supported CONSTANT)
    Q_PROPERTY(QString status READ status CONSTANT)
    Q_PROPERTY(QVector3D cameraPosition READ cameraPosition
                       WRITE setCameraPosition NOTIFY cameraChanged)
    Q_PROPERTY(QVector3D cameraTarget READ cameraTarget
                       WRITE setCameraTarget NOTIFY cameraChanged)
    Q_PROPERTY(QVector3D cameraUp READ cameraUp WRITE setCameraUp
                       NOTIFY cameraChanged)
    // The part of the view the camera frames, in item coordinates, its
    // field of view spanning that part's height (empty: the whole view).
    Q_PROPERTY(QRectF framing READ framing WRITE setFraming NOTIFY
                       cameraChanged)
    Q_PROPERTY(float fieldOfView READ fieldOfView WRITE setFieldOfView NOTIFY
                       cameraChanged)

public:
    explicit GpuRayTracingView(QQuickItem *parent = nullptr);

    QObject *viewer() const;
    void setViewer(QObject *viewer);
    bool active() const;
    void setActive(bool active);
    bool supported() const;
    QString status() const;
    QVector3D cameraPosition() const;
    void setCameraPosition(const QVector3D &position);
    QVector3D cameraTarget() const;
    void setCameraTarget(const QVector3D &target);
    QVector3D cameraUp() const;
    void setCameraUp(const QVector3D &direction);
    float fieldOfView() const;
    void setFieldOfView(float value);
    QRectF framing() const;
    void setFraming(const QRectF &framing);
    RaceViewerController *viewerController() const;

#if FOREVERTAS_GPU_RAY_TRACING
protected:
    QQuickRhiItemRenderer *createRenderer() override;
#endif

signals:
    void viewerChanged();
    void activeChanged();
    void cameraChanged();

private:
    QPointer<RaceViewerController> viewer_;
    QVector3D cameraPosition_{};
    QVector3D cameraTarget_{};
    QVector3D cameraUp_{0.0f, 1.0f, 0.0f};
    float fieldOfView_ = 55.0f;
    QRectF framing_;
    bool active_ = false;
};

}  // namespace forevertas::viewer

#endif
