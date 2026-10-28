// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QElapsedTimer>
#include <QObject>
#include <QPointF>
#include <QString>
class MainWindow;
class QTimer;
struct _SDL_GameController;
class ReaderInput : public QObject {
public:
  explicit ReaderInput(MainWindow *window, QObject *parent);
  ~ReaderInput() override;
  void configure(bool gamepad);
  void pageTurned();

protected:
  bool eventFilter(QObject *object, QEvent *event) override;

private:
  void dispatch(const QString &id);
  QString mapping(const QString &key, bool gamepad = false) const;
  void pollGamepad();
  MainWindow *w;
  QTimer *m_hold, *m_click, *m_padTimer;
  QPointF m_press, m_lastGesture;
  Qt::MouseButton m_button = Qt::NoButton;
  bool m_drag = false, m_consumed = false, m_gamepadStarted = false,
       m_audioStarted = false;
  QString m_gesture;
  int m_wheel = 0, m_axisX = 0, m_axisY = 0;
  _SDL_GameController *m_pad = nullptr;
  quint32 m_previousButtons = 0, m_audioDevice = 0;
  QElapsedTimer m_pollClock, m_repeat, m_gestureClock, m_lastClickClock;
  Qt::MouseButton m_lastClickButton=Qt::NoButton;
};
