#include <QtTest>
#include <QEventLoop>
#include <QTimer>
#include <QSignalSpy>
#include <QJsonArray>
#include <QTemporaryDir>
#include <future>
#include "core/ReviewSessionTestAccess.h"
#include "core/ReviewSession.h"
#include "media/FrameComposer.h"
using namespace openframe;
namespace {
double percentile(QVector<double> values, double quantile) {
    if (values.isEmpty())
        return -1;
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1, qsizetype(std::ceil(quantile * values.size()) - 1))];
}
} // namespace
class ScrubbingTests : public QObject {
    Q_OBJECT
  private slots:
    void hardSeekRejectsUnfinishedDrag() {
        QTemporaryDir directory;
        QStringList paths;
        for (auto color : {Qt::red, Qt::green, Qt::blue}) {
            QImage image(32, 32, QImage::Format_RGB32);
            image.fill(color);
            auto path = directory.filePath(QString::number(paths.size()) + ".bmp");
            QVERIFY(image.save(path));
            paths.append(path);
        }
        ReviewSession session;
        session.setAudioEnabled(false);
        session.importImageSequence(paths, {24, 1});
        QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 5000);
        session.mutateProject("Test canvas", [](Project &p) {
            p.viewer.outputSize = {32, 32};
            p.viewer.showOverlay = false;
        });
        for (bool step : {false, true}) {
            session.seek(0, false);
            QTRY_COMPARE_WITH_TIMEOUT(session.audioDiagnostics()["renderedFrame"].toInteger(),
                                      qint64(0), 5000);
            auto entered = std::make_shared<std::promise<void>>();
            auto enteredFuture = entered->get_future();
            auto release = std::make_shared<std::promise<void>>();
            auto wait = release->get_future().share();
            struct Unblock {
                std::shared_ptr<std::promise<void>> promise;
                ~Unblock() {
                    if (promise)
                        promise->set_value();
                }
            } unblock{release};
            ReviewSessionTestAccess::beforeNextPresentation(session, [entered, wait] {
                entered->set_value();
                wait.wait();
            });
            session.navigate(1, false);
            QVERIFY(enteredFuture.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
            QVector<qint64> delivered;
            auto connection =
                connect(&session, &ReviewSession::imageReady, &session, [&](const QImage &) {
                    delivered.append(session.audioDiagnostics()["renderedFrame"].toInteger());
                });
            if (step)
                session.step(1);
            else
                session.seek(2, true);
            release->set_value();
            unblock.promise.reset();
            QTRY_COMPARE_WITH_TIMEOUT(session.audioDiagnostics()["renderedFrame"].toInteger(),
                                      qint64(2), 5000);
            disconnect(connection);
            QVERIFY(!delivered.contains(1));
            QVERIFY(delivered.contains(2));
            QCOMPARE(session.displayImage().pixelColor(16, 16), QColor(Qt::blue));
        }
        QSignalSpy moved(&session, &ReviewSession::frameChanged);
        session.navigate(1, false);
        for (int n = 0; n < 100; ++n)
            session.navigate(1, false);
        QCOMPARE(moved.size(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(session.audioDiagnostics()["renderedFrame"].toInteger(),
                                  qint64(1), 5000);
    }
    void cancelledGopDecodeResumesExactly() {
        const auto path = QStringLiteral(OPENFRAME_FIXTURES_DIR) + "/sync24-bframes.mp4";
        QVERIFY(QFileInfo::exists(path));
        auto source = probeMedia(path);
        MediaEngine engine(16), oracle(16);
        int checks = 0;
        engine.setOperationCancelled([&] { return ++checks > 12; });
        QVERIFY_EXCEPTION_THROWN(engine.frame(source, source.info.frameCount() - 2),
                                 std::runtime_error);
        engine.setOperationCancelled({});
        for (auto frame :
             {source.info.frameCount() - 3, qint64(4), qint64(7), qint64(12), qint64(9)})
            QVERIFY(engine.frame(source, frame) == oracle.frame(source, frame));
        engine.setOperationCancelled([] { return true; });
        QVERIFY_EXCEPTION_THROWN(engine.frame(source, 9), std::runtime_error);
        engine.setOperationCancelled({});
        QVERIFY(engine.frame(source, 9) == oracle.frame(source, 9));
    }
    void latency_data() {
        QTest::addColumn<QString>("file");
        QTest::addColumn<bool>("projectFile");
        QTest::addColumn<bool>("scrub");
        QStringList fixtures{"sync24-lossless.mkv", "sync24-vfr-audio.mkv", "sync24-bframes.mp4"};
        if (qEnvironmentVariableIntValue("OPENFRAME_SCRUB_1080") == 1)
            fixtures.append({"scrub1080-gop48.mp4", "scrub1080-gop240.mp4"});
        for (const auto &name : fixtures) {
            for (bool scrub : {false, true})
                QTest::newRow(qPrintable(QString(name) + (scrub ? "-audio" : "-silent")))
                    << QStringLiteral(OPENFRAME_FIXTURES_DIR) + "/" + name << false << scrub;
        }
        const auto user = qEnvironmentVariable("OPENFRAME_SCRUB_PROJECT");
        if (!user.isEmpty())
            for (bool scrub : {false, true})
                QTest::newRow(scrub ? "user-project-audio" : "user-project-silent")
                    << user << true << scrub;
    }
    void latency() {
        QFETCH(QString, file);
        QFETCH(bool, projectFile);
        QFETCH(bool, scrub);
        QVERIFY2(QFileInfo::exists(file), "Requested scrubbing fixture is absent");
        ReviewSession session;
        session.setVolume(0);
        QSignalSpy errors(&session, &ReviewSession::error);
        if (projectFile) {
            QVERIFY(session.openProject(file));
            QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 30000);
        } else {
            auto source = probeMedia(file);
            auto p = emptyProject();
            p.sources.append(source);
            p.viewer.assignments = {source.id};
            p.viewer.showOverlay = false;
            session.mutateProject("Benchmark synthetic source",
                                  [&](Project &target) { target = p; });
        }
        QTRY_VERIFY_WITH_TIMEOUT(!session.displayImage().isNull(), 30000);
        QVERIFY(session.durationFrames() > 8);
        const auto count = session.durationFrames();
        const auto initial = session.project();
        MediaEngine oracle;
        QVector<double> latencies, calls, finalSettle, duplicateFirst, dragLatencies;
        int wrongFrames = 0, dragPresented = 0, maxPending = 0;
        auto awaitSeek = [&](qint64 target) {
            QEventLoop loop;
            QTimer timeout;
            timeout.setSingleShot(true);
            QElapsedTimer clock;
            clock.start();
            bool delivered = false;
            auto connection =
                connect(&session, &ReviewSession::imageReady, &loop, [&](const QImage &) {
                    const auto readyMs = clock.nsecsElapsed() / 1e6;
                    const auto rendered = session.audioDiagnostics()["renderedFrame"].toInteger();
                    if (rendered != target) {
                        ++wrongFrames;
                        return;
                    }
                    delivered = true;
                    latencies.append(readyMs);
                    loop.quit();
                });
            connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
            timeout.start(10000);
            session.navigate(target, scrub);
            calls.append(clock.nsecsElapsed() / 1e6);
            if (!delivered)
                loop.exec();
            disconnect(connection);
            if (!delivered)
                return false;
            const auto expected = FrameComposer::compose(initial, target, oracle);
            return session.displayImage() == expected;
        };
        QVector<qint64> targets;
        for (int fraction : {83, 20, 68, 40, 95, 8})
            targets.append((count - 1) * fraction / 100);
        for (int pass = 0; pass < 2; ++pass)
            for (auto target : targets)
                QVERIFY2(awaitSeek(target), "Exact requested seek frame was not delivered");
        for (int burst = 0; burst < 3; ++burst) {
            QEventLoop loop;
            QTimer moves, deadline;
            moves.setTimerType(Qt::PreciseTimer);
            moves.setInterval(4);
            deadline.setSingleShot(true);
            QElapsedTimer clock;
            clock.start();
            int request = 0;
            bool sentAll = false, settled = false;
            qint64 target = -1;
            double requestedAt = 0, lastReadyAt = -1;
            QMap<qint64, double> requested;
            QVector<QPair<qint64, QImage>> presented;
            auto connection =
                connect(&session, &ReviewSession::imageReady, &loop, [&](const QImage &image) {
                    const auto now = clock.nsecsElapsed() / 1e6;
                    const auto rendered = session.audioDiagnostics()["renderedFrame"].toInteger();
                    if (!requested.contains(rendered) || (sentAll && rendered != target)) {
                        ++wrongFrames;
                        return;
                    }
                    presented.append({rendered, image});
                    ++dragPresented;
                    lastReadyAt = now;
                    dragLatencies.append(now - requested.value(rendered));
                    if (sentAll) {
                        settled = true;
                        finalSettle.append(now - requestedAt);
                        loop.quit();
                    }
                });
            connect(&moves, &QTimer::timeout, &loop, [&] {
                target = ((request * 37 + burst * 17) % (count - 1));
                requestedAt = clock.nsecsElapsed() / 1e6;
                requested[target] = requestedAt;
                session.navigate(target, scrub);
                calls.append(clock.nsecsElapsed() / 1e6 - requestedAt);
                maxPending =
                    std::max(maxPending, session.audioDiagnostics()["pendingWorkerJobs"].toInt());
                if (++request == 45) {
                    sentAll = true;
                    moves.stop();
                    session.seek(target, false);
                }
            });
            connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
            deadline.start(10000);
            moves.start();
            loop.exec();
            disconnect(connection);
            QVERIFY2(settled, "Final drag target did not settle");
            QVERIFY(session.displayImage() == FrameComposer::compose(initial, target, oracle));
            for (const auto &value : presented)
                QVERIFY(value.second == FrameComposer::compose(initial, value.first, oracle));
        }
        // Repeated mouse positions within the same integer frame must not starve it.
        {
            QEventLoop loop;
            QTimer moves, deadline;
            moves.setTimerType(Qt::PreciseTimer);
            moves.setInterval(4);
            deadline.setSingleShot(true);
            const auto target = (session.currentFrame() + count / 2) % count;
            QElapsedTimer clock;
            clock.start();
            int requests = 0;
            bool shown = false, all = false;
            auto connection =
                connect(&session, &ReviewSession::imageReady, &loop, [&](const QImage &) {
                    if (session.audioDiagnostics()["renderedFrame"].toInteger() != target) {
                        ++wrongFrames;
                        return;
                    }
                    if (!shown)
                        duplicateFirst.append(clock.nsecsElapsed() / 1e6);
                    shown = true;
                    if (all)
                        loop.quit();
                });
            connect(&moves, &QTimer::timeout, &loop, [&] {
                session.navigate(target, scrub);
                if (++requests == 45) {
                    all = true;
                    moves.stop();
                    if (shown)
                        loop.quit();
                }
            });
            connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
            deadline.start(10000);
            moves.start();
            loop.exec();
            disconnect(connection);
            QVERIFY(shown);
            QVERIFY(session.displayImage() == FrameComposer::compose(initial, target, oracle));
        }
        QCOMPARE(wrongFrames, 0);
        QVERIFY2(errors.isEmpty(),
                 errors.isEmpty() ? "" : qPrintable(errors.first().first().toString()));
        QJsonObject summary{{"case", QString::fromLatin1(QTest::currentDataTag())},
                            {"sourceFrames", count},
                            {"audioScrub", scrub},
                            {"seekP50Ms", percentile(latencies, .5)},
                            {"seekP95Ms", percentile(latencies, .95)},
                            {"navigateCallP95Ms", percentile(calls, .95)},
                            {"dragPresentations", dragPresented},
                            {"dragP95Ms", percentile(dragLatencies, .95)},
                            {"finalSettleP50Ms", percentile(finalSettle, .5)},
                            {"finalSettleP95Ms", percentile(finalSettle, .95)},
                            {"duplicateFirstMs", percentile(duplicateFirst, 1)},
                            {"maximumPendingJobs", maxPending},
                            {"wrongFrames", wrongFrames}};
        qInfo().noquote() << "SCRUB_BENCHMARK"
                          << QJsonDocument(summary).toJson(QJsonDocument::Compact);
    }
};
QTEST_MAIN(ScrubbingTests)
#include "ScrubbingTests.moc"
