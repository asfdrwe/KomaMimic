// SPDX-License-Identifier: GPL-3.0-only
#include "readerinput.h"
#include <QCoreApplication>
#include "mainwindow.h"
#include "pageview.h"
#include "shortcuts.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QSpinBox>
#include <QStatusBar>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <SDL.h>
#include <algorithm>
namespace {
QString buttonName(Qt::MouseButton button) {
  if (button == Qt::LeftButton)
    return "left";
  if (button == Qt::RightButton)
    return "right";
  if (button == Qt::MiddleButton)
    return "middle";
  if (button == Qt::BackButton)
    return "back";
  if (button == Qt::ForwardButton)
    return "forward";
  return {};
}
const QHash<QString, QString> mouseDefaults{{"left", "next"}, {"right", "previous"},
    {"middle", "loupe"}, {"back", "nextBook"}, {"forward", "previousBook"},
    {"wheelUp", "wheelScrollUp"}, {"wheelDown", "wheelScrollDown"},
    {"rightWheelUp", "zoomIn"}, {"rightWheelDown", "zoomOut"},
    {"doubleleft", "slideForward"}, {"doubleright", "slideReverse"},
    {"rightLeft", "nextOne"}, {"leftRight", "previousOne"},
    {"rightMiddle", "scaleEqualHeightToggle"},
    {"gesture/U", "fullscreenGlobal"}, {"gesture/D", "scaleFitToggle"},
    {"gesture/LU", "parentFolder"}, {"gesture/RD", "library"},
    {"gesture/R", "thumbCustom"}, {"gesture/DR", "tray"},
    {"gesture/RR", "nextBook"}, {"gesture/LL", "previousBook"},
    {"gesture/UD", "autoSpread"}};
const QHash<QString, QString> padDefaults{{"a", "contextAccept"},
                                          {"b", "parentFolder"},
                                          {"x", "spread"},
                                          {"y", "thumbnails"},
                                          {"leftshoulder", "previousBook"},
                                          {"rightshoulder", "nextBook"},
                                          {"start", "fullscreenGlobal"},
                                          {"back", "bookLists"},
                                          {"dpup", "scrollUp"},
                                          {"dpdown", "scrollDown"},
                                          {"dpleft", "previous"},
                                          {"dpright", "next"},
                                          {"leftx+", "next"},
                                          {"leftx-", "previous"},
                                          {"lefty+", "scrollDown"},
                                          {"lefty-", "scrollUp"}};
} // namespace
ReaderInput::ReaderInput(MainWindow *window, QObject *parent)
    : QObject(parent), w(window) {
  // Required when the host application supplies main() instead of SDL_main.
  SDL_SetMainReady();
  m_hold = new QTimer(this);
  m_hold->setSingleShot(true);
  m_click = new QTimer(this);
  m_click->setSingleShot(true);
  m_padTimer = new QTimer(this);
  m_padTimer->setInterval(30);
  connect(m_hold, &QTimer::timeout, this, [this] {
    if (m_drag || m_button == Qt::NoButton)
      return;
    QString key = "hold" + buttonName(m_button);
    if (!mapping(key).isEmpty()) {
      dispatch(mapping(key));
      m_consumed = true;
      w->m_view->cancelPointer();
    }
  });
  connect(m_padTimer, &QTimer::timeout, this, &ReaderInput::pollGamepad);
  m_pollClock.start();
  m_repeat.start();
  m_padTimer->start();
  w->m_view->installEventFilter(this);
}
ReaderInput::~ReaderInput() {
  if (m_pad)
    SDL_GameControllerClose(m_pad);
  if (m_audioDevice)
    SDL_CloseAudioDevice(m_audioDevice);
  if (m_gamepadStarted)
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
  if (m_audioStarted)
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}
QString ReaderInput::mapping(const QString &key, bool gamepad) const {
  const auto defaults = gamepad ? padDefaults : mouseDefaults;
  return w->m_settings
      ->value((gamepad ? "input/pad/" : "input/mouse/") + key,
              defaults.value(key))
      .toString();
}
void ReaderInput::dispatch(const QString &id) {
  if (id == "contextAccept") {
    dispatch(w->viewMode() == MainWindow::ViewMode::Reader ? "next"
                                                           : "thumbAccept");
    return;
  }
  auto *action = w->command(id);
  if (!action || !action->isEnabled())
    return;
  for (const auto &spec : shortcutSpecs())
    if (spec.id == id) {
      if (spec.scope == ShortcutScope::Reader &&
          w->viewMode() != MainWindow::ViewMode::Reader)
        return;
      if (spec.scope == ShortcutScope::Thumbnails &&
          w->viewMode() == MainWindow::ViewMode::Reader)
        return;
      break;
    }
  action->trigger();
}
bool ReaderInput::eventFilter(QObject *object, QEvent *event) {
  if (object != w->m_view)
    return QObject::eventFilter(object, event);
  if (event->type() == QEvent::Wheel) {
    auto *wheel = static_cast<QWheelEvent *>(event);
    if (wheel->modifiers() & Qt::ControlModifier)
      return false;
    m_wheel += wheel->angleDelta().y();
    if (std::abs(m_wheel) >= 120) {
      const bool right = wheel->buttons() & Qt::RightButton;
      dispatch(mapping(right ? (m_wheel > 0 ? "rightWheelUp" : "rightWheelDown")
                             : (m_wheel > 0 ? "wheelUp" : "wheelDown")));
      if (right) {
        m_consumed = true;
        m_hold->stop();
        w->m_view->cancelPointer();
      }
      m_wheel %= 120;
    }
    wheel->accept();
    return true;
  }
  if (event->type() == QEvent::MouseButtonDblClick) {
    auto *mouse=static_cast<QMouseEvent *>(event);
    const int interval=w->m_settings->value("input/doubleClickMs",QApplication::doubleClickInterval()).toInt();
    if(m_lastClickClock.isValid() && m_lastClickButton==mouse->button() && m_lastClickClock.elapsed()>interval) {
      // Qt's system interval can be longer than this profile's interval.
      // Treat the second press as an ordinary click in that case.
      m_button=mouse->button();m_press=mouse->position();m_drag=m_consumed=false;
      m_gesture.clear();return false;
    }
    m_click->stop();m_hold->stop();
    dispatch(mapping("double"+buttonName(mouse->button())));
    m_lastClickClock.invalidate();m_consumed=true;w->m_view->cancelPointer();return true;
  }
  if (event->type() == QEvent::MouseButtonPress) {
    auto *mouse = static_cast<QMouseEvent *>(event);
    m_click->stop();
    m_button = mouse->button();
    const auto doubleAction=mapping("double"+buttonName(m_button));
    if(mouse->buttons()==m_button && !doubleAction.isEmpty() && m_lastClickButton==m_button
        && m_lastClickClock.isValid() && m_lastClickClock.elapsed()<=w->m_settings->value("input/doubleClickMs",QApplication::doubleClickInterval()).toInt()) {
      m_hold->stop();m_lastClickClock.invalidate();m_consumed=true;dispatch(doubleAction);w->m_view->cancelPointer();return true;
    }
    m_press = m_lastGesture = mouse->position();
    m_drag = m_consumed = false;
    m_gesture.clear();m_gestureClock.start();
    if(mouse->button()==Qt::MiddleButton && mouse->buttons().testFlag(Qt::RightButton)) {
      dispatch(mapping("rightMiddle"));m_consumed=true;m_hold->stop();w->m_view->cancelPointer();return true;
    }
    if ((mouse->buttons() & (Qt::LeftButton | Qt::RightButton)) ==
        (Qt::LeftButton | Qt::RightButton)) {
      dispatch(mapping(mouse->button() == Qt::LeftButton ? "rightLeft"
                                                         : "leftRight"));
      m_consumed = true;
      w->m_view->cancelPointer();
      return true;
    }
    if (!mapping("hold" + buttonName(m_button)).isEmpty())
      m_hold->start(w->m_settings->value("input/holdMs", 600).toInt());
    return false;
  }
  if (event->type() == QEvent::MouseMove && m_button != Qt::NoButton) {
    auto *mouse = static_cast<QMouseEvent *>(event);
    if ((mouse->position() - m_press).manhattanLength() >
        QApplication::startDragDistance()) {
      m_drag = true;
      m_hold->stop();
    }
    if (m_button == Qt::RightButton &&
        w->m_settings->value("input/gestures", true).toBool() &&
        (mouse->position() - m_lastGesture).manhattanLength() > w->m_settings->value("input/gestureDistance",20).toInt()) {
      const auto delta = mouse->position() - m_lastGesture;
      const QChar direction = std::abs(delta.x()) > std::abs(delta.y())
                                  ? (delta.x() > 0 ? 'R' : 'L')
                                  : (delta.y() > 0 ? 'D' : 'U');
      if (m_gesture.isEmpty() || m_gesture.back() != direction || m_gestureClock.elapsed()>w->m_settings->value("input/gestureSplitMs",200).toInt())
        m_gesture += direction;
      m_gestureClock.restart();
      m_lastGesture = mouse->position();
    }
    return m_consumed;
  }
  if (event->type() == QEvent::MouseButtonRelease) {
    auto *mouse = static_cast<QMouseEvent *>(event);
    m_hold->stop();
    const auto button = m_button;
    m_button = Qt::NoButton;
    if (m_consumed) {
      w->m_view->cancelPointer();
      return true;
    }
    if (!m_gesture.isEmpty()) {
      dispatch(mapping("gesture/" + m_gesture));
      w->m_view->cancelPointer();
      return true;
    }
    if (m_drag)
      return false;
    QString key = buttonName(mouse->button());
    if (w->m_settings->value("input/swapButtons", false).toBool()) {
      if (key == "left")
        key = "right";
      else if (key == "right")
        key = "left";
    }
    m_lastClickButton=mouse->button();m_lastClickClock.restart();
    const auto action = mapping(key);
    w->m_view->cancelPointer();
    if (w->m_settings->value("input/delayDoubleClick", false).toBool() &&
        button == Qt::LeftButton) {
      disconnect(m_click, nullptr, this, nullptr);
      const auto generation = w->m_bookGeneration;
      connect(m_click, &QTimer::timeout, this, [this, action, generation] {
        if (generation == w->m_bookGeneration)
          dispatch(action);
      });
      m_click->start(w->m_settings->value("input/doubleClickMs",QApplication::doubleClickInterval()).toInt());
    } else
      dispatch(action);
    return true;
  }
  return false;
}
void ReaderInput::pollGamepad() {
  if (!w->m_settings->value("input/gamepad", false).toBool()) {
    if (m_pad) {
      SDL_GameControllerClose(m_pad);
      m_pad = nullptr;
    }
    return;
  }
  if (!m_gamepadStarted) {
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
      return;
    m_gamepadStarted = true;
  }
  SDL_GameControllerUpdate();
  if (m_pad && !SDL_GameControllerGetAttached(m_pad)) {
    SDL_GameControllerClose(m_pad);
    m_pad = nullptr;
  }
  if (!m_pad) {
    const int wanted = w->m_settings->value("input/device", 0).toInt();
    if (wanted >= 0 && wanted < SDL_NumJoysticks() &&
        SDL_IsGameController(wanted))
      m_pad = SDL_GameControllerOpen(wanted);
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
      if (!m_pad && SDL_IsGameController(i)) {
        m_pad = SDL_GameControllerOpen(i);
        if (m_pad)
          break;
      }
    if (!m_pad)
      return;
    m_previousButtons = 0;
  }
  if (QApplication::activeModalWidget() || !w->isActiveWindow())
    return;
  quint32 buttons = 0;
  for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX && i < 32; ++i)
    if (SDL_GameControllerGetButton(m_pad, SDL_GameControllerButton(i)))
      buttons |= quint32(1) << i;
  const quint32 pressed = buttons & ~m_previousButtons;
  m_previousButtons = buttons;
  for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX && i < 32; ++i)
    if (pressed & (quint32(1) << i)) {
      const char *name =
          SDL_GameControllerGetStringForButton(SDL_GameControllerButton(i));
      if (name)
        dispatch(mapping(QString::fromLatin1(name), true));
    }
  const int threshold =
      std::clamp(w->m_settings->value("input/deadZone", 25).toInt(), 5, 90) *
      32767 / 100;
  const int x = SDL_GameControllerGetAxis(m_pad, SDL_CONTROLLER_AXIS_LEFTX),
            y = SDL_GameControllerGetAxis(m_pad, SDL_CONTROLLER_AXIS_LEFTY);
  const int dx = std::abs(x) > threshold ? (x > 0 ? 1 : -1) : 0,
            dy = std::abs(y) > threshold ? (y > 0 ? 1 : -1) : 0;
  if ((dx || dy) &&
      (dx != m_axisX || dy != m_axisY ||
       m_repeat.elapsed() >
           w->m_settings->value("input/padRepeat", 250).toInt())) {
    m_repeat.restart();
    if (dx)
      dispatch(mapping(dx > 0 ? "leftx+" : "leftx-", true));
    if (dy)
      dispatch(mapping(dy > 0 ? "lefty+" : "lefty-", true));
  }
  m_axisX = dx;
  m_axisY = dy;
  const int rx = SDL_GameControllerGetAxis(m_pad, SDL_CONTROLLER_AXIS_RIGHTX),
            ry = SDL_GameControllerGetAxis(m_pad, SDL_CONTROLLER_AXIS_RIGHTY);
  if (std::abs(rx) > threshold || std::abs(ry) > threshold)
    w->m_view->scrollBy({std::abs(rx) > threshold ? -rx / 32767.0 * 12 : 0,
                         std::abs(ry) > threshold ? -ry / 32767.0 * 12 : 0});
}
void ReaderInput::configure(bool gamepad) {
  QDialog dialog(w);
  dialog.setObjectName(gamepad ? "gamepadSettings" : "mouseSettings");
  dialog.setWindowTitle(gamepad ? QCoreApplication::translate("KomaMimic", "ゲームパッドの設定")
                                : QCoreApplication::translate("KomaMimic", "マウス・ジェスチャの設定"));
  dialog.resize(700, 650);
  auto *layout = new QVBoxLayout(&dialog);
  auto *form = new QFormLayout;
  layout->addLayout(form);
  auto *enabled = new QCheckBox(&dialog);
  enabled->setChecked(
      w->m_settings
          ->value(gamepad ? "input/gamepad" : "input/gestures", !gamepad)
          .toBool());
  form->addRow(gamepad ? QCoreApplication::translate("KomaMimic", "ゲームパッドを使う") : QCoreApplication::translate("KomaMimic", "右ドラッグのジェスチャを使う"),
               enabled);
  auto *delay = new QCheckBox(&dialog);
  delay->setChecked(
      w->m_settings->value("input/delayDoubleClick", false).toBool());
  if (!gamepad)
    form->addRow(QCoreApplication::translate("KomaMimic", "ダブルクリック判定まで単クリックを待つ"), delay);
  else
    delay->hide();
  auto *swap = new QCheckBox(&dialog);
  swap->setChecked(w->m_settings->value("input/swapButtons", false).toBool());
  if (!gamepad)
    form->addRow(QCoreApplication::translate("KomaMimic", "左右ボタンを交換"), swap);
  else
    swap->hide();
  auto *amount = new QSpinBox(&dialog);
  amount->setRange(gamepad ? 5 : 200, gamepad ? 90 : 3000);
  amount->setValue(w->m_settings
                       ->value(gamepad ? "input/deadZone" : "input/holdMs",
                               gamepad ? 25 : 600)
                       .toInt());
  form->addRow(gamepad ? QCoreApplication::translate("KomaMimic", "デッドゾーン（%）") : QCoreApplication::translate("KomaMimic", "長押し時間（ミリ秒）"), amount);
  auto *repeat = new QSpinBox(&dialog);
  repeat->setRange(50, 2000);
  repeat->setValue(w->m_settings->value("input/padRepeat", 250).toInt());
  if (gamepad)
    form->addRow(QCoreApplication::translate("KomaMimic", "スティックのリピート間隔（ミリ秒）"), repeat);
  else
    repeat->hide();
  auto *doubleInterval=new QSpinBox(&dialog);doubleInterval->setObjectName("input/doubleClickMs");doubleInterval->setRange(100,1000);
  doubleInterval->setValue(w->m_settings->value("input/doubleClickMs",QApplication::doubleClickInterval()).toInt());
  if(!gamepad)form->addRow(QCoreApplication::translate("KomaMimic", "ダブルクリック間隔（ミリ秒）"),doubleInterval);else doubleInterval->hide();
  auto *devices = new QComboBox(&dialog);
  if (gamepad) {
    if (!m_gamepadStarted && SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0)
      m_gamepadStarted = true;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
      const char *name = SDL_GameControllerNameForIndex(i);
      devices->addItem(
          name ? QString::fromUtf8(name) : QCoreApplication::translate("KomaMimic", "デバイス%1").arg(i), i);
    }
    if (devices->count() == 0)
      devices->addItem(QCoreApplication::translate("KomaMimic", "接続されていません"), 0);
    devices->setCurrentIndex(std::max(
        0, devices->findData(w->m_settings->value("input/device", 0))));
    form->addRow(QCoreApplication::translate("KomaMimic", "デバイス"), devices);
  } else
    devices->hide();
  QStringList keys =
      gamepad ? QStringList{"a",
                            "b",
                            "x",
                            "y",
                            "back",
                            "guide",
                            "start",
                            "leftstick",
                            "rightstick",
                            "leftshoulder",
                            "rightshoulder",
                            "dpup",
                            "dpdown",
                            "dpleft",
                            "dpright",
                            "leftx+",
                            "leftx-",
                            "lefty+",
                            "lefty-"}
              : QStringList{"left",           "right",       "middle",
                            "back",           "forward",     "rightWheelUp",
                            "rightWheelDown", "wheelUp",     "wheelDown",
                            "doubleleft",     "doubleright", "doublemiddle",
                            "holdleft",       "holdright",   "holdmiddle",
                            "leftRight",      "rightLeft",   "rightMiddle", "gesture/RR", "gesture/LL", "gesture/UD", "gesture/R",
                            "gesture/L",      "gesture/U",   "gesture/D",
                            "gesture/RU",     "gesture/RD",  "gesture/LU",
                            "gesture/LD",     "gesture/UR",  "gesture/UL",
                            "gesture/DR",     "gesture/DL"};
  auto *table = new QTableWidget(keys.size(), 2, &dialog);
  table->setObjectName("inputBindings");
  table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  table->setHorizontalHeaderLabels({QCoreApplication::translate("KomaMimic", "入力"), QCoreApplication::translate("KomaMimic", "操作")});
  table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  layout->addWidget(table);
  QHash<QString, QComboBox *> editors;
  for (int i = 0; i < keys.size(); ++i) {
    const QHash<QString, QString> labels{
        {"left", QCoreApplication::translate("KomaMimic", "左クリック")},
        {"right", QCoreApplication::translate("KomaMimic", "右クリック")},
        {"middle", QCoreApplication::translate("KomaMimic", "中央クリック")},
        {"back", QCoreApplication::translate("KomaMimic", "戻るボタン")},
        {"forward", QCoreApplication::translate("KomaMimic", "進むボタン")},
        {"wheelUp", QCoreApplication::translate("KomaMimic", "ホイール上")},
        {"wheelDown", QCoreApplication::translate("KomaMimic", "ホイール下")},
        {"rightWheelUp", QCoreApplication::translate("KomaMimic", "右を押しながらホイール上")},
        {"rightWheelDown", QCoreApplication::translate("KomaMimic", "右を押しながらホイール下")},
        {"doubleleft", QCoreApplication::translate("KomaMimic", "左ダブルクリック")},
        {"doubleright", QCoreApplication::translate("KomaMimic", "右ダブルクリック")},
        {"doublemiddle", QCoreApplication::translate("KomaMimic", "中央ダブルクリック")},
        {"holdleft", QCoreApplication::translate("KomaMimic", "左長押し")},
        {"holdright", QCoreApplication::translate("KomaMimic", "右長押し")},
        {"holdmiddle", QCoreApplication::translate("KomaMimic", "中央長押し")},
        {"leftRight", QCoreApplication::translate("KomaMimic", "左を押しながら右")},
        {"rightLeft", QCoreApplication::translate("KomaMimic", "右を押しながら左")},
        {"rightMiddle", QCoreApplication::translate("KomaMimic", "右を押しながら中央")},
        {"leftx+", QCoreApplication::translate("KomaMimic", "左スティック右")},
        {"leftx-", QCoreApplication::translate("KomaMimic", "左スティック左")},
        {"lefty+", QCoreApplication::translate("KomaMimic", "左スティック下")},
        {"lefty-", QCoreApplication::translate("KomaMimic", "左スティック上")},
        {"leftshoulder", QCoreApplication::translate("KomaMimic", "左ショルダー")},
        {"rightshoulder", QCoreApplication::translate("KomaMimic", "右ショルダー")},
        {"leftstick", QCoreApplication::translate("KomaMimic", "左スティック押下")},
        {"rightstick", QCoreApplication::translate("KomaMimic", "右スティック押下")},
        {"dpup", QCoreApplication::translate("KomaMimic", "方向パッド上")},
        {"dpdown", QCoreApplication::translate("KomaMimic", "方向パッド下")},
        {"dpleft", QCoreApplication::translate("KomaMimic", "方向パッド左")},
        {"dpright", QCoreApplication::translate("KomaMimic", "方向パッド右")}};
    auto label = labels.value(keys[i], keys[i].toUpper());
    if (keys[i].startsWith("gesture/")) {
      label = QCoreApplication::translate("KomaMimic", "右ドラッグ ");
      for (const auto c : keys[i].mid(8))
        label += c == 'R' ? "→" : c == 'L' ? "←" : c == 'U' ? "↑" : "↓";
    }
    table->setItem(i, 0, new QTableWidgetItem(label));
    table->item(i,0)->setData(Qt::UserRole,keys[i]);
    auto *choice = new QComboBox(table);
    choice->addItem(QCoreApplication::translate("KomaMimic", "割当なし"), QString{});
    if (gamepad)
      choice->addItem(QCoreApplication::translate("KomaMimic", "現在の画面で決定／次へ"), "contextAccept");
    for (const auto &spec : shortcutSpecs())
      choice->addItem(spec.label, spec.id);
    choice->setCurrentIndex(
        std::max(0, choice->findData(mapping(keys[i], gamepad))));
    table->setCellWidget(i, 1, choice);
    editors[keys[i]] = choice;
  }
  auto *category = new QComboBox(&dialog); category->setObjectName("inputCategory");
  category->addItem(QCoreApplication::translate("KomaMimic", "すべての入力"), "all"); category->addItem(QCoreApplication::translate("KomaMimic", "ボタン"), "buttons");
  category->addItem(gamepad ? QCoreApplication::translate("KomaMimic", "スティック・方向パッド") : QCoreApplication::translate("KomaMimic", "ホイール"), "motion");
  if (!gamepad) category->addItem(QCoreApplication::translate("KomaMimic", "ジェスチャ"), "gestures");
  layout->insertWidget(layout->indexOf(table), category);
  const auto filter = [category, table, keys, gamepad] {
    for (int row = 0; row < keys.size(); ++row) {
      const auto &key = keys[row];
      const auto group = key.startsWith("gesture/") ? QString("gestures") :
        (gamepad ? key.startsWith("leftx") || key.startsWith("lefty") || key.startsWith("dp") : key.contains("wheel", Qt::CaseInsensitive)) ? QString("motion") : QString("buttons");
      table->setRowHidden(row, category->currentData() != "all" && category->currentData() != group);
    }
  };
  connect(category, &QComboBox::currentIndexChanged, &dialog, filter);
  auto *resetVisible = new QPushButton(QCoreApplication::translate("KomaMimic", "表示中の入力を初期値に戻す"), &dialog);
  resetVisible->setObjectName("resetVisibleInputs"); layout->addWidget(resetVisible);
  connect(resetVisible, &QPushButton::clicked, &dialog, [&, gamepad] {
    const auto &defaults = gamepad ? padDefaults : mouseDefaults;
    for (int row = 0; row < keys.size(); ++row) if (!table->isRowHidden(row))
      editors[keys[row]]->setCurrentIndex(std::max(0, editors[keys[row]]->findData(defaults.value(keys[row]))));
  });
  auto *buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  if(!gamepad) {
    auto *reset=buttons->addButton(QCoreApplication::translate("KomaMimic", "マンガ用の初期値に戻す"),QDialogButtonBox::ResetRole);
    reset->setObjectName("resetMouseBindings");
    connect(reset,&QPushButton::clicked,&dialog,[&] {
      for(auto i=editors.cbegin();i!=editors.cend();++i)i.value()->setCurrentIndex(std::max(0,i.value()->findData(mouseDefaults.value(i.key()))));
      doubleInterval->setValue(200);amount->setValue(1000);enabled->setChecked(true);swap->setChecked(false);delay->setChecked(false);
    });
  }
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  if (dialog.exec() != QDialog::Accepted)
    return;
  for (auto it = editors.cbegin(); it != editors.cend(); ++it)
    w->m_settings->setValue((gamepad ? "input/pad/" : "input/mouse/") +
                                it.key(),
                            it.value()->currentData());
  w->m_settings->setValue(gamepad ? "input/gamepad" : "input/gestures",
                          enabled->isChecked());
  w->m_settings->setValue(gamepad ? "input/deadZone" : "input/holdMs",
                          amount->value());
  if (gamepad) {
    w->m_settings->setValue("input/device", devices->currentData());
    w->m_settings->setValue("input/padRepeat", repeat->value());
    if (m_pad) {
      SDL_GameControllerClose(m_pad);
      m_pad = nullptr;
    }
  } else {
    w->m_settings->setValue("input/delayDoubleClick", delay->isChecked());
    w->m_settings->setValue("input/doubleClickMs",doubleInterval->value());
    w->m_settings->setValue("input/swapButtons", swap->isChecked());
  }
  w->m_settings->sync();
}
void ReaderInput::pageTurned() {
  if (!w->m_settings->value("effects/sound", false).toBool())
    return;
  const auto path = w->m_settings->value("effects/soundFile").toString();
  if (path.isEmpty()) {
    QApplication::beep();
    return;
  }
  if (QFileInfo(path).size() > 2 * 1024 * 1024)
    return;
  if (!m_audioStarted) {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
      return;
    m_audioStarted = true;
  }
  SDL_AudioSpec spec;
  Uint8 *buffer = nullptr;
  Uint32 length = 0;
  if (!SDL_LoadWAV(path.toUtf8().constData(), &spec, &buffer, &length))
    return;
  if (m_audioDevice)
    SDL_CloseAudioDevice(m_audioDevice);
  m_audioDevice = SDL_OpenAudioDevice(nullptr, 0, &spec, nullptr, 0);
  if (m_audioDevice) {
    SDL_QueueAudio(m_audioDevice, buffer, length);
    SDL_PauseAudioDevice(m_audioDevice, 0);
  }
  SDL_FreeWAV(buffer);
}
