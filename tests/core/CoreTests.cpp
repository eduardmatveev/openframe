#include <QtTest>
#include <QTemporaryDir>
#include <QEventLoop>
#include <QTimer>
#include <QImage>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QSignalSpy>
#include <cmath>
#include <cstring>
#include <future>
#include <atomic>
#include <QtEndian>
#include "core/Project.h"
#include "core/ReviewSession.h"
#include "core/ReviewSessionTestAccess.h"
#include "media/MediaDecoder.h"
#include "media/FrameComposer.h"
#include "media/Exporter.h"
#include "media/AudioTimeStretcher.h"
#include "media/AudioOutputConverter.h"
using namespace openframe;
namespace {
struct AudioPresentation {
    qint64 playedUs;
    qint64 frame;
    qint64 wallUs = 0;
};
QJsonObject assessAudioPresentations(const QVector<AudioPresentation> &samples, Rational fps,
                                     double rate, qint64 anchor, qint64 firstUs, qint64 lastUs,
                                     qint64 lastWallUs) {
    const auto framePeriodUs = 1000000.0 / (fps.value() * std::abs(rate));
    const auto direction = rate < 0 ? -1 : 1;
    qint64 maximumLag = 0, maximumInterval = 0, maximumAge = 0;
    int count = 0;
    bool coveredStart = false;
    std::optional<AudioPresentation> previous;
    auto observeHold = [&](qint64 until, qint64 wallUntil) {
        if (!previous || until <= firstUs)
            return;
        maximumInterval = std::max(maximumInterval, wallUntil - previous->wallUs);
        const auto dueUs =
            qint64(std::floor(direction * (previous->frame - anchor) * framePeriodUs));
        maximumAge = std::max(maximumAge, until - dueUs);
    };
    for (const auto &sample : samples) {
        if (sample.playedUs > lastUs)
            break;
        observeHold(sample.playedUs, sample.wallUs);
        if (sample.playedUs <= firstUs)
            coveredStart = true;
        if (sample.playedUs >= firstUs) {
            const auto clockFrame =
                anchor + direction * fps.usToFrame(qint64(sample.playedUs * std::abs(rate)));
            maximumLag = std::max(maximumLag, std::abs(clockFrame - sample.frame));
            if (!previous || previous->frame != sample.frame)
                ++count;
        }
        previous = sample;
    }
    observeHold(lastUs, lastWallUs);
    // Cadence uses monotonic imageReady timestamps. The device's played counter
    // is quantized (10 ms on the tested WASAPI output); differencing it would
    // round the two interval endpoints independently and mismeasure GUI cadence.
    // Audio/video phase and content age still use the audio presentation clock.
    // At most one missing update between presentations. A delivered frame may be
    // one frame late and then remain visible until the next update, so its age
    // also includes those two frame intervals. This is separate from, and does
    // not relax, the <=1 frame synchronization bound at EVERY presentation.
    const auto intervalLimit = qint64(std::ceil(2 * framePeriodUs));
    const auto ageLimit = qint64(std::ceil(3 * framePeriodUs));
    const auto requiredCount = std::max(1, int(std::floor((lastUs - firstUs) / framePeriodUs)) - 2);
    QStringList failures;
    if (!coveredStart || lastUs - firstUs < 500000)
        failures.append("Presentation window lacks continuous audio-clock coverage");
    if (maximumLag > 1)
        failures.append(QString("Presented frame lag exceeds one frame: %1").arg(maximumLag));
    if (maximumInterval > intervalLimit)
        failures.append(QString("Presentation interval %1 us exceeds %2 us")
                            .arg(maximumInterval)
                            .arg(intervalLimit));
    if (maximumAge > ageLimit)
        failures.append(
            QString("Shown frame age %1 us exceeds %2 us").arg(maximumAge).arg(ageLimit));
    if (count < requiredCount)
        failures.append(QString("Insufficient presented frames: %1, expected at least %2")
                            .arg(count)
                            .arg(requiredCount));
    return {{"rate", rate},
            {"beginUs", firstUs},
            {"endUs", lastUs},
            {"count", count},
            {"requiredCount", requiredCount},
            {"maximumPresentedLag", maximumLag},
            {"maximumIntervalUs", maximumInterval},
            {"intervalClock", "monotonic"},
            {"intervalLimitUs", intervalLimit},
            {"maximumFrameAgeUs", maximumAge},
            {"ageLimitUs", ageLimit},
            {"error", failures.join("; ")}};
}
} // namespace
class CoreTests : public QObject {
    Q_OBJECT
    QString fixture(const QString &name) {
        return QStringLiteral(OPENFRAME_FIXTURES_DIR) + "/" + name;
    }
    Project withSource(const MediaSource &s) {
        auto p = emptyProject();
        p.sources.append(s);
        Clip c;
        c.id = newId();
        c.sourceId = s.id;
        c.name = s.name;
        p.timelines[0].clips.append(c);
        return p;
    }
    QVector<float> sineFrames(int frames, bool envelope = false, bool sameTone = false) {
        QVector<float> result(frames * 2);
        for (int i = 0; i < frames; ++i)
            for (int ch = 0; ch < 2; ++ch) {
                auto gain = envelope ? .05 + .15 * i / frames : .2;
                result[i * 2 + ch] = static_cast<float>(
                    gain * std::sin(2 * 3.14159265358979323846 * (ch && !sameTone ? 660 : 440) * i /
                                    48000.0));
            }
        return result;
    }
    double toneAmplitude(const QVector<float> &samples, int channel, double frequency,
                         int start = 0, int frames = 48000) {
        frames = std::min(frames, static_cast<int>(samples.size() / 2) - start);
        double real = 0, imaginary = 0;
        for (int i = 0; i < frames; ++i) {
            auto phase = 2 * 3.14159265358979323846 * frequency * i / 48000.0;
            real += samples[(start + i) * 2 + channel] * std::cos(phase);
            imaginary += samples[(start + i) * 2 + channel] * std::sin(phase);
        }
        return 2 * std::hypot(real, imaginary) / frames;
    }
    QVector<float> stretchSignal(const QVector<float> &samples, double tempo, int chunk) {
        try {
            AudioTimeStretcher processor(tempo);
            QVector<float> result;
            for (qsizetype offset = 0; offset < samples.size();) {
                auto count = std::min<qsizetype>(chunk * 2, samples.size() - offset);
                processor.push(
                    QVector<float>(samples.cbegin() + offset, samples.cbegin() + offset + count));
                result += processor.take(AudioTimeStretcher::MaximumBufferedFrames);
                offset += count;
            }
            processor.finish();
            while (processor.availableFrames() > 0)
                result += processor.take(AudioTimeStretcher::MaximumBufferedFrames);
            return result;
        } catch (const std::exception &e) {
            qWarning() << "Time-stretch failure:" << e.what();
            throw;
        }
    }
  private slots:
    void seekBurstCoalescesWithoutLosingAcceptedWork() {
        QTemporaryDir dir;
        QStringList paths;
        for (int i = 0; i < 3; ++i) {
            QImage frame(32, 32, QImage::Format_RGB32);
            frame.fill(i == 0 ? Qt::red : i == 1 ? Qt::green : Qt::blue);
            auto path = dir.filePath(QString("source_%1.bmp").arg(i));
            QVERIFY(frame.save(path));
            paths.append(path);
        }
        ReviewSession session;
        session.importImageSequence(paths, {24, 1});
        QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 10000);
        session.mutateProject("Test view", [](Project &p) {
            p.viewer.showOverlay = false;
            p.viewer.outputSize = {32, 32};
        });
        auto entered = std::make_shared<std::promise<void>>();
        auto enteredFuture = entered->get_future();
        auto release = std::make_shared<std::promise<void>>();
        auto releaseFuture = release->get_future().share();
        struct Unblock {
            std::shared_ptr<std::promise<void>> promise;
            void release() {
                if (promise) {
                    promise->set_value();
                    promise.reset();
                }
            }
            ~Unblock() {
                release();
            }
        } unblock{release};
        ReviewSessionTestAccess::enqueueWorker(session, [entered, releaseFuture]() {
            entered->set_value();
            releaseFuture.wait();
        });
        QVERIFY(enteredFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        std::atomic_int acceptedOperation{0};
        ReviewSessionTestAccess::enqueueWorker(session, [&]() { ++acceptedOperation; });
        session.seek(0, true);
        const auto cancelledEpoch = session.audioDiagnostics()["audioEpoch"].toInteger();
        int maximumPending = 0;
        for (int i = 0; i < 2000; ++i) {
            session.seek(i % 3, false);
            maximumPending =
                std::max(maximumPending, session.audioDiagnostics()["pendingWorkerJobs"].toInt());
        }
        session.seek(2, false);
        QVERIFY2(maximumPending <= 3, "Seek burst retained cancelled render/audio/reset jobs");
        QCOMPARE(acceptedOperation.load(), 0);
        QCOMPARE(session.audioDiagnostics()["pendingChunks"].toInt(), 0);
        QVERIFY(session.audioDiagnostics()["audioEpoch"].toInteger() > cancelledEpoch);

        // Import/export use their own workers and must retain every accepted
        // operation while the presentation worker is deliberately occupied.
        session.importMedia({paths.first()});
        QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 10000);
        QCOMPARE(session.project().sources.size(), 2);
        ExportSettings settings;
        settings.kind = ExportKind::ImageSequence;
        settings.path = dir.filePath("exported");
        settings.lastFrame = 2;
        settings.size = {32, 32};
        settings.includeAudio = false;
        QSignalSpy exported(&session, &ReviewSession::exportFinished);
        session.exportAsync(settings);
        QTRY_COMPARE_WITH_TIMEOUT(exported.size(), 1, 10000);
        QVERIFY(exported.first().first().toBool());
        QCOMPARE(QDir(settings.path).entryList({"*.png"}, QDir::Files).size(), 3);
        QCOMPARE(acceptedOperation.load(), 0);
        const auto finalEpoch = session.audioDiagnostics()["audioEpoch"].toInteger();
        unblock.release();
        QTRY_COMPARE_WITH_TIMEOUT(acceptedOperation.load(), 1, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(session.audioDiagnostics()["renderedFrame"].toInteger(),
                                  qint64(2), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(session.audioDiagnostics()["pendingWorkerJobs"].toInt(), 0,
                                  10000);
        QCOMPARE(session.audioDiagnostics()["audioEpoch"].toInteger(), finalEpoch);
        QVERIFY(!session.audioDiagnostics()["sinkActive"].toBool());
        QCOMPARE(session.displayImage().pixelColor(16, 16), QColor(Qt::blue));
    }
    void negotiatedAudioConversion_data() {
        QTest::addColumn<int>("sampleRate");
        QTest::addColumn<int>("channels");
        QTest::addColumn<int>("sampleFormat");
        QTest::newRow("44100-float-stereo") << 44100 << 2 << int(QAudioFormat::Float);
        QTest::newRow("44100-pcm16-stereo") << 44100 << 2 << int(QAudioFormat::Int16);
        QTest::newRow("44100-pcm16-mono") << 44100 << 1 << int(QAudioFormat::Int16);
        QTest::newRow("22050-u8-mono") << 22050 << 1 << int(QAudioFormat::UInt8);
        QTest::newRow("48000-int32-stereo") << 48000 << 2 << int(QAudioFormat::Int32);
        QTest::newRow("48000-float-stereo") << 48000 << 2 << int(QAudioFormat::Float);
    }
    void negotiatedAudioConversion() {
        QFETCH(int, sampleRate);
        QFETCH(int, channels);
        QFETCH(int, sampleFormat);
        QAudioFormat format;
        format.setSampleRate(sampleRate);
        format.setChannelCount(channels);
        format.setSampleFormat(static_cast<QAudioFormat::SampleFormat>(sampleFormat));
        QVERIFY(AudioOutputConverter::supports(format));
        auto input = sineFrames(96000, false, true);
        auto render = [&](int blockSize) {
            AudioOutputConverter converter(format);
            QByteArray output;
            for (qsizetype i = 0; i < input.size();) {
                auto n = std::min<qsizetype>(blockSize * 2, input.size() - i);
                output +=
                    converter.convert(QVector<float>(input.cbegin() + i, input.cbegin() + i + n));
                i += n;
            }
            output += converter.finish();
            return output;
        };
        const auto output = render(4096);
        QCOMPARE(output.size(), qsizetype(sampleRate * 2 * format.bytesPerFrame()));
        QCOMPARE(output, render(257));
        double real = 0, imaginary = 0;
        for (int i = 0; i < sampleRate; ++i) {
            auto pointer = output.constData() + (i + sampleRate / 2) * format.bytesPerFrame();
            double sample = 0;
            switch (format.sampleFormat()) {
            case QAudioFormat::Float: {
                float value;
                std::memcpy(&value, pointer, sizeof(value));
                sample = value;
                break;
            }
            case QAudioFormat::Int16:
                sample = qFromLittleEndian<qint16>(pointer) / 32768.0;
                break;
            case QAudioFormat::Int32:
                sample = qFromLittleEndian<qint32>(pointer) / 2147483648.0;
                break;
            case QAudioFormat::UInt8:
                sample = (static_cast<unsigned char>(*pointer) - 128) / 128.0;
                break;
            default:
                QFAIL("Unexpected device sample format");
            }
            const auto angle = 2 * 3.14159265358979323846 * 440 * i / sampleRate;
            real += sample * std::cos(angle);
            imaginary += sample * std::sin(angle);
        }
        const auto amplitude = 2 * std::hypot(real, imaginary) / sampleRate;
        QVERIFY(amplitude > .18 && amplitude < .3);
        // A new epoch must contain only its new input, without previous SWR tail.
        AudioOutputConverter clean(format);
        auto silence = clean.convert(QVector<float>(4800 * 2, 0.0f));
        silence += clean.finish();
        for (char sample : silence)
            QCOMPARE(static_cast<unsigned char>(sample),
                     format.sampleFormat() == QAudioFormat::UInt8 ? 128 : 0);
    }
    void exactPresentationBoundaries() {
        for (const auto &name :
             {"sync24-lossless.mkv", "sync24-vfr-audio.mkv", "rational23976-stereo44100.mkv"}) {
            auto source = probeMedia(fixture(name));
            for (qint64 f = 1; f < source.info.frameCount(); ++f) {
                const auto start = source.info.frameStartUs(f);
                QCOMPARE(source.info.frameAtUs(start), f);
                QCOMPARE(source.info.frameAtUs(start - 1), f - 1);
                QCOMPARE(source.info.frameAtUs(source.info.frameEndUs(f - 1) - 1), f - 1);
            }
        }
        MediaInfo rational;
        rational.hasVideo = true;
        rational.timeBase = {1001, 24000};
        rational.framePts = {0, 1, 2, 3, 4};
        for (int f = 1; f < 5; ++f) {
            QCOMPARE(rational.frameAtUs(rational.frameStartUs(f)), qint64(f));
            QCOMPARE(rational.frameAtUs(rational.frameStartUs(f) - 1), qint64(f - 1));
        }
        MediaInfo coarse;
        coarse.hasVideo = true;
        coarse.timeBase = {1, 1000};
        coarse.framePts = {0, 42, 83, 125};
        QCOMPARE(coarse.frameAtUs(41667), qint64(0));
        QCOMPARE(coarse.frameAtSamplingUs(41667), qint64(1));
        QCOMPARE(coarse.frameAtUs(124999), qint64(2));
    }
    void audioTempoSignals_data() {
        QTest::addColumn<double>("tempo");
        for (auto rate : {.0625, .5, 1., 2., 16.})
            QTest::newRow(qPrintable(QString::number(rate))) << rate;
    }
    void audioTempoSignals() {
        QFETCH(double, tempo);
        const int inputFrames = static_cast<int>(48000 * std::max(4.0, 4 * tempo));
        auto input = sineFrames(inputFrames, false, true);
        auto output = stretchSignal(input, tempo, 4096);
        const auto expected = static_cast<qint64>(std::llround(inputFrames / tempo));
        const auto difference = output.size() / 2 - expected;
        qInfo() << "tempo" << tempo << "inputFrames" << inputFrames << "outputFrames"
                << output.size() / 2 << "durationErrorUs" << difference * 1000000 / 48000;
        // A finite WSOLA stream has overlap-window error at its ends. Bound this
        // to 50 ms in source time when slowing down, or 50 ms of output otherwise.
        // Continuous transport uses an exact output clock, with silence outside
        // the owner/range; it never shortens playback to the filter's EOF count.
        const auto boundaryTolerance = static_cast<qint64>(std::ceil(2400 / std::min(1.0, tempo)));
        QVERIFY2(std::abs(difference) <= boundaryTolerance,
                 "Tempo finite-stream duration exceeds its declared boundary tolerance");
        QVERIFY(output.size() / 2 > 96000);
        const auto middle = static_cast<int>(output.size() / 4 - 24000);
        QVERIFY(toneAmplitude(output, 0, 440, middle) > .17);
        QVERIFY(toneAmplitude(output, 1, 440, middle) > .17);
        QVERIFY(toneAmplitude(output, 0, 660, middle) < .005);
        if (tempo != 1)
            QVERIFY(toneAmplitude(output, 0, 440 * tempo, middle) < .01);
        else
            QCOMPARE(output, input);
    }
    void audioTempoContinuousAndReverse() {
        auto input = sineFrames(48000 * 4, true);
        auto regular = stretchSignal(input, .5, 4096);
        auto fragmented = stretchSignal(input, .5, 257);
        QCOMPARE(regular.size(), fragmented.size());
        double maximumDifference = 0;
        for (qsizetype i = 0; i < regular.size(); ++i)
            maximumDifference = std::max(maximumDifference,
                                         static_cast<double>(std::abs(regular[i] - fragmented[i])));
        QVERIFY2(maximumDifference < 1e-6,
                 "Changing transport block boundaries changes the audio signal");
        for (qsizetype a = 0, b = input.size() / 2 - 1; a < b; ++a, --b)
            for (int ch = 0; ch < 2; ++ch)
                std::swap(input[a * 2 + ch], input[b * 2 + ch]);
        auto reverse = stretchSignal(input, .5, 1031);
        QVERIFY(toneAmplitude(reverse, 0, 440, 24000) > .15);
        QVERIFY(toneAmplitude(reverse, 1, 660, 24000) > .15);
        QVERIFY(toneAmplitude(reverse, 0, 440, static_cast<int>(reverse.size() / 2 - 72000)) < .09);
        AudioTimeStretcher bounded(.0625);
        int maximumBuffered = 0;
        for (int i = 0; i < 100; ++i) {
            bounded.push(sineFrames(4096));
            maximumBuffered = std::max(maximumBuffered, bounded.availableFrames());
            bounded.take(bounded.availableFrames());
        }
        QVERIFY(maximumBuffered > 0);
        QVERIFY(maximumBuffered < AudioTimeStretcher::MaximumBufferedFrames);
    }
    void audioTempoExactBackpressureBoundary() {
        AudioTimeStretcher processor(.5);
        for (int i = 0; i < 64 && processor.availableFrames() < 96000; ++i)
            processor.push(sineFrames(4096));
        QVERIFY(processor.availableFrames() >= 96000);
        processor.take(processor.availableFrames() - 96000);
        QCOMPARE(processor.availableFrames(), 96000);
        const auto before = processor.inputFrames();
        for (int i = 0; i < 16; ++i)
            QVERIFY_EXCEPTION_THROWN(processor.push(sineFrames(1024)), std::runtime_error);
        QCOMPARE(processor.inputFrames(), before);
        QCOMPARE(processor.availableFrames(), 96000);
    }
    void pausedSeekAndScrubDisableCancelPendingAudio() {
        ReviewSession session;
        if (!session.audioDiagnostics()["deviceAvailable"].toBool())
            QSKIP("No hardware output; pending audio scrub cancellation has NOT been verified");
        session.importMedia({fixture("two-audio-tracks.mkv")});
        QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 10000);
        session.seek(10, true);
        const auto pending = session.audioDiagnostics();
        QVERIFY(pending["sinkActive"].toBool());
        QVERIFY(pending["pendingChunks"].toInt() > 0);
        session.seek(20, false); // No event-loop turn: A is still pending when B invalidates it.
        QVERIFY(session.audioDiagnostics()["audioEpoch"].toInteger() >
                pending["audioEpoch"].toInteger());
        QTest::qWait(200);
        QCOMPARE(session.currentFrame(), qint64(20));
        QVERIFY(!session.audioDiagnostics()["sinkActive"].toBool());
        QCOMPARE(session.audioDiagnostics()["bufferedBytes"].toInteger(), qint64(0));
        QCOMPARE(session.audioDiagnostics()["pendingChunks"].toInt(), 0);
        session.seek(30, true);
        const auto second = session.audioDiagnostics();
        QVERIFY(second["sinkActive"].toBool());
        QVERIFY(second["pendingChunks"].toInt() > 0);
        session.setAudioScrubEnabled(false);
        QVERIFY(session.audioDiagnostics()["audioEpoch"].toInteger() >
                second["audioEpoch"].toInteger());
        QTest::qWait(200);
        QVERIFY(!session.audioDiagnostics()["sinkActive"].toBool());
        QCOMPARE(session.audioDiagnostics()["bufferedBytes"].toInteger(), qint64(0));
        QCOMPARE(session.audioDiagnostics()["pendingChunks"].toInt(), 0);
    }
    void audioTempoDifferentChannels() {
        const auto output = stretchSignal(sineFrames(48000 * 4), .0625, 4096);
        const auto middle = static_cast<int>(output.size() / 4 - 24000);
        const auto left = toneAmplitude(output, 0, 440, middle);
        const auto right = toneAmplitude(output, 1, 660, middle);
        int strongestFrequency = 0;
        double strongestAmplitude = 0;
        for (int frequency = 650; frequency <= 670; ++frequency) {
            auto amplitude = toneAmplitude(output, 1, frequency, middle);
            if (amplitude > strongestAmplitude) {
                strongestAmplitude = amplitude;
                strongestFrequency = frequency;
            }
        }
        qInfo() << "independent stereo at1/16 amplitudes" << left << right << "right strongestHz"
                << strongestFrequency << "peak" << strongestAmplitude;
        // WSOLA links channels. Different channel tones can attenuate under
        // extreme overlap, but their pitches must not follow playback speed.
        QVERIFY(left > .05 && right > .05);
        QVERIFY(std::abs(strongestFrequency - 660) <= 2);
        QVERIFY(toneAmplitude(output, 1, 660 * .0625, middle) < .01);
    }
    void audioTempoScheduledSpanAndPulses() {
        for (auto tempo : {.0625, 16.}) {
            const int sourceFrames = static_cast<int>(48000 * std::max(4., 4 * tempo));
            const int pulseFrames = static_cast<int>(48000 * std::max(.2, .2 * tempo));
            auto input = sineFrames(sourceFrames, false, true);
            for (int i = pulseFrames; i < sourceFrames - pulseFrames; ++i)
                input[i * 2] = input[i * 2 + 1] = 0;
            for (int i = sourceFrames - pulseFrames; i < sourceFrames; ++i)
                input[i * 2] = input[i * 2 + 1] = static_cast<float>(
                    .2 * std::sin(2 * 3.14159265358979323846 * 880 * i / 48000.0));
            AudioTimeStretcher processor(tempo);
            const auto scheduledFrames = static_cast<int>(std::llround(sourceFrames / tempo));
            const auto tailFrames = static_cast<int>(48000 * std::max(1., .1 / tempo));
            QVector<float> output;
            qint64 read = 0;
            int firstOutputInput = 0, maximumBuffered = 0;
            while (output.size() / 2 < scheduledFrames + tailFrames) {
                const auto wanted = std::min(24000, scheduledFrames + tailFrames -
                                                        static_cast<int>(output.size() / 2));
                while (processor.availableFrames() < wanted) {
                    QVector<float> block(4096 * 2, 0.f);
                    const auto n = std::max<qint64>(0, std::min<qint64>(4096, sourceFrames - read));
                    if (n)
                        std::copy(input.cbegin() + read * 2, input.cbegin() + (read + n) * 2,
                                  block.begin());
                    processor.push(block);
                    read += 4096;
                    if (!firstOutputInput && processor.availableFrames())
                        firstOutputInput = static_cast<int>(read);
                    maximumBuffered = std::max(maximumBuffered, processor.availableFrames());
                }
                output += processor.take(wanted);
            }
            int firstNonzero = -1, lastNonzero = -1;
            for (int i = 0; i < output.size() / 2; ++i)
                if (std::abs(output[i * 2]) > .02) {
                    if (firstNonzero < 0)
                        firstNonzero = i;
                    lastNonzero = i;
                }
            const auto finalBlockStart =
                scheduledFrames - static_cast<int>(pulseFrames / tempo / 2);
            const auto finalTone = toneAmplitude(output, 0, 880, finalBlockStart,
                                                 static_cast<int>(pulseFrames / tempo / 2));
            qInfo() << "stream tempo" << tempo << "scheduledFrames" << scheduledFrames
                    << "firstSignal" << firstNonzero << "lastSignal" << lastNonzero
                    << "firstOutputAfterInputFrames" << firstOutputInput << "bufferedMaximumFrames"
                    << maximumBuffered << "final880Amplitude" << finalTone;
            const auto boundary = static_cast<int>(2400 / std::min(1., tempo));
            QVERIFY(firstNonzero >= 0 && firstNonzero < boundary);
            QVERIFY(std::abs(lastNonzero - scheduledFrames) < boundary);
            QVERIFY2(finalTone > .05, "The last meaningful audio block was lost");
            // First output needs less than 250 ms of source at slow speeds,
            // or less than 250 ms of output-equivalent input when speeding up.
            QVERIFY(firstOutputInput <= 48000 * .25 * std::max(1., tempo));
            QVERIFY(maximumBuffered < AudioTimeStretcher::MaximumBufferedFrames);
            for (int i = scheduledFrames + boundary; i < output.size() / 2; ++i)
                QVERIFY(std::abs(output[i * 2]) < 1e-7);
        }
    }
    void sourceAudioOffsetsAndPersistence() {
        const auto path = fixture("sync24-lossless.mkv");
        QVERIFY2(QFileInfo::exists(path), "Required synthetic audio offset fixture is missing");
        QTemporaryDir dir;
        MediaEngine engine;
        auto source = probeMedia(path);
        auto p = withSource(source);
        const auto raw = engine.sourceAudio(source, 0, 2000000);
        for (const auto &name : {"sync24-vfr-audio.mkv", "start2s-lossless.mkv"}) {
            auto other = probeMedia(fixture(name));
            other.audioOffsetUs = -250000;
            auto otherProject = withSource(other);
            QCOMPARE(engine.ownerAudio(otherProject, other.id, 0, 1500000),
                     engine.timelineAudio(otherProject, {}, 0, 1500000));
            QCOMPARE(engine.ownerAudio(otherProject, other.id, 250000, 500000),
                     engine.sourceAudio(other, 0, 500000));
        }
        p.sources[0].audioOffsetUs = -250000;
        auto delayed = engine.ownerAudio(p, source.id, 0, 2000000);
        for (int i = 0; i < 12000 * 2; ++i)
            QCOMPARE(delayed[i], 0.0f);
        for (int i = 12000 * 2; i < delayed.size(); ++i)
            QCOMPARE(delayed[i], raw[i - 12000 * 2]);
        QCOMPARE(engine.timelineAudio(p, {}, 0, 2000000), delayed);
        QCOMPARE(evaluateAssignment(p, source.id, 0)->audioUs, qint64(-250000));
        QCOMPARE(evaluateTimeline(p, {}, 0)->audioUs, qint64(-250000));
        p.sources[0].audioOffsetUs = 250000;
        auto advanced = engine.ownerAudio(p, source.id, 0, 1000000);
        QCOMPARE(advanced, engine.sourceAudio(source, 250000, 1000000));
        auto replacement = source;
        replacement.id = newId();
        replacement.audioOffsetUs = -250000;
        p.sources.append(replacement);
        auto &clip = p.timelines[0].clips[0];
        clip.audioSourceId = replacement.id;
        clip.audioOffsetUs = 500000;
        QCOMPARE(engine.timelineAudio(p, {}, 0, 1000000), advanced);
        QCOMPARE(evaluateTimeline(p, {}, 0)->audioUs, qint64(250000));
        clip.audioOffsetUs = -250000;
        auto silence = engine.timelineAudio(p, {}, 0, 500000);
        for (auto sample : silence)
            QCOMPARE(sample, 0.0f);
        p.sources[0].audioOffsetUs = MaximumAudioOffsetUs;
        for (auto sample : engine.ownerAudio(p, source.id, 0, 10000))
            QCOMPARE(sample, 0.0f);
        p.sources[0].audioOffsetUs = -MaximumAudioOffsetUs;
        for (auto sample : engine.ownerAudio(p, source.id, 0, 10000))
            QCOMPARE(sample, 0.0f);
        p.sources[0].audioOffsetUs = -250000;
        ExportSettings settings;
        settings.path = dir.filePath("offset.wav");
        settings.kind = ExportKind::Audio;
        settings.timelineId = source.id;
        settings.lastFrame = 47;
        exportProject(p, settings, {}, {});
        auto exported = probeMedia(settings.path);
        QCOMPARE(exported.info.durationUs, qint64(2000000));
        auto encoded = engine.sourceAudio(exported, 0, 2000000);
        QCOMPARE(encoded.size(), delayed.size());
        for (qsizetype i = 0; i < encoded.size(); ++i)
            QVERIFY(std::abs(encoded[i] - delayed[i]) < 4e-5);
        Project loaded;
        QString error;
        QVERIFY2(projectFromJson(projectToJson(p), loaded, &error), qPrintable(error));
        QCOMPARE(loaded.sources[0].audioOffsetUs, qint64(-250000));
        auto legacy = projectToJson(p);
        auto legacySources = legacy["sources"].toArray();
        auto legacySource = legacySources[0].toObject();
        legacySource.remove("audioOffsetUs");
        legacySources[0] = legacySource;
        legacy["sources"] = legacySources;
        QVERIFY2(projectFromJson(legacy, loaded, &error), qPrintable(error));
        QCOMPARE(loaded.sources[0].audioOffsetUs, qint64(0));
        ReviewSession session;
        session.setAudioEnabled(false);
        session.mutateProject("Fixture", [&](Project &target) { target = p; });
        session.setSourceAudioOffset(source.id, 125000);
        QCOMPARE(session.project().source(source.id)->audioOffsetUs, qint64(125000));
        session.undoStack()->undo();
        QCOMPARE(session.project().source(source.id)->audioOffsetUs, qint64(-250000));
        session.undoStack()->redo();
        QCOMPARE(session.project().source(source.id)->audioOffsetUs, qint64(125000));
        session.relinkSource(source.id, path);
        QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 30000);
        QCOMPARE(session.project().source(source.id)->audioOffsetUs, qint64(125000));
        QVERIFY(!validateSourceAudioOffset(MaximumAudioOffsetUs + 1));
        QVERIFY(session.audioTimeStretchingEnabled());
        session.setAudioTimeStretchingEnabled(false);
        QVERIFY(!session.audioTimeStretchingEnabled());
    }
    void actualTimeStretchFullSpan() {
        ReviewSession session;
        qInfo().noquote()
            << "Initial device"
            << QJsonDocument(session.audioDiagnostics()).toJson(QJsonDocument::Compact);
        if (!session.audioDiagnostics()["deviceAvailable"].toBool())
            QSKIP("No hardware output; full tempo playback span has NOT been verified");
        session.importMedia({fixture("two-audio-tracks.mkv")});
        QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 30000);
        const auto sourceId = session.project().sources.first().id;
        session.mutateProject("Long synthetic timeline", [](Project &p) {
            auto clip = p.timelines[0].clips[0];
            for (int i = 0; i < 10; ++i) {
                clip.id = newId();
                p.timelines[0].clips.append(clip);
            }
            p.viewer.outputSize = {160, 90};
            p.viewer.showOverlay = false;
        });
        session.setLoopMode(LoopMode::Once);
        for (auto rate : {.0625, -.0625, 16., -16.}) {
            const auto last = std::abs(rate) < 1 ? qint64(2) : qint64(767);
            // Exercise standalone and timeline owners; both must include the
            // complete terminal frame in either direction (2 seconds at sink).
            session.assignViewer(0, std::abs(rate) < 1 ? sourceId
                                                       : session.project().activeTimelineId);
            session.setRange(0, last);
            session.setPlaybackRate(rate);
            session.seek(rate < 0 ? last : 0, false);
            session.play();
            QElapsedTimer elapsed;
            elapsed.start();
            qint64 maximumProcessed = 0;
            qint64 firstReadyMs = -1;
            bool audioClockSeen = false;
            while (session.playing() && elapsed.elapsed() < 6000) {
                const auto state = session.audioDiagnostics();
                maximumProcessed = std::max(maximumProcessed, state["playedUs"].toInteger());
                audioClockSeen |= state["usingAudioClock"].toBool();
                if (firstReadyMs < 0 && !state["audioPriming"].toBool() &&
                    state["sinkActive"].toBool())
                    firstReadyMs = elapsed.elapsed();
                QTest::qWait(5);
            }
            qInfo() << "full span rate" << rate << "maximumPlayedUs" << maximumProcessed << "wallMs"
                    << elapsed.elapsed() << "firstReadyWallMs" << firstReadyMs << "endFrame"
                    << session.currentFrame();
            QVERIFY(audioClockSeen);
            QVERIFY(!session.playing());
            QVERIFY2(maximumProcessed >= 1950000 && maximumProcessed <= 2050000,
                     "Tempo or reverse playback shortened/extended the planned two-second span");
            QCOMPARE(session.currentFrame(), rate < 0 ? qint64(0) : last);
            QVERIFY(elapsed.elapsed() >= 1950 && elapsed.elapsed() < 6000);
            QVERIFY(firstReadyMs >= 0 && firstReadyMs < 500);
        }
    }
    void actualAudioFailureFallsBack() {
        QTemporaryDir dir;
        ReviewSession session;
        if (!session.audioDiagnostics()["deviceAvailable"].toBool())
            QSKIP("No hardware output; audio failure fallback has NOT been verified");
        const auto path = dir.filePath("synthetic-disappearing.wav");
        QVERIFY(QFile::copy(fixture("stereo44100.wav"), path));
        session.importMedia({path});
        QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 10000);
        QVERIFY(QFile::remove(path));
        QSignalSpy statuses(&session, &ReviewSession::status);
        session.play();
        QTRY_VERIFY_WITH_TIMEOUT(!session.audioDiagnostics()["usingAudioClock"].toBool(), 5000);
        QVERIFY(!session.audioDiagnostics()["sinkActive"].toBool());
        QTRY_VERIFY_WITH_TIMEOUT(session.currentFrame() > 0, 2000);
        bool audioErrorReported = false;
        for (const auto &entry : statuses)
            audioErrorReported |= entry.first().toString().startsWith("Audio:");
        QVERIFY(audioErrorReported);
        session.pause();
        QCOMPARE(session.audioDiagnostics()["bufferedBytes"].toInteger(), qint64(0));
    }
    void actualTimeStretchTransitions() {
        ReviewSession session;
        if (!session.audioDiagnostics()["deviceAvailable"].toBool())
            QSKIP("No hardware output; time-stretch transport has NOT been verified");
        session.importMedia({fixture("two-audio-tracks.mkv")});
        QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 30000);
        const auto source = session.project().sources.first();
        session.mutateProject("Long synthetic timeline", [](Project &p) {
            auto clip = p.timelines[0].clips[0];
            for (int i = 0; i < 10; ++i) {
                clip.id = newId();
                p.timelines[0].clips.append(clip);
            }
            p.viewer.outputSize = {160, 90};
            p.viewer.showOverlay = false;
        });
        session.setLoopMode(LoopMode::Once);
        for (auto rate : {.5, 2., 16., .0625, -2.}) {
            session.pause();
            session.seek(rate < 0 ? 240 : 0, false);
            const auto anchor = session.currentFrame();
            session.setPlaybackRate(rate);
            session.play();
            const auto initial = session.audioDiagnostics();
            QVERIFY(initial["audioPriming"].toBool());
            QCOMPARE(initial["processedUs"].toInteger(), qint64(0));
            QCOMPARE(session.currentFrame(), anchor);
            QTRY_VERIFY_WITH_TIMEOUT(session.audioDiagnostics()["processedUs"].toInteger() > 200000,
                                     5000);
            auto state = session.audioDiagnostics();
            qInfo().noquote() << QJsonDocument(state).toJson(QJsonDocument::Compact);
            QVERIFY(state["usingAudioClock"].toBool());
            QCOMPARE(state["error"].toInt(), 0);
            QVERIFY(state["stretchInputFrames"].toInteger() > 0);
            QVERIFY(state["stretchBufferedFrames"].toInteger() <
                    AudioTimeStretcher::MaximumBufferedFrames);
            const auto expected =
                anchor + static_cast<qint64>(state["playedUs"].toInteger() * rate * 24 / 1000000);
            const auto tolerance =
                std::max(1, static_cast<int>(std::ceil(24 * std::abs(rate) * .02)));
            QVERIFY(std::abs(state["currentFrame"].toInteger() - expected) <= tolerance);
            auto epoch = state["audioEpoch"].toInteger();
            session.seek(120, false);
            QVERIFY(session.audioDiagnostics()["audioEpoch"].toInteger() > epoch);
            QCOMPARE(session.audioDiagnostics()["stretchInputFrames"].toInteger(), qint64(0));
            QTRY_VERIFY_WITH_TIMEOUT(session.audioDiagnostics()["processedUs"].toInteger() > 100000,
                                     5000);
        }
        auto epoch = session.audioDiagnostics()["audioEpoch"].toInteger();
        session.setSourceAudioOffset(source.id, -250000);
        QVERIFY(session.audioDiagnostics()["audioEpoch"].toInteger() > epoch);
        QTRY_VERIFY_WITH_TIMEOUT(session.audioDiagnostics()["processedUs"].toInteger() > 100000,
                                 5000);
        epoch = session.audioDiagnostics()["audioEpoch"].toInteger();
        session.undoStack()->undo();
        QVERIFY(session.audioDiagnostics()["audioEpoch"].toInteger() > epoch);
        QCOMPARE(session.project().source(source.id)->audioOffsetUs, qint64(0));
        session.setAudioTimeStretchingEnabled(false);
        QVERIFY(!session.audioTimeStretchingEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(session.audioDiagnostics()["processedUs"].toInteger() > 100000,
                                 5000);
        session.setAudioTimeStretchingEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(session.audioDiagnostics()["processedUs"].toInteger() > 100000,
                                 5000);
        session.pause();
        QCOMPARE(session.audioDiagnostics()["bufferedBytes"].toInteger(), qint64(0));
    }
    void fullHdViewerPreservesOnePixelDetailAndSavedSizes() {
        QTemporaryDir dir;
        const QSize fullHd(1920, 1080);
        QImage pattern(fullHd, QImage::Format_RGB32);
        pattern.fill(Qt::black);
        for (int y = 0; y < pattern.height(); ++y)
            pattern.setPixelColor(960, y, Qt::white);
        const auto path = dir.filePath("one-pixel-line.bmp");
        QVERIFY(pattern.save(path));

        auto project = emptyProject();
        const auto source = probeMedia(path);
        project.sources.append(source);
        project.viewer.assignments = {source.id};
        project.viewer.showOverlay = false;
        MediaEngine engine;
        const auto frame = FrameComposer::compose(project, 0, engine);
        QCOMPARE(frame.size(), fullHd);
        QCOMPARE(frame.pixelColor(959, 540), QColor(Qt::black));
        QCOMPARE(frame.pixelColor(960, 540), QColor(Qt::white));
        QCOMPARE(frame.pixelColor(961, 540), QColor(Qt::black));

        project.viewer.outputSize = {960, 540};
        const auto small = FrameComposer::compose(project, 0, engine);
        QCOMPARE(small.size(), QSize(960, 540));
        QVERIFY(small.pixelColor(480, 270).red() < frame.pixelColor(960, 540).red());

        Project loaded;
        QString error;
        auto saved = projectToJson(project);
        QVERIFY2(projectFromJson(saved, loaded, &error), qPrintable(error));
        QCOMPARE(loaded.viewer.outputSize, QSize(960, 540));
        auto viewer = saved.value("viewer").toObject();
        viewer.remove("width");
        viewer.remove("height");
        saved.insert("viewer", viewer);
        QVERIFY2(projectFromJson(saved, loaded, &error), qPrintable(error));
        QCOMPARE(loaded.viewer.outputSize, fullHd);
    }
    void globalPanZoomAndLegacyMigration() {
        QTemporaryDir dir;
        auto p = emptyProject();
        for (auto color : {Qt::red, Qt::green}) {
            QImage image(100, 100, QImage::Format_RGB32);
            image.fill(color);
            auto path = dir.filePath(QString::number(p.sources.size()) + ".png");
            QVERIFY(image.save(path));
            auto source = probeMedia(path);
            p.sources.append(source);
            p.viewer.assignments.append(source.id);
        }
        p.viewer.showOverlay = false;
        p.viewer.mode = ViewerMode::Split;
        p.viewer.outputSize = {200, 100};
        p.viewer.panZoom = {2, {.25, 0}, true};
        p.viewer.transforms[0].zoom = 20; // old per-pane zoom no longer affects composition
        p.viewer.transforms[0].pan = {.8, .8};
        MediaEngine engine;
        auto image = FrameComposer::compose(p, 0, engine);
        QCOMPARE(image.pixelColor(125, 50), QColor(Qt::red));
        QCOMPARE(image.pixelColor(175, 50), QColor(Qt::green));
        const QPointF point(.2, .7);
        QCOMPARE(viewerToCanvas(p.viewer, canvasToViewer(p.viewer, point)), point);
        p.viewer.panZoom.enabled = false;
        QCOMPARE(canvasToViewer(p.viewer, point), point);
        QCOMPARE(FrameComposer::compose(p, 0, engine).pixelColor(125, 50), QColor(Qt::green));

        p.viewer.transforms[0].zoom = 2;
        p.viewer.transforms[0].pan = {.1, -.2};
        p.viewer.transforms[0].rotation = 90;
        p.viewer.transforms[1].flipHorizontal = true;
        p.viewTransforms[p.sources[1].id + ":0"] = {3, {.2, .3}, 180, false, true};
        auto old = projectToJson(p, dir.path());
        old.remove("panZoomByOwner");
        auto viewer = old["viewer"].toObject();
        viewer.remove("panZoom");
        old["viewer"] = viewer;
        Project loaded;
        QString error;
        QVERIFY2(projectFromJson(old, loaded, &error, dir.path()), qPrintable(error));
        QCOMPARE(loaded.viewer.panZoom.zoom, 2.0);
        QCOMPARE(loaded.viewer.panZoom.pan, QPointF(.1, -.2));
        QCOMPARE(loaded.viewer.transforms[0].rotation, 90);
        QVERIFY(loaded.viewer.transforms[1].flipHorizontal);
        QCOMPARE(loaded.panZoomByOwner[p.sources[1].id].zoom, 3.0);
        QCOMPARE(loaded.viewTransforms[p.sources[1].id + ":0"].rotation, 180);
        QVERIFY(loaded.viewTransforms[p.sources[1].id + ":0"].flipVertical);
        ReviewSession session;
        session.setAudioEnabled(false);
        session.mutateProject("Synthetic", [&](Project &target) { target = loaded; });
        session.assignViewer(0, p.sources[1].id);
        QCOMPARE(session.project().viewer.panZoom.zoom, 3.0);
        QCOMPARE(session.project().viewer.transforms[0].rotation, 180);
        QVERIFY(session.project().viewer.transforms[0].flipVertical);
        session.assignViewer(0, p.sources[0].id);
        QCOMPARE(session.project().viewer.panZoom.zoom, 2.0);
    }
    void configurableOverlaysAndLocalImages() {
        QTemporaryDir dir;
        QImage base(200, 100, QImage::Format_RGBA8888);
        base.fill(Qt::black);
        const auto sourcePath = dir.filePath("base.png");
        QVERIFY(base.save(sourcePath));
        auto p = withSource(probeMedia(sourcePath));
        p.viewer.showOverlay = false;
        p.viewer.outputSize = base.size();
        auto &overlay = p.viewer.overlay;
        overlay.color = Qt::red;
        overlay.lineWidth = 20;
        overlay.gridColumns = 2;
        overlay.gridRows = 2;
        MediaEngine engine;
        for (const auto type : {OverlayType::SafeArea, OverlayType::Grid, OverlayType::Center,
                                OverlayType::DynamicSymmetry, OverlayType::PhiGrid}) {
            overlay.type = type;
            auto expected = base;
            FrameComposer::paintOverlay(expected, overlay);
            const auto actual = FrameComposer::compose(p, 0, engine);
            QCOMPARE(actual, expected);
            QVERIFY(actual != base);
        }
        overlay.type = OverlayType::Grid;
        QCOMPARE(FrameComposer::compose(p, 0, engine).pixelColor(100, 20), QColor(Qt::red));
        overlay.type = OverlayType::Custom;
        OverlayShape shape;
        shape.id = newId();
        shape.type = OverlayShapeType::Rectangle;
        shape.points = {{.25, .25}, {.75, .75}};
        shape.color = Qt::red;
        shape.fill = Qt::green;
        overlay.shapes = {shape};
        QCOMPARE(FrameComposer::compose(p, 0, engine).pixelColor(100, 50), QColor(Qt::green));
        for (const auto type :
             {OverlayShapeType::Line, OverlayShapeType::Rectangle, OverlayShapeType::Ellipse,
              OverlayShapeType::Polygon, OverlayShapeType::Text}) {
            auto tested = shape;
            tested.type = type;
            tested.width = 20;
            tested.text = "Shape";
            tested.fontSize = 160;
            if (type == OverlayShapeType::Polygon)
                tested.points = {{.2, .2}, {.8, .2}, {.5, .8}};
            overlay.shapes = {tested};
            auto expected = base;
            FrameComposer::paintOverlay(expected, overlay);
            QCOMPARE(FrameComposer::compose(p, 0, engine), expected);
            QVERIFY(expected != base);
        }
        overlay.shapes = {shape};
        const auto custom = FrameComposer::compose(p, 0, engine);
        ExportSettings exportSettings;
        exportSettings.kind = ExportKind::ImageSequence;
        exportSettings.path = dir.filePath("capture");
        exportSettings.includeAudio = false;
        exportSettings.size = base.size();
        exportProject(p, exportSettings, {}, {});
        QCOMPARE(QImage(exportSettings.path + "/frame_000001.png")
                     .convertToFormat(QImage::Format_RGBA8888),
                 custom);
        QImage logo(8, 8, QImage::Format_RGBA8888);
        logo.fill(QColor(0, 255, 255, 128));
        const auto logoPath = dir.filePath("overlay.png");
        QVERIFY(logo.save(logoPath));
        overlay.type = OverlayType::Image;
        overlay.imagePath = logoPath;
        overlay.imageRect = {.25, .25, .5, .5};
        const auto image = FrameComposer::compose(p, 0, engine);
        QCOMPARE(image.pixelColor(100, 50), QColor(0, 128, 128));
        QCOMPARE(image.pixelColor(60, 50), QColor(Qt::black));
        Project loaded;
        QString error;
        QVERIFY2(projectFromJson(projectToJson(p, dir.path()), loaded, &error, dir.path()),
                 qPrintable(error));
        QCOMPARE(loaded.viewer.overlay.imagePath, logoPath);
        QCOMPARE(loaded.viewer.overlay.shapes.first().fill, QColor(Qt::green));
        QCOMPARE(FrameComposer::compose(loaded, 0, engine), image);
        QFile disguised(dir.filePath("disguised.png"));
        QVERIFY(disguised.open(QIODevice::WriteOnly));
        disguised.write("<svg xmlns='http://www.w3.org/2000/svg' width='32' height='32'>"
                        "<image href='http://127.0.0.2:9/must-not-open.png'/>"
                        "<image href='file:////remote.invalid/share/must-not-open.png'/></svg>");
        disguised.close();
        overlay.imagePath = disguised.fileName();
        QVERIFY_EXCEPTION_THROWN(FrameComposer::paintOverlay(base, overlay), std::runtime_error);
        overlay.imagePath = "\\\\remote.invalid\\share\\overlay.png";
        QVERIFY_EXCEPTION_THROWN(FrameComposer::paintOverlay(base, overlay), std::runtime_error);
    }
    void audioTrackSelectionExportAndUndo() {
        const auto path = fixture("two-audio-tracks.mkv");
        if (!QFileInfo::exists(path))
            QSKIP("Synthetic multiple-audio fixture absent");
        QTemporaryDir dir;
        auto source = probeMedia(path);
        QCOMPARE(source.info.audioTracks.size(), 2);
        QCOMPARE(source.info.audioTracks[0].sampleRate, 48000);
        QCOMPARE(source.info.audioTracks[0].channels, 1);
        QCOMPARE(source.info.audioTracks[1].sampleRate, 44100);
        QCOMPARE(source.info.audioTracks[1].channels, 2);
        QCOMPARE(source.info.audioTracks[1].language, QString("fra"));
        const auto selected = source.info.audioTracks[1].streamIndex;
        auto amplitude = [](const QVector<float> &samples, int channel, int frequency) {
            double real = 0, imaginary = 0;
            const auto count = samples.size() / 2;
            for (qsizetype n = 0; n < count; ++n) {
                const double angle = 2 * 3.14159265358979323846 * frequency * n / 48000.0;
                real += samples[n * 2 + channel] * std::cos(angle);
                imaginary += samples[n * 2 + channel] * std::sin(angle);
            }
            return 2 * std::hypot(real, imaginary) / count;
        };
        MediaEngine engine;
        const auto defaultAudio = engine.sourceAudio(source, 300000, 200000);
        QVERIFY(amplitude(defaultAudio, 0, 440) > .13);
        QVERIFY(amplitude(defaultAudio, 0, 880) < .005);
        QVERIFY(selectSourceAudioTrack(source, selected));
        const auto changed = engine.sourceAudio(source, 300000, 200000);
        QVERIFY(amplitude(changed, 0, 880) > .19);
        QVERIFY(amplitude(changed, 1, 660) > .09);
        QVERIFY(amplitude(changed, 0, 440) < .005);
        auto project = withSource(source);
        ExportSettings settings;
        settings.kind = ExportKind::Audio;
        settings.path = dir.filePath("track.wav");
        exportProject(project, settings, {}, {});
        const auto output = probeMedia(settings.path);
        const auto rendered = engine.sourceAudio(output, 300000, 200000);
        QVERIFY(amplitude(rendered, 0, 880) > .19);
        QVERIFY(amplitude(rendered, 1, 660) > .09);
        Project loaded;
        QString error;
        QVERIFY2(projectFromJson(projectToJson(project), loaded, &error), qPrintable(error));
        QCOMPARE(loaded.sources[0].selectedAudioTrack, selected);
        QCOMPARE(loaded.sources[0].info.channels, 2);
        ReviewSession session;
        session.setAudioEnabled(false);
        session.importMedia({path});
        QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 30000);
        const auto id = session.project().sources.first().id;
        session.seek(17, false);
        const auto epoch = session.audioDiagnostics()["audioEpoch"].toInteger();
        session.setSourceAudioTrack(id, selected);
        QCOMPARE(session.currentFrame(), qint64(17));
        QVERIFY(session.audioDiagnostics()["audioEpoch"].toInteger() > epoch);
        QCOMPARE(session.project().source(id)->selectedAudioTrack, selected);
        session.undoStack()->undo();
        QCOMPARE(session.project().source(id)->selectedAudioTrack, -1);
        QCOMPARE(session.currentFrame(), qint64(17));
        session.undoStack()->redo();
        QCOMPARE(session.project().source(id)->selectedAudioTrack, selected);
    }
    void actualAudioTrackSwitchRestartsQueue() {
        const auto path = fixture("two-audio-tracks.mkv");
        if (!QFileInfo::exists(path))
            QSKIP("Synthetic multiple-audio fixture absent");
        ReviewSession session;
        if (!session.audioDiagnostics()["deviceAvailable"].toBool())
            QSKIP("No hardware audio device");
        session.importMedia({path});
        QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 30000);
        const auto source = session.project().sources.first();
        session.play();
        QTRY_VERIFY_WITH_TIMEOUT(session.audioDiagnostics()["processedUs"].toInteger() > 100000,
                                 2000);
        const auto before = session.audioDiagnostics()["audioEpoch"].toInteger();
        const auto frame = session.currentFrame();
        session.setSourceAudioTrack(source.id, source.info.audioTracks[1].streamIndex);
        QVERIFY(session.audioDiagnostics()["audioEpoch"].toInteger() > before);
        QCOMPARE(session.currentFrame(), frame);
        QVERIFY(session.playing());
        QTRY_VERIFY_WITH_TIMEOUT(session.audioDiagnostics()["processedUs"].toInteger() > 100000,
                                 2000);
        QCOMPARE(session.audioDiagnostics()["error"].toInt(), 0);
        const auto secondEpoch = session.audioDiagnostics()["audioEpoch"].toInteger();
        session.undoStack()->undo();
        QVERIFY(session.audioDiagnostics()["audioEpoch"].toInteger() > secondEpoch);
        QCOMPARE(session.project().source(source.id)->selectedAudioTrack, -1);
        QTRY_VERIFY_WITH_TIMEOUT(session.audioDiagnostics()["processedUs"].toInteger() > 100000,
                                 2000);
        QCOMPARE(session.audioDiagnostics()["error"].toInt(), 0);
        session.pause();
    }
    void loopDelayCancelsAndRestoresPicture() {
        QTemporaryDir dir;
        QStringList paths;
        for (int n = 0; n < 4; ++n) {
            QImage image(32, 32, QImage::Format_RGB32);
            image.fill(Qt::red);
            auto path = dir.filePath(QString::number(n) + ".png");
            QVERIFY(image.save(path));
            paths.append(path);
        }
        ReviewSession session;
        session.setAudioEnabled(false);
        session.importImageSequence(paths, {20, 1});
        QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 10000);
        session.assignViewer(0, session.project().sources.first().id);
        QCOMPARE(session.durationFrames(), qint64(4));
        session.mutateProject("Viewer", [](Project &p) {
            p.viewer.outputSize = {64, 64};
            p.viewer.showOverlay = false;
        });
        session.setLoopDelay(250, true);
        session.play();
        QTRY_VERIFY_WITH_TIMEOUT(session.loopDelayActive(), 1000);
        QCOMPARE(session.currentFrame(), qint64(3));
        QTRY_COMPARE(session.displayImage().pixelColor(32, 32),
                     session.project().viewer.background);
        session.pause();
        QVERIFY(!session.loopDelayActive());
        QTRY_COMPARE(session.displayImage().pixelColor(32, 32), QColor(Qt::red));
        session.play();
        QTRY_VERIFY_WITH_TIMEOUT(session.loopDelayActive(), 1000);
        session.seek(0, false);
        QVERIFY(!session.loopDelayActive());
        QTRY_COMPARE(session.displayImage().pixelColor(32, 32), QColor(Qt::red));
        QTRY_VERIFY_WITH_TIMEOUT(session.loopDelayActive(), 1000);
        session.setLoopDelay(0, false);
        QVERIFY(!session.loopDelayActive());
        QTRY_COMPARE(session.displayImage().pixelColor(32, 32), QColor(Qt::red));
        session.pause();
        session.setLoopMode(LoopMode::PingPong);
        session.setLoopDelay(100, false);
        session.seek(0, false);
        session.play();
        QTRY_VERIFY_WITH_TIMEOUT(session.loopDelayActive(), 1000);
        QCOMPARE(session.playbackRate(), 1.0);
        QTRY_VERIFY_WITH_TIMEOUT(session.playbackRate() < 0, 1000);
        QVERIFY(session.playing());
        session.pause();
        session.setLoopMode(LoopMode::Loop);
        session.setPlaybackRate(-1);
        session.seek(3, false);
        session.play();
        QTRY_VERIFY_WITH_TIMEOUT(session.loopDelayActive(), 1000);
        QCOMPARE(session.currentFrame(), qint64(0));
        QTRY_VERIFY_WITH_TIMEOUT(!session.loopDelayActive(), 1000);
        QVERIFY(session.currentFrame() >= 2);
        session.pause();
    }
    void displayNumbering() {
        auto p = emptyProject();
        QCOMPARE(displayFrameNumber(p, {}, 0), qint64(1));
        QCOMPARE(frameFromDisplay(p, {}, 1), qint64(0));
        p.timelines[0].timecodeOffset = 1001;
        p.timelines[0].timecodeOffsetEnabled = true;
        QCOMPARE(displayFrameNumber(p, p.activeTimelineId, 12), qint64(1013));
        QCOMPARE(frameFromDisplay(p, p.activeTimelineId, 1013), qint64(12));
    }
    void rationalTime() {
        Rational rate{24000, 1001};
        QCOMPARE(rate.frameToUs(24000), qint64(1001000000));
        QCOMPARE(rate.usToFrame(1001000000), qint64(24000));
    }
    void vfrTimeline() {
        Project p = emptyProject();
        MediaSource s;
        s.id = newId();
        s.info.hasVideo = true;
        s.info.timeBase = {1, 1000};
        s.info.fps = {24, 1};
        s.info.variableFrameRate = true;
        s.info.framePts = {300, 340, 420, 460};
        s.info.frameDurations = {40, 80, 40, 40};
        p.sources.append(s);
        Clip c;
        c.id = newId();
        c.sourceId = s.id;
        c.inFrame = 1;
        c.outFrame = 2;
        p.timelines[0].clips.append(c);
        QCOMPARE(s.info.frameAtUs(100000), qint64(1));
        QCOMPARE(s.info.frameAtUs(120000), qint64(2));
        QCOMPARE(p.durationFrames(), qint64(3));
        QCOMPARE(evaluateTimeline(p, {}, 0)->sourceFrame, qint64(1));
        QCOMPARE(evaluateTimeline(p, {}, 2)->sourceFrame, qint64(2));
    }
    void projectRoundTrip() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        auto p = emptyProject();
        p.name = "Synthetic project";
        p.loopDelayMs = 125;
        p.loopHoldEmpty = true;
        p.rangeIn = 2;
        p.rangeOut = 30;
        p.viewer.splitVertical = false;
        p.timelines[0].timecodeOffset = 1001;
        p.timelines[0].timecodeOffsetEnabled = true;
        Note note;
        note.id = newId();
        note.timelineId = p.activeTimelineId;
        note.frame = 12;
        note.color = QColor(Qt::cyan);
        note.text = "Quoted, \"note\"\nsecond line";
        p.notes.append(note);
        Annotation a;
        a.id = newId();
        a.timelineId = p.activeTimelineId;
        a.tool = AnnotationTool::Arrow;
        a.points = {{.1, .2}, {.8, .9}};
        a.hold = true;
        a.ghostFrames = 2;
        p.annotations.append(a);
        QString error;
        QVERIFY2(saveProjectFile(p, dir.filePath("project.oframe"), &error), qPrintable(error));
        Project loaded;
        QVERIFY2(loadProjectFile(dir.filePath("project.oframe"), loaded, &error),
                 qPrintable(error));
        QCOMPARE(projectToJson(p), projectToJson(loaded));
        QVERIFY(exportNotesCsv(p, dir.filePath("notes.csv"), &error));
        auto imported = emptyProject();
        QVERIFY2(importNotesCsv(imported, dir.filePath("notes.csv"), &error), qPrintable(error));
        QCOMPARE(imported.notes[0].text, note.text);
        QCOMPARE(imported.notes[0].color, note.color);
    }
    void undo() {
        ReviewSession session;
        session.addNote("One");
        QCOMPARE(session.project().notes.size(), 1);
        session.undoStack()->undo();
        QVERIFY(session.project().notes.isEmpty());
        session.undoStack()->redo();
        QCOMPARE(session.project().notes.size(), 1);
        QVERIFY(session.dirty());
    }
    void cfrIndexAndReverse() {
        auto path = fixture("cfr24-flash-audio.mkv");
        if (!QFileInfo::exists(path))
            QSKIP("Synthetic media fixtures have not been generated");
        auto s = probeMedia(path);
        QCOMPARE(s.info.frameCount(), qint64(120));
        QVERIFY(!s.info.variableFrameRate);
        auto timeline = withSource(s);
        QCOMPARE(timeline.durationFrames(), qint64(120));
        for (qint64 frame = 0; frame < 120; ++frame)
            QCOMPARE(evaluateTimeline(timeline, {}, frame)->sourceFrame, frame);
        QVERIFY(s.info.hasAudio);
        MediaEngine engine(16);
        QVector<qint64> indices{0, 17, 47, 119, 48, 1, 80, 79, 78};
        QHash<qint64, QImage> frames;
        for (auto i : indices) {
            auto image = engine.frame(s, i);
            QVERIFY(!image.isNull());
            frames[i] = image;
        }
        engine.clear();
        std::reverse(indices.begin(), indices.end());
        for (auto i : indices)
            QCOMPARE(engine.frame(s, i), frames[i]);
    }
    void bframesSeek() {
        auto path = fixture("bframes24-audio.mp4");
        if (!QFileInfo::exists(path))
            QSKIP("Synthetic B-frame fixture absent");
        auto s = probeMedia(path);
        QCOMPARE(s.info.frameCount(), qint64(120));
        MediaEngine engine(32);
        QVector<QImage> sequential;
        for (int i = 0; i < 120; ++i)
            sequential.append(engine.frame(s, i));
        engine.clear();
        for (auto i : QList<int>{119, 1, 72, 48, 47, 46, 23, 0, 118})
            QCOMPARE(engine.frame(s, i), sequential[i]);
    }
    void vfrIndex() {
        auto path = fixture("vfr-selected.mkv");
        if (!QFileInfo::exists(path))
            QSKIP("Synthetic VFR fixture absent");
        auto s = probeMedia(path);
        QVERIFY(s.info.frameCount() > 0);
        QVERIFY(s.info.variableFrameRate);
        bool varied = false;
        for (int i = 2; i < s.info.framePts.size(); ++i)
            if (s.info.framePts[i] - s.info.framePts[i - 1] !=
                s.info.framePts[1] - s.info.framePts[0])
                varied = true;
        QVERIFY(varied);
        MediaEngine engine;
        auto last = engine.frame(s, s.info.frameCount() - 1);
        QVERIFY(!last.isNull());
        QCOMPARE(engine.frame(s, 0).size(), last.size());
    }
    void standaloneVfrExportPreservesTime() {
        const auto path = fixture("sync24-vfr-audio.mkv");
        if (!QFileInfo::exists(path))
            QSKIP("Synthetic VFR with PCM fixture absent");
        QTemporaryDir dir;
        const auto source = probeMedia(path);
        QVERIFY(source.info.variableFrameRate);
        QVERIFY(source.info.hasAudio);
        QCOMPARE(source.info.frameCount(), qint64(97));
        // Matroska rounds the final packet's duration on its millisecond time base.
        QVERIFY(std::abs(source.info.frameEndUs(96) - 5000000) <= 1000);
        auto project = withSource(source);
        project.viewer.assignments = {source.id};
        project.viewer.showOverlay = false;
        ExportSettings settings;
        settings.path = dir.filePath("standalone.mkv");
        settings.size = {160, 90};
        settings.outputFps = {24, 1};
        settings.videoCodec = "ffv1";
        settings.audioCodec = "pcm_s16le";
        exportProject(project, settings, {}, {});
        const auto direct = probeMedia(settings.path);
        QCOMPARE(direct.info.frameCount(), qint64(120));
        QVERIFY(std::abs(direct.info.durationUs - 5000000) <= 1000);
        project.viewer.assignments = {project.activeTimelineId};
        settings.path = dir.filePath("timeline.mkv");
        exportProject(project, settings, {}, {});
        const auto timeline = probeMedia(settings.path);
        QCOMPARE(timeline.info.frameCount(), direct.info.frameCount());
        QVERIFY(std::abs(timeline.info.durationUs - direct.info.durationUs) <= 1000);
        MediaEngine engine;
        QVector<int> flashes;
        for (int n = 0; n < 120; ++n) {
            const auto frame = engine.frame(direct, n);
            QCOMPARE(frame, engine.frame(timeline, n));
            if (frame.pixelColor(8, 8) == QColor(Qt::white))
                flashes.append(n);
        }
        // The fixture deliberately drops original frame 97, so white frame 96
        // remains visible for two output ticks. It must not be shortened to one.
        QCOMPARE(flashes, QVector<int>({0, 24, 48, 72, 96, 97}));
        const auto reference = engine.sourceAudio(source, 0, 5000000);
        const auto decoded = engine.sourceAudio(direct, 0, 5000000);
        const auto timelineAudio = engine.sourceAudio(timeline, 0, 5000000);
        QCOMPARE(decoded.size(), qsizetype(480000));
        QCOMPARE(decoded.size(), reference.size());
        QCOMPARE(timelineAudio, decoded);
        double maxError = 0;
        for (qsizetype n = 0; n < decoded.size(); ++n)
            maxError = std::max(maxError, std::abs(double(decoded[n] - reference[n])));
        QVERIFY2(maxError < 4e-5, qPrintable(QString::number(maxError, 'g', 12)));

        project.viewer.assignments = {source.id};
        settings.kind = ExportKind::Audio;
        settings.path = dir.filePath("standalone.wav");
        QCOMPARE(exportProject(project, settings, {}, {}), settings.path);
        const auto wave = probeMedia(settings.path);
        QCOMPARE(wave.info.durationUs, source.info.frameEndUs(96));
        QCOMPARE(engine.sourceAudio(wave, 0, 5000000), decoded);
        settings.kind = ExportKind::Video;
        settings.outputFps = {60, 1};
        settings.path = dir.filePath("sixty.mkv");
        exportProject(project, settings, {}, {});
        const auto sixty = probeMedia(settings.path);
        QCOMPARE(sixty.info.frameCount(), qint64(300));
        QVERIFY(std::abs(sixty.info.durationUs - 5000000) <= 1000);
        QCOMPARE(engine.sourceAudio(sixty, 0, 5000000), decoded);
        settings.firstFrame = 20;
        settings.lastFrame = 50;
        settings.outputFps = {24, 1};
        settings.path = dir.filePath("range.mkv");
        exportProject(project, settings, {}, {});
        const auto range = probeMedia(settings.path);
        const auto rangeStart = source.info.frameStartUs(20);
        const auto span = source.info.frameEndUs(50) - rangeStart;
        QVERIFY(std::abs(range.info.durationUs - span) <= settings.outputFps.frameToUs(1));
        const auto expectedRange = engine.sourceAudio(source, rangeStart, span);
        const auto rangeAudio = engine.sourceAudio(range, 0, span);
        QCOMPARE(rangeAudio.size(), expectedRange.size());
        maxError = 0;
        for (qsizetype n = 0; n < rangeAudio.size(); ++n)
            maxError = std::max(maxError, std::abs(double(rangeAudio[n] - expectedRange[n])));
        QVERIFY(maxError < 4e-5);
    }
    void audioDecode() {
        auto path = fixture("stereo44100.wav");
        if (!QFileInfo::exists(path))
            QSKIP("Synthetic audio fixture absent");
        auto s = probeMedia(path);
        QVERIFY(s.info.hasAudio);
        QVERIFY(!s.info.hasVideo);
        MediaEngine engine;
        auto data = engine.sourceAudio(s, 100000, 200000);
        QCOMPARE(data.size(), qsizetype(19200));
        double energy = 0;
        for (float x : data)
            energy += x * x;
        QVERIFY(energy > 1);
        auto silent = engine.sourceAudio(s, -200000, 100000);
        for (float x : silent)
            QCOMPARE(x, 0.0f);
    }
    void exportAndCancel() {
        auto path = fixture("cfr24-flash-audio.mkv");
        if (!QFileInfo::exists(path))
            QSKIP("Synthetic fixture absent");
        auto s = probeMedia(path);
        auto p = withSource(s);
        p.viewer.showOverlay = false;
        Annotation a;
        a.id = newId();
        a.timelineId = p.activeTimelineId;
        a.frame = 100;
        a.endFrame = 119;
        a.tool = AnnotationTool::Rectangle;
        a.points = {{.1, .1}, {.9, .9}};
        a.color = Qt::magenta;
        a.width = 15;
        p.annotations.append(a);
        QTemporaryDir dir;
        ExportSettings settings;
        settings.path = dir.filePath("output.mkv");
        settings.videoCodec = "ffv1";
        settings.audioCodec = "pcm_s16le";
        settings.size = {320, 180};
        settings.firstFrame = 95;
        settings.lastFrame = 119;
        int lastProgress = 0;
        exportProject(p, settings, {}, [&](int n) { lastProgress = n; });
        QCOMPARE(lastProgress, 100);
        auto output = probeMedia(settings.path);
        QCOMPARE(output.info.frameCount(), qint64(25));
        QVERIFY(output.info.hasAudio);
        MediaEngine reference, decoded;
        QCOMPARE(decoded.frame(output, 24).size(), QSize(320, 180));
        auto annotated = FrameComposer::compose(p, 119, reference, {320, 180}, true);
        auto clean = FrameComposer::compose(p, 119, reference, {320, 180}, false);
        QVERIFY(annotated != clean);
        QCOMPARE(decoded.frame(output, 24), annotated);
        settings.path = dir.filePath("cancelled.mkv");
        QVERIFY_EXCEPTION_THROWN(exportProject(p, settings, [] { return true; }, {}),
                                 std::runtime_error);
        QVERIFY(!QFileInfo::exists(settings.path));
    }
    void steppedAndBookmarkExport() {
        QTemporaryDir dir;
        QStringList images;
        for (int i = 0; i < 5; ++i) {
            QImage image(64, 64, QImage::Format_RGB32);
            image.fill(QColor(i * 40, 255 - i * 40, 60));
            auto file = dir.filePath(QString("input%1.png").arg(i));
            QVERIFY(image.save(file));
            images.append(file);
        }
        auto s = probeImageSequence(images, {24, 1});
        auto p = withSource(s);
        p.viewer.showOverlay = false;
        ExportSettings e;
        e.kind = ExportKind::ImageSequence;
        e.path = dir.filePath("stepped");
        e.size = {64, 64};
        e.frameStep = 2;
        e.includeAudio = false;
        exportProject(p, e, {}, {});
        QCOMPARE(QImage(e.path + "/frame_000002.png").pixelColor(32, 32), QColor(80, 175, 60));
        QVERIFY(QFileInfo::exists(e.path + "/frame_000003.png"));
        QVERIFY(!QFileInfo::exists(e.path + "/frame_000004.png"));
        Bookmark b;
        b.id = newId();
        b.timelineId = p.activeTimelineId;
        b.frame = 3;
        p.bookmarks.append(b);
        b.id = newId();
        b.frame = 1;
        p.bookmarks.append(b);
        e.path = dir.filePath("bookmarks");
        e.bookmarksOnly = true;
        exportProject(p, e, {}, {});
        QCOMPARE(QImage(e.path + "/frame_000001.png").pixelColor(32, 32), QColor(40, 215, 60));
        QCOMPARE(QImage(e.path + "/frame_000002.png").pixelColor(32, 32), QColor(120, 135, 60));
        QVERIFY(!QFileInfo::exists(e.path + "/frame_000003.png"));
    }
    void navigationRange() {
        ReviewSession session;
        session.setAudioEnabled(false);
        session.mutateProject("Synthetic", [](Project &p) {
            MediaSource s;
            s.id = newId();
            s.info.hasVideo = true;
            s.info.timeBase = {1, 24};
            for (int n = 0; n < 24; ++n) {
                s.info.framePts.append(n);
                s.info.frameDurations.append(1);
            }
            p.sources.append(s);
            Clip c;
            c.id = newId();
            c.sourceId = s.id;
            p.timelines[0].clips.append(c);
        });
        session.setRange(11, -1);
        QCOMPARE(session.project().rangeIn, qint64(11));
        QCOMPARE(session.project().rangeOut, qint64(23));
        session.setRange(4, 9);
        session.setLoopOnStepScrub(true);
        session.setLoopStepScrubToRange(true);
        session.seek(9, false);
        session.step(1);
        QCOMPARE(session.currentFrame(), qint64(4));
        session.step(-1);
        QCOMPARE(session.currentFrame(), qint64(9));
        session.setLockStepScrubToRange(true);
        session.step(10);
        QCOMPARE(session.currentFrame(), qint64(9));
        session.seek(20, false);
        QCOMPARE(session.currentFrame(), qint64(20));
    }
    void syncAndFractionalRate() {
        auto path = fixture("sync24-lossless.mkv");
        if (!QFileInfo::exists(path))
            QSKIP("Sync fixture absent");
        auto s = probeMedia(path);
        auto p = withSource(s);
        MediaEngine engine;
        auto audio = engine.timelineAudio(p, {}, 0, 1200000);
        QVERIFY(audio.size() > 48000 * 2);
        double impulse = 0, silence = 0;
        for (int n = 48000; n < 48000 + 1200; ++n)
            impulse += audio[n * 2] * audio[n * 2];
        for (int n = 24000; n < 25200; ++n)
            silence += audio[n * 2] * audio[n * 2];
        QVERIFY(impulse > 1);
        QVERIFY(silence < .01);
        auto rationalPath = fixture("rational23976-stereo44100.mkv");
        if (QFileInfo::exists(rationalPath)) {
            auto r = probeMedia(rationalPath);
            QVERIFY(std::abs(r.info.fps.value() - 24000.0 / 1001) < .001);
            QCOMPARE(r.info.sampleRate, 44100);
            auto sound = engine.sourceAudio(r, 100000, 200000);
            QCOMPARE(sound.size(), qsizetype(19200));
        }
    }
    void actualAudioTransport() {
        auto path = fixture("sync24-lossless.mkv");
        if (!QFileInfo::exists(path))
            QSKIP("Synthetic sync fixture absent");
        ReviewSession session;
        if (!session.audioDiagnostics()["deviceAvailable"].toBool())
            QSKIP("No hardware audio output: QAudioSink playback has NOT been verified");
        session.importMedia({path});
        QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 30000);
        QVERIFY(session.durationFrames() > 70);
        auto exercise = [&](double rate, qint64 frame) {
            session.pause();
            session.setPlaybackRate(rate);
            session.seek(frame, false);
            QVector<AudioPresentation> presentations;
            QElapsedTimer presentationClock;
            presentationClock.start();
            auto connection =
                connect(&session, &ReviewSession::imageReady, &session, [&](const QImage &) {
                    const auto wallUs = presentationClock.nsecsElapsed() / 1000;
                    const auto state = session.audioDiagnostics();
                    presentations.append({state["playedUs"].toInteger(),
                                          state["renderedFrame"].toInteger(), wallUs});
                });
            session.play();
            // Use the real GUI event dispatcher throughout the measurement.
            // Qt 6.8 qWait inserts sleeps up to 10 ms between processEvents calls;
            // those test-only stalls distort an 8 ms transport timer and presentation cadence.
            QEventLoop observation;
            QTimer::singleShot(900, &observation, &QEventLoop::quit);
            observation.exec();
            disconnect(connection);
            const auto observationEndWallUs = presentationClock.nsecsElapsed() / 1000;
            const auto state = session.audioDiagnostics();
            const auto window =
                assessAudioPresentations(presentations, session.project().fps, rate, frame, 100000,
                                         state["playedUs"].toInteger(), observationEndWallUs);
            // All observations above were made while playing. Pause is cleanup only.
            session.pause();
            qInfo().noquote() << QJsonDocument(state).toJson(QJsonDocument::Compact);
            qInfo().noquote() << "Presentation window"
                              << QJsonDocument(window).toJson(QJsonDocument::Compact);
            QVERIFY2(state["sinkActive"].toBool(),
                     "Audio device does not support required format; playback is NOT verified");
            QVERIFY(state["usingAudioClock"].toBool());
            QVERIFY(state["processedUs"].toInteger() > 100000);
            QVERIFY(state["error"].toInt() == 0 || state["error"].toInt() == 3);
            QVERIFY2(window["error"].toString().isEmpty(), qPrintable(window["error"].toString()));
        };
        exercise(1.0, 0);
        exercise(1.0, 24);
        exercise(-1.0, 72);
        exercise(2.0, 12);
        exercise(.5, 36);
    }
    void presentationWindowRejectsSparseOrFrozenRenderer() {
        // A few perfectly synchronized images must not pass the hardware oracle.
        // These counterexamples isolate cadence/coverage from the one-frame bound.
        const Rational fps{24, 1};
        QVector<AudioPresentation> steady;
        for (qint64 f = 0; f < 22; ++f)
            steady.append({(f * 1000000 * fps.den + fps.num - 1) / fps.num, f,
                           (f * 1000000 * fps.den + fps.num - 1) / fps.num});
        auto result = assessAudioPresentations(steady, fps, 1, 0, 100000, 900000, 900000);
        QVERIFY2(result["error"].toString().isEmpty(), qPrintable(result["error"].toString()));
        QVector<AudioPresentation> sparse{
            {0, 0, 0}, {250000, 6, 250000}, {500000, 12, 500000}, {750000, 18, 750000}};
        result = assessAudioPresentations(sparse, fps, 1, 0, 100000, 900000, 900000);
        QCOMPARE(result["maximumPresentedLag"].toInteger(), qint64(0));
        QVERIFY(result["maximumIntervalUs"].toInteger() > result["intervalLimitUs"].toInteger());
        QVERIFY(!result["error"].toString().isEmpty());
        auto frozen = steady;
        frozen.resize(8);
        result = assessAudioPresentations(frozen, fps, 1, 0, 100000, 900000, 900000);
        QCOMPARE(result["maximumPresentedLag"].toInteger(), qint64(0));
        QVERIFY(result["maximumFrameAgeUs"].toInteger() > result["ageLimitUs"].toInteger());
        QVERIFY(!result["error"].toString().isEmpty());
        // One omitted update gives a real 41.667 ms gap at 2x. Quantizing each
        // audio-clock endpoint to 10 ms makes this same gap appear to be 50 ms.
        QVector<AudioPresentation> quantized;
        qint64 maximumAudioGap = 0;
        for (qint64 frame = 0; frame < 44; ++frame) {
            if (frame == 17)
                continue;
            const auto wall = (frame * 1000000 + 47) / 48;
            const auto audio = ((wall + 5000) / 10000) * 10000;
            if (!quantized.isEmpty())
                maximumAudioGap = std::max(maximumAudioGap, audio - quantized.last().playedUs);
            quantized.append({audio, frame, wall});
        }
        QCOMPARE(maximumAudioGap, qint64(50000));
        result = assessAudioPresentations(quantized, fps, 2, 0, 100000, 900000, 900000);
        QVERIFY2(result["error"].toString().isEmpty(), qPrintable(result["error"].toString()));
        QCOMPARE(result["maximumIntervalUs"].toInteger(), qint64(41666));
        auto stalled = quantized;
        for (auto &sample : stalled)
            if (sample.frame >= 18)
                sample.wallUs += 8334;
        result = assessAudioPresentations(stalled, fps, 2, 0, 100000, 900000, 908334);
        QCOMPARE(result["maximumIntervalUs"].toInteger(), qint64(50000));
        QVERIFY(!result["error"].toString().isEmpty());
        auto late = steady;
        for (auto &sample : late)
            sample.frame -= 2;
        result = assessAudioPresentations(late, fps, 1, 0, 100000, 900000, 900000);
        QCOMPARE(result["maximumPresentedLag"].toInteger(), qint64(2));
        QVERIFY(!result["error"].toString().isEmpty());
    }
    void standaloneVfrAudioClock() {
        const auto path = fixture("sync24-vfr-audio.mkv");
        if (!QFileInfo::exists(path))
            QSKIP("Synthetic VFR with PCM fixture absent");
        ReviewSession session;
        if (!session.audioDiagnostics()["deviceAvailable"].toBool())
            QSKIP("No hardware output: standalone VFR audio clock is NOT verified");
        session.importMedia({path});
        QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 30000);
        const auto source = session.project().sources.first();
        session.assignViewer(0, source.id);
        session.setLoopMode(LoopMode::Once);
        session.setRange(0, -1);
        session.seek(0, false);
        session.play();
        // 97 ordinal frames / 24 would have stopped at 4.042 seconds. PTS spans 5 seconds.
        QTest::qWait(4300);
        QVERIFY(session.playing());
        const auto state = session.audioDiagnostics();
        qInfo().noquote() << QJsonDocument(state).toJson(QJsonDocument::Compact);
        QVERIFY(state["usingAudioClock"].toBool());
        QVERIFY(state["processedUs"].toInteger() > 4000000);
        QCOMPARE(state["error"].toInt(), 0);
        const auto expected = source.info.frameAtUs(state["playedUs"].toInteger());
        QVERIFY(std::abs(session.currentFrame() - expected) <= 1);
        QVERIFY(std::abs(state["currentFrame"].toInteger() - state["renderedFrame"].toInteger()) <=
                1);
        QTRY_VERIFY_WITH_TIMEOUT(!session.playing(), 2000);
        QCOMPARE(session.currentFrame(), source.info.frameCount() - 1);
    }
    void sequenceNamingAndBackground() {
        QTemporaryDir dir;
        QImage input(64, 64, QImage::Format_RGB32);
        input.fill(Qt::red);
        auto imagePath = dir.filePath("synthetic.png");
        QVERIFY(input.save(imagePath));
        auto s = probeMedia(imagePath);
        auto p = withSource(s);
        QCOMPARE(p.durationFrames(), qint64(1));
        p.viewer.showOverlay = false;
        ExportSettings e;
        e.kind = ExportKind::ImageSequence;
        e.path = dir.filePath("named");
        e.size = {64, 64};
        e.includeAudio = false;
        e.sequenceBaseName = "review";
        e.sequencePadding = 4;
        e.frameNumbering = "custom";
        e.sequenceStart = 1001;
        e.overrideAnnotationBackground = true;
        e.annotationBackground = QColor(0, 255, 0, 255);
        const auto output = e.path + "/review_1001.png";
        QCOMPARE(exportProject(p, e, {}, {}), output);
        QVERIFY(QFileInfo::exists(output));
        QCOMPARE(QImage(output).pixelColor(32, 32), QColor(Qt::green));
        e.path = dir.path();
        e.publishSequenceIntoExistingDirectory = true;
        e.frameNumbering = "zero_based";
        e.overrideAnnotationBackground = false;
        const auto existing = dir.filePath("review_0000.png");
        QCOMPARE(exportProject(p, e, {}, {}), existing);
        QVERIFY(QFileInfo::exists(existing));
        QCOMPARE(QImage(existing).pixelColor(32, 32), QColor(Qt::red));
        QVERIFY_EXCEPTION_THROWN(exportProject(p, e, {}, {}), std::runtime_error);
        QCOMPARE(QImage(existing).pixelColor(32, 32), QColor(Qt::red));
    }
    void timelineSeamsAndAudioReplacement() {
        auto videoPath = fixture("cfr24-flash-audio.mkv"), audioPath = fixture("stereo44100.wav");
        if (!QFileInfo::exists(videoPath) || !QFileInfo::exists(audioPath))
            QSKIP("Synthetic seam fixtures absent");
        auto video = probeMedia(videoPath), audio = probeMedia(audioPath);
        auto p = withSource(video);
        p.sources.append(audio);
        auto &first = p.timelines[0].clips[0];
        first.inFrame = 12;
        first.outFrame = 35;
        first.muted = true;
        Clip second;
        second.id = newId();
        second.sourceId = video.id;
        second.inFrame = 48;
        second.outFrame = 71;
        second.audioSourceId = audio.id;
        second.audioOffsetUs = 100000;
        second.gain = .5;
        p.timelines[0].clips.append(second);
        QCOMPARE(p.durationFrames(), qint64(48));
        QCOMPARE(evaluateTimeline(p, {}, 23)->sourceFrame, qint64(35));
        QCOMPARE(evaluateTimeline(p, {}, 24)->sourceFrame, qint64(48));
        QCOMPARE(evaluateTimeline(p, {}, 47)->sourceFrame, qint64(71));
        QTemporaryDir dir;
        MediaEngine engine;
        auto muted = engine.timelineAudio(p, {}, 0, 1000000);
        for (float x : muted)
            QCOMPARE(x, 0.0f);
        auto replaced = engine.timelineAudio(p, {}, 1000000, 200000);
        auto expected = engine.sourceAudio(audio, 100000, 200000);
        QCOMPARE(replaced.size(), expected.size());
        for (qsizetype i = 0; i < expected.size(); ++i)
            QVERIFY(std::abs(replaced[i] - expected[i] * .5f) < 1e-6);
        ExportSettings settings;
        settings.path = dir.filePath("multi-clip.mkv");
        settings.videoCodec = "ffv1";
        settings.audioCodec = "pcm_s16le";
        settings.size = {160, 90};
        settings.viewerComposition = false;
        exportProject(p, settings, {}, {});
        auto exported = probeMedia(settings.path);
        QCOMPARE(exported.info.frameCount(), qint64(48));
        auto exportedMute = engine.sourceAudio(exported, 0, 1000000);
        for (float x : exportedMute)
            QCOMPARE(x, 0.0f);
        auto exportedReplacement = engine.sourceAudio(exported, 1000000, 200000);
        QCOMPARE(exportedReplacement.size(), expected.size());
        for (qsizetype i = 0; i < expected.size(); ++i)
            QVERIFY(std::abs(exportedReplacement[i] - expected[i] * .5f) < 4e-5);
        QCOMPARE(engine.frame(exported, 24),
                 FrameComposer::timeline(p, p.activeTimelineId, 24, engine, {160, 90}));
        std::swap(p.timelines[0].clips[0], p.timelines[0].clips[1]);
        QCOMPARE(evaluateTimeline(p, {}, 0)->sourceFrame, qint64(48));
    }
    void cancellationPreservesExisting() {
        auto path = fixture("cfr24-flash-audio.mkv");
        if (!QFileInfo::exists(path))
            QSKIP("Synthetic cancel fixture absent");
        auto p = withSource(probeMedia(path));
        QTemporaryDir dir;
        ExportSettings e;
        e.path = dir.filePath("existing.mkv");
        e.videoCodec = "ffv1";
        e.audioCodec = "pcm_s16le";
        e.size = {160, 90};
        e.lastFrame = 23;
        QFile existing(e.path);
        QVERIFY(existing.open(QIODevice::WriteOnly));
        existing.write("original synthetic marker");
        existing.close();
        bool cancelled = false;
        QVERIFY_EXCEPTION_THROWN(exportProject(
                                     p, e, [&]() { return cancelled; },
                                     [&](int percent) {
                                         if (percent >= 25)
                                             cancelled = true;
                                     }),
                                 std::runtime_error);
        QVERIFY(existing.open(QIODevice::ReadOnly));
        QCOMPARE(existing.readAll(), QByteArray("original synthetic marker"));
        existing.close();
        e.kind = ExportKind::ImageSequence;
        e.path = dir.filePath("cancelled-sequence");
        e.includeAudio = false;
        cancelled = false;
        QVERIFY_EXCEPTION_THROWN(exportProject(
                                     p, e, [&]() { return cancelled; },
                                     [&](int percent) {
                                         if (percent >= 25)
                                             cancelled = true;
                                     }),
                                 std::runtime_error);
        QVERIFY(!QFileInfo::exists(e.path));
        QVERIFY(QDir(dir.path()).entryList({"*.partial-*"}, QDir::Dirs).isEmpty());
    }
    void nonzeroStart() {
        auto path = fixture("start2s-lossless.mkv");
        if (!QFileInfo::exists(path))
            QSKIP("Nonzero-start fixture absent");
        auto source = probeMedia(path);
        QCOMPARE(source.info.frameCount(), qint64(48));
        QCOMPARE(source.info.videoStartUs, qint64(2000000));
        QCOMPARE(source.info.audioStartUs, qint64(2000000));
        QCOMPARE(source.info.durationUs, qint64(2000000));
        auto p = withSource(source);
        QCOMPARE(p.durationFrames(), qint64(48));
        QCOMPARE(evaluateTimeline(p, {}, 0)->sourceFrame, qint64(0));
        QCOMPARE(evaluateTimeline(p, {}, 47)->sourceFrame, qint64(47));
        MediaEngine engine;
        QVERIFY(!engine.frame(source, 0).isNull());
        QVERIFY(!engine.frame(source, 47).isNull());
        auto pcm = engine.timelineAudio(p, {}, 0, 1100000);
        double first = 0, next = 0, silence = 0;
        for (int n = 0; n < 1200; ++n) {
            first += pcm[n * 2] * pcm[n * 2];
            next += pcm[(48000 + n) * 2] * pcm[(48000 + n) * 2];
            silence += pcm[(24000 + n) * 2] * pcm[(24000 + n) * 2];
        }
        QVERIFY(first > 1);
        QVERIFY(next > 1);
        QVERIFY(silence < .01);
    }
    void relinkSequencePreservesMetadata() {
        QTemporaryDir dir;
        QVERIFY(QDir().mkpath(dir.filePath("old")));
        QVERIFY(QDir().mkpath(dir.filePath("new")));
        QStringList paths;
        for (int i = 0; i < 3; ++i) {
            QImage image(32, 32, QImage::Format_RGB32);
            image.fill(QColor(i * 80, 40, 120));
            auto path = dir.filePath(QString("old/take_%1.png").arg(i + 1, 4, 10, QChar('0')));
            QVERIFY(image.save(path));
            paths.append(path);
            QVERIFY(QFile::copy(
                path, dir.filePath(QString("new/renamed_%1.png").arg(101 + i, 4, 10, QChar('0')))));
        }
        ReviewSession session;
        session.setAudioEnabled(false);
        session.importImageSequence(paths, {25, 1});
        QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 10000);
        QCOMPARE(session.project().sources.size(), 1);
        auto id = session.project().sources[0].id;
        session.mutateProject("Metadata", [](Project &p) {
            auto &source = p.sources[0];
            source.name = "Keep name";
            source.inFrame = 1;
            source.outFrame = 2;
            source.timecodeOffset = 1001;
            source.timecodeOffsetEnabled = true;
        });
        session.relinkSource(id, dir.filePath("new/renamed_0101.png"));
        QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 10000);
        auto source = session.project().source(id);
        QVERIFY(source);
        QCOMPARE(source->kind, SourceKind::Sequence);
        QCOMPARE(source->info.frameCount(), qint64(3));
        QCOMPARE(source->info.fps, Rational(25, 1));
        QCOMPARE(source->name, QString("Keep name"));
        QCOMPARE(source->inFrame, qint64(1));
        QCOMPARE(source->outFrame, qint64(2));
        QCOMPARE(source->timecodeOffset, qint64(1001));
        QVERIFY(source->timecodeOffsetEnabled);
        QCOMPARE(QFileInfo(source->sequencePaths[2]).fileName(), QString("renamed_0103.png"));
        session.undoStack()->undo();
        QCOMPARE(session.project().source(id)->sequencePaths, paths);
    }
    void independentAnnotationHistory() {
        ReviewSession session;
        session.setAudioEnabled(false);
        auto owner = session.activeOwnerId();
        session.addNote("Project note");
        Annotation zero;
        zero.timelineId = owner;
        zero.frame = 0;
        zero.endFrame = 0;
        zero.points = {{.1, .1}, {.2, .2}};
        session.addAnnotation(zero);
        auto five = zero;
        five.frame = 5;
        five.endFrame = 5;
        session.addAnnotation(five);
        auto bg = zero;
        bg.background = true;
        bg.frame = 99;
        session.addAnnotation(bg);
        QCOMPARE(session.project().annotations.size(), 3);
        QCOMPARE(session.project().bookmarks.size(), 2);
        session.annotationUndoStack(owner, 5)->undo();
        QCOMPARE(session.project().annotations.size(), 2);
        QCOMPARE(session.project().notes.size(), 1);
        session.undoStack()->undo();
        QVERIFY(session.project().notes.isEmpty());
        QCOMPARE(session.project().annotations.size(), 2);
        session.annotationUndoStack(owner, 0, true)->undo();
        QCOMPARE(session.project().annotations.size(), 1);
        QVERIFY(!session.project().annotations[0].background);
        session.annotationUndoStack(owner, 5)->redo();
        QCOMPARE(session.project().annotations.size(), 2);
        QTemporaryDir dir;
        QVERIFY(session.saveProject(dir.filePath("history.oframe")));
        QVERIFY(!session.dirty());
        session.annotationUndoStack(owner, 5)->undo();
        QVERIFY(session.dirty());
        session.annotationUndoStack(owner, 5)->redo();
        QVERIFY(!session.dirty());
        session.mutateAnnotations("Clear frame", owner, 5, false,
                                  [](Project &p) { p.annotations.clear(); });
        QCOMPARE(session.project().annotations.size(), 1);
        QCOMPARE(session.project().annotations[0].frame, qint64(0));
        session.annotationUndoStack(owner, 5)->undo();
        QCOMPARE(session.project().annotations.size(), 2);
    }
    void directionalGhostingAndBackground() {
        auto p = emptyProject();
        p.viewer.ghostEnabled = true;
        p.viewer.ghostPrevious = 1;
        p.viewer.ghostNext = 1;
        p.viewer.ghostByAnnotations = true;
        Annotation previous;
        previous.id = newId();
        previous.timelineId = p.activeTimelineId;
        previous.frame = 10;
        previous.endFrame = 10;
        previous.color = Qt::white;
        previous.width = 20;
        previous.points = {{.2, .5}, {.4, .5}};
        auto next = previous;
        next.id = newId();
        next.frame = 30;
        next.endFrame = 30;
        next.points = {{.6, .5}, {.8, .5}};
        p.annotations = {previous, next};
        auto render = [&]() {
            QImage image(200, 100, QImage::Format_ARGB32);
            image.fill(Qt::black);
            FrameComposer::paintAnnotations(image, p, p.activeTimelineId, 20);
            return image;
        };
        auto image = render();
        auto left = image.pixelColor(60, 50), right = image.pixelColor(140, 50);
        QVERIFY(left.red() > left.blue());
        QVERIFY(right.blue() > right.red());
        p.viewer.ghostByAnnotations = false;
        image = render();
        QCOMPARE(image.pixelColor(60, 50), QColor(Qt::black));
        QCOMPARE(image.pixelColor(140, 50), QColor(Qt::black));
        p.viewer.ghostByAnnotations = true;
        p.viewer.ghostNext = 0;
        image = render();
        QCOMPARE(image.pixelColor(140, 50), QColor(Qt::black));
        p.annotations[0].hold = true;
        image = render();
        left = image.pixelColor(60, 50);
        QVERIFY(left.red() > 0);
        QCOMPARE(left.red(), left.blue());
        p.annotations[0].hold = false;
        p.viewer.holdAnnotations = true;
        image = render();
        QVERIFY(image.pixelColor(60, 50).red() > 0);
        QCOMPARE(image.pixelColor(60, 50).red(), image.pixelColor(60, 50).blue());
        Annotation background = previous;
        background.id = newId();
        background.background = true;
        background.frame = 999;
        background.endFrame = 999;
        background.points = {{.1, .1}, {.9, .1}};
        p.annotations.append(background);
        image = render();
        QVERIFY(image.pixelColor(100, 10).red() > 0);
    }
    void comparisonPaneAnnotations() {
        QTemporaryDir dir;
        auto p = emptyProject();
        p.viewer.showOverlay = false;
        p.viewer.background = Qt::black;
        p.viewer.outputSize = {400, 240};
        const QVector<QColor> colors{Qt::red, Qt::green, Qt::blue, Qt::yellow};
        for (int n = 0; n < 4; ++n) {
            QImage input(160, 90, QImage::Format_RGB32);
            input.fill(Qt::black);
            const auto path = dir.filePath(QString("pane%1.png").arg(n));
            QVERIFY(input.save(path));
            auto source = probeMedia(path);
            p.sources.append(source);
            Clip clip;
            clip.id = newId();
            clip.sourceId = source.id;
            QString owner = source.id;
            if (n == 0) {
                p.timelines[0].clips.append(clip);
                owner = p.timelines[0].id;
            } else if (n == 2) {
                Timeline timeline;
                timeline.id = newId();
                timeline.clips.append(clip);
                owner = timeline.id;
                p.timelines.append(timeline);
            }
            p.viewer.assignments.append(owner);
            Annotation annotation;
            annotation.id = newId();
            annotation.timelineId = owner;
            annotation.color = colors[n];
            annotation.width = 40;
            annotation.points = {{.2, .25}, {.8, .25}};
            p.annotations.append(annotation);
        }
        MediaEngine engine;
        auto render = [&]() { return FrameComposer::compose(p, 0, engine); };
        p.viewer.mode = ViewerMode::SingleB;
        p.viewer.activePane = 0;
        QCOMPARE(render().pixelColor(200, 60), colors[1]);
        QCOMPARE(displayedAnnotationOwner(p), p.viewer.assignments[1]);
        QCOMPARE(viewerPaneAt(p.viewer, {.25, .75}), 1);
        QCOMPARE(viewerPaneRect(p.viewer, 1), QRectF(0, 0, 1, 1));
        QVERIFY(viewerPaneRect(p.viewer, 0).isEmpty());
        Project reopened;
        QString loadError;
        QVERIFY2(projectFromJson(projectToJson(p), reopened, &loadError), qPrintable(loadError));
        QCOMPARE(reopened.viewer.mode, ViewerMode::SingleB);
        QCOMPARE(FrameComposer::compose(reopened, 0, engine), render());
        ExportSettings pinnedExport;
        pinnedExport.kind = ExportKind::ImageSequence;
        pinnedExport.path = dir.filePath("pinned-b");
        pinnedExport.imageFormat = "bmp";
        pinnedExport.size = p.viewer.outputSize;
        pinnedExport.viewerComposition = false;
        pinnedExport.includeAudio = false;
        pinnedExport.lastFrame = 0;
        const auto pinnedImage = exportProject(p, pinnedExport, {}, {});
        QCOMPARE(QImage(pinnedImage).pixelColor(200, 60), colors[1]);
        p.viewer.mode = ViewerMode::AB;
        p.viewer.activePane = 0;
        QCOMPARE(render().pixelColor(200, 60), colors[0]);
        QCOMPARE(displayedAnnotationOwner(p), p.viewer.assignments[0]);
        p.viewer.activePane = 1;
        QCOMPARE(render().pixelColor(200, 60), colors[1]);
        QCOMPARE(displayedAnnotationOwner(p), p.viewer.assignments[1]);
        QCOMPARE(displayedAnnotationFrame(p, 10), qint64(0));
        QCOMPARE(FrameComposer::compose(p, 10, engine).pixelColor(200, 60), colors[1]);
        QCOMPARE(viewerPaneRect(p.viewer, 1), QRectF(0, 0, 1, 1));
        QVERIFY(viewerPaneRect(p.viewer, 0).isEmpty());

        p.viewer.mode = ViewerMode::Split;
        auto image = render();
        QCOMPARE(image.pixelColor(100, 60), colors[0]);
        QCOMPARE(image.pixelColor(300, 60), colors[1]);
        QCOMPARE(viewerPaneRect(p.viewer, 1), QRectF(.5, 0, .5, 1));
        QCOMPARE(viewerPaneAt(p.viewer, {.75, .2}), 1);
        p.viewer.splitVertical = false;
        image = render();
        QCOMPARE(image.pixelColor(200, 30), colors[0]);
        QCOMPARE(image.pixelColor(200, 150), colors[1]);
        QCOMPARE(viewerPaneRect(p.viewer, 1), QRectF(0, .5, 1, .5));
        QCOMPARE(viewerPaneAt(p.viewer, {.2, .75}), 1);

        p.viewer.mode = ViewerMode::Grid;
        image = render();
        for (int n = 0; n < 4; ++n) {
            QCOMPARE(image.pixelColor(100 + (n % 2) * 200, 30 + (n / 2) * 120), colors[n]);
            QCOMPARE(viewerPaneAt(p.viewer, {.25 + (n % 2) * .5, .25 + (n / 2) * .5}), n);
            p.viewer.activePane = n;
            QCOMPARE(displayedAnnotationOwner(p), p.viewer.assignments[n]);
        }
        p.viewer.transforms[3].zoom = 2;
        p.viewer.transforms[3].pan = {.2, -.1};
        p.viewer.transforms[3].rotation = 90;
        QCOMPARE(render().pixelColor(300, 150), colors[3]);
        // The exact common compositor must also be used by viewer-capture export.
        ExportSettings settings;
        settings.kind = ExportKind::ImageSequence;
        settings.path = dir.filePath("grid-export");
        settings.size = p.viewer.outputSize;
        settings.includeAudio = false;
        exportProject(p, settings, {}, {});
        QCOMPARE(
            QImage(settings.path + "/frame_000001.png").convertToFormat(QImage::Format_RGBA8888),
            render());
        const auto without = FrameComposer::compose(p, 0, engine, {}, false);
        for (int n = 0; n < 4; ++n)
            QCOMPARE(without.pixelColor(100 + (n % 2) * 200, 30 + (n / 2) * 120),
                     QColor(Qt::black));

        p.viewer.mode = ViewerMode::Wipe;
        p.viewer.wipe = .5;
        image = render();
        QCOMPARE(image.pixelColor(100, 60), colors[0]);
        QCOMPARE(image.pixelColor(300, 60), colors[1]);
        QCOMPARE(viewerPaneAt(p.viewer, {.25, .25}), 0);
        QCOMPARE(viewerPaneAt(p.viewer, {.75, .25}), 1);
        p.viewer.mode = ViewerMode::Blend;
        p.viewer.opacity = .5;
        auto blend = render().pixelColor(200, 60);
        QVERIFY(std::abs(blend.red() - 127) <= 2);
        QVERIFY(std::abs(blend.green() - 128) <= 2);
        QCOMPARE(blend.blue(), 0);
        p.viewer.showAnnotations = false;
        QCOMPARE(render().pixelColor(200, 60), QColor(Qt::black));
    }
    void ownerViewsAndLoadPreferences() {
        QTemporaryDir dir;
        QStringList paths;
        for (int i = 0; i < 2; ++i) {
            QImage image(32, 32, QImage::Format_RGB32);
            image.fill(i ? Qt::blue : Qt::red);
            auto path = dir.filePath(QString("view%1.png").arg(i));
            QVERIFY(image.save(path));
            paths.append(path);
        }
        ReviewSession session;
        session.setAudioEnabled(false);
        session.importMedia(paths);
        QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 10000);
        auto a = session.project().sources[0].id, b = session.project().sources[1].id;
        session.assignViewer(0, a);
        auto view = session.project().viewer;
        view.transforms[0].zoom = 2;
        view.transforms[0].pan = {.1, .2};
        session.setViewerSettings(view);
        session.assignViewer(0, b);
        QCOMPARE(session.project().viewer.transforms[0].zoom, 1.0);
        view = session.project().viewer;
        view.transforms[0].zoom = 3;
        session.setViewerSettings(view);
        session.assignViewer(0, a);
        QCOMPARE(session.project().viewer.transforms[0].zoom, 2.0);
        QCOMPARE(session.project().viewer.transforms[0].pan, QPointF(.1, .2));
        Annotation annotation;
        annotation.timelineId = a;
        annotation.hold = true;
        annotation.points = {{.1, .2}, {.3, .4}};
        session.addAnnotation(annotation);
        auto heldView = session.project().viewer;
        heldView.holdAnnotations = true;
        session.setViewerSettings(heldView);
        auto projectPath = dir.filePath("views.oframe");
        QVERIFY(session.saveProject(projectPath));
        ReviewSession restored;
        QVERIFY(restored.openProject(projectPath));
        QCOMPARE(restored.project().viewer.transforms[0].zoom, 2.0);
        QVERIFY(restored.project().annotations[0].hold);
        QVERIFY(restored.project().viewer.holdAnnotations);
        restored.assignViewer(0, b);
        QCOMPARE(restored.project().viewer.transforms[0].zoom, 3.0);
        restored.setProjectLoadOptions(false, false);
        QVERIFY(restored.openProject(projectPath));
        QCOMPARE(restored.project().viewer.transforms[0].zoom, 1.0);
        QCOMPARE(restored.project().viewer.transforms[0].pan, QPointF());
        QVERIFY(!restored.project().annotations[0].hold);
        QVERIFY(!restored.project().viewer.holdAnnotations);
        QVERIFY(!restored.dirty());
    }
    void projectDefaultsAndDisabledUndo() {
        QTemporaryDir dir;
        QImage image(32, 32, QImage::Format_RGB32);
        image.fill(Qt::yellow);
        QVERIFY(image.save(dir.filePath("relative.png")));
        ReviewSession session;
        session.setAudioEnabled(false);
        session.setProjectDefaults({30, 1}, 1001, false, false, dir.path());
        session.newProject();
        QVERIFY(session.project().timelines.isEmpty());
        QCOMPARE(session.project().sourceRootDirectory, dir.path());
        session.importImageSequence({"relative.png"});
        QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 10000);
        QCOMPARE(session.project().sources.size(), 1);
        QCOMPARE(session.project().sources[0].info.fps, Rational(30, 1));
        QCOMPARE(frameDisplayOffset(session.project(), session.project().sources[0].id),
                 qint64(1001));
        session.assignViewer(0, session.project().sources[0].id);
        QVERIFY(session.saveProject(dir.filePath("defaults.oframe")));
        ReviewSession reopened;
        QVERIFY(reopened.openProject(dir.filePath("defaults.oframe")));
        QCOMPARE(reopened.project().sources[0].path, dir.filePath("relative.png"));
        session.setProjectUndoEnabled(false);
        session.addNote("Untracked project edit");
        QVERIFY(session.dirty());
        QCOMPARE(session.undoStack()->count(), 0);
        Annotation a;
        a.timelineId = session.activeOwnerId();
        a.points = {{0., 0.}, {1., 1.}};
        session.addAnnotation(a);
        session.annotationUndoStack(a.timelineId, 0)->undo();
        QVERIFY(session.project().annotations.isEmpty());
        QCOMPARE(session.project().notes.size(), 1);
        QVERIFY(session.saveProject());
        QVERIFY(!session.dirty());
        session.setProjectUndoEnabled(true);
        session.addNote("Undoable note");
        session.undoStack()->undo();
        QCOMPARE(session.project().notes.size(), 1);
        QVERIFY(!session.dirty());
    }
    void bookmarkIntervalsAndMutedNavigation() {
        auto path = fixture("cfr24-flash-audio.mkv");
        if (!QFileInfo::exists(path))
            QSKIP("Synthetic bookmark fixture absent");
        ReviewSession session;
        session.setAudioEnabled(false);
        session.importMedia({path});
        QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 10000);
        session.addBookmark("Start", 0);
        session.addBookmark("One second", 24);
        session.addBookmark("Later", 60);
        session.seek(5, false);
        session.playBookmarks();
        QCOMPARE(session.currentFrame(), qint64(0));
        QTest::qWait(200);
        QCOMPARE(session.currentFrame(), qint64(0));
        QTest::qWait(950);
        QCOMPARE(session.currentFrame(), qint64(24));
        session.pause();
        session.seek(58, false);
        QCOMPARE(session.currentFrame(), qint64(24));
        session.play();
        QVERIFY(!session.bookmarkPlayback());
        QTest::qWait(150);
        QVERIFY(session.currentFrame() > 24 && session.currentFrame() < 60);
        session.pause();
        session.setRange(80, 119);
        QSignalSpy errors(&session, &ReviewSession::error);
        session.playBookmarks();
        QVERIFY(!session.playing());
        QVERIFY(!session.bookmarkPlayback());
        QVERIFY(!errors.isEmpty());
        session.setRange(4, 9);
        session.setLoopOnStepScrub(true);
        session.setLoopStepScrubToRange(true);
        session.seek(9, false);
        session.navigate(10, false);
        QCOMPARE(session.currentFrame(), qint64(4));
        QVERIFY(!session.audioDiagnostics()["sinkActive"].toBool());
    }
    void mpeg4Duration() {
        auto path = fixture("cfr24-flash-audio.mkv");
        if (!QFileInfo::exists(path))
            QSKIP("Synthetic MPEG4 fixture absent");
        auto p = withSource(probeMedia(path));
        QTemporaryDir dir;
        ExportSettings e;
        e.path = dir.filePath("timing.mp4");
        e.videoCodec = "mpeg4";
        e.size = {160, 90};
        e.includeAudio = false;
        exportProject(p, e, {}, {});
        auto source = probeMedia(e.path);
        QCOMPARE(source.info.frameCount(), qint64(120));
        QVERIFY(std::abs(source.info.fps.value() - 24) < 1e-8);
        QCOMPARE(source.info.durationUs, qint64(5000000));
    }
    void forceOverwriteSequenceRollback() {
        QTemporaryDir dir;
        const auto output = dir.filePath("output");
        QVERIFY(QDir().mkpath(output));
        QStringList beforePaths, afterPaths;
        for (int i = 0; i < 2; ++i) {
            QImage before(32, 32, QImage::Format_RGB32);
            before.fill(i ? Qt::green : Qt::red);
            const auto beforePath = dir.filePath(QString("before%1.png").arg(i));
            QVERIFY(before.save(beforePath));
            beforePaths.append(beforePath);
            QImage after(32, 32, QImage::Format_RGB32);
            after.fill(i ? Qt::yellow : Qt::blue);
            const auto afterPath = dir.filePath(QString("after%1.png").arg(i));
            QVERIFY(after.save(afterPath));
            afterPaths.append(afterPath);
        }
        ExportSettings settings;
        settings.kind = ExportKind::ImageSequence;
        settings.path = output;
        settings.publishSequenceIntoExistingDirectory = true;
        settings.size = {32, 32};
        settings.includeAudio = false;
        auto before = withSource(probeImageSequence(beforePaths, {24, 1}));
        before.viewer.showOverlay = false;
        exportProject(before, settings, {}, {});
        QFile unrelated(QDir(output).filePath("keep.txt"));
        QVERIFY(unrelated.open(QIODevice::WriteOnly));
        unrelated.write("unrelated user content");
        unrelated.close();
        auto after = withSource(probeImageSequence(afterPaths, {24, 1}));
        after.viewer.showOverlay = false;
        const auto firstOutput = QDir(output).filePath("frame_000001.png");
        const auto secondOutput = QDir(output).filePath("frame_000002.png");
        QVERIFY_EXCEPTION_THROWN(exportProject(after, settings, {}, {}), std::runtime_error);
        settings.forceOverwrite = true;
        bool originalsWereBackedUp = false;
        QVERIFY_EXCEPTION_THROWN(exportProject(after, settings,
                                               [&]() {
                                                   if (!QFileInfo::exists(firstOutput))
                                                       originalsWereBackedUp = true;
                                                   return originalsWereBackedUp &&
                                                          QFileInfo::exists(firstOutput);
                                               },
                                               {}),
                                 std::runtime_error);
        QVERIFY(originalsWereBackedUp);
        QCOMPARE(QImage(firstOutput).pixelColor(16, 16), QColor(Qt::red));
        QCOMPARE(QImage(secondOutput).pixelColor(16, 16), QColor(Qt::green));
        exportProject(after, settings, {}, {});
        QCOMPARE(QImage(firstOutput).pixelColor(16, 16), QColor(Qt::blue));
        QCOMPARE(QImage(secondOutput).pixelColor(16, 16), QColor(Qt::yellow));
        QVERIFY(unrelated.open(QIODevice::ReadOnly));
        QCOMPARE(unrelated.readAll(), QByteArray("unrelated user content"));
        unrelated.close();
        QVERIFY(QDir(output)
                    .entryList({".openframe-backup-*", ".openframe-partial-*"},
                               QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
                    .isEmpty());
    }
    void autoplayOnViewerAssignment() {
        auto path = fixture("cfr24-flash-audio.mkv");
        if (!QFileInfo::exists(path))
            QSKIP("Synthetic autoplay fixture absent");
        ReviewSession session;
        session.setAudioEnabled(false);
        session.importMedia({path});
        QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 10000);
        session.setAutoplay(true);
        session.assignViewer(0, session.project().sources[0].id);
        QVERIFY(session.playing());
        session.pause();
        session.setActiveTimeline(session.project().timelines[0].id);
        QVERIFY(session.playing());
        session.setAutoplay(false);
        session.assignViewer(0, session.project().sources[0].id);
        QVERIFY(!session.playing());
    }
    void imageSequence() {
        QTemporaryDir dir;
        QStringList images;
        for (int i = 0; i < 3; ++i) {
            QImage image(64, 64, QImage::Format_RGB32);
            image.fill(i == 0 ? Qt::red : i == 1 ? Qt::green : Qt::blue);
            auto path = dir.filePath(QString("source%1.png").arg(i));
            QVERIFY(image.save(path));
            images.append(path);
        }
        auto source = probeImageSequence(images, {24, 1});
        auto p = withSource(source);
        p.viewer.showOverlay = false;
        ExportSettings e;
        e.kind = ExportKind::ImageSequence;
        e.path = dir.filePath("sequence");
        e.size = {64, 64};
        e.includeAudio = false;
        exportProject(p, e, {}, {});
        QVERIFY(QFileInfo::exists(e.path + "/frame_000003.png"));
        QImage image(e.path + "/frame_000003.png");
        QCOMPARE(image.pixelColor(32, 32), QColor(Qt::blue));
    }
};
QTEST_MAIN(CoreTests)
#include "CoreTests.moc"
