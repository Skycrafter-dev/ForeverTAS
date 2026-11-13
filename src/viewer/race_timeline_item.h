#ifndef FOREVERTAS_VIEWER_RACE_TIMELINE_ITEM_H
#define FOREVERTAS_VIEWER_RACE_TIMELINE_ITEM_H

#include "viewer/race_viewer_controller.h"

#include <QElapsedTimer>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QTimer>

class QMouseEvent;
class QPainter;
class QWheelEvent;

namespace forevertas::viewer {

class RaceTimelineItem : public QQuickPaintedItem {
    Q_OBJECT

    Q_PROPERTY(RaceViewerController *viewer READ viewer WRITE setViewer NOTIFY
                       viewerChanged)
    Q_PROPERTY(qreal pixelsPerTick READ pixelsPerTick WRITE setPixelsPerTick
                       NOTIFY pixelsPerTickChanged)

public:
    explicit RaceTimelineItem(QQuickItem *parent = nullptr);

    RaceViewerController *viewer() const;
    void setViewer(RaceViewerController *viewer);

    qreal pixelsPerTick() const;
    void setPixelsPerTick(qreal value);

    void paint(QPainter *painter) override;

signals:
    void viewerChanged();
    void pixelsPerTickChanged();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseUngrabEvent() override;
    void wheelEvent(QWheelEvent *event) override;

private:
    enum class DragMode {
        None,
        Scrub,
        Zoom,
    };

    void queueScrub(qint64 timeMs);
    void applyPendingScrub();
    void finishScrub();
    void disconnectViewer();

    QPointer<RaceViewerController> viewer_;
    QTimer scrubUpdateTimer_;
    QElapsedTimer lastScrubUpdate_;
    qreal pixelsPerTick_ = 3.0;
    bool hasPendingScrub_ = false;
    qint64 pendingScrubTimeMs_ = 0;
    qint64 dragAnchorTimeMs_ = 0;
    qreal dragAnchorY_ = 0.0;
    qreal zoomAnchorPixelsPerTick_ = 3.0;
    DragMode dragMode_ = DragMode::None;
};

void RegisterRaceViewerQmlTypes();

}  // namespace forevertas::viewer

#endif
