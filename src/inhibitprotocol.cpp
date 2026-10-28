// SPDX-License-Identifier: GPL-3.0-only
#include "inhibitprotocol.h"
#include <QCoreApplication>
#include <QRegularExpression>
#include <QUuid>

namespace {
class LinuxLease : public IdleInhibitLease {
public:
  LinuxLease(std::shared_ptr<InhibitBus> bus, InhibitBusCall release,
             const QString &service, QString name)
      : m_bus(std::move(bus)), m_release(std::move(release)),
        m_name(std::move(name)) {
    m_watch = m_bus->watch(service, m_release.service, [this] {
      if (auto callback = lost)
        callback();
    });
  }
  ~LinuxLease() override {
    m_watch.reset();
    m_bus->send(m_release);
    m_bus->close();
  }
  QString name() const override { return m_name; }

private:
  std::shared_ptr<InhibitBus> m_bus;
  InhibitBusCall m_release;
  QString m_name;
  std::unique_ptr<InhibitBusWatch> m_watch;
};
struct Attempt : std::enable_shared_from_this<Attempt> {
  std::shared_ptr<InhibitBus> bus;
  std::shared_ptr<IdleInhibitRequest> request;
  IdleInhibitBackend::Completion done;
  QString error;
  void next(int index) {
    if (request->cancelled)
      return;
    if (index == 3) {
      bus->close();
      done({}, error);
      return;
    }
    const QString screenSaver = "org.freedesktop.ScreenSaver";
    InhibitBusCall call;
    if (index == 0) {
      QVariantMap options{
          {"reason", QCoreApplication::translate("KomaMimic", "KomaMimicで読書中")},
          {"handle_token",
           "komamimic_" + QUuid::createUuid().toString(QUuid::Id128)}};
      call = {"org.freedesktop.portal.Desktop",
              "/org/freedesktop/portal/desktop",
              "org.freedesktop.portal.Inhibit",
              "Inhibit",
              {QString{}, uint(8), options}};
    } else {
      call = {screenSaver,
              index == 1 ? "/org/freedesktop/ScreenSaver" : "/ScreenSaver",
              screenSaver,
              "Inhibit",
              {QString("KomaMimic"), QCoreApplication::translate("KomaMimic", "読書中")}};
    }
    bus->call(call, [self = shared_from_this(), call,
                     index](InhibitBusReply reply) {
      if (self->request->cancelled)
        return;
      if (reply.denied) {
        self->bus->close();
        self->done({}, reply.error);
        return;
      }
      InhibitBusCall release;
      bool valid = reply.error.isEmpty() && reply.arguments.size() == 1;
      const auto service = reply.sender.isEmpty() ? call.service : reply.sender;
      if (index == 0) {
        const auto path =
            valid ? reply.arguments.first().toString() : QString{};
        valid = valid && path != "/" &&
                QRegularExpression("^/([A-Za-z0-9_]+/)*[A-Za-z0-9_]+$")
                    .match(path)
                    .hasMatch();
        release = {
            service, path, "org.freedesktop.portal.Request", "Close", {}};
      } else {
        bool ok = false;
        const uint cookie = valid ? reply.arguments.first().toUInt(&ok) : 0;
        valid = valid && ok &&
                reply.arguments.first().metaType().id() == QMetaType::UInt;
        // Zero is a valid cookie, not an inactive sentinel.
        release = {service, call.path, call.interface, "UnInhibit", {cookie}};
      }
      if (!valid) {
        self->error = reply.error.isEmpty()
                          ? QCoreApplication::translate("KomaMimic", "抑制APIから不正な応答がありました。")
                          : reply.error;
        self->next(index + 1);
        return;
      }
      self->request->cancelAction = {};
      self->done(std::make_unique<LinuxLease>(self->bus, release, call.service,
                                              index == 0
                                                  ? "Linux（Portal）"
                                                  : QCoreApplication::translate("KomaMimic", "Linux（ScreenSaver連携）")),
                 {});
    });
  }
};
} // namespace
void acquireLinuxIdleInhibit(std::shared_ptr<InhibitBus> bus,
                             std::shared_ptr<IdleInhibitRequest> request,
                             IdleInhibitBackend::Completion completion) {
  auto attempt = std::make_shared<Attempt>();
  attempt->bus = bus;
  attempt->request = request;
  attempt->done = std::move(completion);
  request->cancelAction = [weak = std::weak_ptr<InhibitBus>(bus)] {
    if (auto bus = weak.lock())
      bus->close();
  };
  attempt->next(0);
}
