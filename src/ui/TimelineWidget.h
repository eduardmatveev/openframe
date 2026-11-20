#pragma once
#include <QWidget>
#include "BookmarkFilter.h"
namespace openframe {
class ReviewSession;
class TimelineWidget final : public QWidget {
    Q_OBJECT
  public:
    explicit TimelineWidget(ReviewSession *, QWidget *parent = nullptr);
    void setRangeOnly(bool rangeOnly);
    void setPauseOnSeek(bool enabled) {
        m_pauseOnSeek = enabled;
    }
    void setAudioOnScrub(bool enabled) {
        m_audioOnScrub = enabled;
    }
    void setBookmarkFilter(BookmarkFilter filter) {
        m_bookmarkFilter = filter;
        update();
    }
  signals:
    void noteRequested(qint64 frame);

  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;

  private:
    qint64 frameAt(double x) const;
    double xAt(qint64 frame) const;
    ReviewSession *m_session;
    BookmarkFilter m_bookmarkFilter = BookmarkFilter::All;
    bool m_rangeOnly = false, m_pauseOnSeek = true, m_audioOnScrub = true;
    enum class Drag { None, Seek, RangeIn, RangeOut };
    Drag m_drag = Drag::None;
};
} // namespace openframe
