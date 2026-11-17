// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "media.h"
#include <QSettings>
#include <QVideoFrame>
#include <atomic>

enum class DeinterlaceMode { Off, Blend, Bob, Linear };
struct VideoState {
  qint64 position = 0, coverTime = 0, a = -1, b = -1;
  int video = 0, audio = 0, subtitle = -1, coverTrack = 0, aspect = 0;
  DeinterlaceMode deinterlace = DeinterlaceMode::Off;
  bool bottomFirst = false, ab = false, completed = false, coverChosen = false;
  double rate = 1, zoom = 1;
  bool fit = true;
};
struct VideoOptions {
  bool autoplay = true, resume = true, restoreAB = false,
       pauseMinimized = false, animations = false, inhibit = true;
  bool abSeekDisables = false;
  int micro = 3, seek = 10, longSeek = 60, volume = 50, volumeStep = 5, bar = 1,
      fullscreenBar = 0, hideMs = 3000;
  double rateMin = .25, rateMax = 4;
  QList<double> rates{.25, .5, .75, 1, 1.25, 1.5, 2, 3, 4};
  QByteArray output;
  static VideoOptions load(const QSettings &settings);
};
QStringList videoExtensions();
bool isVideoPath(const QString &path);
bool isAnimatedImagePath(const QString &path);
QString videoStateKey(const QString &path);
VideoState readVideoState(const QSettings &settings, const QString &path);
void writeVideoState(QSettings &settings, const QString &path,
                     const VideoState &state);
void initializeVideoDecoding(const QSettings &settings);
QImage deinterlaceImage(const QImage &image, DeinterlaceMode mode,
                        int field = 0);
QImage videoFrameImage(const QVideoFrame &frame, DeinterlaceMode mode,
                       int field = 0);
QRectF videoRectangle(QSize image, QSize viewport, int aspect, bool fit,
                      double zoom);
struct VideoActivity {
  static std::atomic<quint64> opens, frames;
};
ImageResult videoCover(const QString &path, const QString &settingsFile,
                       QSize size, const std::function<bool()> &cancel = {});
