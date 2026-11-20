#pragma once
#include "Project.h"
#include <QObject>
#include <QUndoStack>
#include <memory>
#include <functional>

namespace openframe {
class ReviewSession final : public QObject {
    Q_OBJECT
  public:
    explicit ReviewSession(QObject *parent = nullptr);
    ~ReviewSession() override;
    const Project &project() const;
    qint64 currentFrame() const;
    bool playing() const;
    double playbackRate() const;
    qint64 durationFrames() const;
    QString activeOwnerId() const;
    QString projectPath() const;
    bool dirty() const;
    QImage displayImage() const;
    QUndoStack *undoStack();
    QUndoStack *annotationUndoStack(const QString &ownerId, qint64 frame, bool background = false);
    void mutateAnnotations(const QString &label, const QString &ownerId, qint64 frame,
                           bool background, const std::function<void(Project &)> &operation);
    bool busy() const;
    bool exporting() const;

    void newProject();
    bool openProject(const QString &path);
    bool saveProject(const QString &path = {});
    bool importProject(const QString &path);
    void importMedia(const QStringList &paths);
    void importImageSequence(const QStringList &paths, Rational fps = {0, 0});
    void relinkSource(const QString &sourceId, const QString &path);
    void seek(qint64 frame, bool audioScrub = true);
    void navigate(qint64 frame, bool audioScrub = true);
    void step(qint64 frames);
    void play();
    void playBookmarks();
    void pause();
    void togglePlayback();
    void setPlaybackRate(double rate);
    void setLoopMode(LoopMode mode);
    void setLoopDelay(int milliseconds, bool holdEmpty);
    bool loopDelayActive() const;
    void setSourceAudioTrack(const QString &sourceId, int streamIndex);
    void setSourceAudioOffset(const QString &sourceId, qint64 offsetUs);
    void setRange(qint64 first, qint64 last);
    void setActiveTimeline(const QString &id);
    void assignViewer(int pane, const QString &sourceOrTimelineId);
    void setViewerSettings(const ViewerSettings &settings);
    quint64 beginViewerPreview();
    bool previewComparisonMix(quint64 token, double amount);
    bool previewViewerSettings(quint64 token, const ViewerSettings &settings);
    bool previewAnnotation(quint64 token, const Annotation &annotation);
    bool commitViewerPreview(quint64 token, const QString &label);
    void clearViewerPreview(quint64 token);
    ViewerSettings displayedViewerSettings() const;
    void mutateProject(const QString &label, const std::function<void(Project &)> &operation);
    QString addTimeline(const QString &name);
    void appendSource(const QString &sourceId, const QString &timelineId = {});
    void removeSource(const QString &sourceId);
    void addBookmark(const QString &name, qint64 first, qint64 last = -1);
    void addNote(const QString &text, const QString &author = {});
    void addAnnotation(const Annotation &annotation);
    bool saveNotesCsv(const QString &path);
    bool loadNotesCsv(const QString &path);
    bool exportAsync(const ExportSettings &settings);
    void cancelExport();
    void setAudioEnabled(bool enabled);
    bool audioEnabled() const;
    void setVolume(double volume);
    double volume() const;
    void setAudioScrubEnabled(bool enabled);
    bool audioScrubEnabled() const;
    void setAudioTimeStretchingEnabled(bool enabled);
    bool audioTimeStretchingEnabled() const;
    void setLoopOnStepScrub(bool enabled);
    bool loopOnStepScrub() const;
    void setLoopStepScrubToRange(bool enabled);
    bool loopStepScrubToRange() const;
    void setLockStepScrubToRange(bool enabled);
    bool lockStepScrubToRange() const;
    void setAutoplay(bool enabled);
    bool autoplay() const;
    void setHideAnnotationsDuringPlayback(bool enabled);
    bool hideAnnotationsDuringPlayback() const;
    QJsonObject audioDiagnostics() const;
    void setPlaybackFrameStep(int step);
    int playbackFrameStep() const;
    void setBookmarkPlayback(bool enabled);
    bool bookmarkPlayback() const;
    void setReverseAudioEnabled(bool enabled);
    bool reverseAudioEnabled() const;
    void setProjectUndoEnabled(bool enabled);
    bool projectUndoEnabled() const;
    void setProjectDefaults(Rational sequenceFps, qint64 displayOffset, bool addTimeline,
                            bool activateTimeline, const QString &sourceRootDirectory);
    void setProjectLoadOptions(bool restorePanZoom, bool restoreHold);
    void setCacheLimitMiB(int megabytes);

  signals:
    void projectChanged();
    void frameChanged(qint64 frame);
    void imageReady(const QImage &image);
    void playbackChanged(bool playing);
    void transportSettingsChanged();
    void error(const QString &message);
    void status(const QString &message);
    void busyChanged(bool busy);
    void exportProgress(int percent);
    void exportFinished(bool success, const QString &message, const QString &publishedFile);

  private:
    friend class ReviewSessionTestAccess;
    struct Impl;
    std::unique_ptr<Impl> d;
    void applyProject(const Project &project);
    void requestFrame(bool scrub = false);
    void tick();
    void startPlayback();
    void seekInternal(qint64 frame, bool navigation, bool audioScrub, bool allowProgress = false);
    void refreshLegacyMetadata();
};
} // namespace openframe
