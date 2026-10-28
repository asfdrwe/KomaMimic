// SPDX-License-Identifier: GPL-3.0-only
#include "idleinhibitor.h"
#include <QCoreApplication>
#include <QPointer>
#include <utility>

bool IdleInhibitContext::wanted() const {
  return enabled && hasBook && reading && visible && !minimized && foreground &&
         !closing;
}
void IdleInhibitRequest::cancel() {
  cancelled = true;
  if (auto action = std::exchange(cancelAction, {}))
    action();
}
IdleInhibitor::IdleInhibitor(std::shared_ptr<IdleInhibitBackend> backend,
                             QObject *parent)
    : QObject(parent), m_backend(std::move(backend)) {
  m_retry.setSingleShot(true);
  m_retry.setInterval(5000);
  connect(&m_retry, &QTimer::timeout, this, &IdleInhibitor::acquire);
}
IdleInhibitor::~IdleInhibitor() {
  ++m_generation;
  m_retry.stop();
  if (m_pending)
    m_pending->cancel();
  m_lease.reset();
}
IdleInhibitor *IdleInhibitor::instance() {
  static QPointer<IdleInhibitor> singleton;
  if (!singleton)
    singleton = new IdleInhibitor(createIdleInhibitBackend(),
                                  QCoreApplication::instance());
  return singleton;
}
void IdleInhibitor::setState(State state, const QString &detail) {
  if (m_state == state &&
      (state == State::Active ? m_backendName : m_error) == detail)
    return;
  m_state = state;
  m_error = state == State::Failed ? detail : QString{};
  m_backendName = state == State::Active ? detail : QString{};
  emit statusChanged();
}
QString IdleInhibitor::description() const {
  switch (m_state) {
  case State::Inactive:
    return QCoreApplication::translate("KomaMimic", "抑制していません（前面で読書画面を表示したときに有効）");
  case State::Acquiring:
    return QCoreApplication::translate("KomaMimic", "OSに自動消灯の抑制を要求しています…");
  case State::Active:
    return QCoreApplication::translate("KomaMimic", "自動消灯を抑制中：") + m_backendName;
  case State::Failed:
    return QCoreApplication::translate("KomaMimic", "自動消灯を抑制できません：") + m_error;
  }
  return {};
}
void IdleInhibitor::setRequested(QObject *owner, bool wanted) {
  if (!owner)
    return;
  if (wanted) {
    if (m_owners.contains(owner))
      return;
    if (!m_observed.contains(owner)) {
      m_observed.insert(owner);
      connect(owner, &QObject::destroyed, this, [this, owner] {
        m_observed.remove(owner);
        m_owners.remove(owner);
        if (m_owners.isEmpty())
          deactivate();
      });
    }
    const bool first = m_owners.isEmpty();
    m_owners.insert(owner);
    if (first)
      acquire();
  } else {
    if (!m_owners.remove(owner))
      return;
    if (m_owners.isEmpty())
      deactivate();
  }
}
void IdleInhibitor::deactivate() {
  ++m_generation;
  m_retry.stop();
  if (m_pending)
    m_pending->cancel();
  m_pending.reset();
  m_lease.reset();
  setState(State::Inactive);
}
void IdleInhibitor::acquire() {
  if (m_owners.isEmpty() || m_lease || m_pending)
    return;
  m_retry.stop();
  const auto generation = ++m_generation;
  auto request = std::make_shared<IdleInhibitRequest>();
  m_pending = request;
  QPointer<IdleInhibitor> guard(this);
  setState(State::Acquiring);
  if (!guard || generation != guard->m_generation || guard->m_owners.isEmpty())
    return;
  const auto backend = m_backend;
  backend->acquire(request, [guard, generation](
                                std::unique_ptr<IdleInhibitLease> lease,
                                QString error) {
    // Destroying a stale lease releases the native request even if the window
    // withdrew its intent, or the manager was destroyed, while acquiring it.
    if (!guard || generation != guard->m_generation ||
        guard->m_owners.isEmpty())
      return;
    auto *self = guard.data();
    self->m_pending.reset();
    if (!lease) {
      self->setState(State::Failed,
                     error.isEmpty() ? QCoreApplication::translate("KomaMimic", "OSの抑制APIを利用できません。")
                                     : error);
      if (guard && generation == self->m_generation &&
          !self->m_owners.isEmpty())
        self->m_retry.start();
      return;
    }
    lease->lost = [guard, generation] {
      if (!guard)
        return;
      QMetaObject::invokeMethod(
          guard,
          [guard, generation] {
            if (!guard || generation != guard->m_generation)
              return;
            guard->m_lease.reset();
            guard->acquire();
          },
          Qt::QueuedConnection);
    };
    const auto name = lease->name();
    self->m_lease = std::move(lease);
    self->setState(State::Active, name);
  });
}
