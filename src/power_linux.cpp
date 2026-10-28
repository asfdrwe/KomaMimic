// SPDX-License-Identifier: GPL-3.0-only
#include "idleinhibitor.h"
#include "inhibitprotocol.h"
#include <QCoreApplication>
#include <QPointer>
#include <QTimer>
#include <QUuid>
#include <utility>
#ifdef KOMAMIMIC_DBUS
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#endif

namespace {
#ifdef KOMAMIMIC_DBUS
// The portal's method result is only a request handle. Its Response signal is
// the acknowledgement; subscribing before the call avoids losing an early one.
class PortalPending : public QObject {
  Q_OBJECT
public:
  PortalPending(QDBusConnection connection, InhibitBusCall call,
                std::function<void(InhibitBusReply)> done)
      : QObject(QCoreApplication::instance()), m_connection(connection),
        m_call(std::move(call)), m_done(std::move(done)) {
    auto sender = connection.baseService();
    sender.remove(':');
    sender.replace('.', '_');
    const auto token =
        m_call.arguments[2].toMap().value("handle_token").toString();
    subscribe("/org/freedesktop/portal/desktop/request/" + sender + '/' +
              token);
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(5000);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
      // An accepted request with no acknowledgement may be denied by portal
      // policy. Do not bypass that by falling back to another inhibition API.
      m_reply.error = QCoreApplication::translate("KomaMimic", "Portalの抑制応答がありません。");
      m_reply.denied = true;
      finish(false);
    });
  }
  void start() {
    auto message = QDBusMessage::createMethodCall(
        m_call.service, m_call.path, m_call.interface, m_call.method);
    message.setArguments(m_call.arguments);
    auto *watcher = new QDBusPendingCallWatcher(
        m_connection.asyncCall(message, 1500), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher *watcher) {
              const auto message = watcher->reply();
              m_reply.sender = message.service();
              if (message.type() == QDBusMessage::ErrorMessage) {
                m_reply.error = message.errorMessage();
                finish(false);
                return;
              }
              if (message.arguments().size() != 1 ||
                  message.arguments()[0].metaType() !=
                      QMetaType::fromType<QDBusObjectPath>()) {
                m_reply.error =
                    QCoreApplication::translate("KomaMimic", "Portalから不正なrequest handleを受信しました。");
                finish(false);
                return;
              }
              m_handle = message.arguments()[0].value<QDBusObjectPath>().path();
              m_reply.arguments = {m_handle};
              m_hasReply = true;
              if (!m_paths.contains(m_handle))
                subscribe(m_handle);
              check();
            });
    m_timeout.start();
  }
public slots:
  void response(uint code, const QVariantMap &) {
    m_response = int(code);
    check();
  }

private:
  void subscribe(const QString &path) {
    m_paths.append(path);
    m_connection.connect(m_call.service, path, "org.freedesktop.portal.Request",
                         "Response", this, SLOT(response(uint, QVariantMap)));
  }
  void check() {
    if (!m_hasReply || m_response < 0)
      return;
    if (m_response != 0) {
      m_reply.error = m_response == 1
                          ? QCoreApplication::translate("KomaMimic", "Portalが抑制要求を許可しませんでした。")
                          : QCoreApplication::translate("KomaMimic", "Portal backendが抑制要求に失敗しました。");
      m_reply.denied = m_response == 1;
    }
    finish(m_response == 0);
  }
  void finish(bool success) {
    if (!m_done)
      return;
    m_timeout.stop();
    for (const auto &path : m_paths) {
      m_connection.disconnect(m_call.service, path,
                              "org.freedesktop.portal.Request", "Response",
                              this, SLOT(response(uint, QVariantMap)));
      if (!success) {
        auto close = QDBusMessage::createMethodCall(
            m_reply.sender.isEmpty() ? m_call.service : m_reply.sender, path,
            "org.freedesktop.portal.Request", "Close");
        m_connection.send(close);
      }
    }
    auto done = std::exchange(m_done, {});
    done(std::move(m_reply));
    deleteLater();
  }
  QDBusConnection m_connection;
  InhibitBusCall m_call;
  std::function<void(InhibitBusReply)> m_done;
  QStringList m_paths;
  QString m_handle;
  QTimer m_timeout;
  InhibitBusReply m_reply;
  int m_response = -1;
  bool m_hasReply = false;
};
class Watch : public InhibitBusWatch {
public:
  Watch(const QDBusConnection &connection, const QString &service,
        const QString &owner, std::function<void()> lost)
      : watcher(service, connection, QDBusServiceWatcher::WatchForOwnerChange) {
    QObject::connect(
        &watcher, &QDBusServiceWatcher::serviceOwnerChanged, &watcher,
        [lost](const QString &, const QString &, const QString &) { lost(); });
    health.setInterval(5000);
    QObject::connect(&health, &QTimer::timeout, &watcher, [connection, lost] {
      if (!connection.isConnected())
        lost();
    });
    health.start();
    if (owner.startsWith(':')) {
      auto message = QDBusMessage::createMethodCall(
          "org.freedesktop.DBus", "/org/freedesktop/DBus",
          "org.freedesktop.DBus", "GetNameOwner");
      message.setArguments({service});
      auto *query = new QDBusPendingCallWatcher(
          connection.asyncCall(message, 1500), &watcher);
      QObject::connect(query, &QDBusPendingCallWatcher::finished, &watcher,
                       [lost, owner](QDBusPendingCallWatcher *query) {
                         const QDBusPendingReply<QString> reply = *query;
                         if (reply.isError() || reply.value() != owner)
                           lost();
                         query->deleteLater();
                       });
    }
  }

private:
  QDBusServiceWatcher watcher;
  QTimer health;
};
class Bus : public InhibitBus {
public:
  explicit Bus(const QString &address)
      : m_name("komamimic-inhibit-" +
               QUuid::createUuid().toString(QUuid::WithoutBraces)),
        m_connection(address.isEmpty()
                         ? QDBusConnection::connectToBus(
                               QDBusConnection::SessionBus, m_name)
                         : QDBusConnection::connectToBus(address, m_name)) {}
  ~Bus() override { close(); }
  void close() override {
    if (m_closed)
      return;
    m_closed = true;
    // A private connection is also the cancellation boundary: the server drops
    // requests if an acquire timed out or its reply was lost during shutdown.
    QDBusConnection::disconnectFromBus(m_name);
    m_connection = QDBusConnection(QString{});
    for (const auto &watcher : m_calls)
      if (watcher)
        watcher->deleteLater();
    m_calls.clear();
  }
  void call(const InhibitBusCall &call,
            std::function<void(InhibitBusReply)> completion) override {
    m_calls.removeIf([](const auto &call) { return call.isNull(); });
    if (call.interface == "org.freedesktop.portal.Inhibit") {
      auto *pending =
          new PortalPending(m_connection, call, std::move(completion));
      m_calls.append(pending);
      pending->start();
      return;
    }
    auto message = QDBusMessage::createMethodCall(call.service, call.path,
                                                  call.interface, call.method);
    message.setArguments(call.arguments);
    auto *watcher = new QDBusPendingCallWatcher(
        m_connection.asyncCall(message, 1500), QCoreApplication::instance());
    m_calls.append(watcher);
    QObject::connect(
        watcher, &QDBusPendingCallWatcher::finished, watcher,
        [completion = std::move(completion)](QDBusPendingCallWatcher *watcher) {
          const QDBusMessage reply = watcher->reply();
          InhibitBusReply result;
          result.sender = reply.service();
          if (reply.type() == QDBusMessage::ErrorMessage)
            result.error = reply.errorMessage();
          else {
            result.arguments = reply.arguments();
            for (auto &arg : result.arguments)
              if (arg.metaType() == QMetaType::fromType<QDBusObjectPath>())
                arg = arg.value<QDBusObjectPath>().path();
          }
          completion(std::move(result));
          watcher->deleteLater();
        });
  }
  void send(const InhibitBusCall &call) override {
    if (m_closed)
      return;
    auto message = QDBusMessage::createMethodCall(call.service, call.path,
                                                  call.interface, call.method);
    message.setArguments(call.arguments);
    m_connection.send(message);
  }
  std::unique_ptr<InhibitBusWatch> watch(const QString &service,
                                         const QString &owner,
                                         std::function<void()> lost) override {
    return std::make_unique<Watch>(m_connection, service, owner,
                                   std::move(lost));
  }

private:
  QString m_name;
  QDBusConnection m_connection;
  bool m_closed = false;
  QList<QPointer<QObject>> m_calls;
};
#endif
class Backend : public IdleInhibitBackend {
public:
  void acquire(std::shared_ptr<IdleInhibitRequest> request,
               Completion completion) override {
#ifdef KOMAMIMIC_DBUS
    acquireLinuxIdleInhibit(createInhibitBus(), request, std::move(completion));
#else
    if (!request->cancelled)
      completion({}, QCoreApplication::translate("KomaMimic", "このビルドにはQt D-Busが含まれていません。"));
#endif
  }
};
} // namespace
std::shared_ptr<IdleInhibitBackend> createIdleInhibitBackend() {
  return std::make_shared<Backend>();
}
std::shared_ptr<InhibitBus> createInhibitBus(const QString &address) {
#ifdef KOMAMIMIC_DBUS
  return std::make_shared<Bus>(address);
#else
  Q_UNUSED(address);
  return {};
#endif
}
#ifdef KOMAMIMIC_DBUS
#include "power_linux.moc"
#endif
