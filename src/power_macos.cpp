// SPDX-License-Identifier: GPL-3.0-only
#include "idleinhibitor.h"
#include <QCoreApplication>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/pwr_mgt/IOPMLib.h>

namespace {
class MacLease : public IdleInhibitLease {
public:
  explicit MacLease(IOPMAssertionID id) : m_id(id) {}
  ~MacLease() override { IOPMAssertionRelease(m_id); }
  QString name() const override { return "macOS"; }

private:
  IOPMAssertionID m_id;
};
class Backend : public IdleInhibitBackend {
public:
  void acquire(std::shared_ptr<IdleInhibitRequest> request,
               Completion completion) override {
    if (request->cancelled)
      return;
    IOPMAssertionID id;
    const auto status = IOPMAssertionCreateWithName(
        kIOPMAssertionTypePreventUserIdleDisplaySleep, kIOPMAssertionLevelOn,
        CFSTR("KomaMimic: reading"), &id);
    if (status != kIOReturnSuccess) {
      completion({}, QCoreApplication::translate("KomaMimic", "macOSの画面消灯抑制要求に失敗しました（%1）。")
                         .arg(quint32(status)));
      return;
    }
    completion(std::make_unique<MacLease>(id), {});
  }
};
} // namespace
std::shared_ptr<IdleInhibitBackend> createIdleInhibitBackend() {
  return std::make_shared<Backend>();
}
