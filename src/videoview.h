// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "videomedia.h"
#include <QElapsedTimer>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QPointer>
#include <QWidget>
class QAudioOutput;
class QVideoSink;
class QAudioBufferOutput;
class QMenu;
class QLabel;
class QSlider;
class QTimer;
class QToolButton;
class QFormLayout;
class QLineEdit;

class VideoPreferences : public QWidget {
  Q_OBJECT
public:
  explicit VideoPreferences(QSettings &settings, QWidget *parent = nullptr);
  bool validate();
  bool save(); // true iff the saved decode policy changed
private:
  QSettings &m_settings;
  QHash<QString, QWidget *> m_editors;
  QLabel *m_error = nullptr;
};

class VideoView : public QWidget {
  Q_OBJECT
public:
  explicit VideoView(QSettings &settings, QWidget *parent = nullptr);
  ~VideoView() override;
  void open(const QString &path);
  void leave();
  void save();
  void reloadSettings();
  void command(const QString &id);
  void seek(qint64 milliseconds, bool manual = true);
  void setAspect(int aspect);
  void setDeinterlace(DeinterlaceMode mode);
  void setBottomFirst(bool bottomFirst);
  void setRate(double rate);
  void setScale(bool fit, double zoom = 1);
  void setPresentation(bool fullscreen, bool minimized);
  void populateMenu(QMenu *menu, const QHash<QString, QAction *> &actions);
  QImage coverImage() const;
  void rememberCover();
  QMediaPlayer *player() const { return m_player; }
  const VideoState &state() const { return m_state; }
  QString path() const { return m_path; }
  QString error() const { return m_error; }
  bool ready() const { return !m_frame.isNull(); }
  bool playing() const;
  bool inhibitRequested() const;
  qint64 position() const;
  QRectF imageRect() const;
signals:
  void changed();
  void framePresented();
  void painted();
  void fieldPresented(qint64 startUs, int field);
  void ended();
  void errorOccurred(const QString &error);
  void coverRequested();

protected:
  void paintEvent(QPaintEvent *) override;
  void resizeEvent(QResizeEvent *) override;
  void mouseMoveEvent(QMouseEvent *) override;
  void mouseDoubleClickEvent(QMouseEvent *) override;
  void hideEvent(QHideEvent *) override;

private:
  friend class MainWindow;
  friend class VideoTests;
  void acceptFrame(const QVideoFrame &frame);
  void redraw(int field = -1);
  void updateControls();
  void revealControls();
  void updateBar();
  void selectTrack(int kind, int index);
  void cycleTrack(int kind);
  void play();
  void stop();
  void changeVolume(int delta);
  void applyOutput();
  QSettings &m_settings;
  VideoOptions m_options;
  VideoState m_state;
  QString m_path, m_error;
  QPointer<QMediaPlayer> m_player;
  QAudioOutput *m_audio = nullptr;
  QVideoSink *m_sink = nullptr;
  QVideoFrame m_sourceFrame;
  QImage m_frame;
  QWidget *m_bar = nullptr;
  QLabel *m_time = nullptr;
  QSlider *m_seek = nullptr, *m_volume = nullptr;
  QToolButton *m_playButton = nullptr;
  QTimer *m_fieldTimer = nullptr, *m_hideTimer = nullptr,
         *m_saveTimer = nullptr;
  bool m_fullscreen = false, m_minimized = false, m_pausedForMinimize = false,
       m_active = false, m_loaded = false, m_stopping = false,
       m_seeking = false;
  bool m_muted = false, m_painted = false;
  qint64 m_seekTarget = -1;
  quint64 m_epoch = 0;
};
