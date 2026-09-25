#include "viewer/race_timeline_item.h"

#include "app/panel_wheel_redirector.h"
#include "viewer/gpu_ray_tracing_view.h"
#include "viewer/whiteboard_canvas_item.h"

#include "time_format.h"

#include <QCursor>
#include <QMouseEvent>
#include <QPainter>
#include <QQmlEngine>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace forevertas::viewer {
namespace {

constexpr qreal kMinimumPixelsPerTick = 1.0;
constexpr qreal kMaximumPixelsPerTick = 12.0;
constexpr int kScrubUpdateIntervalMs = 16;

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

QColor CentisecondTickColor(qreal pixelsPerTick, bool darkMode) {
    const QColor microColor(
            darkMode ? QStringLiteral("#343b37")
                     : QStringLiteral("#cbd1c8"));
    const QColor minorColor(
            darkMode ? QStringLiteral("#59635d")
                     : QStringLiteral("#aeb8b0"));
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
    return QString::fromStdString(FormatHumanDurationMilliseconds(
            static_cast<double>(std::max<qint64>(0, tick)) * 10.0));
}

}  // namespace

RaceTimelineItem::RaceTimelineItem(QQuickItem *parent)
    : QQuickPaintedItem(parent) {
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
    setAcceptHoverEvents(true);
    setAntialiasing(false);
    setOpaquePainting(true);
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

bool RaceTimelineItem::darkMode() const {
    return darkMode_;
}

void RaceTimelineItem::setDarkMode(bool value) {
    if (darkMode_ == value) {
        return;
    }
    darkMode_ = value;
    update();
    emit darkModeChanged();
}

void RaceTimelineItem::paint(QPainter *painter) {
    const QRectF area = boundingRect();
    painter->fillRect(
            area,
            QColor(darkMode_ ? QStringLiteral("#101412")
                             : QStringLiteral("#f4f5f2")));

    if (viewer_ == nullptr || !viewer_->loaded() ||
        viewer_->tickCount() <= 0) {
        painter->setPen(
                QColor(darkMode_ ? QStringLiteral("#778079")
                                 : QStringLiteral("#667064")));
        painter->drawText(
                area.adjusted(18.0, 18.0, -18.0, -18.0),
                Qt::AlignCenter | Qt::TextWordWrap,
                QStringLiteral("Search run inputs appear here"));
        return;
    }

    const qreal centerY = area.height() * 0.5;
    const qreal centerX = area.width() * 0.5;
    const qreal rulerWidth = area.width() < 210.0 ? 42.0 : 52.0;
    const qreal rulerRight = rulerWidth - 2.0;
    const qreal accelerationWidth = area.width() < 210.0 ? 18.0 : 24.0;
    const qreal brakeWidth = accelerationWidth * 0.5;
    const qreal controlWidth = accelerationWidth + 2.0 * brakeWidth;
    const qreal controlLeft = centerX - controlWidth * 0.5;
    const qreal controlRight = controlLeft + controlWidth;
    const qreal steeringWidth = std::max<qreal>(
            0.0,
            std::min(controlLeft - rulerWidth - 2.0,
                     area.width() - controlRight - 10.0));

    painter->fillRect(
            QRectF(0.0, 0.0, rulerWidth, area.height()),
            QColor(darkMode_ ? QStringLiteral("#0c100e")
                             : QStringLiteral("#eef2ed")));
    painter->setPen(
            QColor(darkMode_ ? QStringLiteral("#2b332f")
                             : QStringLiteral("#cbd1c8")));
    painter->drawLine(QPointF(rulerRight, 0.0),
                      QPointF(rulerRight, area.height()));

    painter->fillRect(
            QRectF(controlLeft, 0.0, controlWidth, area.height()),
            QColor(darkMode_ ? QStringLiteral("#171d1a")
                             : QStringLiteral("#e1e5df")));
    painter->setPen(
            QColor(darkMode_ ? QStringLiteral("#29312c")
                             : QStringLiteral("#cbd1c8")));
    painter->drawLine(QPointF(controlLeft, 0.0),
                      QPointF(controlLeft, area.height()));
    painter->drawLine(QPointF(controlLeft + brakeWidth, 0.0),
                      QPointF(controlLeft + brakeWidth, area.height()));
    painter->drawLine(
            QPointF(controlLeft + brakeWidth + accelerationWidth, 0.0),
            QPointF(controlLeft + brakeWidth + accelerationWidth,
                    area.height()));
    painter->drawLine(QPointF(controlRight, 0.0),
                      QPointF(controlRight, area.height()));

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
                    (area.height() - centerY) / pixelsPerTick_)) + 1);
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
            QColor(darkMode_ ? QStringLiteral("#9aa69e")
                             : QStringLiteral("#667064")));
    majorTickPen.setCosmetic(true);
    majorTickPen.setWidthF(1.0);
    QPen minorTickPen(
            QColor(darkMode_ ? QStringLiteral("#59635d")
                             : QStringLiteral("#aeb8b0")));
    minorTickPen.setCosmetic(true);
    minorTickPen.setWidthF(1.0);
    const qreal centisecondTickLength =
            CentisecondTickLength(pixelsPerTick_);
    QPen centisecondTickPen(
            CentisecondTickColor(pixelsPerTick_, darkMode_));
    centisecondTickPen.setCosmetic(true);
    centisecondTickPen.setWidthF(1.0);

    for (qint64 tick = firstScaleTick;
         tick <= lastScaleTick;
         tick += scale.smallestTicks) {
        const qreal y = centerY +
                (static_cast<qreal>(tick) - currentTickPosition) *
                        pixelsPerTick_;
        const qreal alignedY = std::floor(y) + 0.5;
        if (alignedY < 0.0 || alignedY >= area.height()) {
            continue;
        }

        const bool majorTick = tick % scale.majorTicks == 0;
        const bool centisecondTick = tick % 10 != 0;
        const bool minorTick = tick % scale.minorTicks == 0;
        const qreal tickStartX = majorTick
                ? 38.0
                : centisecondTick
                ? rulerRight - centisecondTickLength
                : minorTick
                ? 43.0
                : 47.0;
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
                    QColor(darkMode_ ? QStringLiteral("#aeb8b0")
                                     : QStringLiteral("#667064")));
            painter->drawText(
                    QRectF(2.0, alignedY - 8.0, 34.0, 16.0),
                    Qt::AlignRight | Qt::AlignVCenter,
                    FormatTimelineTime(tick));
        }
    }

    const QColor currentTickColor(
            darkMode_ ? QStringLiteral("#f3c85b")
                      : QStringLiteral("#9a5b28"));
    painter->setPen(QPen(currentTickColor, 2.0));
    painter->drawLine(QPointF(0.0, centerY),
                      QPointF(area.width(), centerY));
    painter->setBrush(currentTickColor);
    painter->setPen(Qt::NoPen);
    painter->drawPolygon(QPolygonF{
            QPointF(area.width() - 1.0, centerY),
            QPointF(area.width() - 9.0, centerY - 5.0),
            QPointF(area.width() - 9.0, centerY + 5.0)});
}

void RaceTimelineItem::mousePressEvent(QMouseEvent *event) {
    if (viewer_ == nullptr || !viewer_->loaded() ||
        viewer_->runCount() == 0) {
        event->ignore();
        return;
    }
    viewer_->pause();
    dragAnchorY_ = event->position().y();
    if (event->button() == Qt::RightButton) {
        zoomAnchorPixelsPerTick_ = pixelsPerTick_;
        dragMode_ = DragMode::Zoom;
    } else {
        dragAnchorTimeMs_ = viewer_->timeMs();
        dragMode_ = DragMode::Scrub;
    }
    event->accept();
}

void RaceTimelineItem::mouseMoveEvent(QMouseEvent *event) {
    if (dragMode_ == DragMode::None || viewer_ == nullptr) {
        event->ignore();
        return;
    }
    const qreal deltaY = event->position().y() - dragAnchorY_;
    if (dragMode_ == DragMode::Zoom) {
        setPixelsPerTick(
                zoomAnchorPixelsPerTick_ * std::exp(-deltaY / 120.0));
    } else {
        const qreal deltaTimeMs =
                deltaY / pixelsPerTick_ * viewer_->tickDurationMs();
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
    if (dragMode_ == DragMode::Scrub) {
        applyPendingScrub();
    } else {
        hasPendingScrub_ = false;
    }
    dragMode_ = DragMode::None;
    lastScrubUpdate_.invalidate();
}

void RaceTimelineItem::disconnectViewer() {
    if (viewer_ != nullptr) {
        disconnect(viewer_, nullptr, this, nullptr);
    }
}

void RegisterRaceViewerQmlTypes() {
    static const int timelineTypeId = qmlRegisterType<RaceTimelineItem>(
            "ForeverTAS.Viewer", 1, 0, "RaceTimeline");
    static const int wheelTypeId =
            qmlRegisterType<forevertas::app::PanelWheelRedirector>(
                    "ForeverTAS.Viewer", 1, 0, "PanelWheelRedirector");
    static const int rayTracingTypeId =
            qmlRegisterType<GpuRayTracingView>(
                    "ForeverTAS.Viewer", 1, 0, "GpuRayTracingView");
    static const int whiteboardCanvasTypeId =
            qmlRegisterType<WhiteboardCanvasItem>(
                    "ForeverTAS.Viewer", 1, 0, "WhiteboardCanvasItem");
    Q_UNUSED(timelineTypeId);
    Q_UNUSED(wheelTypeId);
    Q_UNUSED(rayTracingTypeId);
    Q_UNUSED(whiteboardCanvasTypeId);
}

}  // namespace forevertas::viewer
