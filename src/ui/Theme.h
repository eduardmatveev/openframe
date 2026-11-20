#pragma once
#include <QIcon>
#include <QColor>
#include <QString>
class QApplication;
namespace openframe {
QIcon uiIcon(const QString &name, const QColor &color = QColor(195, 198, 202));
void applyDarkTheme(QApplication &app);
} // namespace openframe
