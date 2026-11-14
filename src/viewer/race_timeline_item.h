#ifndef FOREVERTAS_VIEWER_RACE_TIMELINE_ITEM_H
#define FOREVERTAS_VIEWER_RACE_TIMELINE_ITEM_H

#include "viewer/race_viewer_controller.h"

#include <QElapsedTimer>
#include <QPointer>
#include <QQuickPaintedItem>

#include "viewer/pointer_lock.h"
#include <QTimer>

class QHoverEvent;
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
    // Time across instead of down: later to the right, right steering on
    // top, left steering under.
    Q_PROPERTY(bool horizontal READ horizontal WRITE setHorizontal NOTIFY
                       horizontalChanged)

public:
    explicit RaceTimelineItem(QQuickItem *parent = nullptr);

    RaceViewerController *viewer() const;
    void setViewer(RaceViewerController *viewer);

    qreal pixelsPerTick() const;
    void setPixelsPerTick(qreal value);

    bool horizontal() const;
    void setHorizontal(bool value);

    void paint(QPainter *painter) override;

signals:
    void viewerChanged();
    void pixelsPerTickChanged();
    void horizontalChanged();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseUngrabEvent() override;
    void wheelEvent(QWheelEvent *event) override;
    void hoverMoveEvent(QHoverEvent *event) override;

private:
    enum class DragMode {
        None,
        Scrub,    // the inputs follow the pointer
        Cursor,   // the current-tick line (yellow arrow) follows it
        Zoom,
    };

    // Along the time axis: down, or right when horizontal.
    qreal along(const QPointF &point) const;
    qreal alongSize() const;
    bool onCursor(const QPointF &point) const;
    void updateCursorShape(const QPointF &point);
    // The cursor drag: the yellow arrow held like a game holds the pointer.
    void beginCursorGrab(const QMouseEvent *event);
    void moveCursorGrab(qreal travel);
    void endCursorGrab();

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
    qreal dragAnchorAlong_ = 0.0;
    bool horizontal_ = false;
    qreal zoomAnchorPixelsPerTick_ = 3.0;
    DragMode dragMode_ = DragMode::None;
    // A cursor drag holds the pointer (see beginCursorGrab): locked by the
    // compositor, pinned by putting it back, or free (hidden, moving on).
    enum class GrabMode { None, Locked, Pinned, Free };
    GrabMode grabMode_ = GrabMode::None;
    PointerLock pointerLock_;
    QPointF grabPoint_;
    QPointF grabGlobal_;
    qreal grabTravel_ = 0.0;
    // what the pointer went past the run's ends (Free: taken back off)
    qreal grabSlack_ = 0.0;
    bool drawnPointerShown_ = false;
    QPointF drawnPointer_;
};

void RegisterRaceViewerQmlTypes();

}  // namespace forevertas::viewer

#endif
