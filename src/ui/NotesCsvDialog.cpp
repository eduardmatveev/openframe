#include "NotesCsvDialog.h"
#include "core/ReviewSession.h"
#include "integration/LocalPathPolicy.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
namespace openframe {
bool parseNotesCsv(const QString &text, QVector<QStringList> &rows, QString *error) {
    rows.clear();
    if (error)
        error->clear();
    QString cell;
    QStringList row;
    bool quoted = false, closed = false;
    for (qsizetype i = 0; i < text.size(); ++i) {
        QChar c = text[i];
        if (i == 0 && c.unicode() == 0xfeff)
            continue;
        if (quoted) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    cell += '"';
                    ++i;
                } else {
                    quoted = false;
                    closed = true;
                }
            } else
                cell += c;
            continue;
        }
        if (c == '"') {
            if (!cell.isEmpty() || closed) {
                if (error)
                    *error = "Unexpected quote in CSV cell";
                return false;
            }
            quoted = true;
            continue;
        }
        if (c == ',' || c == '\r' || c == '\n') {
            row << cell;
            cell.clear();
            closed = false;
            if (c != ',') {
                if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
                    ++i;
                if (row.size() > 1 || !row.first().isEmpty())
                    rows << row;
                row.clear();
            }
            continue;
        }
        if (closed) {
            if (c.isSpace())
                continue;
            if (error)
                *error = "Unexpected data after quoted CSV cell";
            return false;
        }
        cell += c;
    }
    if (quoted) {
        if (error)
            *error = "Unterminated quoted CSV cell";
        return false;
    }
    if (!cell.isEmpty() || !row.isEmpty() || closed) {
        row << cell;
        rows << row;
    }
    if (rows.isEmpty() || rows.first().size() < 2) {
        if (error)
            *error = "A CSV header with at least two columns is required";
        return false;
    }
    for (qsizetype i = 1; i < rows.size(); ++i)
        if (rows[i].size() != rows.first().size()) {
            if (error)
                *error = QString("CSV row %1 has a different column count").arg(i + 1);
            return false;
        }
    return true;
}
bool importNotesCsvDialog(ReviewSession *session, QWidget *parent, const QString &path) {
    QString error;
    QString safe = LocalPathPolicy::validateInput(path, &error);
    if (safe.isEmpty()) {
        QMessageBox::warning(parent, "Import CSV", error);
        return false;
    }
    QFile file(safe);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024 * 1024) {
        QMessageBox::warning(parent, "Import CSV", "Cannot read CSV, or it exceeds 16 MiB.");
        return false;
    }
    QVector<QStringList> rows;
    if (!parseNotesCsv(QString::fromUtf8(file.readAll()), rows, &error)) {
        QMessageBox::warning(parent, "Import CSV", error);
        return false;
    }
    QStringList headers = rows.first();
    QDialog dialog(parent);
    dialog.setWindowTitle("Import Notes — Column Select & Preview");
    dialog.resize(640, 480);
    auto layout = new QVBoxLayout(&dialog);
    auto form = new QFormLayout;
    auto frame = new QComboBox(&dialog), text = new QComboBox(&dialog),
         author = new QComboBox(&dialog);
    frame->addItems(headers);
    text->addItems(headers);
    author->addItem("No author");
    author->addItems(headers);
    int fi = -1, ti = -1, ai = -1, ownerIndex = -1, createdIndex = -1, colorIndex = -1;
    for (int i = 0; i < headers.size(); ++i) {
        auto header = headers[i].trimmed().toLower();
        if (header == "frame" || header == "frames")
            fi = i;
        if (header == "text" || header == "notes" || header == "note")
            ti = i;
        if (header == "author")
            ai = i;
        if (header == "timeline" || header == "owner")
            ownerIndex = i;
        if (header == "created")
            createdIndex = i;
        if (header == "color" || header == "label")
            colorIndex = i;
    }
    frame->setCurrentIndex(fi >= 0 ? fi : 0);
    text->setCurrentIndex(ti >= 0 ? ti : 1);
    author->setCurrentIndex(ai + 1);
    form->addRow("Frame column", frame);
    form->addRow("Notes column", text);
    form->addRow("Author column", author);
    auto preserve = new QCheckBox("Preserve matching timeline / source IDs", &dialog);
    preserve->setChecked(ownerIndex >= 0);
    preserve->setEnabled(ownerIndex >= 0);
    form->addRow(preserve);
    auto displayedNumbers = new QCheckBox("CSV frame numbers use displayed numbering", &dialog);
    displayedNumbers->setChecked(
        !(headers.contains("id") && headers.contains("timeline") && headers.contains("frame")));
    form->addRow(displayedNumbers);
    layout->addLayout(form);
    auto preview = new QTableWidget(0, 3, &dialog);
    preview->setHorizontalHeaderLabels({"Frame", "Note", "Author"});
    preview->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    preview->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    preview->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    preview->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(preview, 1);
    auto summary = new QLabel(&dialog);
    summary->setWordWrap(true);
    layout->addWidget(summary);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText("Import Notes");
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto refresh = [&] {
        int invalid = 0;
        preview->setRowCount(std::min(100, int(rows.size() - 1)));
        for (int i = 1; i < rows.size(); ++i) {
            bool ok = false;
            auto f = rows[i].value(frame->currentIndex()).trimmed().toLongLong(&ok);
            QString owner = session->activeOwnerId();
            if (preserve->isChecked()) {
                QString candidate = rows[i].value(ownerIndex);
                if (session->project().timeline(candidate) || session->project().source(candidate))
                    owner = candidate;
            }
            if (displayedNumbers->isChecked())
                f = frameFromDisplay(session->project(), owner, f);
            if (!ok || f < 0)
                ++invalid;
            if (i <= 100) {
                preview->setItem(i - 1, 0,
                                 new QTableWidgetItem(rows[i].value(frame->currentIndex())));
                preview->setItem(i - 1, 1,
                                 new QTableWidgetItem(rows[i].value(text->currentIndex())));
                preview->setItem(
                    i - 1, 2,
                    new QTableWidgetItem(author->currentIndex()
                                             ? rows[i].value(author->currentIndex() - 1)
                                             : QString()));
            }
        }
        bool distinct = frame->currentIndex() != text->currentIndex();
        summary->setText(
            !distinct ? "Choose different columns for frame and note."
            : invalid
                ? QString("%1 rows have invalid frame numbers. Choose a numeric frame column.")
                      .arg(invalid)
                : QString("%1 notes will be imported. Preview shows the first %2 rows.")
                      .arg(rows.size() - 1)
                      .arg(std::min(100, int(rows.size() - 1))));
        buttons->button(QDialogButtonBox::Ok)
            ->setEnabled(distinct && invalid == 0 && rows.size() > 1);
        preview->resizeRowsToContents();
    };
    QObject::connect(displayedNumbers, &QCheckBox::toggled, &dialog, [&] { refresh(); });
    QObject::connect(preserve, &QCheckBox::toggled, &dialog, [&] { refresh(); });
    QObject::connect(frame, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
                     [&] { refresh(); });
    QObject::connect(text, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
                     [&] { refresh(); });
    QObject::connect(author, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
                     [&] { refresh(); });
    refresh();
    if (dialog.exec() != QDialog::Accepted)
        return false;
    QVector<Note> notes;
    for (int i = 1; i < rows.size(); ++i) {
        Note note;
        note.id = newId();
        note.timelineId = session->activeOwnerId();
        if (preserve->isChecked()) {
            QString owner = rows[i].value(ownerIndex);
            if (session->project().timeline(owner) || session->project().source(owner))
                note.timelineId = owner;
        }
        note.frame = rows[i].value(frame->currentIndex()).trimmed().toLongLong();
        if (displayedNumbers->isChecked())
            note.frame = frameFromDisplay(session->project(), note.timelineId, note.frame);
        note.text = rows[i].value(text->currentIndex());
        note.author =
            author->currentIndex() ? rows[i].value(author->currentIndex() - 1) : QString();
        if (createdIndex >= 0) {
            auto created = QDateTime::fromString(rows[i].value(createdIndex), Qt::ISODateWithMs);
            if (created.isValid())
                note.created = created;
        }
        if (colorIndex >= 0) {
            QColor label(rows[i].value(colorIndex));
            if (label.isValid())
                note.color = label;
        }
        notes << note;
    }
    session->mutateProject("Import CSV notes", [notes](Project &p) { p.notes += notes; });
    return true;
}
} // namespace openframe
