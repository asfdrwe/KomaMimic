// SPDX-License-Identifier: GPL-3.0-only
#include "videomedia.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QMimeDatabase>
#include <QPainter>
#include <QTimer>
#include <QTransform>
#include <QUrl>
#include <QVideoSink>
#include <algorithm>
#include <cmath>
#include <cstring>

std::atomic<quint64> VideoActivity::opens{0}, VideoActivity::frames{0};
QStringList videoExtensions() {
  return {"mp4",  "m4v", "mkv", "webm", "avi", "mov",  "qt",  "mpg",
          "mpeg", "mpe", "m2v", "ts",   "mts", "m2ts", "m2t", "vob",
          "ogv",  "wmv", "asf", "flv",  "f4v", "3gp",  "3g2", "divx"};
}
bool isVideoPath(const QString &path) {
  return videoExtensions().contains(QFileInfo(path).suffix().toLower()) ||
         QMimeDatabase()
             .mimeTypeForFile(path, QMimeDatabase::MatchExtension)
             .name()
             .startsWith("video/");
}
bool isAnimatedImagePath(const QString &path) {
  return QStringList{"gif", "apng", "webp"}.contains(
      QFileInfo(path).suffix().toLower());
}
QString videoStateKey(const QString &path) {
  return "videos/" +
         QString::fromLatin1(
             QCryptographicHash::hash(
                 QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toUtf8(),
                 QCryptographicHash::Sha256)
                 .toHex());
}
VideoOptions VideoOptions::load(const QSettings &s) {
  VideoOptions o;
  o.autoplay = s.value("video/autoplay", true).toBool();
  o.resume = s.value("video/resume", true).toBool();
  o.restoreAB = s.value("video/restoreAB", false).toBool();
  o.pauseMinimized = s.value("video/pauseMinimized", false).toBool();
  o.animations = s.value("video/animations", false).toBool();
  o.inhibit = s.value("video/inhibit", true).toBool();
  o.abSeekDisables = s.value("video/abSeekDisables", false).toBool();
  o.micro = std::clamp(s.value("video/microSeek", 3).toInt(), 1, 86400);
  o.seek = std::clamp(s.value("video/seek", 10).toInt(), 1, 86400);
  o.longSeek = std::clamp(s.value("video/longSeek", 60).toInt(), 1, 86400);
  o.volume = std::clamp(s.value("video/volume", 50).toInt(), 0, 100);
  o.volumeStep = std::clamp(s.value("video/volumeStep", 5).toInt(), 1, 100);
  o.bar = std::clamp(s.value("video/bar", 1).toInt(), 0, 2);
  o.fullscreenBar = std::clamp(s.value("video/fullscreenBar", 0).toInt(), 0, 2);
  o.hideMs = std::clamp(s.value("video/hideMs", 3000).toInt(), 100, 60000);
  o.output = s.value("video/output").toByteArray();
  o.rateMin = std::clamp(s.value("video/rateMin", .25).toDouble(), .05, 16.);
  o.rateMax =
      std::clamp(s.value("video/rateMax", 4).toDouble(), o.rateMin, 16.);
  if (s.contains("video/rates")) {
    QList<double> rates;
    for (const auto &part : s.value("video/rates").toString().split(',')) {
      bool ok = false;
      const double v = part.trimmed().toDouble(&ok);
      if (ok && std::isfinite(v) && v >= o.rateMin && v <= o.rateMax &&
          !rates.contains(v))
        rates.append(v);
    }
    if (!rates.isEmpty()) {
      std::sort(rates.begin(), rates.end());
      o.rates = rates;
    }
  }
  return o;
}
VideoState readVideoState(const QSettings &s, const QString &path) {
  VideoState v;
  const auto k = videoStateKey(path) + "/";
  v.position = std::max<qint64>(0, s.value(k + "position", 0).toLongLong());
  v.completed = s.value(k + "completed", false).toBool();
  v.video = std::max(0, s.value(k + "video", 0).toInt());
  v.audio = std::max(0, s.value(k + "audio", 0).toInt());
  v.subtitle = std::max(-1, s.value(k + "subtitle", -1).toInt());
  v.coverTime = std::max<qint64>(0, s.value(k + "coverTime", 0).toLongLong());
  v.coverTrack = std::max(0, s.value(k + "coverTrack", 0).toInt());
  v.coverChosen = s.value(k + "coverChosen", false).toBool();
  v.a = s.value(k + "a", -1).toLongLong();
  v.b = s.value(k + "b", -1).toLongLong();
  v.ab = s.value(k + "ab", false).toBool() && v.a >= 0 && v.b > v.a;
  v.aspect = std::clamp(s.value(k + "aspect", 0).toInt(), 0, 3);
  v.deinterlace =
      DeinterlaceMode(std::clamp(s.value(k + "deinterlace", 0).toInt(), 0, 3));
  v.bottomFirst = s.value(k + "bottomFirst", false).toBool();
  v.rate = s.value(k + "rate", 1).toDouble();
  if (!std::isfinite(v.rate) || v.rate <= 0)
    v.rate = 1;
  v.zoom = s.value(k + "zoom", 1).toDouble();
  if (!std::isfinite(v.zoom) || v.zoom <= 0)
    v.zoom = 1;
  v.fit = s.value(k + "fit", true).toBool();
  return v;
}
void writeVideoState(QSettings &s, const QString &path, const VideoState &v) {
  if (path.isEmpty())
    return;
  const auto k = videoStateKey(path) + "/";
  const QVariantMap values{{"position", v.position},
                           {"completed", v.completed},
                           {"video", v.video},
                           {"audio", v.audio},
                           {"subtitle", v.subtitle},
                           {"coverTime", v.coverTime},
                           {"coverTrack", v.coverTrack},
                           {"coverChosen", v.coverChosen},
                           {"a", v.a},
                           {"b", v.b},
                           {"ab", v.ab},
                           {"aspect", v.aspect},
                           {"deinterlace", int(v.deinterlace)},
                           {"bottomFirst", v.bottomFirst},
                           {"rate", v.rate},
                           {"zoom", v.zoom},
                           {"fit", v.fit}};
  for (auto i = values.begin(); i != values.end(); ++i)
    s.setValue(k + i.key(), i.value());
}
void initializeVideoDecoding(const QSettings &s) {
  // The Qt backend caches hardware selection on first use. Never change it
  // when applying preferences to an already running player.
  static bool initialized = false;
  if (initialized)
    return;
  initialized = true;
  if (s.value("video/decode", 0).toInt() == 1)
    qputenv("QT_FFMPEG_DECODING_HW_DEVICE_TYPES", ",");
}
QImage deinterlaceImage(const QImage &input, DeinterlaceMode mode, int field) {
  if (input.isNull() || mode == DeinterlaceMode::Off)
    return input;
  const auto src = input.convertToFormat(QImage::Format_ARGB32);
  auto out = src.copy();
  const int h = src.height(), w = src.width();
  field = field ? 1 : 0;
  if (h < 2)
    return out;
  const auto mix = [](QRgb a, QRgb b) {
    return qRgba((qRed(a) + qRed(b)) / 2, (qGreen(a) + qGreen(b)) / 2,
                 (qBlue(a) + qBlue(b)) / 2, (qAlpha(a) + qAlpha(b)) / 2);
  };
  for (int y = 0; y < h; ++y) {
    auto *dst = reinterpret_cast<QRgb *>(out.scanLine(y));
    if (mode == DeinterlaceMode::Blend) {
      const auto *a = reinterpret_cast<const QRgb *>(src.constScanLine(y));
      const auto *b = reinterpret_cast<const QRgb *>(
          src.constScanLine(std::min(y ^ 1, h - 1)));
      for (int x = 0; x < w; ++x)
        dst[x] = mix(a[x], b[x]);
    } else if (mode == DeinterlaceMode::Bob) {
      const int row = std::min((y / 2) * 2 + field, h - 1);
      std::memcpy(dst, src.constScanLine(row), size_t(w) * 4);
    } else if (y % 2 != field) {
      int before = y - 1, after = y + 1;
      if (before < 0)
        before = after;
      if (after >= h)
        after = before;
      const auto *a = reinterpret_cast<const QRgb *>(src.constScanLine(before));
      const auto *b = reinterpret_cast<const QRgb *>(src.constScanLine(after));
      for (int x = 0; x < w; ++x)
        dst[x] = mix(a[x], b[x]);
    }
  }
  return out;
}
QImage videoFrameImage(const QVideoFrame &frame, DeinterlaceMode mode,
                       int field) {
  auto image = frame.toImage();
  if (image.isNull())
    return {};
  // toImage includes the surface-format transformation, but not the extra
  // per-frame presentation transform. Process scan lines before rotation.
  const auto fmt = frame.surfaceFormat();
  QTransform format;
  format.rotate(int(fmt.rotation()));
  if (fmt.isMirrored())
    format.scale(-1, 1);
  if (mode != DeinterlaceMode::Off) {
    if (!format.isIdentity())
      image = image.transformed(format.inverted());
    image = deinterlaceImage(image, mode, field);
    if (!format.isIdentity())
      image = image.transformed(format);
  }
  QTransform extra;
  extra.rotate(int(frame.rotation()));
  if (frame.mirrored())
    extra.scale(-1, 1);
  if (!extra.isIdentity())
    image = image.transformed(extra);
  return image;
}
QRectF videoRectangle(QSize image, QSize viewport, int aspect, bool fit,
                      double zoom) {
  if (image.isEmpty() || viewport.isEmpty())
    return {};
  const double ratio = aspect == 1   ? 1
                       : aspect == 2 ? 4. / 3
                       : aspect == 3 ? 16. / 9
                                     : double(image.width()) / image.height();
  double h = image.height();
  if (fit)
    h = std::min<double>(viewport.height(), viewport.width() / ratio);
  const QSizeF size(h * ratio * zoom, h * zoom);
  return QRectF((viewport.width() - size.width()) / 2,
                (viewport.height() - size.height()) / 2, size.width(),
                size.height());
}
ImageResult videoCover(const QString &path, const QString &settingsFile,
                       QSize size, const std::function<bool()> &cancel) {
  if (cancel && cancel())
    return {};
  VideoState state;
  if (!settingsFile.isEmpty()) {
    QSettings s(settingsFile, QSettings::IniFormat);
    state = readVideoState(s, path);
  }
  ++VideoActivity::opens;
  QMediaPlayer player;
  QVideoSink sink;
  QEventLoop loop;
  QTimer deadline, poll;
  deadline.setSingleShot(true);
  poll.setInterval(20);
  player.setVideoSink(&sink);
  ImageResult result;
  bool requested = false;
  QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
  QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
    if (cancel && cancel())
      loop.quit();
  });
  QObject::connect(&player, &QMediaPlayer::errorOccurred, &loop,
                   [&](auto, const QString &error) {
                     result.error = error;
                     loop.quit();
                   });
  QObject::connect(&player, &QMediaPlayer::mediaStatusChanged, &loop,
                   [&](auto status) {
                     if (status != QMediaPlayer::LoadedMedia || requested)
                       return;
                     requested = true;
                     if (state.coverTrack < player.videoTracks().size())
                       player.setActiveVideoTrack(state.coverTrack);
                     player.setActiveSubtitleTrack(-1);
                     player.setPosition(state.coverTime);
                     player.pause();
                   });
  QObject::connect(
      &sink, &QVideoSink::videoFrameChanged, &loop, [&](const QVideoFrame &f) {
        if (!requested || !f.isValid() || (cancel && cancel()))
          return;
        // A seek may deliver an old queued frame first. Accept the requested
        // time approximately, without promising frame-exact extraction.
        if (state.coverTime > 0 && f.endTime() >= 0 &&
            f.endTime() / 1000 < state.coverTime - 500)
          return;
        ++VideoActivity::frames;
        auto image =
            videoFrameImage(f, state.deinterlace, state.bottomFirst ? 1 : 0);
        if (image.isNull())
          return;
        if (state.aspect) {
          const double ratio = state.aspect == 1   ? 1
                               : state.aspect == 2 ? 4. / 3
                                                   : 16. / 9;
          image = image.scaled(
              QSize(qRound(image.height() * ratio), image.height()),
              Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
        QImage display(image.size(), QImage::Format_ARGB32);
        display.fill(Qt::black);
        {
          QPainter p(&display);
          p.setRenderHint(QPainter::SmoothPixmapTransform);
          p.drawImage(
              videoRectangle(image.size(), display.size(), 0, true, state.zoom),
              image);
        }
        result.image =
            display.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        loop.quit();
      });
  deadline.start(15000);
  poll.start();
  player.setSource(QUrl::fromLocalFile(path));
  loop.exec();
  player.stop();
  if (cancel && cancel())
    return {};
  if (result.image.isNull() && result.error.isEmpty())
    result.error = QCoreApplication::translate("KomaMimic",
                                               "動画の表紙を取得できません: %1")
                       .arg(path);
  return result;
}
