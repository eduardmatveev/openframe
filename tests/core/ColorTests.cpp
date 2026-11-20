#include <QtTest>
#include <QTemporaryDir>
#include <QColorSpace>
#include <QImageReader>
#include <QJsonArray>
#include <QSignalSpy>
#include <atomic>
#include <mutex>
#include <thread>
#include "core/Project.h"
#include "core/ReviewSession.h"
#include "media/ColorPipeline.h"
#include "media/MediaDecoder.h"
#include "media/FrameComposer.h"
#include "media/Exporter.h"
#include "media/SequenceReader.h"
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
}
using namespace openframe;
class ColorTests : public QObject {
    Q_OBJECT
    QString fixture(QString name) {
        return QStringLiteral(OPENFRAME_FIXTURES_DIR) + "/" + name;
    }
    Project project(const MediaSource &source) {
        auto p = emptyProject();
        p.sources.append(source);
        p.viewer.assignments = {source.id};
        p.viewer.showOverlay = false;
        p.viewer.outputSize = {source.info.width, source.info.height};
        return p;
    }
    QByteArray nativeBytes(const VideoFrame &video) {
        auto f = video.native.get();
        if (!f)
            return {};
        auto n = av_image_get_buffer_size(AVPixelFormat(f->format), f->width, f->height, 1);
        QByteArray bytes(n, Qt::Uninitialized);
        if (av_image_copy_to_buffer(reinterpret_cast<uint8_t *>(bytes.data()), n, f->data,
                                    f->linesize, AVPixelFormat(f->format), f->width, f->height,
                                    1) < 0)
            return {};
        return bytes;
    }
  private slots:
    void nativeRamps_data() {
        QTest::addColumn<QString>("name");
        QTest::addColumn<int>("bits");
        for (auto label : {"sdr", "pq", "hlg"})
            for (int bits : {10, 12}) {
                auto name = QString("%1-ramp-%2bit.mkv").arg(label).arg(bits);
                QTest::newRow(name.toUtf8().constData()) << name << bits;
            }
    }
    void nativeRamps() {
        QFETCH(QString, name);
        QFETCH(int, bits);
        QVERIFY2(QFileInfo::exists(fixture(name)),
                 "Required independently generated ramp fixture missing");
        auto source = probeMedia(fixture(name));
        QCOMPARE(source.info.color.componentBits, bits);
        QCOMPARE(source.info.frameCount(), 24);
        QCOMPARE(source.info.pictureTypes.size(), 24);
        auto p = project(source);
        MediaEngine engine;
        auto native = engine.nativeFrame(source, 0);
        auto encoded = ColorPipeline::encodedRgba(native);
        const int maximum = (1 << bits) - 1;
        for (int x = 0; x <= maximum; ++x) {
            const auto gray = reinterpret_cast<const float *>(encoded.constScanLine(0)) + x * 4;
            QCOMPARE(qRound(gray[0] * maximum), x);
            QCOMPARE(gray[0], gray[1]);
            QCOMPARE(gray[1], gray[2]);
        }
        QTemporaryDir dir;
        ExportSettings e;
        e.path = dir.filePath("native.mkv");
        e.videoCodec = "ffv1";
        e.includeAudio = false;
        e.viewerComposition = false;
        e.includeAnnotations = false;
        e.size = p.viewer.outputSize;
        e.colorMode = ExportColorMode::PreserveSource;
        e.bitDepth = 0;
        QString error;
        QVERIFY2(validateExportSettings(p, e, &error), qPrintable(error));
        exportProject(p, e, {}, {});
        auto restored = probeMedia(e.path);
        QCOMPARE(restored.info.color.pixelFormat, source.info.color.pixelFormat);
        QCOMPARE(restored.info.color.primaries, source.info.color.primaries);
        QCOMPARE(restored.info.color.transfer, source.info.color.transfer);
        QCOMPARE(restored.info.color.matrix, source.info.color.matrix);
        QCOMPARE(restored.info.color.range, source.info.color.range);
        QCOMPARE(restored.info.frameCount(), 24);
        QCOMPARE(restored.info.durationUs, 1000000);
        MediaEngine output;
        for (int f = 0; f < 24; ++f)
            QCOMPARE(nativeBytes(output.nativeFrame(restored, f)),
                     nativeBytes(engine.nativeFrame(source, f)));
        // PNG16 is a normalized integer representation, not a left-shifted container code.
        e.kind = ExportKind::ImageSequence;
        e.path = dir.filePath("png");
        e.bitDepth = 16;
        e.lastFrame = 0;
        exportProject(p, e, {}, {});
        auto raster = readLocalRasterImage(e.path + "/frame_000001.png");
        QCOMPARE(raster.depth(), 64);
        auto values = raster.convertToFormat(QImage::Format_RGBA64);
        for (int y : {0, 16, 32, 48})
            for (int x = 0; x <= maximum; ++x) {
                auto pixel = reinterpret_cast<const QRgba64 *>(values.constScanLine(y))[x];
                const auto code = quint16(std::llround(double(x) * 65535 / maximum));
                QCOMPARE(pixel.red(), quint16(y == 0 || y == 16 ? code : 0));
                QCOMPARE(pixel.green(), quint16(y == 0 || y == 32 ? code : 0));
                QCOMPARE(pixel.blue(), quint16(y == 0 || y == 48 ? code : 0));
                QCOMPARE(pixel.alpha(), quint16(65535));
            }
        const auto profile = rasterColorInfo(raster);
        QCOMPARE(profile.transfer, source.info.color.transfer);
        QCOMPARE(profile.primaries, source.info.color.primaries);
        output.clear();
        engine.clear();
        qInfo() << name
                << "native 24 frames bit-exact; PNG16 all normalized codes and profile verified";
    }
    void composedHighBitPreservesAdjacentCodes() {
        auto source = probeMedia(fixture("sdr-ramp-12bit.mkv"));
        auto p = project(source);
        MediaEngine engine;
        auto image = FrameComposer::composeHighPrecision(p, 0, engine, p.viewer.outputSize, false);
        auto codes = ColorPipeline::quantize(image, 16);
        for (int x = 0; x < 4096; ++x)
            QCOMPARE(reinterpret_cast<const QRgba64 *>(codes.constScanLine(0))[x].red(),
                     quint16(std::llround(x * 65535.0 / 4095)));
        Annotation a;
        a.id = newId();
        a.timelineId = source.id;
        a.points = {{.1, .5}, {.9, .5}};
        a.width = 12;
        a.widthScales = {.2, 1};
        a.color = Qt::red;
        p.annotations.append(a);
        image = FrameComposer::composeHighPrecision(p, 0, engine, p.viewer.outputSize, true);
        codes = ColorPipeline::quantize(image, 16);
        for (int x = 0; x < 4096; ++x)
            QCOMPARE(reinterpret_cast<const QRgba64 *>(codes.constScanLine(0))[x].red(),
                     quint16(std::llround(x * 65535.0 / 4095)));
        QTemporaryDir dir;
        ExportSettings e;
        e.path = dir.filePath("composed.mkv");
        e.videoCodec = "ffv1";
        e.bitDepth = 12;
        e.size = p.viewer.outputSize;
        e.lastFrame = 0;
        e.includeAudio = false;
        exportProject(p, e, {}, {});
        auto rendered = probeMedia(e.path);
        QCOMPARE(rendered.info.color.componentBits, 12);
        auto restored = ColorPipeline::encodedRgba(engine.nativeFrame(rendered, 0));
        for (int x = 0; x < 4096; ++x)
            QCOMPARE(
                qRound(reinterpret_cast<const float *>(restored.constScanLine(0))[x * 4] * 4095),
                x);
        engine.clear();
    }
    void displayConversion_data() {
        QTest::addColumn<QString>("name");
        for (auto label : {"pq", "hlg"})
            for (int bits : {10, 12})
                QTest::newRow(qPrintable(QString("%1%2").arg(label).arg(bits)))
                    << QString("%1-ramp-%2bit.mkv").arg(label).arg(bits);
    }
    void displayConversion() {
        QFETCH(QString, name);
        auto source = probeMedia(fixture(name));
        auto p = project(source);
        MediaEngine engine;
        auto viewer = FrameComposer::compose(p, 0, engine, p.viewer.outputSize, false);
        int last = -1, changes = 0;
        for (int x = 0; x < viewer.width(); ++x) {
            auto value = viewer.pixelColor(x, 0).red();
            QVERIFY(value >= last);
            changes += value != last;
            last = value;
        }
        QCOMPARE(viewer.pixelColor(0, 0), QColor(Qt::black));
        QCOMPARE(viewer.pixelColor(viewer.width() - 1, 0), QColor(Qt::white));
        QVERIFY(changes > 180);
        QTemporaryDir dir;
        ExportSettings e;
        e.videoCodec = "ffv1";
        e.includeAudio = false;
        e.includeAnnotations = false;
        e.size = p.viewer.outputSize;
        e.lastFrame = 0;
        e.path = dir.filePath("display.mkv");
        exportProject(p, e, {}, {});
        auto output = probeMedia(e.path);
        QCOMPARE(output.info.color.primaries, QString("bt709"));
        QCOMPARE(output.info.color.transfer, QString("iec61966-2-1"));
        QCOMPARE(engine.frame(output, 0), viewer);
        e.kind = ExportKind::ImageSequence;
        e.path = dir.filePath("display_png");
        exportProject(p, e, {}, {});
        QCOMPARE(readLocalRasterImage(e.path + "/frame_000001.png")
                     .convertToFormat(QImage::Format_RGBA8888),
                 viewer);
        engine.clear();
    }
    void pqHlgReferenceValuesAndPeakPolicy() {
        QImage image(4, 1, QImage::Format_RGBA32FPx4);
        auto values = reinterpret_cast<float *>(image.bits());
        const double codes[] = {0, .508078421517399, .751827096247041, 1};
        for (int i = 0; i < 4; ++i) {
            for (int n = 0; n < 3; ++n)
                values[i * 4 + n] = float(codes[i]);
            values[i * 4 + 3] = 1;
        }
        auto source = sdrColorInfo(32);
        source.primaries = "bt2020";
        source.transfer = "smpte2084";
        auto target = sdrColorInfo(32);
        target.transfer = "linear";
        ColorSettings settings;
        auto linear = ColorPipeline::convertEncoded(image, source, target, settings, false);
        const double expected[] = {0, 1, 10, 100};
        for (int i = 0; i < 4; ++i)
            QVERIFY(std::abs(reinterpret_cast<const float *>(linear.constBits())[i * 4] -
                             expected[i]) < .002);
        auto defaultMap = ColorPipeline::quantize(
            ColorPipeline::convertEncoded(image, source, sdrColorInfo(), settings, true), 16);
        QCOMPARE(defaultMap.pixelColor(2, 0), QColor(Qt::white));
        QCOMPARE(defaultMap.pixelColor(3, 0), QColor(Qt::white));
        settings.sourcePeakNits = 10000;
        auto expanded = ColorPipeline::quantize(
            ColorPipeline::convertEncoded(image, source, sdrColorInfo(), settings, true), 16);
        QVERIFY(expanded.pixelColor(2, 0).redF() < .999);
        QCOMPARE(expanded.pixelColor(3, 0), QColor(Qt::white));
        source.transfer = "arib-std-b67";
        settings.sourcePeakNits = 1000;
        for (int i = 0; i < 4; ++i)
            for (int n = 0; n < 3; ++n)
                values[i * 4 + n] = .75f;
        linear = ColorPipeline::convertEncoded(image, source, target, settings, false);
        QVERIFY(std::abs(reinterpret_cast<const float *>(linear.constBits())[0] - 2.03152) < .001);
    }
    void limitedRgbAndImageProfiles() {
        auto f = av_frame_alloc();
        QVERIFY(f);
        f->format = AV_PIX_FMT_GBRP10LE;
        f->width = 2;
        f->height = 1;
        QVERIFY(av_frame_get_buffer(f, 32) >= 0);
        f->color_range = AVCOL_RANGE_MPEG;
        f->colorspace = AVCOL_SPC_RGB;
        f->color_primaries = AVCOL_PRI_BT709;
        f->color_trc = AVCOL_TRC_IEC61966_2_1;
        for (int plane = 0; plane < 3; ++plane) {
            auto data = reinterpret_cast<quint16 *>(f->data[plane]);
            data[0] = 64;
            data[1] = 940;
        }
        auto native = retainVideoFrame(f, 0);
        av_frame_free(&f);
        auto image = ColorPipeline::encodedRgba(native);
        auto pixels = reinterpret_cast<const float *>(image.constBits());
        QVERIFY(std::abs(pixels[0]) < 1e-7);
        QVERIFY(std::abs(pixels[4] - 1) < 1e-7);
        f = av_frame_alloc();
        QVERIFY(f);
        f->format = AV_PIX_FMT_YUVA444P10LE;
        f->width = 2;
        f->height = 1;
        QVERIFY(av_frame_get_buffer(f, 32) >= 0);
        f->color_range = AVCOL_RANGE_MPEG;
        f->colorspace = AVCOL_SPC_BT709;
        f->color_primaries = AVCOL_PRI_BT709;
        f->color_trc = AVCOL_TRC_IEC61966_2_1;
        for (int plane = 0; plane < 4; ++plane) {
            auto data = reinterpret_cast<quint16 *>(f->data[plane]);
            data[0] = plane == 0 ? 64 : plane == 3 ? 0 : 512;
            data[1] = plane == 0 ? 940 : plane == 3 ? 1023 : 512;
        }
        native = retainVideoFrame(f, 0);
        av_frame_free(&f);
        image = ColorPipeline::encodedRgba(native);
        pixels = reinterpret_cast<const float *>(image.constBits());
        for (int channel = 0; channel < 3; ++channel) {
            QVERIFY(std::abs(pixels[channel]) < 1e-7);
            QVERIFY(std::abs(pixels[4 + channel] - 1) < 1e-7);
        }
        QCOMPARE(pixels[3], 0.f);
        QCOMPARE(pixels[7], 1.f);
        QImage gamma(16, 16, QImage::Format_RGBA64);
        gamma.fill(QColor::fromRgbF(.5, .5, .5));
        gamma.setColorSpace(
            QColorSpace(QColorSpace::Primaries::SRgb, QColorSpace::TransferFunction::Gamma, 2.2));
        auto c = rasterColorInfo(gamma);
        QCOMPARE(c.transfer, QString("gamma22"));
        ColorPipeline::tagImage(image, c);
        QCOMPARE(image.colorSpace().transferFunction(), QColorSpace::TransferFunction::Gamma);
        QVERIFY(std::abs(image.colorSpace().gamma() - 2.2) < .001);
        gamma.setColorSpace(QColorSpace::AdobeRgb);
        c = rasterColorInfo(gamma);
        QCOMPARE(c.primaries, QString("adobergb"));
        QCOMPARE(c.transfer, QString("gamma22"));
        QCOMPARE(rasterColorInfo(QImage(2, 2, QImage::Format_RGB30)).componentBits, 10);
        QCOMPARE(rasterColorInfo(QImage(2, 2, QImage::Format_Grayscale16)).componentBits, 16);
        QCOMPARE(rasterColorInfo(QImage(2, 2, QImage::Format_RGBA32FPx4)).componentBits, 32);
        gamma.setColorSpace(QColorSpace::ProPhotoRgb);
        QVERIFY_EXCEPTION_THROWN(rasterColorInfo(gamma), std::runtime_error);
    }
    void invalidExportsAndMixedColor() {
        auto source = probeMedia(fixture("pq-ramp-10bit.mkv"));
        auto other = probeMedia(fixture("hlg-ramp-10bit.mkv"));
        auto p = project(source);
        p.sources.append(other);
        QTemporaryDir dir;
        ExportSettings e;
        e.path = dir.filePath("invalid.mp4");
        e.size = p.viewer.outputSize;
        e.includeAudio = false;
        e.bitDepth = 12;
        QString error;
        QVERIFY(!validateExportSettings(p, e, &error));
        QVERIFY_EXCEPTION_THROWN(exportProject(p, e, {}, {}), std::runtime_error);
        QVERIFY(!QFileInfo::exists(e.path));
        e.videoCodec = "ffv1";
        e.path = dir.filePath("invalid.mkv");
        e.bitDepth = 0;
        e.colorMode = ExportColorMode::PreserveSource;
        QVERIFY(!validateExportSettings(p, e, &error));
        e.viewerComposition = false;
        QVERIFY(validateExportSettings(p, e, &error));
        for (const auto &id : {source.id, other.id}) {
            Clip c;
            c.id = newId();
            c.sourceId = id;
            p.timelines[0].clips.append(c);
        }
        p.viewer.assignments = {p.activeTimelineId};
        QVERIFY(!validateExportSettings(p, e, &error));
        QVERIFY_EXCEPTION_THROWN(exportProject(p, e, {}, {}), std::runtime_error);
        QVERIFY(!QFileInfo::exists(e.path));
        e.lastFrame = 23;
        QVERIFY2(validateExportSettings(p, e, &error), qPrintable(error));
        e.kind = ExportKind::ImageSequence;
        e.imageFormat = "jpg";
        e.bitDepth = 16;
        QVERIFY(!validateExportSettings(p, e, &error));
    }
    void newModelRoundTripRejectsWrongTypes() {
        auto source = probeMedia(fixture("sdr-ramp-10bit.mkv"));
        auto p = project(source);
        Annotation a;
        a.id = newId();
        a.timelineId = source.id;
        a.points = {{.1, .1}, {.9, .9}};
        a.widthScales = {.1, .8};
        p.annotations.append(a);
        p.viewer.wipePivotY = .2;
        p.viewer.wipeAngleDegrees = 41;
        p.viewer.color.sourcePeakNits = 4000;
        p.viewer.showPictureType = true;
        auto json = projectToJson(p);
        Project restored;
        QString error;
        QVERIFY2(projectFromJson(json, restored, &error), qPrintable(error));
        QCOMPARE(restored.annotations[0].widthScales, a.widthScales);
        QCOMPARE(restored.viewer.wipeAngleDegrees, 41);
        QCOMPARE(restored.sources[0].info.pictureTypes, source.info.pictureTypes);
        for (const auto &wrong : {QJsonValue(true), QJsonValue("invalid"), QJsonValue(3)}) {
            auto bad = json;
            auto annotations = bad["annotations"].toArray();
            auto record = annotations[0].toObject();
            record["widthScales"] = wrong;
            annotations[0] = record;
            bad["annotations"] = annotations;
            restored.name = "Unchanged";
            QVERIFY(!projectFromJson(bad, restored, &error));
            QCOMPARE(restored.name, QString("Unchanged"));
            bad = json;
            auto viewer = bad["viewer"].toObject();
            viewer["color"] = wrong;
            bad["viewer"] = viewer;
            QVERIFY(!projectFromJson(bad, restored, &error));
        }
        auto old = json;
        auto viewer = old["viewer"].toObject();
        viewer.remove("color");
        viewer.remove("wipePivotY");
        viewer.remove("wipeAngleDegrees");
        old["viewer"] = viewer;
        QVERIFY(projectFromJson(old, restored, &error));
        QCOMPARE(restored.viewer.wipePivotY, .5);
        QCOMPARE(restored.viewer.wipeAngleDegrees, 0);
    }
    void pictureTypesMatchDecodeOrderAndReload() {
        auto source = probeMedia(fixture("bframes24-audio.mp4"));
        QVERIFY(source.info.pictureTypes.contains('I'));
        QVERIFY(source.info.pictureTypes.contains('P'));
        QVERIFY(source.info.pictureTypes.contains('B'));
        QCOMPARE(source.info.pictureTypes.size(), source.info.framePts.size());
        QCOMPARE(source.info.keyFrames.size(), source.info.framePts.size());
        MediaEngine engine;
        for (int n : {119, 0, 75, 2, 1, 60}) {
            auto frame = engine.nativeFrame(source, n);
            QVERIFY(frame.native);
            const char type = frame.native->pict_type == AV_PICTURE_TYPE_I   ? 'I'
                              : frame.native->pict_type == AV_PICTURE_TYPE_P ? 'P'
                              : frame.native->pict_type == AV_PICTURE_TYPE_B ? 'B'
                                                                             : '?';
            QCOMPARE(source.info.pictureTypes[n], type);
            QCOMPARE(frameIsKeyFrame(source.info, n),
                     bool(frame.native->flags & AV_FRAME_FLAG_KEY));
        }
        Project restored;
        QString error;
        QVERIFY(projectFromJson(projectToJson(project(source)), restored, &error));
        QCOMPARE(restored.sources[0].info.pictureTypes, source.info.pictureTypes);
        MediaInfo legacy;
        QCOMPARE(framePictureType(legacy, 0), QString("Unknown"));
        QVERIFY(!frameIsKeyFrame(legacy, 0));
    }
    void sequenceParallelBoundsReverseAndRepair() {
        MediaSource source;
        source.id = newId();
        source.path = "synthetic";
        source.kind = SourceKind::Sequence;
        source.info.hasVideo = true;
        source.info.width = 16;
        source.info.height = 16;
        for (int i = 0; i < 40; ++i) {
            source.sequencePaths.append(QString::number(i));
            source.info.framePts.append(i);
        }
        std::mutex mutex;
        QVector<int> reads;
        std::atomic_bool broken = false;
        SequenceReader reader(32768, 4, [&](const QString &path) {
            int n = path.toInt();
            {
                std::lock_guard lock(mutex);
                reads.append(n);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(n % 3 * 4 + 6));
            if (broken && n == 30)
                throw std::runtime_error("Synthetic missing file");
            QImage i(16, 16, QImage::Format_RGBA8888);
            i.fill(QColor(n, 0, 0));
            return i;
        });
        QCOMPARE(reader.frame(source, 0).raster.pixelColor(0, 0).red(), 0);
        QCOMPARE(reader.frame(source, 39).raster.pixelColor(0, 0).red(), 39);
        QCOMPARE(reader.frame(source, 38).raster.pixelColor(0, 0).red(), 38);
        QTest::qWait(50);
        {
            std::lock_guard lock(mutex);
            QVERIFY(reads.contains(37));
            QVERIFY(reads.contains(36));
        }
        auto d = reader.diagnostics();
        QVERIFY(d["maxActive"].toInt() > 1);
        QVERIFY(d["maxPending"].toInt() <= 8);
        QVERIFY(d["maxResidentBytes"].toDouble() <= 32768);
        reader.clear();
        broken = true;
        QVERIFY_EXCEPTION_THROWN(reader.frame(source, 30), std::runtime_error);
        broken = false;
        QCOMPARE(reader.frame(source, 30).raster.pixelColor(0, 0).red(), 30);
        MediaSource empty;
        QVERIFY_EXCEPTION_THROWN(reader.frame(empty, 0), std::runtime_error);
    }
    void legacyProjectMetadataRehydratesWithoutDirty() {
        auto source = probeMedia(fixture("pq-ramp-10bit.mkv"));
        auto p = project(source);
        auto json = projectToJson(p);
        auto sources = json["sources"].toArray();
        auto item = sources[0].toObject();
        auto info = item["info"].toObject();
        for (auto key : {"color", "frameColors", "pictureTypes", "keyFrames"})
            info.remove(key);
        item["info"] = info;
        sources[0] = item;
        json["sources"] = sources;
        QTemporaryDir dir;
        auto path = dir.filePath("legacy.oframe");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(json).toJson());
        file.close();
        ReviewSession session;
        session.setAudioEnabled(false);
        QSignalSpy errors(&session, &ReviewSession::error);
        QVERIFY(session.openProject(path));
        QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 10000);
        QVERIFY2(errors.isEmpty(),
                 errors.isEmpty() ? "" : qPrintable(errors.first()[0].toString()));
        QVERIFY(!session.dirty());
        const auto &restored = session.project().sources[0];
        QCOMPARE(restored.info.color.transfer, QString("smpte2084"));
        QCOMPARE(restored.info.color.componentBits, 10);
        QCOMPARE(restored.info.pictureTypes, source.info.pictureTypes);
        ExportSettings e;
        e.path = dir.filePath("restored.mkv");
        e.videoCodec = "ffv1";
        e.colorMode = ExportColorMode::PreserveSource;
        e.bitDepth = 0;
        e.viewerComposition = false;
        e.includeAnnotations = false;
        e.includeAudio = false;
        e.size = p.viewer.outputSize;
        e.lastFrame = 0;
        QString error;
        QVERIFY2(validateExportSettings(session.project(), e, &error), qPrintable(error));
        exportProject(session.project(), e, {}, {});
        QCOMPARE(probeMedia(e.path).info.color.transfer, QString("smpte2084"));
    }
    void conversionAndComposition1080pTenFrames() {
        QTemporaryDir dir;
        QImage image(1920, 1080, QImage::Format_RGBA64);
        for (int y = 0; y < 1080; ++y) {
            auto row = reinterpret_cast<QRgba64 *>(image.scanLine(y));
            for (int x = 0; x < 1920; ++x) {
                auto code = quint16(x * 65535 / 1919);
                row[x] = QRgba64::fromRgba64(code, code, code, 65535);
            }
        }
        for (bool hdr : {false, true}) {
            image.setColorSpace(hdr ? QColorSpace::Bt2100Pq : QColorSpace::SRgb);
            auto path = dir.filePath(hdr ? "hdr.png" : "sdr.png");
            QVERIFY(image.save(path));
            auto source = probeImageSequence({path}, {24, 1});
            auto p = project(source);
            MediaEngine engine;
            engine.nativeFrame(source, 0);
            QVector<double> times;
            for (int n = 0; n < 10; ++n) {
                QElapsedTimer clock;
                clock.start();
                auto result = FrameComposer::compose(p, 0, engine, p.viewer.outputSize, false);
                QVERIFY(!result.isNull());
                times.append(clock.nsecsElapsed() / 1000000.0);
            }
            double total = 0;
            for (auto t : times)
                total += t;
            qInfo() << "1920x1080" << (hdr ? "PQ to SDR" : "SDR identity")
                    << "10 compositions meanMs" << total / 10 << "maxMs"
                    << *std::max_element(times.begin(), times.end())
                    << "includes conversion, composition and final quantization; native cache warm";
            engine.clear();
        }
    }
    void viewerPreviewIsTransactional() {
        ReviewSession session;
        session.setAudioEnabled(false);
        auto initial = session.project().viewer;
        auto token = session.beginViewerPreview();
        auto v = initial;
        v.wipe = .3;
        v.wipeAngleDegrees = 90;
        QVERIFY(session.previewViewerSettings(token, v));
        QCOMPARE(session.project().viewer.wipe, .5);
        QCOMPARE(session.displayedViewerSettings().wipe, .3);
        QVERIFY(!session.dirty());
        QCOMPARE(session.undoStack()->count(), 0);
        session.clearViewerPreview(token);
        QCOMPARE(session.displayedViewerSettings().wipe, .5);
        QVERIFY(!session.commitViewerPreview(token, "stale"));
        token = session.beginViewerPreview();
        QVERIFY(session.previewViewerSettings(token, v));
        QVERIFY(session.commitViewerPreview(token, "Wipe drag"));
        QCOMPARE(session.undoStack()->count(), 1);
        QCOMPARE(session.project().viewer.wipe, .3);
        session.undoStack()->undo();
        QCOMPARE(session.project().viewer.wipe, .5);
        token = session.beginViewerPreview();
        session.addTimeline("External change");
        QVERIFY(!session.previewViewerSettings(token, v));
        token = session.beginViewerPreview();
        Annotation a;
        a.id = newId();
        a.points = {{.1, .1}, {.2, .2}};
        a.widthScales = {.2, .5};
        QVERIFY(session.previewAnnotation(token, a));
        QVERIFY(session.project().annotations.isEmpty());
        session.clearViewerPreview(token);
    }
};
QTEST_MAIN(ColorTests)
#include "ColorTests.moc"
