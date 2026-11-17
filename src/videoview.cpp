// SPDX-License-Identifier: GPL-3.0-only
#include "videoview.h"
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QAudioBufferOutput>
#include <QAudioDevice>
#include <QAudioOutput>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMediaDevices>
#include <QMediaMetaData>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVideoSink>
#include <algorithm>
#include <cmath>

namespace {
QString clockText(qint64 ms) {
  const auto sec = std::max<qint64>(0, ms) / 1000;
  return QString("%1:%2:%3")
      .arg(sec / 3600, 2, 10, QChar('0'))
      .arg(sec / 60 % 60, 2, 10, QChar('0'))
      .arg(sec % 60, 2, 10, QChar('0'));
}
QString trackLabel(const QMediaMetaData &md, int index) {
  QStringList parts{QString::number(index + 1)};
  for (auto key : {QMediaMetaData::Title, QMediaMetaData::Language,
                   QMediaMetaData::VideoCodec, QMediaMetaData::AudioCodec}) {
    const auto text = md.stringValue(key);
    if (!text.isEmpty() && text != "Unspecified")
      parts.append(text);
  }
  return parts.join(" — ");
}
} // namespace
VideoView::VideoView(QSettings &settings, QWidget *parent)
    : QWidget(parent), m_settings(settings) {
  setObjectName("videoView");
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
  m_bar = new QWidget(this);
  m_bar->setObjectName("videoControls");
  m_bar->setAutoFillBackground(true);
  auto *layout = new QHBoxLayout(m_bar);
  layout->setContentsMargins(8, 5, 8, 5);
  const auto button = [&](const QString &name, const QString &text,
                          const QString &id) {
    auto *b = new QToolButton(m_bar);
    b->setObjectName(name);
    b->setText(text);
    b->setToolTip(text);
    b->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(b);
    connect(b, &QToolButton::clicked, this, [this, id] { command(id); });
    return b;
  };
  m_playButton = button("videoPlayButton", tr("再生"), "videoPlayPause");
  button("videoStopButton", tr("停止"), "videoStop");
  m_seek = new QSlider(Qt::Horizontal, m_bar);
  m_seek->setObjectName("videoSeekBar");
  m_seek->setRange(0, 10000);
  m_seek->setFocusPolicy(Qt::NoFocus);
  layout->addWidget(m_seek, 1);
  m_time = new QLabel(m_bar);
  m_time->setObjectName("videoTime");
  layout->addWidget(m_time);
  button("videoPositionButton", tr("時刻…"), "videoGoTime");
  button("videoMuteButton", tr("消音"), "videoMute");
  m_volume = new QSlider(Qt::Horizontal, m_bar);
  m_volume->setObjectName("videoVolume");
  m_volume->setRange(0, 100);
  m_volume->setMaximumWidth(100);
  m_volume->setFocusPolicy(Qt::NoFocus);
  layout->addWidget(m_volume);
  connect(m_seek, &QSlider::sliderReleased, this, [this] {
    if (m_player && m_player->duration() > 0)
      seek(qRound64(m_player->duration() * double(m_seek->value()) / 10000));
  });
  connect(m_volume, &QSlider::valueChanged, this, [this](int value) {
    m_options.volume = value;
    m_settings.setValue("video/volume", value);
    if (m_audio)
      m_audio->setVolume(value / 100.f);
    updateControls();
  });
  m_fieldTimer = new QTimer(this);
  m_fieldTimer->setSingleShot(true);
  m_fieldTimer->setTimerType(Qt::PreciseTimer);
  connect(m_fieldTimer, &QTimer::timeout, this, [this] {
    if (m_active && playing())
      redraw(m_state.bottomFirst ? 0 : 1);
  });
  m_hideTimer = new QTimer(this);
  m_hideTimer->setSingleShot(true);
  connect(m_hideTimer, &QTimer::timeout, this, [this] {
    if (QApplication::activePopupWidget() ||
        QApplication::activeModalWidget() ||
        QApplication::mouseButtons() != Qt::NoButton || m_bar->underMouse()) {
      m_hideTimer->start(m_options.hideMs);
      return;
    }
    const int mode = m_fullscreen ? m_options.fullscreenBar : m_options.bar;
    if (mode == 0)
      m_bar->hide();
  });
  m_saveTimer = new QTimer(this);
  m_saveTimer->setInterval(5000);
  connect(m_saveTimer, &QTimer::timeout, this, [this] {
    if (m_active && m_loaded && m_painted)
      save();
  });
  reloadSettings();
}
VideoView::~VideoView() {
  leave();
  if (m_player)
    delete m_player;
}
void VideoView::reloadSettings() {
  m_options = VideoOptions::load(m_settings);
  m_muted = m_settings.value("video/muted", false).toBool();
  {
    QSignalBlocker b(m_volume);
    m_volume->setValue(m_options.volume);
  }
  if (m_audio) {
    m_audio->setVolume(m_options.volume / 100.f);
    m_audio->setMuted(m_muted);
    applyOutput();
  }
  updateBar();
}
bool VideoView::playing() const {
  return m_player && m_player->playbackState() == QMediaPlayer::PlayingState;
}
qint64 VideoView::position() const {
  return m_player ? m_player->position() : 0;
}
bool VideoView::inhibitRequested() const {
  return m_options.inhibit && m_active && playing() && !m_minimized &&
         isVisible();
}
void VideoView::open(const QString &path) {
  leave();
  ++m_epoch;
  const auto epoch = m_epoch;
  if (m_player) {
    m_player->disconnect(this);
    m_player->deleteLater();
  }
  m_audio = nullptr;
  m_sink = nullptr;
  m_options = VideoOptions::load(m_settings);
  m_state = readVideoState(m_settings, path);
  const bool restoreVideo = m_settings.contains(videoStateKey(path) + "/video"),
             restoreAudio = m_settings.contains(videoStateKey(path) + "/audio");
  if (!m_options.restoreAB)
    m_state.ab = false;
  m_path = QFileInfo(path).absoluteFilePath();
  m_error.clear();
  m_sourceFrame = {};
  m_frame = {};
  m_painted = false;
  m_active = true;
  m_loaded = false;
  m_stopping = false;
  m_seekTarget = -1;
  m_pausedForMinimize = false;
  m_state.rate = std::clamp(m_state.rate, m_options.rateMin, m_options.rateMax);
  m_player = new QMediaPlayer(this);
  m_sink = new QVideoSink(m_player);
  m_player->setVideoSink(m_sink);
  // Device-independent tests use decoded buffers instead of opening a user's
  // speaker. Normal application playback always has an audio output.
  if (qApp->property("komamimicTestSilent").toBool())
    m_player->setAudioBufferOutput(new QAudioBufferOutput(m_player));
  else {
    m_audio = new QAudioOutput(m_player);
    m_player->setAudioOutput(m_audio);
    m_audio->setVolume(m_options.volume / 100.f);
    m_audio->setMuted(m_muted);
    applyOutput();
    auto *devices = new QMediaDevices(m_player);
    connect(devices, &QMediaDevices::audioOutputsChanged, this, [this, epoch] {
      if (epoch == m_epoch && m_active)
        applyOutput();
    });
  }
  m_player->setPitchCompensation(true);
  m_player->setPlaybackRate(m_state.rate);
  connect(m_sink, &QVideoSink::videoFrameChanged, this,
          [this, epoch](const QVideoFrame &frame) {
            if (epoch == m_epoch && m_active)
              acceptFrame(frame);
          });
  connect(m_sink, &QVideoSink::subtitleTextChanged, this, [this, epoch] {
    if (epoch == m_epoch)
      update();
  });
  connect(m_player, &QMediaPlayer::errorOccurred, this,
          [this, epoch](auto, const QString &error) {
            if (epoch != m_epoch || !m_active)
              return;
            m_error = error;
            emit errorOccurred(error);
            emit changed();
            update();
          });
  connect(
      m_player, &QMediaPlayer::mediaStatusChanged, this,
      [this, epoch, restoreVideo, restoreAudio](auto status) {
        if (epoch != m_epoch || !m_active)
          return;
        if (status == QMediaPlayer::LoadedMedia && !m_loaded) {
          m_loaded = true;
          if (restoreVideo && m_state.video < m_player->videoTracks().size())
            m_player->setActiveVideoTrack(m_state.video);
          if (restoreAudio && m_state.audio < m_player->audioTracks().size())
            m_player->setActiveAudioTrack(m_state.audio);
          m_state.video = m_player->activeVideoTrack();
          m_state.audio = m_player->activeAudioTrack();
          m_player->setActiveSubtitleTrack(
              m_state.subtitle < m_player->subtitleTracks().size()
                  ? m_state.subtitle
                  : -1);
          qint64 pos =
              m_options.resume && !m_state.completed ? m_state.position : 0;
          if (m_state.ab)
            pos = m_state.a;
          m_seekTarget = pos;
          m_player->setPosition(pos);
          m_state.completed = false;
          if (m_options.autoplay && !(m_minimized && m_options.pauseMinimized))
            m_player->play();
          else
            m_player->pause();
          updateControls();
          emit changed();
        } else if (status == QMediaPlayer::EndOfMedia && !m_stopping) {
          m_fieldTimer->stop();
          if (m_state.ab) {
            seek(m_state.a, false);
            m_player->play();
            return;
          }
          m_state.completed = true;
          m_state.position = 0;
          writeVideoState(m_settings, m_path, m_state);
          updateControls();
          emit changed();
          emit ended();
        }
      });
  connect(m_player, &QMediaPlayer::positionChanged, this,
          [this, epoch](qint64 pos) {
            if (epoch != m_epoch || !m_active || !m_loaded || m_stopping)
              return;
            if (!m_seeking && m_state.ab && playing() && pos >= m_state.b) {
              seek(m_state.a, false);
              return;
            }
            if (!m_state.completed)
              m_state.position = pos;
            updateControls();
          });
  connect(m_player, &QMediaPlayer::playbackStateChanged, this, [this, epoch] {
    if (epoch != m_epoch)
      return;
    if (!playing())
      m_fieldTimer->stop();
    updateControls();
    emit changed();
  });
  connect(m_player, &QMediaPlayer::durationChanged, this,
          [this] { updateControls(); });
  connect(m_player, &QMediaPlayer::seekableChanged, this,
          [this] { updateControls(); });
  QTimer::singleShot(15000, m_player, [this, epoch] {
    if (epoch == m_epoch && m_active && !ready() && m_error.isEmpty()) {
      m_error = tr("動画の映像を取得できません。");
      m_player->pause();
      emit errorOccurred(m_error);
      update();
    }
  });
  ++VideoActivity::opens;
  updateControls();
  m_player->setSource(QUrl::fromLocalFile(m_path));
  m_saveTimer->start();
  revealControls();
  update();
}
void VideoView::save() {
  if (m_path.isEmpty())
    return;
  if (m_player && m_loaded && !m_stopping && !m_state.completed)
    m_state.position = m_player->position();
  writeVideoState(m_settings, m_path, m_state);
}
void VideoView::leave() {
  if (!m_active)
    return;
  save();
  m_active = false;
  ++m_epoch;
  m_saveTimer->stop();
  m_fieldTimer->stop();
  m_hideTimer->stop();
  if (m_player)
    m_player->stop();
  m_loaded = false;
  m_pausedForMinimize = false;
  emit changed();
}
void VideoView::play() {
  if (!m_player || !m_active)
    return;
  m_stopping = false;
  m_state.completed = false;
  if (m_state.ab && (position() < m_state.a || position() >= m_state.b))
    seek(m_state.a, false);
  m_player->play();
}
void VideoView::stop() {
  if (!m_player)
    return;
  save();
  m_stopping = true;
  m_fieldTimer->stop();
  m_seekTarget = -1;
  m_player->stop();
  updateControls();
  emit changed();
}
void VideoView::seek(qint64 ms, bool manual) {
  if (!m_player || !m_loaded || !m_player->isSeekable())
    return;
  ms = std::max<qint64>(0, ms);
  if (m_player->duration() > 0)
    ms = std::min(ms, m_player->duration());
  if (manual && m_state.ab && (ms < m_state.a || ms >= m_state.b)) {
    if (m_options.abSeekDisables)
      m_state.ab = false;
    else
      ms = m_state.a;
  }
  m_fieldTimer->stop();
  m_stopping = false;
  m_state.completed = false;
  m_seekTarget = ms;
  m_seeking = true;
  m_player->setPosition(ms);
  m_seeking = false;
  if (m_player->playbackState() == QMediaPlayer::StoppedState)
    m_player->pause();
  m_state.position = ms;
  updateControls();
  emit changed();
}
void VideoView::acceptFrame(const QVideoFrame &frame) {
  if (!frame.isValid())
    return; // Retain the final image at EOF.
  if (m_seekTarget >= 0) {
    if (frame.endTime() >= 0 && frame.endTime() / 1000 < m_seekTarget - 500)
      return;
    m_seekTarget = -1;
  }
  ++VideoActivity::frames;
  m_sourceFrame = frame;
  m_fieldTimer->stop();
  redraw();
  if (playing() && (m_state.deinterlace == DeinterlaceMode::Bob ||
                    m_state.deinterlace == DeinterlaceMode::Linear)) {
    qint64 duration = frame.endTime() - frame.startTime();
    if (duration <= 0) {
      const auto rate = frame.streamFrameRate();
      duration = rate > 0 ? qRound64(1000000 / rate) : 40000;
    }
    m_fieldTimer->start(
        std::max(1, qRound(duration / 2000. / m_player->playbackRate())));
  }
  emit framePresented();
}
void VideoView::redraw(int field) {
  if (!m_sourceFrame.isValid())
    return;
  if (field < 0)
    field = m_state.bottomFirst ? 1 : 0;
  m_frame = videoFrameImage(m_sourceFrame, m_state.deinterlace, field);
  emit fieldPresented(m_sourceFrame.startTime(), field);
  update();
}
void VideoView::setAspect(int value) {
  m_state.aspect = std::clamp(value, 0, 3);
  save();
  update();
  emit changed();
}
void VideoView::setDeinterlace(DeinterlaceMode mode) {
  m_fieldTimer->stop();
  m_state.deinterlace = mode;
  redraw();
  save();
  emit changed();
}
void VideoView::setBottomFirst(bool value) {
  m_fieldTimer->stop();
  m_state.bottomFirst = value;
  redraw();
  save();
  emit changed();
}
void VideoView::setRate(double value) {
  if (!std::isfinite(value) || value < m_options.rateMin ||
      value > m_options.rateMax)
    return;
  m_fieldTimer->stop();
  m_state.rate = value;
  if (m_player)
    m_player->setPlaybackRate(value);
  save();
  emit changed();
}
void VideoView::setScale(bool fit, double zoom) {
  m_state.fit = fit;
  m_state.zoom = std::clamp(zoom, .01, 100.);
  save();
  update();
  emit changed();
}
void VideoView::changeVolume(int delta) {
  m_volume->setValue(std::clamp(m_options.volume + delta, 0, 100));
}
void VideoView::applyOutput() {
  if (!m_audio)
    return;
  QAudioDevice selected;
  for (const auto &d : QMediaDevices::audioOutputs())
    if (d.id() == m_options.output) {
      selected = d;
      break;
    }
  m_audio->setDevice(selected);
}
void VideoView::selectTrack(int kind, int index) {
  if (!m_player || !m_loaded)
    return;
  const bool wasPlaying = playing();
  const auto pos = position();
  m_fieldTimer->stop();
  if (kind == 0) {
    m_player->setActiveVideoTrack(index);
    m_state.video = m_player->activeVideoTrack();
  } else if (kind == 1) {
    m_player->setActiveAudioTrack(index);
    m_state.audio = m_player->activeAudioTrack();
  } else {
    m_player->setActiveSubtitleTrack(index);
    m_state.subtitle = m_player->activeSubtitleTrack();
  }
  if (m_player->isSeekable())
    seek(pos, false);
  if (wasPlaying)
    m_player->play();
  save();
  emit changed();
}
void VideoView::cycleTrack(int kind) {
  if (!m_player)
    return;
  const auto count = kind == 0   ? m_player->videoTracks().size()
                     : kind == 1 ? m_player->audioTracks().size()
                                 : m_player->subtitleTracks().size();
  if (!count)
    return;
  const int current = kind == 0   ? m_player->activeVideoTrack()
                      : kind == 1 ? m_player->activeAudioTrack()
                                  : m_player->activeSubtitleTrack();
  selectTrack(kind, kind == 2 ? (current + 2) % (count + 1) - 1
                              : (current + 1) % count);
}
void VideoView::command(const QString &id) {
  if (id == "videoPlayPause") {
    m_pausedForMinimize = false;
    if (playing()) {
      m_player->pause();
      save();
    } else
      play();
  } else if (id == "videoStop")
    stop();
  else if (id == "videoRestart") {
    seek(m_state.ab ? m_state.a : 0, false);
    play();
  } else if (id == "videoNext")
    seek(position() + qint64(m_options.seek) * 1000);
  else if (id == "videoPrevious")
    seek(position() - qint64(m_options.seek) * 1000);
  else if (id == "videoNextOne")
    seek(position() + qint64(m_options.micro) * 1000);
  else if (id == "videoPreviousOne")
    seek(position() - qint64(m_options.micro) * 1000);
  else if (id == "videoLongNext")
    seek(position() + qint64(m_options.longSeek) * 1000);
  else if (id == "videoLongPrevious")
    seek(position() - qint64(m_options.longSeek) * 1000);
  else if (id == "videoFirst")
    seek(0);
  else if (id == "videoLast" && m_player && m_player->duration() > 0)
    seek(m_player->duration());
  else if (id == "videoVolumeUp")
    changeVolume(m_options.volumeStep);
  else if (id == "videoVolumeDown")
    changeVolume(-m_options.volumeStep);
  else if (id == "videoMute") {
    m_muted = !m_muted;
    m_settings.setValue("video/muted", m_muted);
    if (m_audio)
      m_audio->setMuted(m_muted);
  } else if (id == "videoRateReset")
    setRate(1);
  else if (id == "videoRateUp") {
    for (double v : m_options.rates)
      if (v > m_state.rate + .00001) {
        setRate(v);
        break;
      }
  } else if (id == "videoRateDown") {
    for (auto i = m_options.rates.crbegin(); i != m_options.rates.crend(); ++i)
      if (*i < m_state.rate - .00001) {
        setRate(*i);
        break;
      }
  } else if (id == "videoVideoTrack")
    cycleTrack(0);
  else if (id == "videoAudioTrack")
    cycleTrack(1);
  else if (id == "videoSubtitle")
    cycleTrack(2);
  else if (id == "videoAspect")
    setAspect((m_state.aspect + 1) % 4);
  else if (id == "videoDeinterlace")
    setDeinterlace(DeinterlaceMode((int(m_state.deinterlace) + 1) % 4));
  else if (id == "videoFieldOrder")
    setBottomFirst(!m_state.bottomFirst);
  else if (id == "videoSetA") {
    m_state.a = position();
    if (m_state.b <= m_state.a)
      m_state.ab = false;
  } else if (id == "videoSetB") {
    m_state.b = position();
    if (m_state.a < 0 || m_state.b <= m_state.a)
      m_state.ab = false;
  } else if (id == "videoAB") {
    if (m_player && m_player->isSeekable() && m_state.a >= 0 &&
        m_state.b > m_state.a) {
      m_state.ab = !m_state.ab;
      if (m_state.ab && !m_stopping)
        seek(position() < m_state.a || position() >= m_state.b ? m_state.a
                                                               : position(),
             false);
    }
  } else if (id == "videoClearAB") {
    m_state.a = m_state.b = -1;
    m_state.ab = false;
  } else if (id == "videoCover")
    emit coverRequested();
  else if (id == "videoGoTime" && m_player && m_player->isSeekable()) {
    bool ok = false;
    const double seconds = QInputDialog::getDouble(
        this, tr("時刻を指定"), tr("先頭からの秒数"), position() / 1000., 0,
        m_player->duration() > 0 ? m_player->duration() / 1000. : 864000, 3,
        &ok);
    if (ok)
      seek(qRound64(seconds * 1000));
  }
  save();
  updateControls();
  update();
  emit changed();
}
void VideoView::setPresentation(bool fullscreen, bool minimized) {
  m_fullscreen = fullscreen;
  if (minimized != m_minimized) {
    m_minimized = minimized;
    if (m_options.pauseMinimized && minimized && playing()) {
      m_pausedForMinimize = true;
      m_player->pause();
      save();
    } else if (!minimized && m_pausedForMinimize) {
      m_pausedForMinimize = false;
      if (m_active)
        play();
    }
  }
  updateBar();
  emit changed();
}
void VideoView::updateControls() {
  const auto duration = m_player ? m_player->duration() : 0;
  m_playButton->setText(playing() ? tr("一時停止") : tr("再生"));
  m_playButton->setEnabled(m_active && m_loaded);
  findChild<QToolButton *>("videoStopButton")->setEnabled(m_active && m_loaded);
  m_time->setText(clockText(position()) + " / " +
                  (duration > 0 ? clockText(duration) : tr("不明")));
  m_seek->setEnabled(m_loaded && m_player && m_player->isSeekable() &&
                     duration > 0);
  if (!m_seek->isSliderDown()) {
    QSignalBlocker b(m_seek);
    m_seek->setValue(duration > 0 ? int(position() * 10000 / duration) : 0);
  }
  m_volume->setToolTip(tr("音量 %1% %2")
                           .arg(m_options.volume)
                           .arg(m_muted ? tr("（消音）") : QString{}));
}
void VideoView::updateBar() {
  const int mode = m_fullscreen ? m_options.fullscreenBar : m_options.bar;
  m_hideTimer->stop();
  m_bar->setVisible(mode != 2);
  if (mode == 0)
    m_hideTimer->start(m_options.hideMs);
}
void VideoView::revealControls() {
  if ((m_fullscreen ? m_options.fullscreenBar : m_options.bar) != 2)
    m_bar->show();
  if ((m_fullscreen ? m_options.fullscreenBar : m_options.bar) == 0)
    m_hideTimer->start(m_options.hideMs);
}
QRectF VideoView::imageRect() const {
  return videoRectangle(m_frame.size(), size(), m_state.aspect, m_state.fit,
                        m_state.zoom);
}
QImage VideoView::coverImage() const {
  if (m_frame.isNull())
    return {};
  QImage image(size().expandedTo(QSize(1, 1)), QImage::Format_RGB32);
  image.fill(Qt::black);
  QPainter painter(&image);
  painter.setRenderHint(QPainter::SmoothPixmapTransform);
  painter.drawImage(imageRect(), m_frame);
  return image;
}
void VideoView::rememberCover() {
  if (!m_player || m_frame.isNull())
    return;
  m_state.coverChosen = true;
  m_state.coverTime = m_sourceFrame.startTime() >= 0
                          ? m_sourceFrame.startTime() / 1000
                          : position();
  m_state.coverTrack = m_player->activeVideoTrack();
  save();
}
void VideoView::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.fillRect(rect(), Qt::black);
  if (!m_frame.isNull()) {
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(imageRect(), m_frame);
  }
  if (!m_error.isEmpty()) {
    p.setPen(Qt::white);
    p.drawText(rect().adjusted(20, 20, -20, -60),
               Qt::AlignCenter | Qt::TextWordWrap, m_error);
  }
  if (m_sink && !m_sink->subtitleText().isEmpty()) {
    auto font = p.font();
    font.setPixelSize(std::max(16, height() / 24));
    p.setFont(font);
    const QRect area = rect().adjusted(20, 20, -20, -70);
    p.setPen(Qt::black);
    p.drawText(area.translated(2, 2),
               Qt::AlignBottom | Qt::AlignHCenter | Qt::TextWordWrap,
               m_sink->subtitleText());
    p.setPen(Qt::white);
    p.drawText(area, Qt::AlignBottom | Qt::AlignHCenter | Qt::TextWordWrap,
               m_sink->subtitleText());
  }
  p.end();
  if (m_active && !m_frame.isNull() && !m_painted) {
    m_painted = true;
    emit painted();
  }
}
void VideoView::resizeEvent(QResizeEvent *) {
  const int h = m_bar->sizeHint().height();
  m_bar->setGeometry(0, height() - h, width(), h);
}
void VideoView::mouseMoveEvent(QMouseEvent *) { revealControls(); }
void VideoView::mouseDoubleClickEvent(QMouseEvent *) {
  command("videoPlayPause");
}
void VideoView::hideEvent(QHideEvent *) { m_hideTimer->stop(); }
void VideoView::populateMenu(QMenu *menu,
                             const QHash<QString, QAction *> &actions) {
  const auto add = [&](QMenu *m, const QStringList &ids) {
    for (const auto &id : ids)
      if (actions.contains(id))
        m->addAction(actions[id]);
  };
  add(menu, {"videoPlayPause", "videoStop", "videoRestart"});
  auto *seeks = menu->addMenu(tr("シーク"));
  add(seeks, {"videoGoTime", "videoPrevious", "videoNext", "videoPreviousOne",
              "videoNextOne", "videoLongPrevious", "videoLongNext",
              "videoFirst", "videoLast"});
  auto *rate = menu->addMenu(tr("再生速度"));
  rate->setObjectName("videoRateMenu");
  auto *rateGroup = new QActionGroup(rate);
  add(rate, {"videoRateDown", "videoRateUp", "videoRateReset"});
  connect(rate, &QMenu::aboutToShow, this, [this, rate, rateGroup, actions] {
    rate->clear();
    auto *group = rateGroup;
    for (double value : m_options.rates) {
      auto *a = rate->addAction(tr("%1倍").arg(value));
      a->setCheckable(true);
      a->setChecked(qAbs(value - m_state.rate) < .00001);
      group->addAction(a);
      connect(a, &QAction::triggered, this, [this, value] { setRate(value); });
    }
    rate->addSeparator();
    for (const auto &id : {"videoRateDown", "videoRateUp", "videoRateReset"})
      rate->addAction(actions.value(id));
  });
  auto *ab = menu->addMenu(tr("A-B反復"));
  add(ab, {"videoSetA", "videoSetB", "videoAB", "videoClearAB"});
  for (int kind = 0; kind < 3; ++kind) {
    auto *tracks = menu->addMenu(kind == 0   ? tr("映像トラック")
                                 : kind == 1 ? tr("音声トラック")
                                             : tr("字幕"));
    tracks->setObjectName(QString("videoTrackMenu%1").arg(kind));
    auto *trackGroup = new QActionGroup(tracks);
    const QString cycle = kind == 0   ? "videoVideoTrack"
                          : kind == 1 ? "videoAudioTrack"
                                      : "videoSubtitle";
    add(tracks, {cycle});
    connect(tracks, &QMenu::aboutToShow, this,
            [this, tracks, kind, trackGroup, actions, cycle] {
              tracks->clear();
              tracks->addAction(actions.value(cycle));
              tracks->addSeparator();
              if (!m_player)
                return;
              auto *group = trackGroup;
              const auto list = kind == 0   ? m_player->videoTracks()
                                : kind == 1 ? m_player->audioTracks()
                                            : m_player->subtitleTracks();
              const int current = kind == 0   ? m_player->activeVideoTrack()
                                  : kind == 1 ? m_player->activeAudioTrack()
                                              : m_player->activeSubtitleTrack();
              for (int i = kind == 2 ? -1 : 0; i < list.size(); ++i) {
                auto *a = tracks->addAction(i < 0 ? tr("非表示")
                                                  : trackLabel(list[i], i));
                a->setCheckable(true);
                a->setChecked(i == current);
                group->addAction(a);
                connect(a, &QAction::triggered, this,
                        [this, kind, i] { selectTrack(kind, i); });
              }
            });
  }
  add(menu, {"videoVolumeUp", "videoVolumeDown", "videoMute"});
  auto *outputs = menu->addMenu(tr("音声出力先"));
  auto *outputGroup = new QActionGroup(outputs);
  connect(outputs, &QMenu::aboutToShow, this, [this, outputs, outputGroup] {
    outputs->clear();
    auto *group = outputGroup;
    const auto devices = QMediaDevices::audioOutputs();
    auto *def = outputs->addAction(tr("システム既定"));
    def->setCheckable(true);
    const bool available =
        std::any_of(devices.cbegin(), devices.cend(), [this](const auto &d) {
          return d.id() == m_options.output;
        });
    def->setChecked(m_options.output.isEmpty() || !available);
    group->addAction(def);
    connect(def, &QAction::triggered, this, [this] {
      m_options.output = {};
      m_settings.setValue("video/output", QByteArray{});
      applyOutput();
    });
    for (const auto &d : devices) {
      auto *a = outputs->addAction(d.description());
      a->setCheckable(true);
      a->setChecked(d.id() == m_options.output);
      group->addAction(a);
      connect(a, &QAction::triggered, this, [this, d] {
        m_options.output = d.id();
        m_settings.setValue("video/output", d.id());
        applyOutput();
      });
    }
  });
  auto *aspect = menu->addMenu(tr("アスペクト比"));
  const QStringList aspects{tr("動画内指定"), "1:1", "4:3", "16:9"};
  auto *aspectGroup = new QActionGroup(aspect);
  for (int i = 0; i < aspects.size(); ++i) {
    auto *a = aspect->addAction(aspects[i]);
    a->setCheckable(true);
    aspectGroup->addAction(a);
    connect(a, &QAction::triggered, this, [this, i] { setAspect(i); });
    connect(aspect, &QMenu::aboutToShow, a,
            [this, a, i] { a->setChecked(m_state.aspect == i); });
  }
  add(aspect, {"videoAspect"});
  auto *deint = menu->addMenu(tr("デインタレース"));
  auto *dg = new QActionGroup(deint);
  const QStringList names{tr("無効"), "Blend", "Bob", "Linear"};
  for (int i = 0; i < names.size(); ++i) {
    auto *a = deint->addAction(names[i]);
    a->setCheckable(true);
    dg->addAction(a);
    connect(a, &QAction::triggered, this,
            [this, i] { setDeinterlace(DeinterlaceMode(i)); });
    connect(deint, &QMenu::aboutToShow, a,
            [this, a, i] { a->setChecked(int(m_state.deinterlace) == i); });
  }
  deint->addSeparator();
  auto *fg = new QActionGroup(deint);
  for (int i = 0; i < 2; ++i) {
    auto *a = deint->addAction(i ? tr("下フィールド先（BFF）")
                                 : tr("上フィールド先（TFF）"));
    a->setCheckable(true);
    fg->addAction(a);
    connect(a, &QAction::triggered, this, [this, i] { setBottomFirst(i); });
    connect(deint, &QMenu::aboutToShow, a,
            [this, a, i] { a->setChecked(m_state.bottomFirst == bool(i)); });
  }
  add(deint, {"videoDeinterlace", "videoFieldOrder"});
  menu->addSeparator();
  add(menu,
      {"videoCover", "videoRefreshCover", "videoSettings", "videoPreferences"});
}
