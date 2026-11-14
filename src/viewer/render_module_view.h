#ifndef FOREVERTAS_VIEWER_RENDER_MODULE_VIEW_H
#define FOREVERTAS_VIEWER_RENDER_MODULE_VIEW_H

#include "viewer/render_module_api.h"

#include <QPointer>
#include <QQuickItem>
#include <QString>
#include <QImage>
#include <QRect>
#include <QVariantMap>
#include <QVector3D>

#include <memory>
#include <mutex>

#include <vector>

class QThread;

namespace forevertas::viewer {

class RaceViewerController;
class RenderModuleRenderer;

// The race view drawn by an optional render module (render_module_api.h).
// The module draws the loaded map, and the selected run's car, on the
// scene graph's render thread inside ForeverTAS's own OpenGL context, from
// this item's camera at this item's race time, in the same frame as the 3D
// view. The item shows the frame and is a texture provider, so the 3D view
// can paint its map meshes with the same image and keep its own cars and
// overlays in front of or behind them.
class RenderModuleView : public QQuickItem {
    Q_OBJECT
    // Whether a module was found next to the executable.
    Q_PROPERTY(bool available READ available CONSTANT)
    // The mode's name in the Render list; empty without a module.
    Q_PROPERTY(QString label READ label CONSTANT)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(QString packsDirectory READ packsDirectory WRITE
                       setPacksDirectory NOTIFY sourceChanged)
    Q_PROPERTY(QString mapPath READ mapPath WRITE setMapPath NOTIFY
                       sourceChanged)
    // The race view's controller: the module drives its selected run.
    Q_PROPERTY(QObject *viewer READ viewer WRITE setViewer NOTIFY
                       viewerChanged)
    // Race time shown, in milliseconds.
    Q_PROPERTY(double raceTimeMs READ raceTimeMs WRITE setRaceTimeMs NOTIFY
                       frameChanged)
    // Whether the module draws the selected run's car.
    Q_PROPERTY(bool showCar READ showCar WRITE setShowCar NOTIFY
                       frameChanged)
    Q_PROPERTY(QVector3D cameraPosition READ cameraPosition WRITE
                       setCameraPosition NOTIFY frameChanged)
    Q_PROPERTY(QVector3D cameraTarget READ cameraTarget WRITE
                       setCameraTarget NOTIFY frameChanged)
    Q_PROPERTY(QVector3D cameraUp READ cameraUp WRITE setCameraUp NOTIFY
                       frameChanged)
    Q_PROPERTY(float fieldOfView READ fieldOfView WRITE setFieldOfView NOTIFY
                       frameChanged)
    Q_PROPERTY(float clipNear READ clipNear WRITE setClipNear NOTIFY
                       frameChanged)
    Q_PROPERTY(float clipFar READ clipFar WRITE setClipFar NOTIFY
                       frameChanged)
    // True once a frame of the current map is showing.
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    // What is happening instead, when not ready ("Loading the map…", or
    // why it failed).
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    // The frames' size in pixels, for the 3D view's sampling.
    Q_PROPERTY(QSize frameSize READ frameSize NOTIFY readyChanged)
    // The part of the view the camera frames, in item coordinates, its
    // field of view spanning that part's height (empty: the whole view).
    // The module then draws a larger image centred on it, of which the view
    // shows imageOrigin, frameSize.
    Q_PROPERTY(QRectF framing READ framing WRITE setFraming NOTIFY
                       frameChanged)
    Q_PROPERTY(QSize imageSize READ imageSize NOTIFY frameChanged)
    Q_PROPERTY(QPointF imageOrigin READ imageOrigin NOTIFY frameChanged)

public:
    explicit RenderModuleView(QQuickItem *parent = nullptr);
    ~RenderModuleView() override;

    bool available() const;
    QString label() const;
    bool active() const;
    void setActive(bool active);
    QString packsDirectory() const;
    void setPacksDirectory(const QString &directory);
    QString mapPath() const;
    void setMapPath(const QString &path);
    QObject *viewer() const;
    void setViewer(QObject *viewer);
    double raceTimeMs() const;
    void setRaceTimeMs(double timeMs);
    bool showCar() const;
    void setShowCar(bool show);
    // The car of the module's last frame, for the diagnostics: available,
    // raceTimeMs, tick, position (the module's own simulation), shown,
    // controls, frames, and the camera it was drawn from.
    Q_INVOKABLE QVariantMap carState() const;
    // The In-game picture for the diagnostics (grabbing the item does not
    // see the module's texture): asks for the part of the view in rect
    // (item coordinates) to be read on the next frame; picture() then gives
    // {serial, image} once read (serial counts the pictures read).
    Q_INVOKABLE int requestPicture(const QRectF &rect);
    Q_INVOKABLE QVariantMap picture() const;
    QVector3D cameraPosition() const;
    void setCameraPosition(const QVector3D &position);
    QVector3D cameraTarget() const;
    void setCameraTarget(const QVector3D &target);
    QVector3D cameraUp() const;
    void setCameraUp(const QVector3D &up);
    float fieldOfView() const;
    void setFieldOfView(float degrees);
    float clipNear() const;
    void setClipNear(float distance);
    float clipFar() const;
    void setClipFar(float distance);
    bool ready() const;
    QString status() const;
    QSize frameSize() const;
    QRectF framing() const;
    void setFraming(const QRectF &framing);
    QSize imageSize() const;
    QPointF imageOrigin() const;

    bool isTextureProvider() const override;
    QSGTextureProvider *textureProvider() const override;

signals:
    void activeChanged();
    void sourceChanged();
    void viewerChanged();
    void frameChanged();
    void readyChanged();
    void statusChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *node, UpdatePaintNodeData *) override;
    void geometryChange(const QRectF &newGeometry,
                        const QRectF &oldGeometry) override;
    void releaseResources() override;

private:
    void startLoad();
    void loadPrepared(FtrmModule *module, const QString &error,
                      quint64 source);
    void uploadFinished(quint64 source, const QString &error);
    void frameDrawn(quint64 source);
    void frameFailed(quint64 source, const QString &error);
    void refreshControls();
    void cleanup();
    void reloadLater();
    void setFrameValue(bool changed);
    void setStatus(const QString &status);
    void setReady(bool ready);
    QSize pixelSize() const;
    // The module's image and where the view's top left falls in it, and the
    // field of view that image needs.
    void imageLayout(QSize *size, QPointF *origin, float *fieldOfView) const;
    void ensureRenderer() const;

    bool active_ = false;
    QString packsDirectory_;
    QString mapPath_;
    QPointer<RaceViewerController> viewer_;
    double raceTimeMs_ = 0.0;
    bool showCar_ = true;
    QVector3D cameraPosition_{};
    QVector3D cameraTarget_{0.0f, 0.0f, 1.0f};
    QVector3D cameraUp_{0.0f, 1.0f, 0.0f};
    float fieldOfView_ = 60.0f;
    float clipNear_ = 0.2f;
    float clipFar_ = 50000.0f;
    bool ready_ = false;
    QString status_;
    QSize frameSize_;
    QRectF framing_;

    // Which packs + map is wanted; bumped on every change.
    quint64 source_ = 0;
    // The source loading or loaded (0: none).
    quint64 loadingSource_ = 0;
    QThread *loader_ = nullptr;
    // Prepared on the loader, handed to the render thread in sync.
    FtrmModule *prepared_ = nullptr;
    quint64 preparedSource_ = 0;

    std::vector<FtrmControl> controls_;
    quint64 controlsRevision_ = 1;

    // Lives on the render thread.
    mutable RenderModuleRenderer *renderer_ = nullptr;
public:
    // The last frame's car, written on the render thread.
    struct CarSnapshot {
        std::mutex mutex;
        bool available = false;
        FtrmCarState car{};
        FtrmCamera camera{};
        // requestPicture / picture
        bool pictureRequested = false;
        QRect pictureRect;
        int pictureSerial = 0;
        QImage picture;
    };
private:
    std::shared_ptr<CarSnapshot> carSnapshot_ = std::make_shared<CarSnapshot>();
};

}  // namespace forevertas::viewer

#endif
