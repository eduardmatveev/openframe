#pragma once
#include <QMainWindow>
#include <QHash>
#include <QPointer>
#include <QVector>
#include <QKeySequence>
#include <functional>
#include "core/Project.h"
#include "BookmarkFilter.h"
class QAction;
class QMenu;
class QComboBox;
class QDockWidget;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QProgressDialog;
class QSpinBox;
class QSlider;
class QTableWidget;
class QTextEdit;
class QToolBar;
class QTreeWidget;
class QTreeWidgetItem;
class QTimer;
class QUndoStack;
namespace openframe {
class ReviewSession;
class ApiServer;
class ViewerWidget;
class TimelineWidget;
class MainWindow final : public QMainWindow {
    Q_OBJECT
  public:
    explicit MainWindow(ReviewSession *, ApiServer *server = nullptr, QWidget *parent = nullptr);
    ViewerWidget *viewer() const {
        return m_viewer;
    }
    QList<QAction *> commandActions() const;
    void renderSyntheticPreview(const QImage &image);
    QJsonObject captureSyntheticDialogs(const QString &directory);
    QJsonObject captureSyntheticTransientTools(const QString &directory);
    QJsonObject captureSyntheticDrawingTools(const QString &directory);
    void setSmokeMode(bool smoke) {
        m_smokeMode = smoke;
    }

  protected:
    void closeEvent(QCloseEvent *) override;
    void dragEnterEvent(QDragEnterEvent *) override;
    void dropEvent(QDropEvent *) override;

  private:
    QAction *action(const QString &id, const QString &label, const QString &icon,
                    const QList<QKeySequence> &shortcuts, const std::function<void()> &callback,
                    bool checkable = false);
    QUndoStack *activeUndoStack();
    void refreshUndoActions();
    void ghostingDialog();
    void overlayDialog();
    void setOverlayType(OverlayType type);
    void loopDelayDialog();
    void createMenus();
    void createToolbars();
    void createDocks();
    void refreshProject();
    void refreshFrame(qint64);
    void refreshDetails();
    void refreshNotes();
    void refreshClips();
    void refreshViewerSelectors();
    void importMediaPaths(const QStringList &paths);
    void refreshTransportState();
    void importFiles();
    void importFolder();
    void importSequence();
    void openProject();
    void saveAs();
    bool confirmClose();
    void addFolder();
    void addTimeline();
    void appendSelected();
    void deleteSelected();
    void renameSelected();
    void relinkSelected();
    void projectContextMenu(QPoint);
    void clipContextMenu(QPoint);
    void noteContextMenu(QPoint);
    void moveClip(int);
    void editClip();
    void replaceClipAudio();
    void removeClip();
    void editSelectedProperties();
    void assignSelected(int);
    void changeViewer(std::function<void(ViewerSettings &)>);
    void toggleBookmark(bool range);
    void jumpBookmark(int direction, bool ranges = false);
    void jumpEdit(int direction);
    void editNote(const QString &noteId = {});
    void copyBookmark(bool cut);
    void pasteBookmark();
    void setBookmarkFilter(BookmarkFilter);
    void removeEmptyBookmarks();
    void exportDialog();
    void preferencesDialog();
    void projectSettingsDialog();
    void mayaDialog();
    void documentationDialog();
    void resetLayout();
    void updateTitle();
    void applyPreferences();
    void storeSettings();
    QString selectedSourceId() const;
    QString selectedTimelineId() const;
    void log(const QString &text);
    void showError(const QString &text);
    void updateRecent(const QString &key, const QString &path);
    void rebuildRecentMenus();
    ReviewSession *m_session;
    ApiServer *m_server;
    ViewerWidget *m_viewer;
    TimelineWidget *m_timeline;
    TimelineWidget *m_rangeTimeline;
    QHash<QString, QAction *> m_actions;
    QHash<QString, QList<QKeySequence>> m_defaultShortcuts;
    QTreeWidget *m_projectTree;
    QTableWidget *m_clipTable;
    QTableWidget *m_notesTable;
    QDockWidget *m_projectDock;
    QDockWidget *m_detailsDock;
    QDockWidget *m_notesDock;
    QDockWidget *m_consoleDock;
    QLabel *m_thumbnail;
    QLabel *m_mediaInfo;
    QLabel *m_frameLabel;
    QLabel *m_durationLabel;
    QLabel *m_zoomLabel;
    QLabel *m_networkLabel;
    QLabel *m_selectionLabel;
    QTextEdit *m_console;
    QToolBar *m_viewerBar;
    QToolBar *m_playbackBar;
    QToolBar *m_annotationBar;
    QToolBar *m_annotationOptions;
    QComboBox *m_viewerSelectors[4]{};
    QComboBox *m_speed;
    QComboBox *m_loop;
    QComboBox *m_bookmarkFilterCombo;
    BookmarkFilter m_bookmarkFilter = BookmarkFilter::All;
    QSlider *m_mix;
    QSlider *m_volume;
    QAction *m_volumeControlAction = nullptr;
    QAction *m_volumePopupAction = nullptr;
    QSpinBox *m_frameSpin;
    QSpinBox *m_rangeIn;
    QSpinBox *m_rangeOut;
    QPointer<QProgressDialog> m_exportProgress;
    QMenu *m_recentFilesMenu = nullptr;
    QMenu *m_recentProjectsMenu = nullptr;
    QTimer *m_bookmarkTimer;
    QVector<Annotation> m_annotationClipboard;
    QByteArray m_defaultLayout;
    bool m_pauseOnSeek = true, m_highlightCurrentClip = true, m_rangeTransport = false;
    bool m_firstPreferencesApply = true;
    bool m_refreshing = false;
    bool m_smokeMode = false;
    bool m_hideAnnotationsWhilePlaying = true;
    bool m_sideRight = false;
    OverlayType m_lastOverlayType = OverlayType::Grid;
    QString m_lastTabletDiagnostic;
};
} // namespace openframe
