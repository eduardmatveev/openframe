#include "LocalFileDialog.h"
#include "Theme.h"
#include "integration/LocalPathPolicy.h"
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QTreeWidget>
#include <QVBoxLayout>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace openframe {
namespace {
QString lexicalAbsolute(QString path) {
    path.replace('\\', '/');
    if (QDir::isRelativePath(path))
        path = QDir::currentPath() + "/" + path;
    return path;
}
QString lexicalParent(const QString &path) {
    auto index = path.lastIndexOf('/');
    if (index < 0)
        return {};
    if (index == 2 && path.size() > 1 && path[1] == ':')
        return path.left(3);
    if (index == 0)
        return "/";
    return path.left(index);
}
QStringList localRoots() {
    QStringList roots;
#ifdef Q_OS_WIN
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i)
        if (mask & (1u << i)) {
            QString root = QString(QChar('A' + i)) + ":/";
            if (!LocalPathPolicy::validateDirectory(root).isEmpty())
                roots << root;
        }
#else
    if (!LocalPathPolicy::validateDirectory("/").isEmpty())
        roots << "/";
#endif
    return roots;
}
} // namespace
QString createLocalDirectoryChain(const QString &input, QString *error) {
    if (error)
        error->clear();
    QString path = lexicalAbsolute(input);
    QString checkError;
    QString normalized = input;
    normalized.replace('\\', '/');
    if (input.isEmpty() || normalized.startsWith("//") || normalized.contains("://") ||
        normalized.split('/').contains("..")) {
        if (error)
            *error = "Only explicit local directory paths without parent traversal are accepted";
        return {};
    }
    QString existing = LocalPathPolicy::validateDirectory(path, &checkError);
    if (!existing.isEmpty())
        return existing;
    // Validate lexical prefixes before any existence query. validateOutput allows a
    // missing final component only after checking every ancestor without following links.
    QStringList pending;
    QString cursor = path;
    for (int i = 0; i < 256; ++i) {
        QString allowed = LocalPathPolicy::validateOutput(cursor, &checkError);
        if (!allowed.isEmpty()) {
            pending.prepend(cursor);
            auto parent = lexicalParent(cursor);
            if (!LocalPathPolicy::validateDirectory(parent, &checkError).isEmpty()) {
                for (const auto &dir : pending) {
                    auto output = LocalPathPolicy::validateOutput(dir, &checkError);
                    if (output.isEmpty() || !QDir().mkdir(output)) {
                        if (error)
                            *error = checkError.isEmpty() ? "Could not create local directory"
                                                          : checkError;
                        return {};
                    }
                    if (LocalPathPolicy::validateDirectory(output, &checkError).isEmpty()) {
                        if (error)
                            *error = checkError;
                        return {};
                    }
                }
                return LocalPathPolicy::validateDirectory(path, error);
            }
            cursor = parent;
        } else {
            auto parent = lexicalParent(cursor);
            if (parent.isEmpty() || parent == cursor) {
                if (error)
                    *error = checkError;
                return {};
            }
            pending.prepend(cursor);
            cursor = parent;
        }
    }
    if (error)
        *error = "Too many directory components";
    return {};
}
LocalFileDialog::LocalFileDialog(QWidget *parent, const QString &title, const QString &initial,
                                 const QString &filter, Mode mode)
    : QDialog(parent), m_mode(mode) {
    setWindowTitle(title);
    resize(760, 520);
    auto layout = new QVBoxLayout(this);
    auto pathRow = new QHBoxLayout;
    auto roots = m_roots = new QComboBox(this);
    roots->setObjectName("localDrive");
    roots->addItems(localRoots());
    roots->setMinimumWidth(65);
    pathRow->addWidget(roots);
    m_path = new QLineEdit(this);
    m_path->setObjectName("localDirectory");
    m_path->setPlaceholderText("Local folder path");
    pathRow->addWidget(m_path, 1);
    auto go = new QPushButton("Go", this), up = new QPushButton("Up", this);
    pathRow->addWidget(go);
    pathRow->addWidget(up);
    layout->addLayout(pathRow);
    m_entries = new QTreeWidget(this);
    m_entries->setHeaderLabels({"Name", "Size", "Modified"});
    m_entries->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_entries->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_entries->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_entries->setRootIsDecorated(false);
    m_entries->setSelectionMode(mode == Mode::OpenMany ? QAbstractItemView::ExtendedSelection
                                                       : QAbstractItemView::SingleSelection);
    m_entries->setSortingEnabled(true);
    layout->addWidget(m_entries, 1);
    m_name = new QLineEdit(this);
    m_name->setPlaceholderText(mode == Mode::Directory ? "Current directory"
                                                       : "File name or local file path");
    if (mode != Mode::Directory)
        layout->addWidget(m_name);
    m_filter = new QComboBox(this);
    m_filter->addItems(filter.isEmpty() ? QStringList{"All files (*)"} : filter.split(";;"));
    if (mode != Mode::Directory)
        layout->addWidget(m_filter);
    m_error = new QLabel(this);
    m_error->setWordWrap(true);
    m_error->setStyleSheet("color:#e4a27e;");
    layout->addWidget(m_error);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Open)
        ->setText(mode == Mode::Save        ? "Save"
                  : mode == Mode::Directory ? "Select Folder"
                                            : "Open");
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { acceptSelection(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(go, &QPushButton::clicked, this, [this] { navigate(m_path->text()); });
    connect(m_path, &QLineEdit::returnPressed, this, [this] { navigate(m_path->text()); });
    connect(m_name, &QLineEdit::returnPressed, this, [this] { acceptSelection(); });
    connect(up, &QPushButton::clicked, this, [this] { navigate(lexicalParent(m_directory)); });
    connect(roots, qOverload<int>(&QComboBox::activated), this,
            [this, roots](int i) { navigate(roots->itemText(i)); });
    connect(m_filter, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] { fillEntries(); });
    connect(m_entries, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        QString path = m_directory + "/" + item->text(0);
        if (item->data(0, Qt::UserRole).toBool())
            navigate(path);
        else if (m_mode != Mode::Directory) {
            m_name->setText(item->text(0));
            acceptSelection();
        }
    });
    connect(m_entries, &QTreeWidget::itemSelectionChanged, this, [this] {
        auto items = m_entries->selectedItems();
        if (items.size() == 1 && !items.first()->data(0, Qt::UserRole).toBool())
            m_name->setText(items.first()->text(0));
    });
    QString start = initial, initialName;
    if (!start.isEmpty()) {
        auto valid = LocalPathPolicy::validateDirectory(start);
        if (valid.isEmpty()) {
            QString candidate = lexicalAbsolute(start);
            QString output = LocalPathPolicy::validateOutput(candidate);
            if (!output.isEmpty()) {
                initialName = output.mid(output.lastIndexOf('/') + 1);
                start = lexicalParent(output);
            } else
                start.clear();
        } else
            start = valid;
    }
    if (start.isEmpty()) {
        start = LocalPathPolicy::validateDirectory(QDir::currentPath());
        if (start.isEmpty() && roots->count())
            start = roots->itemText(0);
    }
    navigate(start);
    m_name->setText(initialName);
}
void LocalFileDialog::navigate(const QString &path) {
    QString error;
    auto safe = LocalPathPolicy::validateDirectory(path, &error);
    if (safe.isEmpty()) {
        m_error->setText(error);
        return;
    }
    m_directory = safe;
    m_path->setText(safe);
    for (int i = 0; i < m_roots->count(); ++i)
        if (safe.startsWith(m_roots->itemText(i), Qt::CaseInsensitive)) {
            m_roots->setCurrentIndex(i);
            break;
        }
    m_error->clear();
    m_name->clear();
    fillEntries();
}
void LocalFileDialog::fillEntries() {
    if (m_directory.isEmpty())
        return;
    QString error;
    if (LocalPathPolicy::validateDirectory(m_directory, &error).isEmpty()) {
        m_error->setText(error);
        return;
    }
    m_entries->clear();
    QString filter = m_filter->currentText();
    QRegularExpression parentheses("\\(([^)]+)\\)");
    auto match = parentheses.match(filter);
    QStringList patterns =
        match.hasMatch() ? match.captured(1).split(' ', Qt::SkipEmptyParts) : QStringList{"*"};
    auto add = [&](const QString &name, bool directory, quint64 size, const QString &date) {
        if (name == "." || name == "..")
            return;
        if (!directory && (m_mode == Mode::Directory || !QDir::match(patterns, name)))
            return;
        auto item = new QTreeWidgetItem(
            m_entries, {name,
                        directory            ? QString()
                        : size < 1024        ? QString::number(size) + " B"
                        : size < 1024 * 1024 ? QString::number(size / 1024.0, 'f', 1) + " KB"
                                             : QString::number(size / 1048576.0, 'f', 1) + " MB",
                        date});
        item->setData(0, Qt::UserRole, directory);
        item->setIcon(0, uiIcon(directory ? "folder" : "media"));
    };
#ifdef Q_OS_WIN
    WIN32_FIND_DATAW entryData{};
    QString glob = QDir::toNativeSeparators(m_directory + "/*");
    HANDLE search =
        FindFirstFileExW(reinterpret_cast<LPCWSTR>(glob.utf16()), FindExInfoBasic, &entryData,
                         FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (entryData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                continue;
            QString name = QString::fromWCharArray(entryData.cFileName);
            bool directory = (entryData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            quint64 size = (quint64(entryData.nFileSizeHigh) << 32) | entryData.nFileSizeLow;
            ULARGE_INTEGER ticks{};
            ticks.LowPart = entryData.ftLastWriteTime.dwLowDateTime;
            ticks.HighPart = entryData.ftLastWriteTime.dwHighDateTime;
            qint64 ms = qint64(ticks.QuadPart / 10000) - 11644473600000LL;
            add(name, directory, size,
                QDateTime::fromMSecsSinceEpoch(ms).toString("yyyy-MM-dd hh:mm"));
        } while (FindNextFileW(search, &entryData));
        FindClose(search);
    }
#else
    for (const auto &name :
         QDir(m_directory).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
        QString path = QDir(m_directory).filePath(name);
        bool directory = !LocalPathPolicy::validateDirectory(path).isEmpty();
        if (!directory && LocalPathPolicy::validateInput(path).isEmpty())
            continue;
        QFileInfo info(path);
        add(name, directory, quint64(info.size()),
            info.lastModified().toString("yyyy-MM-dd hh:mm"));
    }
#endif
    m_entries->sortItems(0, Qt::AscendingOrder);
}
void LocalFileDialog::acceptSelection() {
    m_selected.clear();
    QString error;
    if (m_mode == Mode::Directory) {
        QString path = m_directory;
        auto items = m_entries->selectedItems();
        if (items.size() == 1 && items.first()->data(0, Qt::UserRole).toBool())
            path += "/" + items.first()->text(0);
        auto safe = LocalPathPolicy::validateDirectory(path, &error);
        if (safe.isEmpty()) {
            m_error->setText(error);
            return;
        }
        m_selected << safe;
        accept();
        return;
    }
    if (m_mode == Mode::OpenMany && m_entries->selectedItems().size() > 1) {
        for (auto item : m_entries->selectedItems()) {
            if (item->data(0, Qt::UserRole).toBool())
                continue;
            auto safe = LocalPathPolicy::validateInput(m_directory + "/" + item->text(0), &error);
            if (safe.isEmpty()) {
                m_error->setText(error);
                m_selected.clear();
                return;
            }
            m_selected << safe;
        }
        if (!m_selected.isEmpty())
            accept();
        return;
    }
    QString text = m_name->text().trimmed();
    if (text.isEmpty()) {
        auto items = m_entries->selectedItems();
        if (items.size() == 1 && items.first()->data(0, Qt::UserRole).toBool()) {
            navigate(m_directory + "/" + items.first()->text(0));
            return;
        }
        m_error->setText("Enter a file name.");
        return;
    }
    QString path = QDir::isRelativePath(text) ? m_directory + "/" + text : text;
    if (m_mode == Mode::Save) {
        QString leaf = path.mid(path.lastIndexOf('/') + 1);
        if (!leaf.contains('.')) {
            auto match = QRegularExpression("\\*\\.([a-zA-Z0-9]+)").match(m_filter->currentText());
            if (match.hasMatch())
                path += '.' + match.captured(1);
        }
        auto safe = LocalPathPolicy::validateOutput(path, &error);
        if (safe.isEmpty()) {
            m_error->setText(error);
            return;
        }
        if (!LocalPathPolicy::validateInput(safe).isEmpty() &&
            QMessageBox::question(
                this, "Replace file?", "The local file already exists. Replace it?",
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
        m_selected << safe;
    } else {
        auto safe = LocalPathPolicy::validateInput(path, &error);
        if (safe.isEmpty()) {
            auto dir = LocalPathPolicy::validateDirectory(path);
            if (!dir.isEmpty()) {
                navigate(dir);
                return;
            }
            m_error->setText(error);
            return;
        }
        m_selected << safe;
    }
    accept();
}
QStringList LocalFileDialog::localFiles(const QString &directory, const QStringList &patterns) {
    QStringList paths;
    QString safe = LocalPathPolicy::validateDirectory(directory);
    if (safe.isEmpty())
        return paths;
#ifdef Q_OS_WIN
    WIN32_FIND_DATAW entryData{};
    QString glob = QDir::toNativeSeparators(safe + "/*");
    HANDLE search =
        FindFirstFileExW(reinterpret_cast<LPCWSTR>(glob.utf16()), FindExInfoBasic, &entryData,
                         FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (entryData.dwFileAttributes &
                (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
                continue;
            QString name = QString::fromWCharArray(entryData.cFileName);
            if (!QDir::match(patterns, name))
                continue;
            auto path = LocalPathPolicy::validateInput(safe + "/" + name);
            if (!path.isEmpty())
                paths << path;
        } while (FindNextFileW(search, &entryData));
        FindClose(search);
    }
#else
    for (const auto &name :
         QDir(safe).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::NoSymLinks))
        if (QDir::match(patterns, name)) {
            auto path = LocalPathPolicy::validateInput(safe + "/" + name);
            if (!path.isEmpty())
                paths << path;
        }
#endif
    paths.sort(Qt::CaseInsensitive);
    return paths;
}
QStringList LocalFileDialog::getOpenFileNames(QWidget *p, const QString &t, const QString &d,
                                              const QString &f) {
    LocalFileDialog dialog(p, t, d, f, Mode::OpenMany);
    return dialog.exec() == QDialog::Accepted ? dialog.m_selected : QStringList{};
}
QString LocalFileDialog::getOpenFileName(QWidget *p, const QString &t, const QString &d,
                                         const QString &f) {
    LocalFileDialog dialog(p, t, d, f, Mode::Open);
    return dialog.exec() == QDialog::Accepted ? dialog.m_selected.value(0) : QString();
}
QString LocalFileDialog::getSaveFileName(QWidget *p, const QString &t, const QString &d,
                                         const QString &f) {
    LocalFileDialog dialog(p, t, d, f, Mode::Save);
    return dialog.exec() == QDialog::Accepted ? dialog.m_selected.value(0) : QString();
}
QString LocalFileDialog::getExistingDirectory(QWidget *p, const QString &t, const QString &d) {
    LocalFileDialog dialog(p, t, d, {}, Mode::Directory);
    return dialog.exec() == QDialog::Accepted ? dialog.m_selected.value(0) : QString();
}
} // namespace openframe
