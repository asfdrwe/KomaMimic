// SPDX-License-Identifier: GPL-3.0-only
#include "displayjobs.h"
#include "shortcuts.h"
#include "test_support.h"
#include "thumbnailcache.h"
#include "thumbnailsource.h"
#include "videoview.h"
#include <QAudioBuffer>
#include <QAudioBufferOutput>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSettings>
#include <QScopeGuard>
#include <QSignalBlocker>
#include <QSignalSpy>
#include <QSlider>
#include <QStackedWidget>
#include <QTimer>
#include <QVideoSink>
#include <cmath>
#include <cstring>
#include <latch>

class VideoTests : public QObject {
  Q_OBJECT
  QString fixture(const QString &name) const {
    return QString::fromUtf8(KOMAMIMIC_VIDEO_FIXTURES) + "/" + name;
  }
  static QString playbackDiagnostic(const VideoView &view) {
    const auto *player = view.player();
    return QString("source=%1; mediaStatus=%2; playbackState=%3; error=%4 (%5); viewError=%6")
        .arg(player->source().toString())
        .arg(int(player->mediaStatus()))
        .arg(int(player->playbackState()))
        .arg(int(player->error()))
        .arg(player->errorString(), view.error());
  }
  static bool red(const QImage &image) {
    if (image.isNull())
      return false;
    const auto c = image.pixelColor(image.width() / 2, image.height() / 2);
    return c.red() > 180 && c.blue() < 70;
  }
  static bool blue(const QImage &image) {
    if (image.isNull())
      return false;
    const auto c = image.pixelColor(image.width() / 2, image.height() / 2);
    return c.blue() > 180 && c.red() < 70;
  }
  static bool green(const QImage &image) {
    if (image.isNull())
      return false;
    const auto c = image.pixelColor(image.width() / 2, image.height() / 2);
    return c.green() > 70 && c.red() < 70 && c.blue() < 70;
  }
private slots:
  void initTestCase() {
    qApp->setProperty("komamimicTestSilent", true);
    QVERIFY(!qApp->property("komamimicTestRoot").toString().isEmpty());
    QVERIFY(QFileInfo::exists(fixture("VIDEO-FIXTURES.json")));
  }
  void deinterlacePixelsAndFieldOrder() {
    QImage input(4, 6, QImage::Format_ARGB32);
    const int values[]{20, 80, 100, 160, 180, 240};
    for (int y = 0; y < 6; ++y)
      for (int x = 0; x < 4; ++x)
        input.setPixelColor(x, y, QColor(values[y], values[y], values[y]));
    QCOMPARE(deinterlaceImage(input, DeinterlaceMode::Off), input);
    const int top[]{20, 20, 100, 100, 180, 180},
        bottom[]{80, 80, 160, 160, 240, 240};
    const int linearTop[]{20, 60, 100, 140, 180, 180},
        linearBottom[]{80, 80, 120, 160, 200, 240};
    for (int field : {0, 1}) {
      const auto bob = deinterlaceImage(input, DeinterlaceMode::Bob, field),
                 linear =
                     deinterlaceImage(input, DeinterlaceMode::Linear, field);
      for (int y = 0; y < 6; ++y) {
        QCOMPARE(bob.pixelColor(0, y).red(), field ? bottom[y] : top[y]);
        QCOMPARE(linear.pixelColor(0, y).red(),
                 field ? linearBottom[y] : linearTop[y]);
      }
    }
    auto alternating = input;
    for (int y = 0; y < 6; ++y)
      for (int x = 0; x < 4; ++x)
        alternating.setPixelColor(x, y, y % 2 ? Qt::white : Qt::black);
    const auto blended = deinterlaceImage(alternating, DeinterlaceMode::Blend);
    for (int y = 0; y < 6; ++y)
      QCOMPARE(blended.pixelColor(0, y).red(), 127);
    QCOMPARE(videoRectangle({320, 240}, {800, 600}, 3, true, 1).size(),
             QSizeF(800, 450));
    QCOMPARE(videoRectangle({320, 240}, {800, 600}, 1, true, 1).size(),
             QSizeF(600, 600));
  }
  void pausedSeekAndCompletion() {
    QTemporaryDir dir;
    QSettings settings(dir.filePath("settings.ini"), QSettings::IniFormat);
    settings.setValue("video/autoplay", false);
    VideoView view(settings);
    view.resize(400, 300);
    view.show();
    view.open(fixture("colors.avi"));
    QTRY_VERIFY2_WITH_TIMEOUT(view.ready(), qPrintable(playbackDiagnostic(view)), 10000);
    QCOMPARE(view.player()->playbackState(), QMediaPlayer::PausedState);
    QVERIFY(red(view.coverImage()));
    view.seek(3200);
    QTRY_VERIFY_WITH_TIMEOUT(green(view.coverImage()), 5000);
    QCOMPARE(view.player()->playbackState(), QMediaPlayer::PausedState);
    QVERIFY(qAbs(view.m_sourceFrame.startTime() / 1000 - 3200) <= 500);
    view.setRate(4);
    view.command("videoPlayPause");
    QTRY_COMPARE_WITH_TIMEOUT(view.player()->mediaStatus(),
                              QMediaPlayer::EndOfMedia, 7000);
    QVERIFY(view.state().completed);
    QVERIFY(blue(view.coverImage()));
    view.open(fixture("colors.avi"));
    QTRY_VERIFY2(view.ready(), qPrintable(playbackDiagnostic(view)));
    QVERIFY(red(view.coverImage()));
  }
  void tracksSubtitlesAndSeekMaintainPause() {
    QTemporaryDir dir;
    QSettings settings(dir.filePath("s.ini"), QSettings::IniFormat);
    settings.setValue("video/autoplay", false);
    VideoView view(settings);
    view.resize(400, 300);
    view.show();
    view.open(fixture("multi.mkv"));
    QTRY_VERIFY2_WITH_TIMEOUT(view.ready(), qPrintable(playbackDiagnostic(view)), 10000);
    QCOMPARE(view.player()->videoTracks().size(), 2);
    QCOMPARE(view.player()->audioTracks().size(), 2);
    QCOMPARE(view.player()->subtitleTracks().size(), 2);
    QCOMPARE(view.player()->activeSubtitleTrack(), -1);
    QVERIFY(red(view.coverImage()));
    view.command("videoVideoTrack");
    QTRY_VERIFY_WITH_TIMEOUT(blue(view.coverImage()), 5000);
    QCOMPARE(view.player()->activeVideoTrack(), 1);
    view.command("videoVideoTrack");
    QTRY_VERIFY(red(view.coverImage()));
    QCOMPARE(view.player()->activeVideoTrack(), 0);
    view.command("videoAudioTrack");
    QCOMPARE(view.player()->activeAudioTrack(), 1);
    view.command("videoAudioTrack");
    QCOMPARE(view.player()->activeAudioTrack(), 0);
    view.command("videoSubtitle");
    view.seek(800);
    view.command("videoPlayPause");
    QTRY_VERIFY_WITH_TIMEOUT(
        view.m_sink->subtitleText().contains(QString::fromUtf8("日本語")),
        4000);
    const auto clean = view.coverImage();
    QVERIFY(red(clean)); // subtitle is a separate overlay
    view.command("videoSubtitle");
    view.seek(1000);
    QTRY_VERIFY(view.m_sink->subtitleText().contains("English"));
    view.command("videoSubtitle");
    QTRY_VERIFY(view.m_sink->subtitleText().isEmpty());
    QCOMPARE(view.player()->activeSubtitleTrack(), -1);
  }
  void settingsAndCycles() {
    QTemporaryDir dir;
    QSettings settings(dir.filePath("s.ini"), QSettings::IniFormat);
    settings.setValue("video/autoplay", false);
    settings.setValue("video/rates", "0.5,1,2");
    VideoView view(settings);
    view.open(fixture("colors.avi"));
    QTRY_VERIFY2(view.ready(), qPrintable(playbackDiagnostic(view)));
    view.command("videoRateUp");
    QCOMPARE(view.state().rate, 2.);
    view.command("videoRateUp");
    QCOMPARE(view.state().rate, 2.);
    view.command("videoRateDown");
    QCOMPARE(view.state().rate, 1.);
    view.command("videoRateDown");
    view.command("videoRateDown");
    QCOMPARE(view.state().rate, .5);
    view.setBottomFirst(true);
    for (int i = 1; i <= 4; ++i) {
      view.command("videoDeinterlace");
      QCOMPARE(int(view.state().deinterlace), i % 4);
      QVERIFY(view.state().bottomFirst);
    }
    view.command("videoFieldOrder");
    QVERIFY(!view.state().bottomFirst);
    for (int i = 0; i < 30; ++i)
      view.command("videoVolumeUp");
    QCOMPARE(view.findChild<QSlider *>("videoVolume")->value(), 100);
    for (int i = 0; i < 30; ++i)
      view.command("videoVolumeDown");
    QCOMPARE(view.findChild<QSlider *>("videoVolume")->value(), 0);
    view.setAspect(2);
    view.seek(3000);
    view.save();
    view.leave();
    settings.sync();
    const auto saved = readVideoState(settings, fixture("colors.avi"));
    QCOMPARE(saved.aspect, 2);
    QVERIFY(saved.position >= 2500);
  }
  void baselineMp4DecodesAudioAndVideo() {
    QTemporaryDir dir;
    QSettings settings(dir.filePath("s.ini"), QSettings::IniFormat);
    VideoView view(settings);
    view.open(fixture("sample.mp4"));
    QTRY_VERIFY2_WITH_TIMEOUT(view.ready(), qPrintable(playbackDiagnostic(view)), 10000);
    QVERIFY(view.player()->hasAudio());
    QVERIFY(view.player()->hasVideo());
    QVERIFY(view.player()->pitchCompensationAvailability() !=
            QMediaPlayer::PitchCompensationAvailability::Unavailable);
    QVERIFY(view.player()->pitchCompensation());
    QVERIFY(view.playing());
    QSignalSpy audio(view.player()->audioBufferOutput(),
                     &QAudioBufferOutput::audioBufferReceived);
    QTRY_VERIFY(audio.count() > 2);
    for (double rate : {.25, .5, 1., 2., 4.}) {
      view.setRate(rate);
      QCOMPARE(view.player()->playbackRate(), rate);
      QVERIFY(view.player()->pitchCompensation());
    }
  }
  void audioTrackSelectionChangesDecodedFrequency() {
    QTemporaryDir dir;
    QSettings settings(dir.filePath("s.ini"), QSettings::IniFormat);
    settings.setValue("video/autoplay", false);
    VideoView view(settings);
    view.open(fixture("multi.mkv"));
    QTRY_VERIFY2(view.ready(), qPrintable(playbackDiagnostic(view)));
    QVector<double> samples;
    int sampleRate = 0;
    connect(view.player()->audioBufferOutput(),
            &QAudioBufferOutput::audioBufferReceived, &view,
            [&](const QAudioBuffer &buffer) {
              const auto format = buffer.format();
              sampleRate = format.sampleRate();
              const int channels = format.channelCount();
              if (!channels)
                return;
              for (int i = 0; i < buffer.sampleCount(); i += channels) {
                if (format.sampleFormat() == QAudioFormat::Int16)
                  samples.append(buffer.constData<qint16>()[i] / 32768.);
                else if (format.sampleFormat() == QAudioFormat::Float)
                  samples.append(buffer.constData<float>()[i]);
              }
            });
    for (int track = 0; track < 2; ++track) {
      if (track)
        view.command("videoAudioTrack");
      view.seek(500);
      samples.clear();
      view.player()->play();
      QTRY_VERIFY_WITH_TIMEOUT(samples.size() > 18000, 4000);
      view.player()->pause();
      const int start = samples.size() - 8192;
      int cycles = 0;
      for (int i = start + 1; i < samples.size(); ++i)
        if (samples[i - 1] <= 0 && samples[i] > 0)
          ++cycles;
      const double frequency =
          cycles * double(sampleRate) / (samples.size() - start);
      QVERIFY2(std::abs(frequency - (track ? 880 : 440)) < 25,
               qPrintable(QString::number(frequency)));
    }
  }
  void tsTracksRemainIndependent() {
    QTemporaryDir dir;
    QSettings settings(dir.filePath("s.ini"), QSettings::IniFormat);
    settings.setValue("video/autoplay", false);
    VideoView view(settings);
    view.resize(400, 300);
    view.open(fixture("programs.ts"));
    QTRY_VERIFY2_WITH_TIMEOUT(view.ready(), qPrintable(playbackDiagnostic(view)), 10000);
    QCOMPARE(view.player()->videoTracks().size(), 2);
    QCOMPARE(view.player()->audioTracks().size(), 2);
    QCOMPARE(view.player()->activeAudioTrack(), 0);
    view.command("videoVideoTrack");
    QTRY_VERIFY(blue(view.coverImage()));
    QCOMPARE(view.player()->activeAudioTrack(), 0);
    view.command("videoAudioTrack");
    QCOMPARE(view.player()->activeAudioTrack(), 1);
  }
  void bobPresentsTwoFieldsInTimeOrder_data() {
    QTest::addColumn<bool>("bottom");
    QTest::addColumn<int>("method");
    QTest::newRow("Bob TFF") << false << int(DeinterlaceMode::Bob);
    QTest::newRow("Bob BFF") << true << int(DeinterlaceMode::Bob);
    QTest::newRow("Linear TFF") << false << int(DeinterlaceMode::Linear);
    QTest::newRow("Linear BFF") << true << int(DeinterlaceMode::Linear);
  }
  void bobPresentsTwoFieldsInTimeOrder() {
    QFETCH(bool, bottom);
    QFETCH(int, method);
    QTemporaryDir dir;
    QSettings settings(dir.filePath("s.ini"), QSettings::IniFormat);
    settings.setValue("video/autoplay", false);
    VideoView view(settings);
    view.open(fixture("colors.avi"));
    QTRY_VERIFY2(view.ready(), qPrintable(playbackDiagnostic(view)));
    view.setBottomFirst(bottom);
    view.setDeinterlace(DeinterlaceMode(method));
    QSignalSpy fields(&view, &VideoView::fieldPresented);
    view.player()->play();
    QTRY_VERIFY_WITH_TIMEOUT(fields.size() >= 6, 3000);
    view.player()->pause();
    QMap<qint64, QList<int>> groups;
    for (const auto &args : fields)
      groups[args[0].toLongLong()].append(args[1].toInt());
    int pairs = 0;
    for (const auto &group : groups)
      if (group.size() == 2) {
        QCOMPARE(group, QList<int>({bottom ? 1 : 0, bottom ? 0 : 1}));
        ++pairs;
      }
    QVERIFY(pairs >= 2);
    QVERIFY(!view.m_fieldTimer->isActive());
    view.seek(3000);
    QVERIFY(!view.m_fieldTimer->isActive());
  }
  void startPositionAndResumeAreIndependent_data() {
    QTest::addColumn<bool>("autoplay");
    QTest::addColumn<bool>("resume");
    for (bool autoplay : {false, true})
      for (bool resume : {false, true})
        QTest::newRow(qPrintable(QString("%1-%2").arg(autoplay).arg(resume)))
            << autoplay << resume;
  }
  void startPositionAndResumeAreIndependent() {
    QFETCH(bool, autoplay);
    QFETCH(bool, resume);
    QTemporaryDir dir;
    QSettings settings(dir.filePath("s.ini"), QSettings::IniFormat);
    VideoState state;
    state.position = 3200;
    writeVideoState(settings, fixture("colors.avi"), state);
    settings.setValue("video/autoplay", autoplay);
    settings.setValue("video/resume", resume);
    VideoView view(settings);
    view.resize(400, 300);
    view.open(fixture("colors.avi"));
    QTRY_VERIFY2(view.ready(), qPrintable(playbackDiagnostic(view)));
    QCOMPARE(view.playing(), autoplay);
    QVERIFY(resume ? green(view.coverImage()) : red(view.coverImage()));
    view.leave();
    QVERIFY(!view.playing());
    if (resume)
      QVERIFY(readVideoState(settings, fixture("colors.avi")).position >= 3000);
  }
  void sourceChangeCancelsOldCallbacksAndUnsupportedInputRecovers() {
    QTemporaryDir dir;
    QSettings settings(dir.filePath("s.ini"), QSettings::IniFormat);
    settings.setValue("video/autoplay", false);
    VideoState state;
    state.video = 1;
    writeVideoState(settings, fixture("multi.mkv"), state);
    VideoView view(settings);
    view.resize(400, 300);
    view.open(fixture("colors.avi"));
    view.open(fixture("multi.mkv"));
    QTRY_VERIFY_WITH_TIMEOUT(blue(view.coverImage()), 10000);
    view.open(dir.filePath("missing.mp4"));
    QTRY_VERIFY_WITH_TIMEOUT(!view.error().isEmpty(), 5000);
    view.open(fixture("colors.avi"));
    QTRY_VERIFY(red(view.coverImage()));
    QVERIFY(view.error().isEmpty());
  }
  void abStopSeekAndRestore() {
    QTemporaryDir dir;
    QSettings settings(dir.filePath("s.ini"), QSettings::IniFormat);
    settings.setValue("video/autoplay", false);
    VideoView view(settings);
    view.open(fixture("colors.avi"));
    QTRY_VERIFY2(view.ready(), qPrintable(playbackDiagnostic(view)));
    view.seek(1000);
    view.command("videoSetA");
    view.seek(3000);
    view.command("videoSetB");
    QVERIFY(!view.state().ab);
    view.command("videoAB");
    QVERIFY(view.state().ab);
    view.seek(4500);
    QCOMPARE(view.player()->position(), view.state().a);
    QVERIFY(view.state().ab);
    view.command("videoStop");
    QCOMPARE(view.player()->playbackState(), QMediaPlayer::StoppedState);
    QCOMPARE(view.position(), 0);
    QVERIFY(view.state().ab);
    view.command("videoPlayPause");
    QTRY_VERIFY(view.playing());
    QVERIFY(view.position() >= view.state().a);
    view.command("videoPlayPause");
    settings.setValue("video/abSeekDisables", true);
    view.reloadSettings();
    view.seek(4500);
    QVERIFY(!view.state().ab);
    QCOMPARE(view.state().a, qint64(1000));
    QCOMPARE(view.state().b, qint64(3000));
    view.command("videoAB");
    view.leave();
    view.open(fixture("colors.avi"));
    QTRY_VERIFY2(view.ready(), qPrintable(playbackDiagnostic(view)));
    QVERIFY(!view.state().ab);
    QCOMPARE(view.state().a, qint64(1000));
    settings.setValue("video/restoreAB", true);
    view.command("videoAB");
    view.leave();
    view.open(fixture("colors.avi"));
    QTRY_VERIFY2(view.ready(), qPrintable(playbackDiagnostic(view)));
    QVERIFY(view.state().ab);
  }
  void minimizedPauseReason() {
    QTemporaryDir dir;
    QSettings settings(dir.filePath("s.ini"), QSettings::IniFormat);
    settings.setValue("video/autoplay", false);
    VideoView view(settings);
    view.open(fixture("colors.avi"));
    QTRY_VERIFY2(view.ready(), qPrintable(playbackDiagnostic(view)));
    view.command("videoPlayPause");
    view.setPresentation(false, true);
    QVERIFY(view.playing());
    view.setPresentation(false, false);
    settings.setValue("video/pauseMinimized", true);
    view.reloadSettings();
    view.setPresentation(false, true);
    QVERIFY(!view.playing());
    view.setPresentation(false, false);
    QVERIFY(view.playing());
    view.command("videoPlayPause");
    view.setPresentation(false, true);
    view.setPresentation(false, false);
    QCOMPARE(view.player()->playbackState(), QMediaPlayer::PausedState);
  }
  void coversReuseCacheAndPreserveConditions() {
    QTemporaryDir dir;
    const auto path = dir.filePath("colors.avi");
    QVERIFY(QFile::copy(fixture("colors.avi"), path));
    QSettings settings(dir.filePath("s.ini"), QSettings::IniFormat);
    auto state = readVideoState(settings, path);
    state.coverTime = 3200;
    writeVideoState(settings, path, state);
    settings.sync();
    auto cache = std::make_shared<ThumbnailDiskCache>(dir.filePath("cache"));
    const auto cover = [&](bool refresh = false,
                           const QJsonObject &observed = {}) {
      return cachedCover(path, {}, cache, {}, refresh, 0, false, true, 0,
                         observed, false, nullptr, settings.fileName());
    };
    const auto generated = cover();
    QVERIFY2(!generated.image.isNull(), qPrintable(generated.error));
    QVERIFY(green(generated.image));
    const auto opens = VideoActivity::opens.load(),
               frames = VideoActivity::frames.load(),
               generations = ThumbnailActivity::generations.load();
    const auto pngs =
        QDir(cache->directory()).entryList({"*.png"}, QDir::Files);
    QCOMPARE(pngs.size(), 1);
    const auto png = cache->directory() + "/" + pngs.first();
    const auto created = QFileInfo(png).lastModified();
    QVERIFY(QFile::remove(path));
    QVERIFY(green(cover().image));
    cache = std::make_shared<ThumbnailDiskCache>(cache->directory());
    QVERIFY(green(cover().image));
    QCOMPARE(VideoActivity::opens.load(), opens);
    QCOMPARE(VideoActivity::frames.load(), frames);
    QCOMPARE(ThumbnailActivity::generations.load(), generations);
    QCOMPARE(QFileInfo(png).lastModified(), created);
    settings.setValue("video/decode", 1);
    settings.sync();
    QVERIFY(green(cover().image));
    QCOMPARE(VideoActivity::opens.load(), opens);
    QVERIFY(QFile::copy(fixture("colors.avi"), path));
    QJsonObject old{{"modified", QString::number(created.toMSecsSinceEpoch())},
                    {"bytes", QString::number(QFileInfo(path).size())}};
    QVERIFY(green(cover(false, old).image));
    QCOMPARE(VideoActivity::opens.load(), opens);
    const auto refreshed = cover(true);
    QVERIFY2(green(refreshed.image), qPrintable(refreshed.error));
    QVERIFY(VideoActivity::opens.load() > opens);
    cache->clear();
    QCOMPARE(readVideoState(settings, path).coverTime, qint64(3200));
    QVERIFY(green(cover().image));
  }
  void mixedModeNavigationAndKeys() {
    QTemporaryDir dir;
    const auto one = dir.filePath("1.avi"), zip = dir.filePath("2.zip"),
               three = dir.filePath("3.avi");
    QVERIFY(QFile::copy(fixture("colors.avi"), one));
    QVERIFY(QFile::copy(fixture("colors.avi"), three));
    QVERIFY(TestData::write(
        zip,
        TestData::zip({{"1.png", TestData::png(TestData::solid(Qt::green))}})));
    const auto config = dir.filePath("s.ini");
    {
      QSettings s(config, QSettings::IniFormat);
      s.setValue("video/autoplay", false);
      s.setValue("display/filename", false);
    }
    MainWindow window(config);
    window.show();
    window.openBook(one);
    QTRY_VERIFY_WITH_TIMEOUT(window.renderReady(), 10000);
    QCOMPARE(window.viewMode(), MainWindow::ViewMode::Video);
    QTest::keyClick(window.m_video, Qt::Key_Space);
    QTRY_VERIFY(window.m_video->playing());
    QTest::keyClick(window.m_video, Qt::Key_Space);
    QVERIFY(!window.m_video->playing());
    QTest::keyClick(window.m_video, Qt::Key_Down);
    QCOMPARE(window.m_video->findChild<QSlider *>("videoVolume")->value(), 45);
    window.moveBook(1);
    QTRY_COMPARE_WITH_TIMEOUT(window.bookPath(), zip, 10000);
    QTRY_VERIFY(window.renderReady());
    QCOMPARE(window.viewMode(), MainWindow::ViewMode::Reader);
    window.moveBook(1);
    QTRY_COMPARE_WITH_TIMEOUT(window.bookPath(), three, 10000);
    QTRY_VERIFY(window.renderReady());
    QCOMPARE(window.viewMode(), MainWindow::ViewMode::Video);
    window.showCovers();
    QCOMPARE(window.viewMode(), MainWindow::ViewMode::Covers);
    QVERIFY(!window.m_video->playing());
    QVERIFY(window.findChild<QMenu *>("menuVideo"));
    window.close();
  }
  void sourceAspectRatioAndSpeedTimeline() {
    QTemporaryDir dir;
    QSettings settings(dir.filePath("s.ini"), QSettings::IniFormat);
    settings.setValue("video/autoplay", false);
    VideoView view(settings);
    view.resize(600, 400);
    view.show();
    view.open(fixture("sar.mkv"));
    QTRY_VERIFY2(view.ready(), qPrintable(playbackDiagnostic(view)));
    QCOMPARE(view.imageRect().width() / view.imageRect().height(), 3.);
    view.setAspect(1);
    QCOMPARE(view.imageRect().width() / view.imageRect().height(), 1.);
    view.command("videoAspect");
    QCOMPARE(view.imageRect().width() / view.imageRect().height(), 4. / 3);
    view.setAspect(0);
    const auto elapsedAtRate = [&](double rate) -> qint64 {
      view.player()->pause();
      view.setRate(rate);
      if (view.position() > 0 || view.m_sourceFrame.startTime() > 0) {
        view.seek(0);
        if (!QTest::qWaitFor([&] {
              return view.position() == 0 && view.m_seekTarget < 0 &&
                     view.m_sourceFrame.startTime() < 100000;
            }, 5000))
          return -1;
      }
      view.command("videoPlayPause");
      // Measure the running media clock, after asynchronous seek/startup has
      // completed. Initialization latency is not playback speed.
      if (!QTest::qWaitFor([&] { return view.position() >= 200; }, 5000))
        return -1;
      const auto target = view.position() + 800;
      QElapsedTimer timer;
      timer.start();
      while (view.position() < target && timer.elapsed() < 5000)
        QTest::qWait(10);
      view.player()->pause();
      return view.position() >= target ? timer.elapsed() : -1;
    };
    const auto slow = elapsedAtRate(.5), fast = elapsedAtRate(2);
    QVERIFY2(slow > 0 && fast > 0, qPrintable(QString("slow=%1ms fast=%2ms").arg(slow).arg(fast)));
    QVERIFY2(slow > fast * 2,
             qPrintable(QString("slow=%1ms fast=%2ms").arg(slow).arg(fast)));
  }
  void fieldSchedulingAtBroadcastRates() {
    QTemporaryDir dir;
    QSettings settings(dir.filePath("s.ini"), QSettings::IniFormat);
    settings.setValue("video/autoplay", false);
    VideoView view(settings);
    view.open(fixture("colors.avi"));
    QTRY_VERIFY2(view.ready(), qPrintable(playbackDiagnostic(view)));
    view.player()->play();
    QTRY_VERIFY(view.playing());
    QSignalBlocker sinkEvents(view.m_sink);
    QSignalSpy fields(&view, &VideoView::fieldPresented);
    QImage image(4, 6, QImage::Format_ARGB32);
    image.fill(Qt::red);
    struct Timing {
      qint64 durationUs;
      double speed;
      int fieldDelayMs;
    };
    // Independent expectations for 25 and 30000/1001 fps, rounded to ms.
    const Timing cases[]{{40000, 1, 20}, {33367, 1, 17},  {40000, 2, 10},
                         {33367, 2, 8},  {40000, .5, 40}, {33367, .5, 33}};
    for (const auto mode : {DeinterlaceMode::Bob, DeinterlaceMode::Linear})
      for (bool bottom : {false, true})
        for (const auto &timing : cases) {
          view.setDeinterlace(mode);
          view.setBottomFirst(bottom);
          view.setRate(timing.speed);
          fields.clear();
          QVideoFrame frame(image);
          frame.setStartTime(1000000);
          frame.setEndTime(1000000 + timing.durationUs);
          view.acceptFrame(frame);
          QCOMPARE(fields.size(), 1); // second field is not presented early
          QCOMPARE(fields[0][1].toInt(), bottom ? 1 : 0);
          QVERIFY(view.m_fieldTimer->isActive());
          QCOMPARE(view.m_fieldTimer->interval(), timing.fieldDelayMs);
          // Advance the reserved callback explicitly, without wall-clock sleep.
          view.m_fieldTimer->stop();
          QVERIFY(QMetaObject::invokeMethod(view.m_fieldTimer, "timeout",
                                            Qt::DirectConnection));
          QCOMPARE(fields.size(), 2);
          QCOMPARE(fields[1][0].toLongLong(), qint64(1000000));
          QCOMPARE(fields[1][1].toInt(), bottom ? 0 : 1);
        }
    view.player()->pause();
  }
  void firstPaintPrecedesPersistenceAndCoverStoreDoesNotBlockUi() {
    QTemporaryDir dir;
    QSettings settings(dir.filePath("s.ini"), QSettings::IniFormat);
    settings.setValue("video/autoplay", false);
    settings.sync();
    MainWindow window(settings.fileName());
    std::latch releaseOld(1);
    auto cleanup = qScopeGuard([&] { releaseOld.count_down(); });
    window.m_displayJobs->submit<int>(DisplayJobs::Lane::Display, "open", "old-open",
        [&](const auto &) { releaseOld.wait(); return 1; }, [](int, bool) {});
    std::atomic_bool oldStoreRan{false};
    bool oldStoreCancelled = false;
    window.m_displayJobs->submit<int>(
        DisplayJobs::Lane::Store, "history-store", "old-history",
        [&](const auto &) {
          oldStoreRan = true;
          return 1;
        },
        [&](int, bool cancelled) { oldStoreCancelled = cancelled; });
    window.m_cacheMaintenanceNeeded = true;
    const auto path = fixture("colors.avi");
    window.openBook(path); // hidden until the decoded frame is ready
    releaseOld.count_down();
    cleanup.dismiss();
    QTRY_VERIFY(window.renderReady());
    QVERIFY(!window.m_readerPresented);
    QVERIFY(!settings.contains("lastBook"));
    QTest::qWait(150); // includes the background timer while still unpainted
    QVERIFY(!oldStoreRan);
    QVERIFY(oldStoreCancelled);
    QVERIFY(window.m_cacheMaintenanceNeeded);
    window.show();
    QTRY_VERIFY(window.m_readerPresented);
    QCOMPARE(settings.value("lastBook").toString(), path);
    window.m_video->seek(3200);
    QTRY_VERIFY(green(window.m_video->coverImage()));
    const auto cache =
        std::make_shared<ThumbnailDiskCache>(dir.filePath("cache"));
    window.m_thumbnailDiskCache = cache;
    QVERIFY(QDir().mkpath(cache->directory()));
    QLockFile busy(cache->directory() + "/.lock");
    QVERIFY(busy.tryLock());
    const auto opens = VideoActivity::opens.load();
    QElapsedTimer timer;
    timer.start();
    window.command("videoCover")->trigger();
    QVERIFY2(timer.elapsed() < 500, "Cover persistence blocked the GUI thread");
    bool responsive = false;
    QTimer::singleShot(0, &window, [&] { responsive = true; });
    QTRY_VERIFY_WITH_TIMEOUT(responsive, 500);
    // Completed pixels must be usable while the regular PNG writer is busy.
    QTRY_VERIFY_WITH_TIMEOUT(green(cachedCover(path, {}, cache, {}, false, 0,
                                               true, false, 0, {}, true)
                                       .image),
                             500);
    QCOMPARE(VideoActivity::opens.load(), opens);
    QCOMPARE(QDir(cache->directory()).entryList({"*.png"}, QDir::Files).size(),
             0);
    busy.unlock();
    QTRY_VERIFY(!window.m_displayJobs->contains(path));
    QCOMPARE(QDir(cache->directory()).entryList({"*.png"}, QDir::Files).size(),
             1);
  }
  void hiddenCoversDoNotOpenVideos() {
    QTemporaryDir dir;
    for (const auto &name : {"first.avi", "second.avi", "third.avi"})
      QVERIFY(QFile::copy(fixture("colors.avi"), dir.filePath(name)));
    MainWindow window(dir.filePath("s.ini"));
    window.show();
    const auto opens = VideoActivity::opens.load();
    const auto generations = ThumbnailActivity::generations.load();
    window.openBook(dir.filePath("first.avi"));
    QTRY_VERIFY(window.renderReady());
    QTest::qWait(100);
    QCOMPARE(window.viewMode(), MainWindow::ViewMode::Video);
    QCOMPARE(VideoActivity::opens.load(), opens + 1);
    QCOMPARE(ThumbnailActivity::generations.load(), generations);
    QVERIFY(!window.m_thumbnailPreloadTimer->isActive());
  }
  void decodePolicyOnlyChangesInNewProcess() {
    QTemporaryDir dir;
    const auto config = dir.filePath("s.ini");
    for (int policy : {0, 1}) {
      {
        QSettings settings(config, QSettings::IniFormat);
        settings.setValue("video/decode", policy);
        settings.sync();
      }
      QProcess child;
      auto env = QProcessEnvironment::systemEnvironment();
      env.remove("QT_FFMPEG_DECODING_HW_DEVICE_TYPES");
      child.setProcessEnvironment(env);
      child.start(QString::fromUtf8(KOMAMIMIC_VIDEO_PROBE),
                  {"--decode-policy", config});
      QVERIFY(child.waitForFinished(10000));
      QCOMPARE(child.exitCode(), 0);
      const auto report =
          QJsonDocument::fromJson(child.readAllStandardOutput()).object();
      QCOMPARE(report.value("hardware_types").toString(),
               policy ? QString(",") : QString{});
      QVERIFY(report.value("unchanged").toBool());
    }
  }
  void decodePreferencesRestartNotice() {
    QTemporaryDir dir;
    MainWindow window(dir.filePath("s.ini"));
    window.show();
    int notices = 0;
    QTimer dismiss;
    connect(&dismiss, &QTimer::timeout, &window, [&] {
      if (auto *m =
              qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
        if (m->objectName() == "videoRestartNotice")
          ++notices;
        m->accept();
      }
    });
    dismiss.start(10);
    const auto edit = [&](int index, bool accepted) {
      QTimer::singleShot(0, &window, [&window, index, accepted] {
        auto *d = window.findChild<QDialog *>("preferencesDialog");
        QVERIFY(d);
        auto *box = d->findChild<QComboBox *>("video/decode");
        QVERIFY(box);
        box->setCurrentIndex(index);
        if (accepted)
          d->findChild<QDialogButtonBox *>()
              ->button(QDialogButtonBox::Ok)
              ->click();
        else
          d->reject();
      });
      window.command("videoPreferences")->trigger();
    };
    edit(1, false);
    QCOMPARE(notices, 0);
    edit(1, true);
    QCOMPARE(notices, 1);
    edit(1, true);
    QCOMPARE(notices, 1);
    edit(0, true);
    QCOMPARE(notices, 2);
    QSettings saved(dir.filePath("s.ini"), QSettings::IniFormat);
    QCOMPARE(saved.value("video/decode").toInt(), 0);
  }
};
ISOLATED_TEST_MAIN(VideoTests)
#include "video_tests.moc"
