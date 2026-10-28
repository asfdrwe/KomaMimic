// SPDX-License-Identifier: GPL-3.0-only
#include "idleinhibitor.h"
#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QWidget>
#include <windows.h>

namespace {
class WindowsLease : public IdleInhibitLease,
                     public QAbstractNativeEventFilter {
public:
  explicit WindowsLease(EXECUTION_STATE previous) : m_previous(previous) {
    QCoreApplication::instance()->installNativeEventFilter(this);
  }
  ~WindowsLease() override {
    QCoreApplication::instance()->removeNativeEventFilter(this);
    // Execution state is thread-local. Acquire and release stay on the Qt GUI
    // thread, and preserve any requirements that preceded our request.
    SetThreadExecutionState(m_previous | ES_CONTINUOUS);
  }
  QString name() const override { return "Windows"; }
  bool nativeEventFilter(const QByteArray &type, void *message,
                         qintptr *result) override {
    if (type != "windows_generic_MSG" && type != "windows_dispatcher_MSG")
      return false;
    const auto *msg = static_cast<const MSG *>(message);
    // Display execution state alone does not inhibit a screen saver. Only
    // intercept our foreground window's ordinary SC_SCREENSAVE notification;
    // secure savers, manual locking and OS security policy remain untouched.
    if (msg->message != WM_SYSCOMMAND ||
        (msg->wParam & 0xfff0) != SC_SCREENSAVE || (msg->wParam & 1))
      return false;
    const auto *widget = QWidget::find(WId(msg->hwnd));
    if (!widget || !widget->window()->isActiveWindow() ||
        !widget->window()->isVisible() || widget->window()->isMinimized())
      return false;
    if (result)
      *result = 0;
    return true;
  }

private:
  EXECUTION_STATE m_previous;
};
class Backend : public IdleInhibitBackend {
public:
  void acquire(std::shared_ptr<IdleInhibitRequest> request,
               Completion completion) override {
    if (request->cancelled)
      return;
    const auto previous =
        SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED);
    if (!previous) {
      completion({}, QCoreApplication::translate("KomaMimic", "Windowsの画面消灯抑制要求に失敗しました。"));
      return;
    }
    completion(std::make_unique<WindowsLease>(previous), {});
  }
};
} // namespace
std::shared_ptr<IdleInhibitBackend> createIdleInhibitBackend() {
  return std::make_shared<Backend>();
}
