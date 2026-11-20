#pragma once
#include "ColorPipeline.h"
#include <functional>
namespace openframe {
class SequenceReader {
  public:
    using Reader = std::function<QImage(const QString &)>;
    explicit SequenceReader(qint64 budgetBytes, int threads = 4, Reader reader = {});
    ~SequenceReader();
    VideoFrame frame(const MediaSource &, qint64 ordinal);
    void prefetch(const MediaSource &, const QVector<qint64> &ordinals);
    void clear();
    void setBudget(qint64 bytes);
    QJsonObject diagnostics() const;
  private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
}
