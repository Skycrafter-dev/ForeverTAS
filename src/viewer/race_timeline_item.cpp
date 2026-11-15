#include "viewer/race_timeline_item.h"

#include "app/app_settings.h"
#include "app/dock_layout.h"
#include "app/panel_wheel_redirector.h"
#include "viewer/gpu_ray_tracing_view.h"
#include "viewer/render_module_view.h"
#include "viewer/video_export_controller.h"
#include "viewer/whiteboard_canvas_item.h"

#include "time_format.h"

#include <QCursor>
#include <QGuiApplication>
#include <QHoverEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QScreen>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace forevertas::viewer {
namespace {

constexpr qreal kMinimumPixelsPerTick = 1.0;
constexpr qreal kMaximumPixelsPerTick = 12.0;
constexpr int kScrubUpdateIntervalMs = 16;
// How close to the current-tick line a press grabs it (pixels along time).
constexpr qreal kCursorGrab = 7.0;

struct TimelineScale {
    qint64 majorTicks = 100;
    qint64 minorTicks = 10;
    qint64 smallestTicks = 10;
};

qreal InterpolationProgress(qreal value, qreal start, qreal end) {
    return std::clamp((value - start) / (end - start), 0.0, 1.0);
}

qreal CentisecondTickLength(qreal pixelsPerTick) {
    constexpr qreal revealStart = 5.0;
    constexpr qreal microAnchor = 7.0;
    constexpr qreal fullAnchor = 12.0;
    constexpr qreal microLength = 3.0;
    constexpr qreal fullLength = 7.0;

    if (pixelsPerTick <= microAnchor) {
        return microLength * InterpolationProgress(
                pixelsPerTick, revealStart, microAnchor);
    }
    return microLength +
            (fullLength - microLength) * InterpolationProgress(
                    pixelsPerTick, microAnchor, fullAnchor);
}

QColor InterpolateColor(const QColor &start,
                        const QColor &end,
                        qreal progress) {
    progress = std::clamp(progress, 0.0, 1.0);
    return QColor::fromRgbF(
            start.redF() + (end.redF() - start.redF()) * progress,
            start.greenF() + (end.greenF() - start.greenF()) * progress,
            start.blueF() + (end.blueF() - start.blueF()) * progress,
            start.alphaF() + (end.alphaF() - start.alphaF()) * progress);
}

QColor CentisecondTickColor(qreal pixelsPerTick) {
    const QColor microColor(QStringLiteral("#343b37"));
    const QColor minorColor(QStringLiteral("#59635d"));
    if (pixelsPerTick <= 7.0) {
        QColor color = microColor;
        color.setAlphaF(InterpolationProgress(pixelsPerTick, 5.0, 7.0));
        return color;
    }
    return InterpolateColor(
            microColor,
            minorColor,
            InterpolationProgress(pixelsPerTick, 7.0, 12.0));
}

TimelineScale SelectTimelineScale(qreal pixelsPerTick) {
    constexpr qreal targetMajorPixels = 120.0;
    constexpr qint64 majorSteps[] = {1, 2, 5, 10, 20, 50, 100};

    TimelineScale scale;
    for (const qint64 step : majorSteps) {
        if (static_cast<qreal>(step) * pixelsPerTick >=
            targetMajorPixels) {
            scale.majorTicks = step;
            break;
        }
    }

    scale.minorTicks = scale.majorTicks >= 20 ? 10 : 1;
    scale.smallestTicks = CentisecondTickLength(pixelsPerTick) > 0.0
            ? 1
            : scale.minorTicks;
    return scale;
}

QString FormatTimelineTime(qint64 tick) {
    return QString::fromStdString(FormatRaceTimeMilliseconds(
            static_cast<double>(std::max<qint64>(0, tick)) * 10.0));
}

}  // namespace

RaceTimelineItem::RaceTimelineItem(QQuickItem *parent)
    : QQuickPaintedItem(parent) {
    connect(&pointerLock_, &PointerLock::moved, this, [this](const QPointF &delta) {
        if (dragMode_ == DragMode::Cursor && grabMode_ == GrabMode::Locked && viewer_ != nullptr) {
            moveCursorGrab(grabTravel_ + (horizontal_ ? delta.x() : delta.y()));
        }
    });
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
    setAcceptHoverEvents(true);
    setAntialiasing(false);
    setOpaquePainting(false);
    setFillColor(Qt::transparent);
    setCursor(QCursor(Qt::SizeVerCursor));
    scrubUpdateTimer_.setSingleShot(true);
    scrubUpdateTimer_.setTimerType(Qt::PreciseTimer);
    connect(&scrubUpdateTimer_,
            &QTimer::timeout,
            this,
            &RaceTimelineItem::applyPendingScrub);
}

RaceViewerController *RaceTimelineItem::viewer() const {
    return viewer_;
}

void RaceTimelineItem::setViewer(RaceViewerController *viewer) {
    if (viewer_ == viewer) {
        return;
    }
    scrubUpdateTimer_.stop();
    hasPendingScrub_ = false;
    dragMode_ = DragMode::None;
    lastScrubUpdate_.invalidate();
    disconnectViewer();
    viewer_ = viewer;
    if (viewer_ != nullptr) {
        connect(viewer_,
                &RaceViewerController::timeChanged,
                this,
                [this]() { update(); });
        connect(viewer_,
                &RaceViewerController::timelineChanged,
                this,
                [this]() { update(); });
        connect(viewer_,
                &RaceViewerController::stateChanged,
                this,
                [this]() { update(); });
        connect(viewer_,
                &QObject::destroyed,
                this,
                [this]() {
                    viewer_ = nullptr;
                    update();
                    emit viewerChanged();
                });
    }
    update();
    emit viewerChanged();
}

qreal RaceTimelineItem::pixelsPerTick() const {
    return pixelsPerTick_;
}

void RaceTimelineItem::setPixelsPerTick(qreal value) {
    const qreal clamped = std::clamp(
            value, kMinimumPixelsPerTick, kMaximumPixelsPerTick);
    if (qFuzzyCompare(pixelsPerTick_, clamped)) {
        return;
    }
    pixelsPerTick_ = clamped;
    update();
    emit pixelsPerTickChanged();
}

bool RaceTimelineItem::horizontal() const {
    return horizontal_;
}

void RaceTimelineItem::setHorizontal(bool value) {
    if (horizontal_ == value) {
        return;
    }
    horizontal_ = value;
    setCursor(QCursor(horizontal_ ? Qt::SizeHorCursor : Qt::SizeVerCursor));
    update();
    emit horizontalChanged();
}

qreal RaceTimelineItem::along(const QPointF &point) const {
    return horizontal_ ? point.x() : point.y();
}

qreal RaceTimelineItem::alongSize() const {
    return horizontal_ ? width() : height();
}

bool RaceTimelineItem::onCursor(const QPointF &point) const {
    return std::abs(along(point) - alongSize() * 0.5) <= kCursorGrab;
}

void RaceTimelineItem::updateCursorShape(const QPointF &point) {
    if (dragMode_ != DragMode::None) {
        return;
    }
    setCursor(QCursor(onCursor(point) ? Qt::OpenHandCursor
                                      : horizontal_ ? Qt::SizeHorCursor
                                                    : Qt::SizeVerCursor));
}

void RaceTimelineItem::hoverMoveEvent(QHoverEvent *event) {
    updateCursorShape(event->position());
    QQuickPaintedItem::hoverMoveEvent(event);
}

void RaceTimelineItem::paint(QPainter *painter) {
    const QRectF area = boundingRect();
    // The timeline sits on a glass panel: dark washes, not solid fills.
    painter->fillRect(area, QColor(0, 0, 0, 56));

    if (viewer_ == nullptr || !viewer_->loaded() ||
        viewer_->tickCount() <= 0) {
        painter->setPen(
                QColor(QStringLiteral("#778079")));
        painter->drawText(
                area.adjusted(18.0, 18.0, -18.0, -18.0),
                Qt::AlignCenter | Qt::TextWordWrap,
                QStringLiteral("Search run inputs appear here"));
        return;
    }

    // Drawn in the vertical layout's frame: x across (the ruler, left
    // steering, brake / gas / brake, right steering), y along time. Horizontal
    // turns that frame a quarter turn (x across becomes up, y along becomes
    // right); the labels stay upright.
    const qreal across = horizontal_ ? area.height() : area.width();
    const qreal length = horizontal_ ? area.width() : area.height();
    const QTransform frame = horizontal_
            ? QTransform(0.0, -1.0, 1.0, 0.0, 0.0, area.height())
            : QTransform();
    const QTransform base = painter->transform();
    painter->setTransform(frame, true);
    const qreal centerY = length * 0.5;
    const qreal rulerWidth = horizontal_ ? 26.0 : across < 210.0 ? 42.0 : 52.0;
    const qreal rulerRight = rulerWidth - 2.0;
    // The pedals' columns keep to a tenth of the room (steering is the
    // finer input), the controls centred in what the ruler leaves.
    const qreal room = std::max<qreal>(0.0, across - rulerWidth);
    const qreal accelerationWidth = std::clamp<qreal>(room * 0.1, 6.0, 24.0);
    const qreal brakeWidth = accelerationWidth * 0.5;
    const qreal controlWidth = accelerationWidth + 2.0 * brakeWidth;
    const qreal controlLeft = rulerWidth + (room - controlWidth) * 0.5;
    const qreal controlRight = controlLeft + controlWidth;
    const qreal steeringWidth = std::max<qreal>(
            0.0,
            std::min(controlLeft - rulerWidth - 2.0, across - controlRight - 2.0));

    painter->fillRect(
            QRectF(0.0, 0.0, rulerWidth, length),
            QColor(0, 0, 0, 46));
    painter->setPen(
            QColor(QStringLiteral("#2b332f")));
    painter->drawLine(QPointF(rulerRight, 0.0),
                      QPointF(rulerRight, length));

    painter->fillRect(
            QRectF(controlLeft, 0.0, controlWidth, length),
            QColor(255, 255, 255, 10));
    painter->setPen(
            QColor(QStringLiteral("#29312c")));
    painter->drawLine(QPointF(controlLeft, 0.0),
                      QPointF(controlLeft, length));
    painter->drawLine(QPointF(controlLeft + brakeWidth, 0.0),
                      QPointF(controlLeft + brakeWidth, length));
    painter->drawLine(
            QPointF(controlLeft + brakeWidth + accelerationWidth, 0.0),
            QPointF(controlLeft + brakeWidth + accelerationWidth,
                    length));
    painter->drawLine(QPointF(controlRight, 0.0),
                      QPointF(controlRight, length));

    const qreal currentTickPosition =
            static_cast<qreal>(viewer_->timeMs()) /
            static_cast<qreal>(viewer_->tickDurationMs());
    const qint64 firstVisible = std::max<qint64>(
            0,
            static_cast<qint64>(std::floor(
                    currentTickPosition - centerY / pixelsPerTick_)) - 1);
    const qint64 lastVisible = std::min<qint64>(
            viewer_->tickCount() - 1,
            static_cast<qint64>(std::ceil(
                    currentTickPosition +
                    (length - centerY) / pixelsPerTick_)) + 1);
    const qreal rowHeight = pixelsPerTick_;

    QVector<QRectF> leftSteering;
    QVector<QRectF> rightSteering;
    QVector<QRectF> acceleration;
    QVector<QRectF> brakeLeft;
    QVector<QRectF> brakeRight;
    leftSteering.reserve(static_cast<qsizetype>(lastVisible - firstVisible + 1));
    rightSteering.reserve(leftSteering.capacity());
    acceleration.reserve(leftSteering.capacity());
    brakeLeft.reserve(leftSteering.capacity());
    brakeRight.reserve(leftSteering.capacity());

    QFont timeFont = painter->font();
    timeFont.setPixelSize(10);
    painter->setFont(timeFont);

    for (qint64 tick = firstVisible; tick <= lastVisible; ++tick) {
        const qreal boundaryY = centerY +
                (static_cast<qreal>(tick) - currentTickPosition) *
                        pixelsPerTick_;
        const RaceViewerInputSample sample = viewer_->inputSample(tick);
        const qreal top = boundaryY;
        const qreal steering = std::clamp<qreal>(sample.steering, -1.0, 1.0);
        if (steering < 0.0) {
            const qreal length = -steering * steeringWidth;
            leftSteering.push_back(
                    QRectF(controlLeft - length, top, length, rowHeight));
        } else if (steering > 0.0) {
            rightSteering.push_back(
                    QRectF(controlRight,
                           top,
                           steering * steeringWidth,
                           rowHeight));
        }
        if (sample.accelerate > 0.0f) {
            acceleration.push_back(
                    QRectF(controlLeft + brakeWidth,
                           top,
                           accelerationWidth,
                           rowHeight));
        }
        if (sample.brake > 0.0f) {
            brakeLeft.push_back(
                    QRectF(controlLeft, top, brakeWidth, rowHeight));
            brakeRight.push_back(
                    QRectF(controlRight - brakeWidth,
                           top,
                           brakeWidth,
                           rowHeight));
        }
    }

    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(QStringLiteral("#4f9ddd")));
    painter->drawRects(leftSteering);
    painter->drawRects(rightSteering);
    painter->setBrush(QColor(QStringLiteral("#3dbd73")));
    painter->drawRects(acceleration);
    painter->setBrush(QColor(QStringLiteral("#df5555")));
    painter->drawRects(brakeLeft);
    painter->drawRects(brakeRight);

    const TimelineScale scale = SelectTimelineScale(pixelsPerTick_);
    const qint64 firstScaleTick =
            ((std::max<qint64>(0, firstVisible) + scale.smallestTicks - 1) /
             scale.smallestTicks) * scale.smallestTicks;
    const qint64 lastScaleTick = std::min<qint64>(
            viewer_->tickCount(), lastVisible + 1);

    QPen majorTickPen(
            QColor(QStringLiteral("#9aa69e")));
    majorTickPen.setCosmetic(true);
    majorTickPen.setWidthF(1.0);
    QPen minorTickPen(
            QColor(QStringLiteral("#59635d")));
    minorTickPen.setCosmetic(true);
    minorTickPen.setWidthF(1.0);
    const qreal centisecondTickLength =
            CentisecondTickLength(pixelsPerTick_);
    QPen centisecondTickPen(
            CentisecondTickColor(pixelsPerTick_));
    centisecondTickPen.setCosmetic(true);
    centisecondTickPen.setWidthF(1.0);

    for (qint64 tick = firstScaleTick;
         tick <= lastScaleTick;
         tick += scale.smallestTicks) {
        const qreal y = centerY +
                (static_cast<qreal>(tick) - currentTickPosition) *
                        pixelsPerTick_;
        const qreal alignedY = std::floor(y) + 0.5;
        if (alignedY < 0.0 || alignedY >= length) {
            continue;
        }

        const bool majorTick = tick % scale.majorTicks == 0;
        const bool centisecondTick = tick % 10 != 0;
        const bool minorTick = tick % scale.minorTicks == 0;
        const qreal tickStartX = majorTick
                ? rulerRight - (horizontal_ ? 10.0 : 12.0)
                : centisecondTick
                ? rulerRight - centisecondTickLength
                : minorTick
                ? rulerRight - 7.0
                : rulerRight - 3.0;
        painter->setPen(majorTick
                                ? majorTickPen
                                : centisecondTick
                                ? centisecondTickPen
                                : minorTick
                                ? minorTickPen
                                : centisecondTickPen);
        painter->drawLine(QPointF(tickStartX, alignedY),
                          QPointF(rulerRight, alignedY));

        if (majorTick) {
            painter->setPen(
                    QColor(QStringLiteral("#aeb8b0")));
            if (horizontal_) {
                // upright, under its tick
                const QPointF at = frame.map(QPointF(7.0, alignedY));
                painter->save();
                painter->setTransform(base);
                painter->drawText(QRectF(at.x() - 30.0, at.y() - 8.0, 60.0, 16.0),
                                  Qt::AlignCenter, FormatTimelineTime(tick));
                painter->restore();
            } else {
                painter->drawText(
                        QRectF(2.0, alignedY - 8.0, 34.0, 16.0),
                        Qt::AlignRight | Qt::AlignVCenter,
                        FormatTimelineTime(tick));
            }
        }
    }

    const QColor currentTickColor(QStringLiteral("#f3c85b"));
    painter->setPen(QPen(currentTickColor, 2.0));
    painter->drawLine(QPointF(0.0, centerY),
                      QPointF(across, centerY));
    // the handle: grab it to move the current tick like a cursor
    painter->setBrush(currentTickColor);
    painter->setPen(Qt::NoPen);
    painter->drawPolygon(QPolygonF{
            QPointF(across - 1.0, centerY),
            QPointF(across - 12.0, centerY - 7.0),
            QPointF(across - 12.0, centerY + 7.0)});
    // the grip held during a cursor drag (beginCursorGrab), in place of the
    // pointer: a yellow knob with arrows along time
    if (drawnPointerShown_ && dragMode_ == DragMode::Cursor) {
        painter->setTransform(base);
        painter->setRenderHint(QPainter::Antialiasing, true);
        const QPointF p = drawnPointer_;
        const QPointF axis = horizontal_ ? QPointF(1.0, 0.0) : QPointF(0.0, 1.0);
        const QPointF side(axis.y(), axis.x());
        painter->setPen(QPen(QColor(20, 20, 20, 210), 1.5));
        painter->setBrush(currentTickColor);
        painter->drawEllipse(p, 6.5, 6.5);
        for (const qreal direction : {-1.0, 1.0}) {
            const QPointF tip = p + axis * (direction * 15.0);
            const QPointF base0 = p + axis * (direction * 9.0) + side * 5.0;
            const QPointF base1 = p + axis * (direction * 9.0) - side * 5.0;
            painter->drawPolygon(QPolygonF{tip, base0, base1});
        }
    }
}

void RaceTimelineItem::mousePressEvent(QMouseEvent *event) {
    if (viewer_ == nullptr || !viewer_->loaded() ||
        viewer_->runCount() == 0) {
        event->ignore();
        return;
    }
    viewer_->pause();
    dragAnchorAlong_ = along(event->position());
    if (event->button() == Qt::RightButton) {
        zoomAnchorPixelsPerTick_ = pixelsPerTick_;
        dragMode_ = DragMode::Zoom;
    } else {
        dragAnchorTimeMs_ = viewer_->timeMs();
        dragMode_ = onCursor(event->position()) ? DragMode::Cursor : DragMode::Scrub;
        if (dragMode_ == DragMode::Cursor) beginCursorGrab(event);
    }
    event->accept();
}

// The yellow arrow held like a game holds the pointer: the real pointer is
// hidden and kept still (locked by the compositor on Wayland, put back after
// each move elsewhere), its motion moves time, and the timeline draws a grip
// that wraps around at its ends; the pointer comes back where the grip is.
void RaceTimelineItem::beginCursorGrab(const QMouseEvent *event) {
    grabPoint_ = event->position();
    grabTravel_ = 0.0;
    grabSlack_ = 0.0;
    grabGlobal_ = event->globalPosition();
    if (pointerLock_.lock(window())) {
        grabMode_ = GrabMode::Locked;
    } else if (QGuiApplication::platformName().startsWith(QLatin1String("wayland"))) {
        grabMode_ = GrabMode::Free;   // no lock offered: the pointer moves on, hidden
    } else {
        grabMode_ = GrabMode::Pinned;
    }
    setCursor(QCursor(Qt::BlankCursor));
    drawnPointer_ = grabPoint_;
    drawnPointerShown_ = true;
    update();
}

void RaceTimelineItem::moveCursorGrab(qreal travel) {
    // time follows the grip: down (or right) is later. Past the run's start
    // or end the grip stops with time, so going back answers at once.
    const qreal pixelsPerMs = pixelsPerTick_ / viewer_->tickDurationMs();
    const qreal earliest = -static_cast<qreal>(dragAnchorTimeMs_) * pixelsPerMs;
    const qreal latest = static_cast<qreal>(viewer_->durationMs() - dragAnchorTimeMs_) * pixelsPerMs;
    const qreal held = std::clamp(travel, std::min(earliest, 0.0), std::max(latest, 0.0));
    grabSlack_ += held - travel;
    travel = held;
    grabTravel_ = travel;
    queueScrub(static_cast<qint64>(std::llround(static_cast<qreal>(dragAnchorTimeMs_) + travel / pixelsPerMs)));
    const qreal size = alongSize();
    qreal value = std::fmod(along(grabPoint_) + travel, size);
    if (value < 0.0) value += size;
    drawnPointer_ = grabPoint_;
    if (horizontal_) {
        drawnPointer_.setX(value);
    } else {
        drawnPointer_.setY(value);
    }
    update();
}

void RaceTimelineItem::endCursorGrab() {
    if (grabMode_ == GrabMode::None) return;
    const QPointF shown = drawnPointer_;
    if (grabMode_ == GrabMode::Locked) {
        pointerLock_.unlock(mapToScene(shown));
    } else if (grabMode_ == GrabMode::Pinned) {
        QCursor::setPos(window() != nullptr ? window()->screen() : nullptr, mapToGlobal(shown).toPoint());
    }
    grabMode_ = GrabMode::None;
    drawnPointerShown_ = false;
    update();
}

void RaceTimelineItem::mouseMoveEvent(QMouseEvent *event) {
    if (dragMode_ == DragMode::None || viewer_ == nullptr) {
        event->ignore();
        return;
    }
    if (dragMode_ == DragMode::Cursor) {
        if (grabMode_ == GrabMode::Pinned) {
            const QPointF delta = event->globalPosition() - grabGlobal_;
            if (delta.isNull()) {
                event->accept();
                return;   // the pointer put back
            }
            QCursor::setPos(window() != nullptr ? window()->screen() : nullptr, grabGlobal_.toPoint());
            if (QCursor::pos() != grabGlobal_.toPoint()) {
                grabMode_ = GrabMode::Free;   // the system would not move it after all
            }
            moveCursorGrab(grabTravel_ + (horizontal_ ? delta.x() : delta.y()));
        } else if (grabMode_ == GrabMode::Free) {
            moveCursorGrab(along(event->position()) - along(grabPoint_) + grabSlack_);
        }
        event->accept();
        return;
    }
    const qreal delta = along(event->position()) - dragAnchorAlong_;
    if (dragMode_ == DragMode::Zoom) {
        setPixelsPerTick(
                zoomAnchorPixelsPerTick_ * std::exp(-delta / 120.0));
    } else {
        // the inputs follow the pointer
        const qreal deltaTimeMs =
                delta / pixelsPerTick_ * viewer_->tickDurationMs();
        queueScrub(static_cast<qint64>(std::llround(
                static_cast<qreal>(dragAnchorTimeMs_) - deltaTimeMs)));
    }
    event->accept();
}

void RaceTimelineItem::mouseReleaseEvent(QMouseEvent *event) {
    finishScrub();
    event->accept();
}

void RaceTimelineItem::mouseUngrabEvent() {
    finishScrub();
}

void RaceTimelineItem::wheelEvent(QWheelEvent *event) {
    if (viewer_ == nullptr || !viewer_->loaded() ||
        viewer_->runCount() == 0) {
        event->ignore();
        return;
    }
    const qreal steps = static_cast<qreal>(event->angleDelta().y()) / 120.0;
    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        setPixelsPerTick(pixelsPerTick_ * std::pow(1.15, steps));
    } else {
        viewer_->pause();
        viewer_->setCurrentTick(
                viewer_->currentTick() -
                static_cast<qint64>(std::llround(steps * 10.0)));
    }
    event->accept();
}

void RaceTimelineItem::queueScrub(qint64 timeMs) {
    pendingScrubTimeMs_ =
            viewer_ == nullptr
            ? 0
            : std::clamp<qint64>(
                      timeMs, 0, viewer_->timelineSeekLimitMs());
    hasPendingScrub_ = true;
    if (!lastScrubUpdate_.isValid() ||
        lastScrubUpdate_.elapsed() >= kScrubUpdateIntervalMs) {
        applyPendingScrub();
        return;
    }
    if (!scrubUpdateTimer_.isActive()) {
        scrubUpdateTimer_.start(std::max(
                1,
                kScrubUpdateIntervalMs -
                        static_cast<int>(lastScrubUpdate_.elapsed())));
    }
}

void RaceTimelineItem::applyPendingScrub() {
    if (!hasPendingScrub_) {
        return;
    }
    hasPendingScrub_ = false;
    if (viewer_ != nullptr) {
        viewer_->setTimeMs(pendingScrubTimeMs_);
    }
    if (lastScrubUpdate_.isValid()) {
        lastScrubUpdate_.restart();
    } else {
        lastScrubUpdate_.start();
    }
}

void RaceTimelineItem::finishScrub() {
    scrubUpdateTimer_.stop();
    if (dragMode_ == DragMode::Scrub || dragMode_ == DragMode::Cursor) {
        applyPendingScrub();
    } else {
        hasPendingScrub_ = false;
    }
    endCursorGrab();
    dragMode_ = DragMode::None;
    lastScrubUpdate_.invalidate();
    setCursor(QCursor(horizontal_ ? Qt::SizeHorCursor : Qt::SizeVerCursor));
}

void RaceTimelineItem::disconnectViewer() {
    if (viewer_ != nullptr) {
        disconnect(viewer_, nullptr, this, nullptr);
    }
}

void RegisterRaceViewerQmlTypes() {
    static const int settingsTypeId = qmlRegisterSingletonInstance(
            "ForeverTAS.Viewer", 1, 0, "AppSettings",
            &forevertas::app::AppSettings::Instance());
    static const int timelineTypeId = qmlRegisterType<RaceTimelineItem>(
            "ForeverTAS.Viewer", 1, 0, "RaceTimeline");
    static const int wheelTypeId =
            qmlRegisterType<forevertas::app::PanelWheelRedirector>(
                    "ForeverTAS.Viewer", 1, 0, "PanelWheelRedirector");
    static const int dockLayoutTypeId =
            qmlRegisterType<forevertas::app::DockLayoutModel>(
                    "ForeverTAS.Viewer", 1, 0, "DockLayout");
    static const int rayTracingTypeId =
            qmlRegisterType<GpuRayTracingView>(
                    "ForeverTAS.Viewer", 1, 0, "GpuRayTracingView");
    static const int renderModuleTypeId =
            qmlRegisterType<RenderModuleView>(
                    "ForeverTAS.Viewer", 1, 0, "RenderModuleView");
    static const int videoExportTypeId =
            qmlRegisterType<VideoExportController>(
                    "ForeverTAS.Viewer", 1, 0, "VideoExport");
    static const int whiteboardCanvasTypeId =
            qmlRegisterType<WhiteboardCanvasItem>(
                    "ForeverTAS.Viewer", 1, 0, "WhiteboardCanvasItem");
    Q_UNUSED(settingsTypeId);
    Q_UNUSED(timelineTypeId);
    Q_UNUSED(wheelTypeId);
    Q_UNUSED(dockLayoutTypeId);
    Q_UNUSED(rayTracingTypeId);
    Q_UNUSED(renderModuleTypeId);
    Q_UNUSED(videoExportTypeId);
    Q_UNUSED(whiteboardCanvasTypeId);
}

}  // namespace forevertas::viewer
