// SPDX-License-Identifier: GPL-3.0-only
// Exercise the generated manual inputs through the real public reader APIs.
#include "book.h"
#include "publication.h"
#include "videomedia.h"
#include "test_paths.h"
#include <QAudioBuffer>
#include <QAudioBufferOutput>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMediaPlayer>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoSink>
#include <algorithm>
#include <cstdio>

int main(int argc, char **argv) {
  const auto progress = [](const QString &message) {
    const auto bytes = message.toUtf8();
    fprintf(stderr, "%s\n", bytes.constData());
    fflush(stderr);
  };
  progress("BEGIN QGuiApplication");
  QGuiApplication app(argc, argv);
  progress("PASS QGuiApplication: " + QGuiApplication::platformName());
  auto arguments = app.arguments();
  QString outputPath;
  if (arguments.size() >= 3 && arguments.at(arguments.size() - 2) == "--output") {
    outputPath = arguments.takeLast();
    arguments.removeLast();
  }
  const auto writeReport = [&](const QByteArray &bytes) {
    if (outputPath.isEmpty()) {
      if (fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout) == size_t(bytes.size()) && fflush(stdout) == 0)
        return true;
      progress("Cannot write probe report to stdout");
      return false;
    }
    QFile output(outputPath);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate) || output.write(bytes) != bytes.size() || !output.flush()) {
      progress("Cannot write probe report: " + outputPath + ": " + output.errorString());
      return false;
    }
    return true;
  };
  const QJsonObject locations{
      {"cache",
       QStandardPaths::writableLocation(QStandardPaths::CacheLocation)},
      {"config",
       QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)},
      {"data",
       QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)},
      {"generic_data",
       QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)},
      {"temp", QDir::tempPath()}};
  const auto privateRoot = qEnvironmentVariable("KOMAMIMIC_TEST_ROOT");
  if (!privateRoot.isEmpty())
    for (const auto &value : locations) {
      if (!testPathIsInside(privateRoot, value.toString())) {
        progress("OS directory is outside the test environment: " +
                 value.toString());
        return 2;
      }
    }
  if (arguments.size() == 2 &&
      arguments.at(1) == "--standard-paths") {
    const auto json = QJsonDocument(locations).toJson();
    return writeReport(json) ? 0 : 2;
  }
  if (arguments.size() == 3 &&
      arguments.at(1) == "--decode-policy") {
    QSettings settings(arguments.at(2), QSettings::IniFormat);
    initializeVideoDecoding(settings);
    const auto first = qgetenv("QT_FFMPEG_DECODING_HW_DEVICE_TYPES");
    settings.setValue("video/decode",
                      1 - settings.value("video/decode", 0).toInt());
    initializeVideoDecoding(settings); // A running process keeps its policy.
    const auto result =
        QJsonDocument(
            QJsonObject{
                {"hardware_types", QString::fromUtf8(first)},
                {"unchanged",
                 first == qgetenv("QT_FFMPEG_DECODING_HW_DEVICE_TYPES")}})
            .toJson();
    return writeReport(result) ? 0 : 2;
  }
  if (arguments.size() == 3 && arguments.at(1) == "--video") {
    QMediaPlayer player;
    QVideoSink sink;
    QAudioBufferOutput audio;
    QEventLoop loop;
    QTimer deadline;
    player.setVideoSink(&sink);
    player.setAudioBufferOutput(&audio);
    int frames = 0, buffers = 0;
    QString error;
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    deadline.setSingleShot(true);
    const auto complete = [&] {
      if (frames >= 3 && buffers >= 2)
        loop.quit();
    };
    QObject::connect(&sink, &QVideoSink::videoFrameChanged, &loop,
                     [&](const QVideoFrame &frame) {
                       if (frame.isValid() && !frame.toImage().isNull()) {
                         ++frames;
                         complete();
                       }
                     });
    QObject::connect(&audio, &QAudioBufferOutput::audioBufferReceived, &loop,
                     [&](const QAudioBuffer &buffer) {
                       if (buffer.frameCount() > 0) {
                         ++buffers;
                         complete();
                       }
                     });
    QObject::connect(&player, &QMediaPlayer::errorOccurred, &loop,
                     [&](auto, const QString &text) {
                       error = text;
                       loop.quit();
                     });
    deadline.start(10000);
    player.setSource(
        QUrl::fromLocalFile(arguments.at(2) + "/sample.mp4"));
    player.play();
    loop.exec();
    progress(QString("VIDEO decoded: frames=%1; audio_buffers=%2; error=%3").arg(frames).arg(buffers).arg(error));
    const bool passed = frames >= 3 && buffers >= 2 && error.isEmpty();
    const auto result =
        QJsonDocument(QJsonObject{{"all_passed", passed},
                                  {"video_decoded", frames >= 3},
                                  {"audio_decoded", buffers >= 2},
                                  {"frames", frames},
                                  {"audio_buffers", buffers},
                                  {"qt_version", qVersion()},
                                  {"error", error}})
            .toJson();
    // Save and flush the result before stopping/tearing down native media.
    // Deployment checks use a separate file rather than buffered CRT stdout.
    const bool written = writeReport(result);
    progress(written ? "PASS video report saved" : "FAIL video report save");
    player.stop();
    if (!written) return 2;
    return passed ? 0 : 1;
  }
  if (arguments.size() != 2)
    return 2;
  const QString root = arguments.at(1) + '/';
  QJsonArray results;
  bool failed = false;
  const auto check = [&](const QString &name, bool ok,
                         const QString &detail = {}) {
    results.append(
        QJsonObject{{"case", name}, {"pass", ok}, {"detail", detail}});
    failed |= !ok;
    progress((ok ? "PASS " : "FAIL ") + name);
  };
  for (const auto &[name, count] :
       QVector<QPair<QString, int>>{{"images", 7},
                                    {"books/01-first.zip", 6},
                                    {"books/02-next.zip", 6},
                                    {"books/03-cp932.zip", 1},
                                    {"日本語 空白/書庫 & 01.cbz", 6},
                                    {"books/05-pages.tar", 6},
                                    {"books/06-pages.tar.gz", 6},
                                    {"animation/tiff/two-pages.tiff", 2},
                                    {"documents/three-pages.pdf", 3}}) {
    progress("BEGIN " + name);
    const auto opened = Book::open(root + name);
    bool ok = opened.book && opened.book->pages.size() == count;
    QString error = opened.error;
    if (opened.book && opened.book->pages.size() != count)
      error += QString("expected %1 pages, got %2")
                   .arg(count)
                   .arg(opened.book->pages.size());
    if (ok)
      for (int i = 0; i < count; ++i) {
        const auto image = opened.book->image(i, {160, 240});
        ok &= image.error.isEmpty() && !image.image.isNull();
        if (!image.error.isEmpty())
          error += image.error;
      }
    check(name, ok, error);
    if (opened.book && name.endsWith("03-cp932.zip"))
      check("cp932-name",
            opened.book->pages.first().name == QString("表紙ソ.png"),
            opened.book->pages.first().name);
  }
  OpenOptions pdfOptions;
  pdfOptions.pdfResources = true;
  progress("BEGIN pdf-internal-images");
  const auto pdf =
      Book::open(root + "documents/three-pages.pdf", {}, pdfOptions);
  bool pdfOk = pdf.book && pdf.book->pages.size() == 3;
  if (pdfOk)
    for (int i = 0; i < 3; ++i) {
      const auto image = pdf.book->image(i);
      pdfOk &= image.error.isEmpty() && image.image.size() == QSize(128, 192);
    }
  check("pdf-internal-images", pdfOk, pdf.error);
  for (const auto &name :
       {"animation/gif/two-frames.gif", "animation/apng/two-frames.apng"}) {
    progress(QString("BEGIN ") + name);
    QFile input(root + name);
    if (!input.open(QIODevice::ReadOnly)) {
      check(name, false, input.errorString());
      continue;
    }
    const auto decoded = decodeImage(input.readAll(), name);
    bool ok = decoded.error.isEmpty() && decoded.frames.size() == 2;
    if (ok)
      ok = decoded.frames[0].pixelColor(30, 30) == QColor(Qt::red) &&
           decoded.frames[1].pixelColor(30, 30) == QColor(Qt::blue) &&
           decoded.delays == QVector<int>{500, 500};
    check(name, ok, decoded.error);
  }
  for (const auto &name :
       {"aozora-utf8.txt", "aozora-cp932.txt", "sample.md"}) {
    progress(QString("BEGIN ") + name);
    OpenOptions options;
    if (QString(name).contains("cp932"))
      options.text.encoding = "CP932";
    const auto opened = Book::open(root + "documents/" + name, {}, options);
    bool ok = opened.book && opened.book->publication &&
              !opened.book->pages.isEmpty();
    if (ok) {
      const auto image = opened.book->preview(0, {160, 240});
      ok = image.error.isEmpty() && !image.image.isNull();
      ok &= !opened.book->image(0)
                 .error.isEmpty(); // No raster reading/export path.
    }
    check(name, ok, opened.error);
  }
  for (const auto &name : {"reflow.epub", "fixed.epub"}) {
    progress(QString("BEGIN ") + name);
    const auto publication = readPublication(root + "documents/" + name, {});
    const auto readingSections = std::count_if(
        publication->sections.cbegin(), publication->sections.cend(),
        [](const auto &section) { return !section.auxiliary; });
    bool ok = publication->error.isEmpty() && readingSections == 3 &&
              publication->toc.size() == 3;
    if (ok)
      for (const auto &section : publication->sections)
        if (!section.auxiliary)
          ok &= section.fixed == QString(name).startsWith("fixed") &&
                section.anchors.contains("p20");
    const auto cover = epubCover(root + "documents/" + name);
    QJsonArray sections;
    for (const auto &section : publication->sections)
      sections.append(QJsonObject{{"path", section.path},
                                  {"fixed", section.fixed},
                                  {"p20", section.anchors.contains("p20")}});
    check(name, ok && cover.error.isEmpty() && !cover.image.isNull(),
          QString::fromUtf8(
              QJsonDocument(
                  QJsonObject{{"error", publication->error + cover.error},
                              {"sections", sections},
                              {"toc", publication->toc.size()},
                              {"cover", !cover.image.isNull()}})
                  .toJson(QJsonDocument::Compact)));
  }
  progress("BEGIN mixed-archive-entry-paths");
  const auto mixed = Book::open(root + "books/04-mixed-documents.zip");
  bool mixedOk = mixed.book && mixed.book->archiveDirectory &&
                 mixed.book->pages.size() == 3;
  if (mixedOk)
    for (int i = 0; i < mixed.book->pages.size(); ++i) {
      const auto entry = Book::open(mixed.book->entryPath(i));
      mixedOk &= bool(entry.book);
    }
  check("mixed-archive-entry-paths", mixedOk, mixed.error);
  const auto output =
      QJsonDocument(QJsonObject{{"all_passed", !failed}, {"results", results}})
          .toJson();
  if (!writeReport(output)) return 2;
  return failed ? 1 : 0;
}
