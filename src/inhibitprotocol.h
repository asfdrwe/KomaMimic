// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "idleinhibitor.h"
#include <QVariant>

struct InhibitBusCall {
  QString service, path, interface, method;
  QVariantList arguments;
};
struct InhibitBusReply {
  QVariantList arguments;
  QString error, sender;
  bool denied = false;
};
class InhibitBusWatch {
public:
  virtual ~InhibitBusWatch() = default;
};
class InhibitBus {
public:
  virtual ~InhibitBus() = default;
  virtual void call(const InhibitBusCall &call,
                    std::function<void(InhibitBusReply)> completion) = 0;
  virtual void send(const InhibitBusCall &call) = 0;
  virtual void close() = 0;
  virtual std::unique_ptr<InhibitBusWatch>
  watch(const QString &service, const QString &owner,
        std::function<void()> lost) = 0;
};
// Protocol logic uses only Qt Core; tests can cover fallback and cancellation
// without creating a real inhibition or requiring an unrestricted session bus.
void acquireLinuxIdleInhibit(std::shared_ptr<InhibitBus> bus,
                             std::shared_ptr<IdleInhibitRequest> request,
                             IdleInhibitBackend::Completion completion);
// An explicit address is used by integration tests to isolate the real wire
// protocol on a private daemon, never on the user's desktop session.
std::shared_ptr<InhibitBus> createInhibitBus(const QString &address = {});
