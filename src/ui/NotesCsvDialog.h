#pragma once
#include <QStringList>
#include <QVector>
class QWidget;
namespace openframe {
class ReviewSession;
bool parseNotesCsv(const QString &text, QVector<QStringList> &rows, QString *error = nullptr);
bool importNotesCsvDialog(ReviewSession *session, QWidget *parent, const QString &path);
} // namespace openframe
