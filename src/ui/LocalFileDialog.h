#pragma once
#include <QDialog>
#include <QStringList>
class QComboBox;
class QLabel;
class QLineEdit;
class QTreeWidget;
namespace openframe {
// Filesystem navigation is checked before enumeration. No native shell dialogs,
// QFileSystemModel, network locations, thumbnails or shell icon providers.
class LocalFileDialog final : public QDialog {
  public:
    static QStringList localFiles(const QString &directory, const QStringList &patterns);
    static QStringList getOpenFileNames(QWidget *parent, const QString &title,
                                        const QString &directory = {}, const QString &filter = {});
    static QString getOpenFileName(QWidget *parent, const QString &title,
                                   const QString &directory = {}, const QString &filter = {});
    static QString getSaveFileName(QWidget *parent, const QString &title,
                                   const QString &directory = {}, const QString &filter = {});
    static QString getExistingDirectory(QWidget *parent, const QString &title,
                                        const QString &directory = {});

  private:
    enum class Mode { Open, OpenMany, Save, Directory };
    LocalFileDialog(QWidget *, const QString &, const QString &, const QString &, Mode);
    void navigate(const QString &path);
    void acceptSelection();
    void fillEntries();
    QStringList m_selected;
    QString m_directory;
    Mode m_mode;
    QLineEdit *m_path;
    QLineEdit *m_name;
    QComboBox *m_filter;
    QComboBox *m_roots;
    QTreeWidget *m_entries;
    QLabel *m_error;
};
// Validates each existing ancestor before optionally creating the next directory.
// Used by the synthetic capture mode; never follows a reparse point or UNC path.
QString createLocalDirectoryChain(const QString &path, QString *error = nullptr);
} // namespace openframe
