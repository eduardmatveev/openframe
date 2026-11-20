#pragma once

#include <QColor>
#include <QDateTime>
#include <QImage>
#include <QJsonObject>
#include <QMap>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QStringList>
#include <QVector>
#include <optional>

namespace openframe {

struct Rational {
    qint64 num = 24;
    qint64 den = 1;
    double value() const;
    bool valid() const;
    qint64 frameToUs(qint64 frame) const;
    qint64 usToFrame(qint64 us) const;
    bool operator==(const Rational &) const = default;
};

enum class SourceKind { Video, Audio, Image, Sequence };
enum class LoopMode { Once, Loop, PingPong };
// Stored numerically in project files. Keep existing mode values stable.
enum class ViewerMode { Single, AB, Wipe, Blend, Split, Grid, SingleB };
enum class AnnotationTool { Pencil, Arrow, Rectangle, Ellipse, Text, Eraser };
enum class OverlayType { Off, SafeArea, Grid, Center, DynamicSymmetry, PhiGrid, Custom, Image };
enum class OverlayShapeType { Line, Rectangle, Ellipse, Polygon, Text };

struct VideoColorInfo {
    QString pixelFormat;
    int componentBits = 0; // 0 = unknown, never inferred from container/codec name.
    QString primaries = QStringLiteral("unknown");
    QString transfer = QStringLiteral("unknown");
    QString matrix = QStringLiteral("unknown");
    QString range = QStringLiteral("unknown");
    QString chromaLocation = QStringLiteral("unspecified");
    double masteringMinNits = 0;
    double masteringMaxNits = 0;
    double maxCLL = 0;
    double maxFALL = 0;
    QVector<double> masteringChromaticities; // Rx,Ry,Gx,Gy,Bx,By,Wx,Wy; empty = absent.
    bool operator==(const VideoColorInfo &) const = default;
};
struct ColorSettings {
    bool hdrToneMapping = true;
    double sourcePeakNits = 0; // 0 = content metadata, otherwise 1000-nit fallback.
    double referenceWhiteNits = 100;
    double displayPeakNits = 100;
    double mobiusTransition = .3;
    bool operator==(const ColorSettings &) const = default;
};

struct AudioTrack {
    int streamIndex = -1;
    QString codec;
    QString title;
    QString language;
    int sampleRate = 0;
    int channels = 0;
    qint64 startUs = 0;
    qint64 durationUs = 0;
    bool isDefault = false;
    QVector<qint64> packetPts;
    QVector<qint64> sampleStarts;
};

struct MediaInfo {
    bool hasVideo = false;
    bool hasAudio = false;
    bool variableFrameRate = false;
    int width = 0;
    int height = 0;
    int sampleRate = 0;
    int channels = 0;
    qint64 durationUs = 0;
    qint64 videoStartUs = 0;
    qint64 audioStartUs = 0;
    Rational fps{24, 1};
    Rational timeBase{1, 1000000};
    // Presentation timestamps in stream time_base, in decoded display order.
    // These are an index, never decompressed frames or average-fps estimates.
    QVector<qint64> framePts;
    QVector<qint64> frameDurations;
    VideoColorInfo color;
    QMap<qint64, VideoColorInfo> frameColors; // Sparse display-order changes from color.
    QByteArray pictureTypes; // One I/P/B/S/i/p/b/? character per decoded display frame.
    QByteArray keyFrames; // One '0'/'1' per display frame; independent of picture type.
    QVector<qint64> audioPacketPts;
    QVector<qint64> audioSampleStarts;
    QVector<AudioTrack> audioTracks;
    qint64 frameCount() const;
    qint64 frameStartUs(qint64 frame) const;
    qint64 frameEndUs(qint64 frame) const;
    qint64 frameAtUs(qint64 localUs) const;
    qint64 frameAtSamplingUs(qint64 localUs) const; // Quantize onto the stream time-base grid.
};

struct MediaSource {
    QString id;
    QString name;
    QString path;
    QString folderId;
    SourceKind kind = SourceKind::Video;
    QStringList sequencePaths;
    MediaInfo info;
    bool missing = false;
    qint64 inFrame = 0;
    qint64 outFrame = -1;
    qint64 timecodeOffset = 0;
    bool timecodeOffsetEnabled = false;
    int selectedAudioTrack = -1; // AVStream index; -1 chooses the default audio stream.
    qint64 audioOffsetUs = 0;    // Positive values advance audio relative to video.
};
struct MediaFolder {
    QString id;
    QString name;
    QString parentId;
};
struct Clip {
    QString id;
    QString sourceId;
    QString name;
    qint64 inFrame = 0;
    qint64 outFrame = -1; // inclusive, -1 means source end
    QString audioSourceId;
    qint64 audioOffsetUs = 0;
    double gain = 1.0;
    bool muted = false;
};
struct Timeline {
    QString id;
    QString name;
    Rational fps{24, 1};
    QVector<Clip> clips;
    QString folderId;
    qint64 timecodeOffset = 0;
    bool timecodeOffsetEnabled = false;
};
struct Bookmark {
    QString id;
    QString timelineId;
    qint64 frame = 0;
    qint64 endFrame = 0;
    QString name;
    QString text;
    QColor color{255, 188, 64};
    bool operator==(const Bookmark &) const = default;
};
struct Note {
    QString id;
    QString timelineId;
    qint64 frame = 0;
    QString text;
    QString author;
    QColor color{Qt::transparent};
    QDateTime created = QDateTime::currentDateTimeUtc();
};
struct Annotation {
    QString id;
    QString timelineId;
    qint64 frame = 0;
    qint64 endFrame = 0;
    QString layer = QStringLiteral("Default");
    bool visible = true;
    bool hold = false;
    int ghostFrames = 0;
    AnnotationTool tool = AnnotationTool::Pencil;
    double opacity = 1.0;
    bool background = false;
    QVector<QPointF> points; // normalized owner pane coordinates [0,1], independent of video zoom
    QVector<double> widthScales; // Empty = constant width; otherwise one finite [0,1] per point.
    QColor color{255, 204, 64};
    double width = 3.0; // relative to 1920-pixel output
    QString text;
    QPointF textPosition{0.05, 0.1};
    int fontSize = 28;
    bool operator==(const Annotation &) const = default;
};
struct ViewTransform {
    double zoom = 1.0;
    QPointF pan;
    int rotation = 0;
    bool flipHorizontal = false;
    bool flipVertical = false;
};
struct PanZoom {
    double zoom = 1.0;
    QPointF pan;
    bool enabled = true;
};
struct OverlayShape {
    QString id;
    OverlayShapeType type = OverlayShapeType::Line;
    QVector<QPointF> points;
    QColor color{255, 255, 255, 180};
    QColor fill{Qt::transparent};
    double width = 2.0;
    double opacity = 1.0;
    QString text;
    int fontSize = 28;
    bool visible = true;
};
struct OverlaySettings {
    OverlayType type = OverlayType::Off;
    QColor color{255, 255, 255, 85};
    double opacity = 1.0;
    double lineWidth = 1.0;
    int gridColumns = 3;
    int gridRows = 3;
    QPointF safeMargins{.05, .05};
    QPointF safeInnerMargins{.1, .1};
    double centerSize = 24;
    QVector<OverlayShape> shapes;
    QString imagePath;
    QRectF imageRect{0, 0, 1, 1};
    double imageOpacity = 1.0;
    bool imageKeepAspect = true;
};
struct ViewerSettings {
    ViewerMode mode = ViewerMode::Single;
    QStringList assignments; // source or timeline IDs; empty = active timeline
    int activePane = 0;
    double wipe = 0.5;
    double wipePivotY = 0.5;
    double wipeAngleDegrees = 0;
    double opacity = 0.5;
    bool splitVertical = true;
    QVector<ViewTransform> transforms{4};
    PanZoom panZoom;
    OverlaySettings overlay;
    ColorSettings color;
    bool showPictureType = false;
    bool showOverlay = true;
    bool showAnnotations = true;
    bool holdAnnotations = false;
    bool overlayGrid = false;
    bool overlaySafe = false;
    bool overlayCenter = false;
    QColor background{24, 25, 29};
    QColor annotationBackground{0, 0, 0, 0};
    bool ghostEnabled = false;
    int ghostPrevious = 1;
    int ghostNext = 1;
    bool ghostByAnnotations = false;
    QColor ghostPreviousTint{255, 90, 90};
    QColor ghostNextTint{90, 160, 255};
    static constexpr QSize DefaultOutputSize{1920, 1080};
    QSize outputSize = DefaultOutputSize;
};
struct Project {
    QString name = QStringLiteral("Untitled");
    Rational fps{24, 1};
    QVector<MediaFolder> folders;
    QVector<MediaSource> sources;
    QVector<Timeline> timelines;
    QString activeTimelineId;
    QString defaultTimelineId;
    QVector<Bookmark> bookmarks;
    QVector<Note> notes;
    QVector<Annotation> annotations;
    ViewerSettings viewer;
    QMap<QString, ViewTransform> viewTransforms;
    QMap<QString, PanZoom> panZoomByOwner;
    QString sourceRootDirectory;
    qint64 rangeIn = 0;
    qint64 rangeOut = -1;
    LoopMode loopMode = LoopMode::Loop;
    int loopDelayMs = 0;
    bool loopHoldEmpty = false;
    const MediaSource *source(const QString &id) const;
    const Timeline *timeline(const QString &id) const;
    const Timeline *activeTimeline() const;
    qint64 durationFrames(const QString &timelineId = {}) const;
};
struct EvaluatedFrame {
    QString sourceId;
    QString clipId;
    qint64 sourceFrame = 0;
    qint64 sourceUs = 0;
    qint64 timelineFrame = 0;
    qint64 clipStartFrame = 0;
    qint64 clipDurationFrames = 0;
    QString audioSourceId;
    qint64 audioUs = 0;
    double gain = 1.0;
    bool muted = false;
};

QString newId();
Project emptyProject();
QPointF canvasToViewer(const ViewerSettings &, QPointF normalizedCanvasPosition);
QPointF viewerToCanvas(const ViewerSettings &, QPointF normalizedViewerPosition);
bool selectSourceAudioTrack(MediaSource &, int streamIndex, QString *error = nullptr);
inline constexpr qint64 MaximumAudioOffsetUs = 9007199254740991LL;
bool validateSourceAudioOffset(qint64 offsetUs, QString *error = nullptr);
qint64 shiftedAudioTime(qint64 timeUs, qint64 offsetUs);
qint64 sourceAudioTime(const MediaSource &, qint64 videoTimeUs);
bool validateViewerSettings(const ViewerSettings &, QString *error = nullptr);
bool validateColorSettings(const ColorSettings &, QString *error = nullptr);
bool validateAnnotation(const Annotation &, QString *error = nullptr);
VideoColorInfo frameColorInfo(const MediaInfo &, qint64 ordinal);
QString framePictureType(const MediaInfo &, qint64 ordinal);
bool frameIsKeyFrame(const MediaInfo &, qint64 ordinal);
QJsonObject videoColorToJson(const VideoColorInfo &);
bool isSinglePaneMode(ViewerMode);
int primaryViewerPane(const ViewerSettings &);
int activeViewerPane(const ViewerSettings &);
QRectF viewerPaneRect(const ViewerSettings &, int pane); // normalized composition coordinates
int viewerPaneAt(const ViewerSettings &, QPointF normalizedPosition);
QString viewerPaneOwner(const Project &, int pane);
qint64 viewerPaneFrame(const Project &, qint64 frame, int pane);
QString displayedAnnotationOwner(const Project &);
qint64 displayedAnnotationFrame(const Project &, qint64 frame);
qint64 frameDisplayOffset(const Project &, const QString &ownerId = {});
qint64 displayFrameNumber(const Project &, const QString &ownerId, qint64 rawFrame);
qint64 frameFromDisplay(const Project &, const QString &ownerId, qint64 displayedFrame);
qint64 clipDurationFrames(const Project &, const Timeline &, const Clip &);
std::optional<EvaluatedFrame> evaluateTimeline(const Project &, const QString &timelineId,
                                               qint64 frame);
std::optional<EvaluatedFrame> evaluateAssignment(const Project &, const QString &assignment,
                                                 qint64 frame);
QJsonObject projectToJson(const Project &, const QString &baseDirectory = {});
bool projectFromJson(const QJsonObject &, Project &, QString *error,
                     const QString &baseDirectory = {});
bool saveProjectFile(const Project &, const QString &path, QString *error);
bool loadProjectFile(const QString &path, Project &, QString *error);
bool exportNotesCsv(const Project &, const QString &path, QString *error);
bool importNotesCsv(Project &, const QString &path, QString *error);

enum class ExportKind { Video, Audio, ImageSequence };
enum class ExportColorMode { DisplayConverted, PreserveSource };
struct ExportSettings {
    QString path;
    ExportKind kind = ExportKind::Video;
    QString timelineId;
    qint64 firstFrame = 0;
    qint64 lastFrame = -1;
    QSize size{1920, 1080};
    Rational outputFps{0, 0}; // Video CFR sampling rate; 0/0 uses the selected owner's rate.
    bool viewerComposition = true;
    bool includeAnnotations = true;
    bool includeAudio = true;
    QString videoCodec = QStringLiteral("mpeg4");
    QString audioCodec = QStringLiteral("aac");
    QString imageFormat = QStringLiteral("png");
    int crf = 18;
    int frameStep = 1;
    bool bookmarksOnly = false;
    QString frameNumbering = QStringLiteral("one_based");
    qint64 sequenceStart = 1;
    int sequencePadding = 6;
    QString sequenceBaseName = QStringLiteral("frame");
    bool publishSequenceIntoExistingDirectory = false;
    bool forceOverwrite = false;
    bool overrideAnnotationBackground = false;
    QColor annotationBackground{0, 0, 0, 0};
    ExportColorMode colorMode = ExportColorMode::DisplayConverted;
    int bitDepth = 8; // 0 = auto; explicit 8/10/12/16 must be supported, never silently reduced.
};
} // namespace openframe
Q_DECLARE_METATYPE(openframe::Project)
