#include "Project.h"
#include "ViewerGeometry.h"
#include "integration/LocalPathPolicy.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
extern "C" {
#include <libavutil/mathematics.h>
}

namespace openframe {
namespace {
qint64 rescale(qint64 n, qint64 a, qint64 b) {
    return b > 0 && a >= 0 ? av_rescale_rnd(n, a, b, AV_ROUND_DOWN) : 0;
}
QJsonValue integer(qint64 x) {
    return QString::number(x);
}
qint64 readInt(const QJsonValue &v, qint64 fallback = 0) {
    if (v.isUndefined())
        return fallback;
    if (v.isString()) {
        bool ok = false;
        auto x = v.toString().toLongLong(&ok);
        if (ok)
            return x;
    }
    if (v.isDouble() && std::isfinite(v.toDouble()) &&
        std::abs(v.toDouble()) <= static_cast<double>(MaximumAudioOffsetUs) &&
        std::trunc(v.toDouble()) == v.toDouble())
        return static_cast<qint64>(v.toDouble());
    throw std::runtime_error("Invalid integer in project");
}
QJsonObject rateJson(Rational r) {
    return {{"num", integer(r.num)}, {"den", integer(r.den)}};
}
Rational readRate(QJsonValue v, Rational fallback = {24, 1}) {
    if (v.isUndefined())
        return fallback;
    auto o = v.toObject();
    Rational r{readInt(o["num"]), readInt(o["den"])};
    if (!r.valid() || r.num > 1000000000 || r.den > 1000000000)
        throw std::runtime_error("Invalid rational time base");
    return r;
}
QJsonArray pointJson(QPointF p) {
    return {p.x(), p.y()};
}
QPointF readPoint(QJsonValue v) {
    auto a = v.toArray();
    return {a.at(0).toDouble(), a.at(1).toDouble()};
}
void requireMemberType(const QJsonObject &o,const QString &key,QJsonValue::Type type) {
    if(o.contains(key)&&o.value(key).type()!=type)throw std::runtime_error(("Invalid type for "+key).toStdString());
}
double boundedNumber(const QJsonObject &o, const QString &key, double fallback, double low,
                     double high) {
    requireMemberType(o,key,QJsonValue::Double);
    const auto value = o.value(key).toDouble(fallback);
    if (!std::isfinite(value) || value < low || value > high)
        throw std::runtime_error(("Invalid " + key).toStdString());
    return value;
}
QJsonObject panZoomJson(const PanZoom &v) {
    return {{"zoom", v.zoom}, {"pan", pointJson(v.pan)}, {"enabled", v.enabled}};
}
PanZoom readPanZoom(const QJsonObject &o) {
    PanZoom v;
    v.zoom = boundedNumber(o, "zoom", 1, .01, 64);
    v.pan = readPoint(o["pan"]);
    if (!std::isfinite(v.pan.x()) || !std::isfinite(v.pan.y()) || std::abs(v.pan.x()) > 100 ||
        std::abs(v.pan.y()) > 100)
        throw std::runtime_error("Invalid viewer pan");
    v.enabled = o["enabled"].toBool(true);
    return v;
}
QJsonObject overlayJson(const OverlaySettings &v, const QString &directory) {
    QJsonArray shapes;
    for (const auto &s : v.shapes) {
        QJsonArray points;
        for (const auto &p : s.points)
            points.append(pointJson(p));
        shapes.append(QJsonObject{{"id", s.id},
                                  {"type", int(s.type)},
                                  {"points", points},
                                  {"color", s.color.name(QColor::HexArgb)},
                                  {"fill", s.fill.name(QColor::HexArgb)},
                                  {"width", s.width},
                                  {"opacity", s.opacity},
                                  {"text", s.text},
                                  {"fontSize", s.fontSize},
                                  {"visible", s.visible}});
    }
    auto imagePath = v.imagePath;
    if (!imagePath.isEmpty() && !directory.isEmpty()) {
        auto relative = QDir(directory).relativeFilePath(imagePath);
        if (relative != ".." && !relative.startsWith("../") && !relative.startsWith("..\\"))
            imagePath = relative;
    }
    return {{"type", int(v.type)},
            {"color", v.color.name(QColor::HexArgb)},
            {"opacity", v.opacity},
            {"lineWidth", v.lineWidth},
            {"gridColumns", v.gridColumns},
            {"gridRows", v.gridRows},
            {"safeMargins", pointJson(v.safeMargins)},
            {"safeInnerMargins", pointJson(v.safeInnerMargins)},
            {"centerSize", v.centerSize},
            {"shapes", shapes},
            {"imagePath", imagePath},
            {"imageRect", QJsonArray{v.imageRect.x(), v.imageRect.y(), v.imageRect.width(),
                                     v.imageRect.height()}},
            {"imageOpacity", v.imageOpacity},
            {"imageKeepAspect", v.imageKeepAspect}};
}
OverlaySettings readOverlay(const QJsonObject &o, const QString &directory) {
    OverlaySettings v;
    const auto type = o["type"].toInt();
    if (type < 0 || type > int(OverlayType::Image))
        throw std::runtime_error("Invalid overlay type");
    v.type = OverlayType(type);
    v.color = QColor(o["color"].toString(v.color.name(QColor::HexArgb)));
    v.opacity = boundedNumber(o, "opacity", 1, 0, 1);
    v.lineWidth = boundedNumber(o, "lineWidth", 1, .1, 64);
    v.gridColumns = int(boundedNumber(o, "gridColumns", 3, 1, 128));
    v.gridRows = int(boundedNumber(o, "gridRows", 3, 1, 128));
    if (o.contains("safeMargins"))
        v.safeMargins = readPoint(o["safeMargins"]);
    if (o.contains("safeInnerMargins"))
        v.safeInnerMargins = readPoint(o["safeInnerMargins"]);
    auto validPoint = [](QPointF p, double maximum) {
        return std::isfinite(p.x()) && std::isfinite(p.y()) && p.x() >= 0 && p.y() >= 0 &&
               p.x() <= maximum && p.y() <= maximum;
    };
    if (!v.color.isValid() || !validPoint(v.safeMargins, .49) ||
        !validPoint(v.safeInnerMargins, .49))
        throw std::runtime_error("Invalid overlay color or safe margins");
    v.centerSize = boundedNumber(o, "centerSize", 24, 1, 1024);
    auto shapes = o["shapes"].toArray();
    if (shapes.size() > 256)
        throw std::runtime_error("Too many custom overlay shapes");
    qsizetype totalPoints = 0;
    for (auto item : shapes) {
        auto a = item.toObject();
        OverlayShape s;
        s.id = a["id"].toString();
        if (s.id.isEmpty())
            s.id = newId();
        auto shapeType = a["type"].toInt();
        if (shapeType < 0 || shapeType > int(OverlayShapeType::Text))
            throw std::runtime_error("Invalid custom overlay shape type");
        s.type = OverlayShapeType(shapeType);
        const auto points = a["points"].toArray();
        totalPoints += points.size();
        if (points.size() > 4096 || totalPoints > 65536)
            throw std::runtime_error("Custom overlay point limit exceeded");
        for (auto point : points) {
            auto p = readPoint(point);
            if (!validPoint(p, 1))
                throw std::runtime_error("Invalid custom overlay point");
            s.points.append(p);
        }
        s.color = QColor(a["color"].toString(s.color.name(QColor::HexArgb)));
        s.fill = QColor(a["fill"].toString(s.fill.name(QColor::HexArgb)));
        if (!s.color.isValid() || !s.fill.isValid())
            throw std::runtime_error("Invalid shape color");
        s.width = boundedNumber(a, "width", 2, .1, 64);
        s.opacity = boundedNumber(a, "opacity", 1, 0, 1);
        s.fontSize = int(boundedNumber(a, "fontSize", 28, 1, 512));
        s.visible = a["visible"].toBool(true);
        s.text = a["text"].toString();
        if (s.text.size() > 16384)
            throw std::runtime_error("Overlay text limit exceeded");
        v.shapes.append(s);
    }
    v.imagePath = o["imagePath"].toString();
    if (!v.imagePath.isEmpty()) {
        if (!QDir::isAbsolutePath(v.imagePath) && !directory.isEmpty())
            v.imagePath = QDir(directory).absoluteFilePath(v.imagePath);
        QString error;
        auto safe = LocalPathPolicy::validateInput(v.imagePath, &error);
        if (safe.isEmpty())
            safe = LocalPathPolicy::validateOutput(v.imagePath, &error);
        if (safe.isEmpty())
            throw std::runtime_error(error.toStdString());
        v.imagePath = safe;
    }
    const auto rect = o["imageRect"].toArray();
    if (!rect.isEmpty()) {
        if (rect.size() != 4)
            throw std::runtime_error("Invalid overlay image rectangle");
        v.imageRect = {rect[0].toDouble(), rect[1].toDouble(), rect[2].toDouble(),
                       rect[3].toDouble()};
        if (!validPoint(v.imageRect.topLeft(), 1) ||
            !validPoint({v.imageRect.width(), v.imageRect.height()}, 1) || v.imageRect.isEmpty())
            throw std::runtime_error("Invalid overlay image bounds");
    }
    v.imageOpacity = boundedNumber(o, "imageOpacity", 1, 0, 1);
    v.imageKeepAspect = o["imageKeepAspect"].toBool(true);
    return v;
}
QJsonObject colorSettingsJson(const ColorSettings &v) {
    return {{"hdrToneMapping",v.hdrToneMapping},{"sourcePeakNits",v.sourcePeakNits},
            {"referenceWhiteNits",v.referenceWhiteNits},{"displayPeakNits",v.displayPeakNits},
            {"mobiusTransition",v.mobiusTransition}};
}
ColorSettings readColorSettings(const QJsonObject &o) {
    ColorSettings v;
    requireMemberType(o,"hdrToneMapping",QJsonValue::Bool);
    v.hdrToneMapping=o["hdrToneMapping"].toBool(true);
    v.sourcePeakNits=boundedNumber(o,"sourcePeakNits",0,0,10000);
    v.referenceWhiteNits=boundedNumber(o,"referenceWhiteNits",100,1,1000);
    v.displayPeakNits=boundedNumber(o,"displayPeakNits",100,1,1000);
    v.mobiusTransition=boundedNumber(o,"mobiusTransition",.3,0,.95);
    return v;
}
VideoColorInfo readVideoColor(const QJsonObject &o) {
    VideoColorInfo v;
    auto text=[&](const char *key,QString fallback) {
        requireMemberType(o,key,QJsonValue::String);
        auto value=o.value(key).toString(fallback);
        if(value.size()>128) throw std::runtime_error("Invalid color metadata string");
        return value;
    };
    v.pixelFormat=text("pixelFormat",{});
    auto bits=boundedNumber(o,"componentBits",0,0,64);
    if(std::trunc(bits)!=bits)throw std::runtime_error("Invalid component bit depth");
    v.componentBits=int(bits);
    v.primaries=text("primaries","unknown"); v.transfer=text("transfer","unknown");
    v.matrix=text("matrix","unknown"); v.range=text("range","unknown");
    v.chromaLocation=text("chromaLocation","unspecified");
    v.masteringMinNits=boundedNumber(o,"masteringMinNits",0,0,10000);
    v.masteringMaxNits=boundedNumber(o,"masteringMaxNits",0,0,10000);
    v.maxCLL=boundedNumber(o,"maxCLL",0,0,10000); v.maxFALL=boundedNumber(o,"maxFALL",0,0,10000);
    requireMemberType(o,"masteringChromaticities",QJsonValue::Array);
    auto chroma=o["masteringChromaticities"].toArray();
    if(!chroma.isEmpty()&&chroma.size()!=8) throw std::runtime_error("Invalid mastering chromaticities");
    for(auto x:chroma) {
        if(!x.isDouble()||!std::isfinite(x.toDouble())||x.toDouble()<0||x.toDouble()>1)
            throw std::runtime_error("Invalid mastering chromaticity");
        v.masteringChromaticities.append(x.toDouble());
    }
    return v;
}
QString pathForWrite(const QString &p, const QString &dir) {
    if (dir.isEmpty())
        return p;
    auto relative = QDir(dir).relativeFilePath(p);
    return relative == ".." || relative.startsWith("../") || relative.startsWith("..\\") ? p
                                                                                         : relative;
}
QString pathForRead(const QString &p, const QString &dir) {
    if (p == "." && !dir.isEmpty())
        return QDir(dir).absolutePath();
    return QDir::isAbsolutePath(p) || dir.isEmpty() ? p : QDir(dir).absoluteFilePath(p);
}
QString csvQuote(QString x) {
    x.replace('"', "\"\"");
    return '"' + x + '"';
}
} // namespace

QJsonObject videoColorToJson(const VideoColorInfo &v) {
    QJsonArray chroma; for(auto x:v.masteringChromaticities) chroma.append(x);
    return {{"pixelFormat",v.pixelFormat},{"componentBits",v.componentBits},{"primaries",v.primaries},
            {"transfer",v.transfer},{"matrix",v.matrix},{"range",v.range},{"chromaLocation",v.chromaLocation},
            {"masteringMinNits",v.masteringMinNits},{"masteringMaxNits",v.masteringMaxNits},
            {"maxCLL",v.maxCLL},{"maxFALL",v.maxFALL},{"masteringChromaticities",chroma}};
}
VideoColorInfo frameColorInfo(const MediaInfo &info,qint64 ordinal) {
    auto it=info.frameColors.upperBound(ordinal);
    return it==info.frameColors.cbegin()?info.color:(--it).value();
}
QString framePictureType(const MediaInfo &info,qint64 ordinal) {
    if(ordinal<0||ordinal>=info.pictureTypes.size()) return "Unknown";
    switch(info.pictureTypes[ordinal]) {
    case 'I':return "I"; case 'P':return "P"; case 'B':return "B"; case 'S':return "S";
    case 'i':return "SI"; case 'p':return "SP"; case 'b':return "BI"; default:return "Unknown";
    }
}
bool frameIsKeyFrame(const MediaInfo &info,qint64 ordinal) {
    return ordinal>=0&&ordinal<info.keyFrames.size()&&info.keyFrames[ordinal]=='1';
}
bool validateColorSettings(const ColorSettings &v,QString *error) {
    try {
        for(auto x:{v.sourcePeakNits,v.referenceWhiteNits,v.displayPeakNits,v.mobiusTransition})
            if(!std::isfinite(x)) throw std::runtime_error("Non-finite color setting");
        readColorSettings(colorSettingsJson(v)); return true;
    } catch(const std::exception &e) { if(error)*error=QString::fromUtf8(e.what()); return false; }
}
bool validateAnnotation(const Annotation &a,QString *error) {
    auto fail=[&](const char *message){if(error)*error=QString::fromUtf8(message);return false;};
    if(a.points.size()>1000000||(!a.widthScales.isEmpty()&&a.widthScales.size()!=a.points.size()))
        return fail("Invalid annotation point/width count");
    for(auto x:a.widthScales) if(!std::isfinite(x)||x<0||x>1) return fail("Invalid stroke width scale");
    for(auto pt:a.points) if(!std::isfinite(pt.x())||!std::isfinite(pt.y())||pt.x()<0||pt.y()<0||pt.x()>1||pt.y()>1)
        return fail("Invalid annotation coordinates");
    if(!std::isfinite(a.width)||a.width<.1||a.width>2000||!std::isfinite(a.opacity)||a.opacity<0||a.opacity>1)
        return fail("Invalid annotation width/opacity");
    return true;
}
double Rational::value() const {
    return den != 0 ? static_cast<double>(num) / den : 0;
}
bool Rational::valid() const {
    return num > 0 && den > 0;
}
qint64 Rational::frameToUs(qint64 f) const {
    return valid() ? av_rescale_q(f, AVRational{static_cast<int>(den), static_cast<int>(num)},
                                  AVRational{1, 1000000})
                   : 0;
}
qint64 Rational::usToFrame(qint64 us) const {
    return valid() ? av_rescale_q_rnd(us, AVRational{1, 1000000},
                                      AVRational{static_cast<int>(den), static_cast<int>(num)},
                                      AV_ROUND_DOWN)
                   : 0;
}
qint64 MediaInfo::frameCount() const {
    return hasVideo ? framePts.size()
                    : std::max<qint64>(1, av_rescale_q_rnd(durationUs, {1, 1000000},
                                                           {static_cast<int>(fps.den),
                                                            static_cast<int>(fps.num)},
                                                           AV_ROUND_UP));
}
qint64 MediaInfo::frameStartUs(qint64 f) const {
    if (framePts.isEmpty())
        return fps.frameToUs(f);
    f = std::clamp<qint64>(f, 0, framePts.size() - 1);
    return av_rescale_q(framePts[f] - framePts[0],
                        {static_cast<int>(timeBase.num), static_cast<int>(timeBase.den)},
                        {1, 1000000});
}
qint64 MediaInfo::frameEndUs(qint64 f) const {
    if (framePts.isEmpty())
        return durationUs > 0 ? std::min(durationUs, fps.frameToUs(f + 1)) : fps.frameToUs(f + 1);
    f = std::clamp<qint64>(f, 0, framePts.size() - 1);
    if (f + 1 < framePts.size())
        return frameStartUs(f + 1);
    // A CFR stream can store 24 fps in 1-ms ticks (last duration rounds to 41 ms).
    // Its complete span still contains N rational frame periods, not N-1 plus a rounded packet.
    if(!variableFrameRate&&fps.valid())return std::max(frameStartUs(f)+1,fps.frameToUs(f+1));
    qint64 dur = f < frameDurations.size() ? frameDurations[f] : 0;
    if (dur > 0)
        return frameStartUs(f) +
               av_rescale_q(dur, {static_cast<int>(timeBase.num), static_cast<int>(timeBase.den)},
                            {1, 1000000});
    return frameStartUs(f) + std::max<qint64>(1, fps.frameToUs(1));
}
qint64 MediaInfo::frameAtUs(qint64 us) const {
    if (framePts.isEmpty())
        return std::max<qint64>(0, fps.usToFrame(us));
    // Compare in the same microsecond domain as frameStartUs(). Rounding the
    // query into coarse stream ticks can select the next frame before its start
    // (notably end-1us on reverse playback); simply flooring ticks breaks exact
    // round trips for fractional time bases instead.
    const AVRational tb{static_cast<int>(timeBase.num), static_cast<int>(timeBase.den)};
    auto it =
        std::upper_bound(framePts.begin(), framePts.end(), us, [&](qint64 timeUs, qint64 pts) {
            return timeUs < av_rescale_q(pts - framePts[0], tb, {1, 1000000});
        });
    return std::max<qint64>(0, std::distance(framePts.begin(), it) - 1);
}
qint64 MediaInfo::frameAtSamplingUs(qint64 us) const {
    if (framePts.isEmpty())
        return std::max<qint64>(0, fps.usToFrame(us));
    // CFR sampling must respect the original timestamp quantization: 24 fps in
    // a 1 ms stream has starts 0/42/83 ms, while the output grid is 0/41.667/83.333.
    // Quantize the sampling instant to that grid, without changing presentation
    // clock lookup (which must never advance before a frame's actual start).
    const auto ticks =
        av_rescale_q(us, {1, 1000000},
                     {static_cast<int>(timeBase.num), static_cast<int>(timeBase.den)}) +
        framePts[0];
    auto it = std::upper_bound(framePts.begin(), framePts.end(), ticks);
    return std::max<qint64>(0, std::distance(framePts.begin(), it) - 1);
}
QString newId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
Project emptyProject() {
    Project p;
    Timeline t;
    t.id = newId();
    t.name = "Timeline 1";
    p.timelines.append(t);
    p.activeTimelineId = t.id;
    p.defaultTimelineId = t.id;
    return p;
}
QPointF canvasToViewer(const ViewerSettings &v, QPointF p) {
    return v.panZoom.enabled
               ? (p - QPointF(.5, .5)) * v.panZoom.zoom + QPointF(.5, .5) + v.panZoom.pan
               : p;
}
QPointF viewerToCanvas(const ViewerSettings &v, QPointF p) {
    return v.panZoom.enabled
               ? (p - QPointF(.5, .5) - v.panZoom.pan) / v.panZoom.zoom + QPointF(.5, .5)
               : p;
}
bool validateViewerSettings(const ViewerSettings &v, QString *error) {
    try {
        const auto finite = [](double value) {
            if (!std::isfinite(value))
                throw std::runtime_error("Viewer settings contain a non-finite number");
        };
        finite(v.wipe); finite(v.wipePivotY); finite(v.wipeAngleDegrees);
        if(v.wipe<0||v.wipe>1||v.wipePivotY<0||v.wipePivotY>1||std::abs(v.wipeAngleDegrees)>360000)
            throw std::runtime_error("Invalid wipe pivot or angle");
        QString colorError;
        if(!validateColorSettings(v.color,&colorError)) throw std::runtime_error(colorError.toStdString());
        finite(v.panZoom.zoom);
        finite(v.panZoom.pan.x());
        finite(v.panZoom.pan.y());
        finite(v.overlay.opacity);
        finite(v.overlay.lineWidth);
        finite(v.overlay.centerSize);
        finite(v.overlay.imageOpacity);
        for (auto point :
             {v.overlay.safeMargins, v.overlay.safeInnerMargins, v.overlay.imageRect.topLeft(),
              QPointF(v.overlay.imageRect.width(), v.overlay.imageRect.height())}) {
            finite(point.x());
            finite(point.y());
        }
        for (const auto &shape : v.overlay.shapes) {
            finite(shape.width);
            finite(shape.opacity);
            for (auto point : shape.points) {
                finite(point.x());
                finite(point.y());
            }
        }
        readPanZoom(panZoomJson(v.panZoom));
        readOverlay(overlayJson(v.overlay, {}), {});
        return true;
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return false;
    }
}
bool validateSourceAudioOffset(qint64 offsetUs, QString *error) {
    if (offsetUs >= -MaximumAudioOffsetUs && offsetUs <= MaximumAudioOffsetUs)
        return true;
    if (error)
        *error = "Source audio offset exceeds the exact integer microsecond range";
    return false;
}
qint64 shiftedAudioTime(qint64 timeUs, qint64 offsetUs) {
    if (offsetUs > 0 && timeUs > std::numeric_limits<qint64>::max() - offsetUs)
        return std::numeric_limits<qint64>::max();
    if (offsetUs < 0 && timeUs < std::numeric_limits<qint64>::min() - offsetUs)
        return std::numeric_limits<qint64>::min();
    return timeUs + offsetUs;
}
qint64 sourceAudioTime(const MediaSource &s, qint64 videoTimeUs) {
    if (s.info.hasVideo) {
        videoTimeUs = shiftedAudioTime(videoTimeUs, s.info.videoStartUs);
        videoTimeUs = shiftedAudioTime(videoTimeUs, -s.info.audioStartUs);
    }
    return shiftedAudioTime(videoTimeUs, s.audioOffsetUs);
}
bool selectSourceAudioTrack(MediaSource &s, int streamIndex, QString *error) {
    const AudioTrack *selected = nullptr;
    if (streamIndex < -1) {
        if (error)
            *error = "Invalid audio stream index";
        return false;
    }
    for (const auto &track : s.info.audioTracks)
        if (streamIndex == track.streamIndex || (streamIndex == -1 && track.isDefault)) {
            selected = &track;
            break;
        }
    if (!selected && streamIndex == -1 && !s.info.audioTracks.isEmpty())
        selected = &s.info.audioTracks.first();
    if (!selected) {
        if (streamIndex == -1) {
            s.selectedAudioTrack = -1;
            return true;
        }
        if (error)
            *error = "Audio track does not exist";
        return false;
    }
    s.selectedAudioTrack = streamIndex;
    s.info.hasAudio = true;
    s.info.sampleRate = selected->sampleRate;
    s.info.channels = selected->channels;
    s.info.audioStartUs = selected->startUs;
    s.info.audioPacketPts = selected->packetPts;
    s.info.audioSampleStarts = selected->sampleStarts;
    s.info.durationUs =
        s.info.hasVideo ? std::max(s.info.frameEndUs(s.info.frameCount() - 1),
                                   selected->startUs - s.info.videoStartUs + selected->durationUs)
                        : selected->durationUs;
    return true;
}
const MediaSource *Project::source(const QString &id) const {
    for (const auto &s : sources)
        if (s.id == id)
            return &s;
    return nullptr;
}
const Timeline *Project::timeline(const QString &id) const {
    for (const auto &t : timelines)
        if (t.id == id)
            return &t;
    return nullptr;
}
const Timeline *Project::activeTimeline() const {
    return timeline(activeTimelineId);
}
int activeViewerPane(const ViewerSettings &v) {
    return v.mode == ViewerMode::Single
               ? 0
               : std::clamp(v.activePane, 0, v.mode == ViewerMode::Grid ? 3 : 1);
}
QRectF viewerPaneRect(const ViewerSettings &v, int pane) {
    if (pane < 0 || pane > (v.mode == ViewerMode::Grid ? 3 : 1))
        return {};
    if (v.mode == ViewerMode::Single || v.mode == ViewerMode::AB)
        return pane == activeViewerPane(v) ? QRectF(0, 0, 1, 1) : QRectF{};
    if (v.mode == ViewerMode::Split)
        return v.splitVertical ? QRectF(pane * .5, 0, .5, 1) : QRectF(0, pane * .5, 1, .5);
    if (v.mode == ViewerMode::Grid)
        return QRectF((pane % 2) * .5, (pane / 2) * .5, .5, .5);
    return {0, 0, 1, 1};
}
int viewerPaneAt(const ViewerSettings &v, QPointF position) {
    if (v.mode == ViewerMode::Split)
        return (v.splitVertical ? position.x() : position.y()) < .5 ? 0 : 1;
    if (v.mode == ViewerMode::Grid)
        return (position.x() < .5 ? 0 : 1) + (position.y() < .5 ? 0 : 2);
    if (v.mode == ViewerMode::Wipe)
        return wipePaneAt(v,position,v.outputSize);
    return activeViewerPane(v);
}
QString viewerPaneOwner(const Project &p, int pane) {
    const auto owner = p.viewer.assignments.value(pane);
    return owner.isEmpty() ? p.activeTimelineId : owner;
}
qint64 viewerPaneFrame(const Project &p, qint64 frame, int pane) {
    if (const auto source = p.source(viewerPaneOwner(p, pane)))
        return std::clamp<qint64>(frame, 0, std::max<qint64>(0, source->info.frameCount() - 1));
    return frame;
}
QString displayedAnnotationOwner(const Project &p) {
    return viewerPaneOwner(p, activeViewerPane(p.viewer));
}
qint64 displayedAnnotationFrame(const Project &p, qint64 frame) {
    return viewerPaneFrame(p, frame, activeViewerPane(p.viewer));
}
qint64 frameDisplayOffset(const Project &p, const QString &ownerId) {
    auto id = ownerId;
    if (id.isEmpty()) {
        id = p.viewer.assignments.value(0);
        if (id.isEmpty())
            id = p.activeTimelineId;
    }
    if (auto source = p.source(id))
        return source->timecodeOffsetEnabled ? source->timecodeOffset : 1;
    if (auto timeline = p.timeline(id))
        return timeline->timecodeOffsetEnabled ? timeline->timecodeOffset : 1;
    return 1;
}
qint64 displayFrameNumber(const Project &p, const QString &ownerId, qint64 frame) {
    return frame + frameDisplayOffset(p, ownerId);
}
qint64 frameFromDisplay(const Project &p, const QString &ownerId, qint64 frame) {
    return frame - frameDisplayOffset(p, ownerId);
}
qint64 clipDurationFrames(const Project &p, const Timeline &t, const Clip &c) {
    auto s = p.source(c.sourceId);
    if (!s)
        return 0;
    auto last =
        c.outFrame < 0 ? s->info.frameCount() - 1 : std::min(c.outFrame, s->info.frameCount() - 1);
    if (last < c.inFrame)
        return 0;
    if (s->info.hasVideo && !s->info.variableFrameRate)
        return std::max<qint64>(
            1, av_rescale_q_rnd(
                   last - c.inFrame + 1,
                   {static_cast<int>(s->info.fps.den), static_cast<int>(s->info.fps.num)},
                   {static_cast<int>(t.fps.den), static_cast<int>(t.fps.num)}, AV_ROUND_UP));
    auto us = s->info.frameEndUs(last) - s->info.frameStartUs(c.inFrame);
    return std::max<qint64>(
        1,
        av_rescale_q_rnd(us, {1, 1000000},
                         {static_cast<int>(t.fps.den), static_cast<int>(t.fps.num)}, AV_ROUND_UP));
}
qint64 Project::durationFrames(const QString &id) const {
    auto t = timeline(id.isEmpty() ? activeTimelineId : id);
    if (!t)
        return 0;
    qint64 n = 0;
    for (const auto &c : t->clips)
        n += clipDurationFrames(*this, *t, c);
    return n;
}
std::optional<EvaluatedFrame> evaluateTimeline(const Project &p, const QString &id, qint64 f) {
    auto t = p.timeline(id.isEmpty() ? p.activeTimelineId : id);
    if (!t || f < 0)
        return {};
    qint64 start = 0;
    for (const auto &c : t->clips) {
        auto n = clipDurationFrames(p, *t, c);
        if (f < start + n) {
            auto s = p.source(c.sourceId);
            if (!s)
                return {};
            auto local = s->info.frameStartUs(c.inFrame) + t->fps.frameToUs(f - start);
            auto sourceOrdinal =
                s->info.variableFrameRate
                    ? s->info.frameAtSamplingUs(local)
                    : c.inFrame + av_rescale_q_rnd(
                                      f - start,
                                      {static_cast<int>(t->fps.den), static_cast<int>(t->fps.num)},
                                      {static_cast<int>(s->info.fps.den),
                                       static_cast<int>(s->info.fps.num)},
                                      AV_ROUND_DOWN);
            auto sf = std::clamp<qint64>(sourceOrdinal, c.inFrame,
                                         c.outFrame < 0 ? s->info.frameCount() - 1 : c.outFrame);
            EvaluatedFrame e;
            e.sourceId = s->id;
            e.clipId = c.id;
            e.sourceFrame = sf;
            e.sourceUs = local;
            e.timelineFrame = f;
            e.clipStartFrame = start;
            e.clipDurationFrames = n;
            e.audioSourceId = c.audioSourceId.isEmpty() ? s->id : c.audioSourceId;
            // Source audio is aligned to absolute stream timestamps; replacements start at clip
            // zero.
            auto audioSource = p.source(e.audioSourceId);
            e.audioUs = c.audioSourceId.isEmpty()
                            ? sourceAudioTime(*s, local)
                            : shiftedAudioTime(t->fps.frameToUs(f - start),
                                               audioSource ? audioSource->audioOffsetUs : 0);
            e.audioUs = shiftedAudioTime(e.audioUs, c.audioOffsetUs);
            e.gain = c.gain;
            e.muted = c.muted;
            return e;
        }
        start += n;
    }
    return {};
}
std::optional<EvaluatedFrame> evaluateAssignment(const Project &p, const QString &id, qint64 f) {
    if (auto s = p.source(id)) {
        EvaluatedFrame e;
        e.sourceId = s->id;
        e.sourceFrame = std::clamp<qint64>(f, 0, std::max<qint64>(0, s->info.frameCount() - 1));
        e.sourceUs = s->info.frameStartUs(e.sourceFrame);
        e.timelineFrame = f;
        e.audioSourceId = s->id;
        e.audioUs = sourceAudioTime(*s, e.sourceUs);
        return e;
    }
    return evaluateTimeline(p, id, f);
}

QJsonObject projectToJson(const Project &p, const QString &dir) {
    QJsonObject o{{"format", "OpenFrame"},
                  {"version", 1},
                  {"name", p.name},
                  {"fps", rateJson(p.fps)},
                  {"activeTimelineId", p.activeTimelineId},
                  {"defaultTimelineId", p.defaultTimelineId},
                  {"rangeIn", integer(p.rangeIn)},
                  {"rangeOut", integer(p.rangeOut)},
                  {"loopMode", static_cast<int>(p.loopMode)},
                  {"loopDelayMs", p.loopDelayMs},
                  {"loopHoldEmpty", p.loopHoldEmpty}};
    QJsonArray folders;
    for (const auto &f : p.folders)
        folders.append(QJsonObject{{"id", f.id}, {"name", f.name}, {"parentId", f.parentId}});
    o["folders"] = folders;
    o["sourceRootDirectory"] =
        p.sourceRootDirectory.isEmpty() ? QString{} : pathForWrite(p.sourceRootDirectory, dir);
    const auto mediaDirectory = p.sourceRootDirectory.isEmpty() ? dir : p.sourceRootDirectory;
    QJsonObject savedViews;
    for (auto it = p.viewTransforms.cbegin(); it != p.viewTransforms.cend(); ++it) {
        const auto &t = it.value();
        savedViews[it.key()] = QJsonObject{{"zoom", t.zoom},
                                           {"pan", pointJson(t.pan)},
                                           {"rotation", t.rotation},
                                           {"flipHorizontal", t.flipHorizontal},
                                           {"flipVertical", t.flipVertical}};
    }
    o["viewTransforms"] = savedViews;
    QJsonObject panZoomViews;
    for (auto it = p.panZoomByOwner.cbegin(); it != p.panZoomByOwner.cend(); ++it)
        panZoomViews[it.key()] = panZoomJson(it.value());
    o["panZoomByOwner"] = panZoomViews;
    QJsonArray sources;
    for (const auto &s : p.sources) {
        QJsonArray sequence;
        for (const auto &path : s.sequencePaths)
            sequence.append(pathForWrite(path, mediaDirectory));
        QJsonArray pts, durations, audioPts, audioSamples;
        for (auto x : s.info.audioPacketPts)
            audioPts.append(integer(x));
        for (auto x : s.info.audioSampleStarts)
            audioSamples.append(integer(x));
        for (auto x : s.info.framePts)
            pts.append(integer(x));
        for (auto x : s.info.frameDurations)
            durations.append(integer(x));
        QJsonArray tracks;
        for (const auto &a : s.info.audioTracks) {
            QJsonArray packetPts, sampleStarts;
            for (auto x : a.packetPts)
                packetPts.append(integer(x));
            for (auto x : a.sampleStarts)
                sampleStarts.append(integer(x));
            tracks.append(QJsonObject{{"streamIndex", a.streamIndex},
                                      {"codec", a.codec},
                                      {"title", a.title},
                                      {"language", a.language},
                                      {"sampleRate", a.sampleRate},
                                      {"channels", a.channels},
                                      {"startUs", integer(a.startUs)},
                                      {"durationUs", integer(a.durationUs)},
                                      {"isDefault", a.isDefault},
                                      {"packetPts", packetPts},
                                      {"sampleStarts", sampleStarts}});
        }
        QJsonArray colorChanges;
        for(auto it=s.info.frameColors.cbegin();it!=s.info.frameColors.cend();++it)
            colorChanges.append(QJsonObject{{"frame",integer(it.key())},{"color",videoColorToJson(it.value())}});
        QJsonObject info{{"color",videoColorToJson(s.info.color)},
                         {"frameColors",colorChanges},
                         {"pictureTypes",QString::fromLatin1(s.info.pictureTypes)},
                         {"keyFrames",QString::fromLatin1(s.info.keyFrames)},
                         {"video", s.info.hasVideo},
                         {"audio", s.info.hasAudio},
                         {"variableFrameRate", s.info.variableFrameRate},
                         {"width", s.info.width},
                         {"height", s.info.height},
                         {"sampleRate", s.info.sampleRate},
                         {"channels", s.info.channels},
                         {"durationUs", integer(s.info.durationUs)},
                         {"videoStartUs", integer(s.info.videoStartUs)},
                         {"audioStartUs", integer(s.info.audioStartUs)},
                         {"fps", rateJson(s.info.fps)},
                         {"timeBase", rateJson(s.info.timeBase)},
                         {"pts", pts},
                         {"durations", durations},
                         {"audioPacketPts", audioPts},
                         {"audioSampleStarts", audioSamples},
                         {"audioTracks", tracks}};
        sources.append(QJsonObject{{"id", s.id},
                                   {"name", s.name},
                                   {"path", pathForWrite(s.path, mediaDirectory)},
                                   {"folderId", s.folderId},
                                   {"inFrame", integer(s.inFrame)},
                                   {"outFrame", integer(s.outFrame)},
                                   {"timecodeOffset", integer(s.timecodeOffset)},
                                   {"timecodeOffsetEnabled", s.timecodeOffsetEnabled},
                                   {"selectedAudioTrack", s.selectedAudioTrack},
                                   {"audioOffsetUs", integer(s.audioOffsetUs)},
                                   {"kind", static_cast<int>(s.kind)},
                                   {"sequence", sequence},
                                   {"info", info}});
    }
    o["sources"] = sources;
    QJsonArray timelines;
    for (const auto &t : p.timelines) {
        QJsonArray clips;
        for (const auto &c : t.clips)
            clips.append(QJsonObject{{"id", c.id},
                                     {"sourceId", c.sourceId},
                                     {"name", c.name},
                                     {"in", integer(c.inFrame)},
                                     {"out", integer(c.outFrame)},
                                     {"audioSourceId", c.audioSourceId},
                                     {"audioOffsetUs", integer(c.audioOffsetUs)},
                                     {"gain", c.gain},
                                     {"muted", c.muted}});
        timelines.append(QJsonObject{{"id", t.id},
                                     {"name", t.name},
                                     {"fps", rateJson(t.fps)},
                                     {"folderId", t.folderId},
                                     {"timecodeOffset", integer(t.timecodeOffset)},
                                     {"timecodeOffsetEnabled", t.timecodeOffsetEnabled},
                                     {"clips", clips}});
    }
    o["timelines"] = timelines;
    QJsonArray bookmarks;
    for (const auto &b : p.bookmarks)
        bookmarks.append(QJsonObject{{"id", b.id},
                                     {"timelineId", b.timelineId},
                                     {"frame", integer(b.frame)},
                                     {"endFrame", integer(b.endFrame)},
                                     {"name", b.name},
                                     {"text", b.text},
                                     {"color", b.color.name(QColor::HexArgb)}});
    o["bookmarks"] = bookmarks;
    QJsonArray notes;
    for (const auto &n : p.notes)
        notes.append(QJsonObject{{"id", n.id},
                                 {"timelineId", n.timelineId},
                                 {"frame", integer(n.frame)},
                                 {"text", n.text},
                                 {"author", n.author},
                                 {"color", n.color.name(QColor::HexArgb)},
                                 {"created", n.created.toString(Qt::ISODateWithMs)}});
    o["notes"] = notes;
    QJsonArray annotations;
    for (const auto &a : p.annotations) {
        QJsonArray points;
        for (auto pt : a.points)
            points.append(pointJson(pt));
        QJsonArray widths; for(auto x:a.widthScales) widths.append(x);
        annotations.append(QJsonObject{{"widthScales",widths},{"id", a.id},
                                       {"timelineId", a.timelineId},
                                       {"frame", integer(a.frame)},
                                       {"endFrame", integer(a.endFrame)},
                                       {"layer", a.layer},
                                       {"visible", a.visible},
                                       {"hold", a.hold},
                                       {"ghostFrames", a.ghostFrames},
                                       {"tool", static_cast<int>(a.tool)},
                                       {"opacity", a.opacity},
                                       {"background", a.background},
                                       {"points", points},
                                       {"color", a.color.name(QColor::HexArgb)},
                                       {"width", a.width},
                                       {"text", a.text},
                                       {"textPosition", pointJson(a.textPosition)},
                                       {"fontSize", a.fontSize}});
    }
    o["annotations"] = annotations;
    auto &v = p.viewer;
    QJsonArray assignments;
    for (const auto &id : v.assignments)
        assignments.append(id);
    QJsonArray transforms;
    for (auto t : v.transforms)
        transforms.append(QJsonObject{{"zoom", t.zoom},
                                      {"pan", pointJson(t.pan)},
                                      {"rotation", t.rotation},
                                      {"flipHorizontal", t.flipHorizontal},
                                      {"flipVertical", t.flipVertical}});
    o["viewer"] =
        QJsonObject{{"mode", static_cast<int>(v.mode)},
                    {"assignments", assignments},
                    {"activePane", v.activePane},
                    {"wipe", v.wipe},
                    {"wipePivotY",v.wipePivotY},{"wipeAngleDegrees",v.wipeAngleDegrees},
                    {"color",colorSettingsJson(v.color)},{"showPictureType",v.showPictureType},
                    {"opacity", v.opacity},
                    {"splitVertical", v.splitVertical},
                    {"transforms", transforms},
                    {"panZoom", panZoomJson(v.panZoom)},
                    {"overlaySettings", overlayJson(v.overlay, dir)},
                    {"overlay", v.showOverlay},
                    {"annotations", v.showAnnotations},
                    {"holdAnnotations", v.holdAnnotations},
                    {"grid", v.overlayGrid},
                    {"safe", v.overlaySafe},
                    {"center", v.overlayCenter},
                    {"background", v.background.name(QColor::HexArgb)},
                    {"annotationBackground", v.annotationBackground.name(QColor::HexArgb)},
                    {"ghostEnabled", v.ghostEnabled},
                    {"ghostPrevious", v.ghostPrevious},
                    {"ghostNext", v.ghostNext},
                    {"ghostByAnnotations", v.ghostByAnnotations},
                    {"ghostPreviousTint", v.ghostPreviousTint.name(QColor::HexArgb)},
                    {"ghostNextTint", v.ghostNextTint.name(QColor::HexArgb)},
                    {"width", v.outputSize.width()},
                    {"height", v.outputSize.height()}};
    return o;
}

bool projectFromJson(const QJsonObject &o, Project &out, QString *error, const QString &dir) {
    try {
        if (o["format"] != "OpenFrame" || o["version"].toInt() != 1)
            throw std::runtime_error("Unsupported project format or version");
        Project p;
        p.name = o["name"].toString("Untitled");
        p.fps = readRate(o["fps"]);
        p.activeTimelineId = o["activeTimelineId"].toString();
        p.defaultTimelineId = o["defaultTimelineId"].toString(p.activeTimelineId);
        p.rangeIn = readInt(o["rangeIn"]);
        p.rangeOut = readInt(o["rangeOut"], -1);
        p.loopMode = static_cast<LoopMode>(std::clamp(o["loopMode"].toInt(1), 0, 2));
        p.loopDelayMs = int(boundedNumber(o, "loopDelayMs", 0, 0, 600000));
        p.loopHoldEmpty = o["loopHoldEmpty"].toBool();
        auto root = o["sourceRootDirectory"].toString();
        p.sourceRootDirectory = root.isEmpty() ? QString{} : pathForRead(root, dir);
        if (!p.sourceRootDirectory.isEmpty()) {
            QString pathError;
            if (LocalPathPolicy::validateOutput(
                    QDir(p.sourceRootDirectory).filePath(".openframe-policy-check"), &pathError)
                    .isEmpty())
                throw std::runtime_error(pathError.toStdString());
        }
        const auto mediaDirectory = p.sourceRootDirectory.isEmpty() ? dir : p.sourceRootDirectory;
        auto savedViews = o["viewTransforms"].toObject();
        const auto panZoomViews = o["panZoomByOwner"].toObject();
        for (auto it = panZoomViews.begin(); it != panZoomViews.end(); ++it)
            p.panZoomByOwner[it.key()] = readPanZoom(it.value().toObject());
        for (auto it = savedViews.begin(); it != savedViews.end(); ++it) {
            auto a = it.value().toObject();
            ViewTransform t;
            t.zoom = std::clamp(a["zoom"].toDouble(1), .01, 100.0);
            t.pan = readPoint(a["pan"]);
            t.rotation = a["rotation"].toInt() % 360;
            t.flipHorizontal = a["flipHorizontal"].toBool();
            t.flipVertical = a["flipVertical"].toBool();
            p.viewTransforms[it.key()] = t;
        }
        QSet<QString> ids;
        auto checkId = [&](const QString &id) {
            if (id.isEmpty() || ids.contains(id))
                throw std::runtime_error("Missing or duplicate project ID");
            ids.insert(id);
        };
        for (auto x : o["folders"].toArray()) {
            auto a = x.toObject();
            MediaFolder f{a["id"].toString(), a["name"].toString(), a["parentId"].toString()};
            checkId(f.id);
            p.folders.append(f);
        }
        for (auto x : o["sources"].toArray()) {
            auto a = x.toObject();
            MediaSource s;
            s.id = a["id"].toString();
            checkId(s.id);
            s.name = a["name"].toString();
            s.path = pathForRead(a["path"].toString(), mediaDirectory);
            s.folderId = a["folderId"].toString();
            s.inFrame = readInt(a["inFrame"]);
            s.outFrame = readInt(a["outFrame"], -1);
            s.timecodeOffset = readInt(a["timecodeOffset"]);
            s.timecodeOffsetEnabled = a["timecodeOffsetEnabled"].toBool();
            s.selectedAudioTrack = a["selectedAudioTrack"].toInt(-1);
            s.audioOffsetUs = readInt(a.value("audioOffsetUs"));
            if (!validateSourceAudioOffset(s.audioOffsetUs))
                throw std::runtime_error("Invalid source audio offset");
            s.kind = static_cast<SourceKind>(std::clamp(a["kind"].toInt(), 0, 3));
            QString e;
            if (LocalPathPolicy::validateOutput(s.path, &e).isEmpty())
                throw std::runtime_error(e.toStdString());
            s.missing = !QFileInfo::exists(s.path);
            for (auto v : a["sequence"].toArray()) {
                auto path = pathForRead(v.toString(), mediaDirectory);
                if (LocalPathPolicy::validateOutput(path, &e).isEmpty())
                    throw std::runtime_error(e.toStdString());
                s.sequencePaths.append(path);
                s.missing |= !QFileInfo::exists(path);
            }
            auto i = a["info"].toObject();
            s.info.hasVideo = i["video"].toBool();
            s.info.hasAudio = i["audio"].toBool();
            s.info.variableFrameRate = i["variableFrameRate"].toBool();
            s.info.width = i["width"].toInt();
            s.info.height = i["height"].toInt();
            s.info.sampleRate = i["sampleRate"].toInt();
            s.info.channels = i["channels"].toInt();
            s.info.durationUs = readInt(i["durationUs"]);
            s.info.videoStartUs = readInt(i["videoStartUs"]);
            s.info.audioStartUs = readInt(i["audioStartUs"]);
            s.info.fps = readRate(i["fps"]);
            s.info.timeBase = readRate(i["timeBase"], {1, 1000000});
            if (s.info.width < 0 || s.info.height < 0 || s.info.width > 32768 ||
                s.info.height > 32768 || s.info.durationUs < 0)
                throw std::runtime_error("Invalid media dimensions or duration");
            auto audioPts = i["audioPacketPts"].toArray();
            auto audioSamples = i["audioSampleStarts"].toArray();
            if (audioPts.size() > 4000000 || audioPts.size() != audioSamples.size())
                throw std::runtime_error("Invalid audio sample index");
            for (auto v : audioPts)
                s.info.audioPacketPts.append(readInt(v));
            for (auto v : audioSamples)
                s.info.audioSampleStarts.append(readInt(v));
            if (!std::is_sorted(s.info.audioPacketPts.begin(), s.info.audioPacketPts.end()) ||
                !std::is_sorted(s.info.audioSampleStarts.begin(), s.info.audioSampleStarts.end()))
                throw std::runtime_error("Invalid audio sample index order");
            auto pts = i["pts"].toArray();
            if (pts.size() > 4000000)
                throw std::runtime_error("Media index exceeds safety limit");
            for (auto v : pts)
                s.info.framePts.append(readInt(v));
            for (auto v : i["durations"].toArray())
                s.info.frameDurations.append(readInt(v));
            if (!std::is_sorted(s.info.framePts.begin(), s.info.framePts.end()) ||
                (!s.info.frameDurations.isEmpty() &&
                 s.info.frameDurations.size() != s.info.framePts.size()))
                throw std::runtime_error("Invalid presentation timestamp index");
            if (s.info.hasVideo && s.info.framePts.isEmpty())
                throw std::runtime_error("Video has no presentation timestamp index");
            requireMemberType(i,"color",QJsonValue::Object);
            requireMemberType(i,"pictureTypes",QJsonValue::String);
            requireMemberType(i,"keyFrames",QJsonValue::String);
            requireMemberType(i,"frameColors",QJsonValue::Array);
            s.info.color=readVideoColor(i.value("color").toObject());
            s.info.pictureTypes=i["pictureTypes"].toString().toLatin1();
            s.info.keyFrames=i["keyFrames"].toString().toLatin1();
            if((!s.info.pictureTypes.isEmpty()&&s.info.pictureTypes.size()!=s.info.framePts.size())||
               (!s.info.keyFrames.isEmpty()&&s.info.keyFrames.size()!=s.info.framePts.size()))
                throw std::runtime_error("Frame metadata index size mismatch");
            for(auto type:s.info.pictureTypes) if(!QByteArray("IPBSipb?").contains(type))
                throw std::runtime_error("Invalid picture type");
            for(auto key:s.info.keyFrames) if(key!='0'&&key!='1') throw std::runtime_error("Invalid key-frame flag");
            for(auto item:i["frameColors"].toArray()) {
                if(!item.isObject())throw std::runtime_error("Invalid color change record");
                auto change=item.toObject();
                requireMemberType(change,"color",QJsonValue::Object);
                auto frame=readInt(change["frame"]);
                if(frame<0||frame>=s.info.framePts.size()||s.info.frameColors.contains(frame))
                    throw std::runtime_error("Invalid frame color index");
                s.info.frameColors[frame]=readVideoColor(change["color"].toObject());
            }
            const auto tracks = i["audioTracks"].toArray();
            if (tracks.size() > 64)
                throw std::runtime_error("Too many audio tracks");
            QSet<int> streamIds;
            for (const auto &item : tracks) {
                const auto a = item.toObject();
                AudioTrack t;
                t.streamIndex = a["streamIndex"].toInt(-1);
                if (t.streamIndex < 0 || streamIds.contains(t.streamIndex))
                    throw std::runtime_error("Invalid audio stream index");
                streamIds.insert(t.streamIndex);
                t.codec = a["codec"].toString();
                t.title = a["title"].toString();
                t.language = a["language"].toString();
                t.sampleRate = int(boundedNumber(a, "sampleRate", 48000, 1, 768000));
                t.channels = int(boundedNumber(a, "channels", 2, 1, 64));
                t.startUs = readInt(a["startUs"]);
                t.durationUs = readInt(a["durationUs"]);
                if (t.durationUs < 0)
                    throw std::runtime_error("Invalid audio track duration");
                t.isDefault = a["isDefault"].toBool();
                const auto pts = a["packetPts"].toArray(), samples = a["sampleStarts"].toArray();
                if (pts.size() != samples.size() || pts.size() > 4000000)
                    throw std::runtime_error("Invalid audio track PCM index");
                for (auto v : pts)
                    t.packetPts.append(readInt(v));
                for (auto v : samples)
                    t.sampleStarts.append(readInt(v));
                if (!std::is_sorted(t.packetPts.begin(), t.packetPts.end()) ||
                    !std::is_sorted(t.sampleStarts.begin(), t.sampleStarts.end()))
                    throw std::runtime_error("Non-monotonic audio track index");
                s.info.audioTracks.append(t);
            }
            QString trackError;
            if (!selectSourceAudioTrack(s, s.selectedAudioTrack, &trackError))
                throw std::runtime_error(trackError.toStdString());
            p.sources.append(s);
        }
        for (auto x : o["timelines"].toArray()) {
            auto a = x.toObject();
            Timeline t;
            t.id = a["id"].toString();
            checkId(t.id);
            t.name = a["name"].toString();
            t.fps = readRate(a["fps"]);
            t.folderId = a["folderId"].toString();
            t.timecodeOffset = readInt(a["timecodeOffset"]);
            t.timecodeOffsetEnabled = a["timecodeOffsetEnabled"].toBool();
            for (auto y : a["clips"].toArray()) {
                auto b = y.toObject();
                Clip c;
                c.id = b["id"].toString();
                checkId(c.id);
                c.sourceId = b["sourceId"].toString();
                c.name = b["name"].toString();
                c.inFrame = readInt(b["in"]);
                c.outFrame = readInt(b["out"], -1);
                c.audioSourceId = b["audioSourceId"].toString();
                c.audioOffsetUs = readInt(b["audioOffsetUs"]);
                c.gain = std::clamp(b["gain"].toDouble(1), 0.0, 8.0);
                c.muted = b["muted"].toBool();
                auto source = p.source(c.sourceId);
                if (!source || c.inFrame < 0 || c.inFrame >= source->info.frameCount() ||
                    c.outFrame >= source->info.frameCount() ||
                    (c.outFrame >= 0 && c.outFrame < c.inFrame) ||
                    (!c.audioSourceId.isEmpty() && !p.source(c.audioSourceId)))
                    throw std::runtime_error("Invalid clip reference or source range");
                t.clips.append(c);
            }
            p.timelines.append(t);
        }
        if (!p.activeTimelineId.isEmpty() && !p.activeTimeline())
            throw std::runtime_error("Active timeline does not exist");
        if (!p.defaultTimelineId.isEmpty() && !p.timeline(p.defaultTimelineId))
            throw std::runtime_error("Default timeline does not exist");
        for (auto x : o["bookmarks"].toArray()) {
            auto a = x.toObject();
            Bookmark b;
            b.id = a["id"].toString();
            checkId(b.id);
            b.timelineId = a["timelineId"].toString();
            b.frame = readInt(a["frame"]);
            b.endFrame = readInt(a["endFrame"], b.frame);
            b.name = a["name"].toString();
            b.text = a["text"].toString();
            b.color = QColor(a["color"].toString());
            p.bookmarks.append(b);
        }
        for (auto x : o["notes"].toArray()) {
            auto a = x.toObject();
            Note n;
            n.id = a["id"].toString();
            checkId(n.id);
            n.timelineId = a["timelineId"].toString();
            n.frame = readInt(a["frame"]);
            n.text = a["text"].toString();
            n.author = a["author"].toString();
            n.color = QColor(a["color"].toString("#00000000"));
            n.created = QDateTime::fromString(a["created"].toString(), Qt::ISODateWithMs);
            p.notes.append(n);
        }
        for (auto x : o["annotations"].toArray()) {
            auto b = x.toObject();
            Annotation a;
            a.id = b["id"].toString();
            checkId(a.id);
            a.timelineId = b["timelineId"].toString();
            a.frame = readInt(b["frame"]);
            a.endFrame = readInt(b["endFrame"], a.frame);
            a.layer = b["layer"].toString("Default");
            a.visible = b["visible"].toBool(true);
            a.hold = b["hold"].toBool();
            a.ghostFrames = std::clamp(b["ghostFrames"].toInt(), 0, 120);
            a.tool = static_cast<AnnotationTool>(std::clamp(b["tool"].toInt(), 0, 5));
            a.opacity = std::clamp(b["opacity"].toDouble(1), 0.0, 1.0);
            a.background = b["background"].toBool();
            for (auto pt : b["points"].toArray())
                a.points.append(readPoint(pt));
            requireMemberType(b,"widthScales",QJsonValue::Array);
            for(auto x:b["widthScales"].toArray()) {
                if(!x.isDouble()) throw std::runtime_error("Invalid width scale");
                a.widthScales.append(x.toDouble());
            }
            a.color = QColor(b["color"].toString());
            a.width = std::clamp(b["width"].toDouble(3), 0.1, 2000.0);
            a.text = b["text"].toString();
            a.textPosition = readPoint(b["textPosition"]);
            a.fontSize = std::clamp(b["fontSize"].toInt(28), 6, 300);
            QString annotationError;
            if(!validateAnnotation(a,&annotationError)) throw std::runtime_error(annotationError.toStdString());
            p.annotations.append(a);
        }
        auto v = o["viewer"].toObject();
        p.viewer.mode = static_cast<ViewerMode>(std::clamp(v["mode"].toInt(), 0, 5));
        for (auto id : v["assignments"].toArray())
            p.viewer.assignments.append(id.toString());
        p.viewer.activePane = std::clamp(v["activePane"].toInt(), 0, 3);
        p.viewer.wipe = std::clamp(v["wipe"].toDouble(.5), 0.0, 1.0);
        p.viewer.wipePivotY=boundedNumber(v,"wipePivotY",.5,0,1);
        p.viewer.wipeAngleDegrees=boundedNumber(v,"wipeAngleDegrees",0,-360000,360000);
        requireMemberType(v,"color",QJsonValue::Object);
        requireMemberType(v,"showPictureType",QJsonValue::Bool);
        p.viewer.color=readColorSettings(v.value("color").toObject());
        p.viewer.showPictureType=v["showPictureType"].toBool();
        p.viewer.opacity = std::clamp(v["opacity"].toDouble(.5), 0.0, 1.0);
        p.viewer.splitVertical = v["splitVertical"].toBool(true);
        p.viewer.transforms.clear();
        for (auto x : v["transforms"].toArray()) {
            auto a = x.toObject();
            ViewTransform t;
            t.zoom = std::clamp(a["zoom"].toDouble(1), .01, 100.0);
            t.pan = readPoint(a["pan"]);
            t.rotation = a["rotation"].toInt() % 360;
            t.flipHorizontal = a["flipHorizontal"].toBool();
            t.flipVertical = a["flipVertical"].toBool();
            p.viewer.transforms.append(t);
        }
        while (p.viewer.transforms.size() < 4)
            p.viewer.transforms.append(ViewTransform{});
        if (v.contains("panZoom"))
            p.viewer.panZoom = readPanZoom(v["panZoom"].toObject());
        else {
            // Old OpenFrame projects stored separate pane zooms. Preserve the primary
            // owner's view as the new whole-canvas zoom; rotation and flips remain intact.
            p.viewer.panZoom.zoom = std::clamp(p.viewer.transforms[0].zoom, .01, 64.0);
            p.viewer.panZoom.pan = p.viewer.transforms[0].pan;
            for (auto it = p.viewTransforms.cbegin(); it != p.viewTransforms.cend(); ++it)
                if (it.key().endsWith(":0"))
                    p.panZoomByOwner[it.key().chopped(2)] =
                        PanZoom{std::clamp(it->zoom, .01, 64.0), it->pan, true};
        }
        if (!v.contains("panZoom"))
            p.panZoomByOwner[viewerPaneOwner(p, 0)] = p.viewer.panZoom;
        p.viewer.showOverlay = v["overlay"].toBool(true);
        p.viewer.showAnnotations = v["annotations"].toBool(true);
        p.viewer.holdAnnotations = v["holdAnnotations"].toBool();
        p.viewer.overlayGrid = v["grid"].toBool();
        p.viewer.overlaySafe = v["safe"].toBool();
        p.viewer.overlayCenter = v["center"].toBool();
        if (v.contains("overlaySettings"))
            p.viewer.overlay = readOverlay(v["overlaySettings"].toObject(), dir);
        else
            p.viewer.overlay.type = p.viewer.overlayGrid     ? OverlayType::Grid
                                    : p.viewer.overlaySafe   ? OverlayType::SafeArea
                                    : p.viewer.overlayCenter ? OverlayType::Center
                                                             : OverlayType::Off;
        p.viewer.background = QColor(v["background"].toString("#18191d"));
        p.viewer.annotationBackground = QColor(v["annotationBackground"].toString("#00000000"));
        p.viewer.ghostEnabled = v["ghostEnabled"].toBool();
        p.viewer.ghostPrevious = std::clamp(v["ghostPrevious"].toInt(1), 0, 120);
        p.viewer.ghostNext = std::clamp(v["ghostNext"].toInt(1), 0, 120);
        p.viewer.ghostByAnnotations = v["ghostByAnnotations"].toBool();
        p.viewer.ghostPreviousTint = QColor(v["ghostPreviousTint"].toString("#ffff5a5a"));
        p.viewer.ghostNextTint = QColor(v["ghostNextTint"].toString("#ff5aa0ff"));
        p.viewer.outputSize = {std::clamp(v["width"].toInt(960), 16, 8192),
                               std::clamp(v["height"].toInt(540), 16, 8192)};
        out = std::move(p);
        return true;
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return false;
    }
}
bool saveProjectFile(const Project &p, const QString &path, QString *error) {
    auto safe = LocalPathPolicy::validateOutput(path, error);
    if (safe.isEmpty())
        return false;
    QSaveFile file(safe);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    auto bytes = QJsonDocument(projectToJson(p, QFileInfo(safe).absolutePath())).toJson();
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}
bool loadProjectFile(const QString &path, Project &p, QString *error) {
    auto safe = LocalPathPolicy::validateInput(path, error);
    if (safe.isEmpty())
        return false;
    QFile file(safe);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    if (file.size() > 256 * 1024 * 1024) {
        if (error)
            *error = "Project exceeds 256 MiB limit";
        return false;
    }
    QJsonParseError pe;
    auto doc = QJsonDocument::fromJson(file.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error)
            *error = pe.errorString();
        return false;
    }
    return projectFromJson(doc.object(), p, error, QFileInfo(safe).absolutePath());
}
bool exportNotesCsv(const Project &p, const QString &path, QString *error) {
    auto safe = LocalPathPolicy::validateOutput(path, error);
    if (safe.isEmpty())
        return false;
    QSaveFile f(safe);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    QByteArray bytes = "id,timeline,frame,author,text,created,color\r\n";
    for (const auto &n : p.notes)
        bytes += (csvQuote(n.id) + ',' + csvQuote(n.timelineId) + ',' + QString::number(n.frame) +
                  ',' + csvQuote(n.author) + ',' + csvQuote(n.text) + ',' +
                  csvQuote(n.created.toString(Qt::ISODateWithMs)) + ',' +
                  csvQuote(n.color.name(QColor::HexArgb)) + "\r\n")
                     .toUtf8();
    if (f.write(bytes) != bytes.size() || !f.commit()) {
        if (error)
            *error = f.errorString();
        return false;
    }
    return true;
}
bool importNotesCsv(Project &p, const QString &path, QString *error) {
    auto safe = LocalPathPolicy::validateInput(path, error);
    if (safe.isEmpty())
        return false;
    QFile f(safe);
    if (!f.open(QIODevice::ReadOnly) || f.size() > 16 * 1024 * 1024) {
        if (error)
            *error = "Cannot open notes CSV or file is too large";
        return false;
    }
    auto text = QString::fromUtf8(f.readAll());
    QVector<QStringList> rows;
    QStringList row;
    QString cell;
    bool quoted = false;
    for (qsizetype i = 0; i < text.size(); ++i) {
        auto c = text[i];
        if (c == '"') {
            if (quoted && i + 1 < text.size() && text[i + 1] == '"') {
                cell += '"';
                ++i;
            } else
                quoted = !quoted;
        } else if (!quoted && c == ',') {
            row.append(cell);
            cell.clear();
        } else if (!quoted && (c == '\n' || c == '\r')) {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
                ++i;
            row.append(cell);
            cell.clear();
            rows.append(row);
            row.clear();
        } else
            cell += c;
    }
    if (quoted) {
        if (error)
            *error = "Unterminated CSV quote";
        return false;
    }
    if (!cell.isEmpty() || !row.isEmpty()) {
        row.append(cell);
        rows.append(row);
    }
    if (rows.isEmpty() ||
        (rows[0] != QStringList{"id", "timeline", "frame", "author", "text", "created"} &&
         rows[0] != QStringList{"id", "timeline", "frame", "author", "text", "created", "color"})) {
        if (error)
            *error = "Unsupported CSV columns";
        return false;
    }
    QVector<Note> parsed;
    for (qsizetype i = 1; i < rows.size(); ++i) {
        auto r = rows[i];
        if (r.size() != rows[0].size()) {
            if (error)
                *error = "CSV row has incorrect column count";
            return false;
        }
        bool ok = false;
        auto frame = r[2].toLongLong(&ok);
        if (!ok || frame < 0) {
            if (error)
                *error = "CSV frame is invalid";
            return false;
        }
        Note n;
        n.id = newId();
        n.timelineId = (p.timeline(r[1]) || p.source(r[1])) ? r[1] : p.activeTimelineId;
        n.frame = frame;
        n.author = r[3];
        n.text = r[4];
        n.created = QDateTime::fromString(r[5], Qt::ISODateWithMs);
        if (r.size() > 6) {
            n.color = QColor(r[6]);
            if (!n.color.isValid()) {
                if (error)
                    *error = "CSV note color is invalid";
                return false;
            }
        }
        parsed.append(n);
    }
    p.notes += parsed;
    return true;
}
} // namespace openframe
