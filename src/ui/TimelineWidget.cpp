#include "TimelineWidget.h"
#include "core/ReviewSession.h"
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
namespace openframe {
TimelineWidget::TimelineWidget(ReviewSession *s, QWidget *p) : QWidget(p), m_session(s) {
    setObjectName("globalTimeControl");
    setFixedHeight(72);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    connect(s, &ReviewSession::projectChanged, this, qOverload<>(&TimelineWidget::update));
    connect(s, &ReviewSession::frameChanged, this, qOverload<>(&TimelineWidget::update));
    setToolTip(
        "Click or drag to seek · Drag range handles to trim playback · Double-click to add a note");
}
void TimelineWidget::setRangeOnly(bool range) {
    m_rangeOnly = range;
    setFixedHeight(range ? 45 : 72);
    update();
}
qint64 TimelineWidget::frameAt(double x) const {
    const auto &p = m_session->project();
    qint64 lo = m_rangeOnly ? p.rangeIn : 0;
    qint64 hi = m_rangeOnly && p.rangeOut >= 0
                    ? p.rangeOut
                    : std::max<qint64>(0, m_session->durationFrames() - 1);
    return std::clamp(
        lo + qint64(std::llround((x - 10) / std::max(1, width() - 20) * double(hi - lo))), lo, hi);
}
double TimelineWidget::xAt(qint64 f) const {
    const auto &p = m_session->project();
    qint64 lo = m_rangeOnly ? p.rangeIn : 0;
    qint64 hi = m_rangeOnly && p.rangeOut >= 0
                    ? p.rangeOut
                    : std::max<qint64>(0, m_session->durationFrames() - 1);
    return 10 + double(f - lo) / std::max<qint64>(1, hi - lo) * std::max(1, width() - 20);
}
void TimelineWidget::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), QColor("#292b2e"));
    const auto &pr = m_session->project();
    const qint64 duration = std::max<qint64>(1, m_session->durationFrames());
    auto rangeEnd = pr.rangeOut >= 0 ? pr.rangeOut : duration - 1;
    int lineY = m_rangeOnly ? 28 : 37;
    p.fillRect(QRectF(10, lineY, width() - 20, 10), QColor("#1c1e21"));
    p.fillRect(QRectF(xAt(pr.rangeIn), lineY, std::max(1.0, xAt(rangeEnd) - xAt(pr.rangeIn)), 10),
               QColor("#5c646d"));
    if (!m_rangeOnly) {
        if (auto tl = pr.activeTimeline()) {
            qint64 offset = 0;
            int index = 0;
            for (const auto &c : tl->clips) {
                auto n = clipDurationFrames(pr, *tl, c);
                QColor col = index % 2 ? QColor("#475b68") : QColor("#506775");
                p.fillRect(
                    QRectF(xAt(offset), 52, std::max(1.0, xAt(offset + n) - xAt(offset) - 1), 16),
                    col);
                p.setPen(QColor("#c3ccd2"));
                p.drawText(QRectF(xAt(offset) + 4, 52,
                                  std::max(0.0, xAt(offset + n) - xAt(offset) - 6), 16),
                           Qt::AlignVCenter | Qt::AlignLeft, c.name);
                offset += n;
                ++index;
            }
        }
    }
    qint64 first = m_rangeOnly ? pr.rangeIn : 0, last = m_rangeOnly ? rangeEnd : duration - 1;
    double rough = double(std::max<qint64>(1, last - first)) / std::max(2, width() / 75);
    double power = std::pow(10, std::floor(std::log10(std::max(rough, 1.0))));
    qint64 step = std::max<qint64>(1, qint64(std::ceil(rough / power) * power));
    p.setFont(QFont("Segoe UI", 8));
    p.setPen(QColor("#92979e"));
    for (qint64 f = (first / step) * step; f <= last; f += step) {
        if (f < first)
            continue;
        double x = xAt(f);
        p.drawLine(QPointF(x, lineY - 7), QPointF(x, lineY - 1));
        p.drawText(QRectF(x - 25, 2, 50, 16), Qt::AlignCenter,
                   QString::number(displayFrameNumber(pr, m_session->activeOwnerId(), f)));
        if (last - f < step)
            break;
    }
    for (const auto &b : pr.bookmarks) {
        if (b.timelineId != m_session->activeOwnerId() || b.frame < first || b.frame > last ||
            !bookmarkMatches(pr, b, m_bookmarkFilter))
            continue;
        p.setPen(Qt::NoPen);
        p.setBrush(b.color);
        QPolygonF q;
        q << QPointF(xAt(b.frame) - 4, lineY - 10) << QPointF(xAt(b.frame) + 4, lineY - 10)
          << QPointF(xAt(b.frame), lineY - 5);
        p.drawPolygon(q);
        if (b.endFrame > b.frame)
            p.fillRect(QRectF(xAt(b.frame), lineY - 5, xAt(b.endFrame) - xAt(b.frame), 2), b.color);
    }
    p.setPen(QColor("#d5b467"));
    for (auto f : {pr.rangeIn, rangeEnd}) {
        double x = xAt(f);
        p.drawLine(QPointF(x, lineY - 3), QPointF(x, lineY + 13));
        p.drawLine(QPointF(x - 3, lineY + 13), QPointF(x + 3, lineY + 13));
    }
    double x = xAt(m_session->currentFrame());
    p.setPen(QPen(QColor("#72b8db"), 2));
    p.drawLine(QPointF(x, 20), QPointF(x, height() - 2));
    p.setBrush(QColor("#72b8db"));
    QPolygonF arrow;
    arrow << QPointF(x - 4, 19) << QPointF(x + 4, 19) << QPointF(x, 24);
    p.drawPolygon(arrow);
}
void TimelineWidget::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton)
        return;
    setFocus();
    if (m_pauseOnSeek)
        m_session->pause();
    auto &p = m_session->project();
    auto out = p.rangeOut >= 0 ? p.rangeOut : m_session->durationFrames() - 1;
    int y = m_rangeOnly ? 28 : 37;
    if (e->position().y() >= y - 4 && std::abs(e->position().x() - xAt(p.rangeIn)) < 7)
        m_drag = Drag::RangeIn;
    else if (e->position().y() >= y - 4 && std::abs(e->position().x() - xAt(out)) < 7)
        m_drag = Drag::RangeOut;
    else {
        m_drag = Drag::Seek;
        m_session->navigate(frameAt(e->position().x()), m_audioOnScrub);
    }
}
void TimelineWidget::mouseMoveEvent(QMouseEvent *e) {
    auto f = frameAt(e->position().x());
    if (m_drag == Drag::Seek) {
        if (f != m_session->currentFrame())
            m_session->navigate(f, m_audioOnScrub);
    } else if (m_drag == Drag::RangeIn)
        m_session->setRange(std::min(f, m_session->project().rangeOut < 0
                                            ? m_session->durationFrames() - 1
                                            : m_session->project().rangeOut),
                            (m_session->project().rangeOut < 0 ? m_session->durationFrames() - 1
                                                               : m_session->project().rangeOut));
    else if (m_drag == Drag::RangeOut)
        m_session->setRange(m_session->project().rangeIn,
                            std::max(f, m_session->project().rangeIn));
    else
        setToolTip(
            QString("Frame %1")
                .arg(displayFrameNumber(m_session->project(), m_session->activeOwnerId(), f)));
}
void TimelineWidget::mouseReleaseEvent(QMouseEvent *event) {
    if (m_drag == Drag::Seek && event->button() == Qt::LeftButton) {
        m_session->navigate(frameAt(event->position().x()), false);
        m_session->seek(m_session->currentFrame(), false);
    }
    m_drag = Drag::None;
}
void TimelineWidget::mouseDoubleClickEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton)
        emit noteRequested(frameAt(e->position().x()));
}
void TimelineWidget::wheelEvent(QWheelEvent *e) {
    m_session->step((e->modifiers() & Qt::ShiftModifier ? 10 : 1) *
                    (e->angleDelta().y() > 0 ? -1 : 1));
    e->accept();
}
} // namespace openframe
