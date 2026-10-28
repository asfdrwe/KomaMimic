// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QObject>
#include <QSet>
#include <QTimer>
#include <functional>
#include <memory>

struct IdleInhibitContext {
  bool enabled = false, hasBook = false, reading = false, visible = false,
       minimized = false, foreground = false, closing = false;
  bool wanted() const;
};
struct IdleInhibitRequest {
  bool cancelled = false;
  std::function<void()> cancelAction;
  void cancel();
};
class IdleInhibitLease {
public:
  virtual ~IdleInhibitLease() = default;
  virtual QString name() const = 0;
  std::function<void()> lost;
};
class IdleInhibitBackend {
public:
  using Completion =
      std::function<void(std::unique_ptr<IdleInhibitLease>, QString)>;
  virtual ~IdleInhibitBackend() = default;
  virtual void acquire(std::shared_ptr<IdleInhibitRequest> request,
                       Completion completion) = 0;
};
std::shared_ptr<IdleInhibitBackend> createIdleInhibitBackend();

// One process-wide instance owns the native request; windows only register
// intent. An injectable backend also lets tests exercise asynchronous ownership
// without changing the developer's actual power settings.
class IdleInhibitor : public QObject {
  Q_OBJECT
public:
  enum class State { Inactive, Acquiring, Active, Failed };
  Q_ENUM(State)
  explicit IdleInhibitor(std::shared_ptr<IdleInhibitBackend> backend,
                         QObject *parent = nullptr);
  ~IdleInhibitor() override;
  static IdleInhibitor *instance();
  void setRequested(QObject *owner, bool wanted);
  State state() const { return m_state; }
  QString description() const;
  QString error() const { return m_error; }
  int requestCount() const { return m_owners.size(); }
signals:
  void statusChanged();

private:
  void acquire();
  void deactivate();
  void setState(State state, const QString &detail = {});
  QSet<QObject *> m_owners, m_observed;
  std::shared_ptr<IdleInhibitBackend> m_backend;
  std::shared_ptr<IdleInhibitRequest> m_pending;
  std::unique_ptr<IdleInhibitLease> m_lease;
  QTimer m_retry;
  State m_state = State::Inactive;
  QString m_error, m_backendName;
  quint64 m_generation = 0;
};
