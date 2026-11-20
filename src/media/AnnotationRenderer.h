#pragma once
#include <QImage>
#include <QString>
namespace openframe {
struct Project;
QImage renderAnnotationLayer(const Project &project, const QString &owner, qint64 frame,
                             QSize size);
} // namespace openframe
