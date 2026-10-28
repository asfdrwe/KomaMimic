// SPDX-License-Identifier: GPL-3.0-only
#include "idleinhibitor.h"
#include "inhibitprotocol.h"
#include "test_support.h"
#include <QAction>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QTextStream>
#ifdef KOMAMIMIC_TEST_DBUS
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QUuid>
#endif
#include <vector>

namespace {
struct Counts {
  int acquired = 0, released = 0;
};
class Lease : public IdleInhibitLease {
public:
  explicit Lease(std::shared_ptr<Counts> counts) : counts(std::move(counts)) {
    ++this->counts->acquired;
  }
  ~Lease() override { ++counts->released; }
  QString name() const override { return "Test backend"; }
  std::shared_ptr<Counts> counts;
};
class Backend : public IdleInhibitBackend {
public:
  std::shared_ptr<Counts> counts = std::make_shared<Counts>();
  std::vector<Completion> replies;
  std::vector<std::shared_ptr<IdleInhibitRequest>> requests;
  bool immediate = false;
  Lease *last = nullptr;
  void acquire(std::shared_ptr<IdleInhibitRequest> request,
               Completion done) override {
    requests.push_back(request);
    if (immediate) {
      auto lease = std::make_unique<Lease>(counts);
      last = lease.get();
      done(std::move(lease), {});
    } else
      replies.push_back(std::move(done));
  }
  void succeed(int index) {
    auto done = std::move(replies.at(index));
    auto lease = std::make_unique<Lease>(counts);
    last = lease.get();
    done(std::move(lease), {});
  }
  void fail(int index) {
    auto done = std::move(replies.at(index));
    done({}, "test API unavailable");
  }
};
struct Loss {
  std::function<void()> callback;
};
class Watch : public InhibitBusWatch {
public:
  explicit Watch(std::shared_ptr<Loss> loss) : loss(std::move(loss)) {}
  ~Watch() override { loss->callback = {}; }
  std::shared_ptr<Loss> loss;
};
class Bus : public InhibitBus {
public:
  QVector<InhibitBusCall> calls, sent;
  std::vector<std::function<void(InhibitBusReply)>> replies;
  std::shared_ptr<Loss> loss = std::make_shared<Loss>();
  int closes = 0;
  QString watched;
  void call(const InhibitBusCall &call,
            std::function<void(InhibitBusReply)> done) override {
    calls.append(call);
    replies.push_back(std::move(done));
  }
  void send(const InhibitBusCall &call) override { sent.append(call); }
  void close() override { ++closes; }
  std::unique_ptr<InhibitBusWatch>
  watch(const QString &service, const QString &,
        std::function<void()> callback) override {
    watched = service;
    loss->callback = std::move(callback);
    return std::make_unique<Watch>(loss);
  }
  void answer(int index, const QVariantList &args, const QString &error = {}) {
    auto done = std::move(replies.at(index));
    done({args, error, ":1.42"});
  }
};
#ifdef KOMAMIMIC_TEST_DBUS
class PortalRequest : public QObject {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Request")
public:
  int closes = 0;
public slots:
  void Close() { ++closes; }
};
class PortalService : public QObject, protected QDBusContext {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Inhibit")
public:
  explicit PortalService(QDBusConnection connection) : bus(connection) {}
  QDBusConnection bus;
  uint response = 0;
  bool early = true;
  int calls = 0;
  QStringList paths;
  PortalRequest request;
public slots:
  QDBusObjectPath Inhibit(const QString &, uint flags,
                          const QVariantMap &options) {
    ++calls;
    auto sender = message().service();
    auto name = sender;
    name.remove(':');
    name.replace('.', '_');
    const auto path = "/org/freedesktop/portal/desktop/request/" + name + '/' +
                      options.value("handle_token").toString();
    paths.append(path);
    bus.registerObject(path, &request, QDBusConnection::ExportAllSlots);
    const auto sendResponse = [this, sender, path, flags] {
      auto signal = QDBusMessage::createTargetedSignal(
          sender, path, "org.freedesktop.portal.Request", "Response");
      signal.setArguments({flags == 8 ? response : uint(2), QVariantMap{}});
      bus.send(signal);
    };
    if (early)
      sendResponse();
    else
      QTimer::singleShot(20, this, sendResponse);
    return QDBusObjectPath(path);
  }
};
class ScreenSaverService : public QObject {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.freedesktop.ScreenSaver")
public:
  int calls = 0;
public slots:
  uint Inhibit(const QString &, const QString &) {
    ++calls;
    return 0;
  }
  void UnInhibit(uint) {}
};
#endif
} // namespace
class PowerTests : public QObject {
  Q_OBJECT
private slots:
#ifdef KOMAMIMIC_TEST_DBUS
  void privateBusTransport() {
    const auto daemonPath = QStandardPaths::findExecutable("dbus-daemon");
    if (daemonPath.isEmpty())
      QSKIP("dbus-daemon is not installed; private-bus integration not "
            "available.");
    QProcess daemon;
    daemon.start(daemonPath, {"--session", "--nofork", "--print-address=1"});
    QVERIFY(daemon.waitForStarted(3000));
    if (!daemon.waitForReadyRead(3000)) {
      const auto reason = daemon.readAllStandardError();
      daemon.kill();
      daemon.waitForFinished();
      if (reason.contains("Operation not permitted") ||
          reason.contains("Permission denied"))
        QSKIP("The execution sandbox denies private D-Bus socket creation.");
      QFAIL(qPrintable(QString::fromUtf8(reason)));
    }
    const auto address =
        QString::fromUtf8(daemon.readAllStandardOutput()).trimmed();
    QVERIFY(address.startsWith("unix:"));
    const auto name =
        "komamimic-power-test-" + QUuid::createUuid().toString(QUuid::Id128);
    {
      auto server = QDBusConnection::connectToBus(address, name);
      QVERIFY2(server.isConnected(), qPrintable(server.lastError().message()));
      PortalService portal(server);
      ScreenSaverService saver;
      QVERIFY(server.registerService("org.freedesktop.portal.Desktop"));
      QVERIFY(server.registerService("org.freedesktop.ScreenSaver"));
      QVERIFY(server.registerObject("/org/freedesktop/portal/desktop", &portal,
                                    QDBusConnection::ExportAllSlots));
      QVERIFY(server.registerObject("/org/freedesktop/ScreenSaver", &saver,
                                    QDBusConnection::ExportAllSlots));
      std::unique_ptr<IdleInhibitLease> lease;
      QString error;
      bool done = false;
      auto acquire = [&] {
        done = false;
        error.clear();
        acquireLinuxIdleInhibit(createInhibitBus(address),
                                std::make_shared<IdleInhibitRequest>(),
                                [&](auto value, QString message) {
                                  lease = std::move(value);
                                  error = message;
                                  done = true;
                                });
      };
      acquire();
      QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
      QVERIFY2(lease, qPrintable(error));
      QVERIFY(lease->name().contains("Portal"));
      lease.reset();
      portal.early = false;
      acquire();
      QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
      QVERIFY2(lease, qPrintable(error));
      QVERIFY(lease->name().contains("Portal"));
      lease.reset();
      portal.response = 1;
      acquire();
      QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
      QVERIFY(!lease);
      QVERIFY(error.contains("許可"));
      QCOMPARE(saver.calls, 0);
      portal.response = 2;
      acquire();
      QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
      QVERIFY2(lease, qPrintable(error));
      QVERIFY(lease->name().contains("ScreenSaver"));
      QCOMPARE(saver.calls, 1);
      bool lost = false;
      lease->lost = [&] { lost = true; };
      server.unregisterService("org.freedesktop.ScreenSaver");
      QTRY_VERIFY(lost);
      lease.reset();
      server.unregisterService("org.freedesktop.portal.Desktop");
      server.unregisterService("org.freedesktop.ScreenSaver");
      for (const auto &path : portal.paths)
        server.unregisterObject(path);
      server.unregisterObject("/org/freedesktop/portal/desktop");
      server.unregisterObject("/org/freedesktop/ScreenSaver");
    }
    QDBusConnection::disconnectFromBus(name);
    daemon.terminate();
    if (!daemon.waitForFinished(3000)) {
      daemon.kill();
      daemon.waitForFinished();
    }
  }
#endif
  void eligibility_data() {
    QTest::addColumn<int>("missing");
    QTest::newRow("foreground-reading") << -1;
    for (int i = 0; i < 7; ++i)
      QTest::newRow(qPrintable(QString("condition-%1").arg(i))) << i;
  }
  void eligibility() {
    QFETCH(int, missing);
    IdleInhibitContext c{true, true, true, true, false, true, false};
    switch (missing) {
    case 0:
      c.enabled = false;
      break;
    case 1:
      c.hasBook = false;
      break;
    case 2:
      c.reading = false;
      break;
    case 3:
      c.visible = false;
      break;
    case 4:
      c.minimized = true;
      break;
    case 5:
      c.foreground = false;
      break;
    case 6:
      c.closing = true;
      break;
    }
    QCOMPARE(c.wanted(), missing == -1);
  }
  void aggregateWindowsAndDestruction() {
    auto backend = std::make_shared<Backend>();
    IdleInhibitor manager(backend);
    QObject a;
    auto b = std::make_unique<QObject>();
    manager.setRequested(&a, true);
    manager.setRequested(&a, true);
    manager.setRequested(b.get(), true);
    QCOMPARE(backend->requests.size(), size_t(1));
    QCOMPARE(manager.requestCount(), 2);
    backend->succeed(0);
    QCOMPARE(manager.state(), IdleInhibitor::State::Active);
    manager.setRequested(&a, false);
    QCOMPARE(backend->counts->released, 0);
    b.reset();
    QCOMPARE(manager.state(), IdleInhibitor::State::Inactive);
    QCOMPARE(backend->counts->released, 1);
    manager.setRequested(&a, true);
    backend->succeed(1);
    QCOMPARE(backend->counts->acquired, 2);
  }
  void withdrawnAndDestroyedWhilePending() {
    auto backend = std::make_shared<Backend>();
    QObject owner;
    {
      IdleInhibitor manager(backend);
      manager.setRequested(&owner, true);
      manager.setRequested(&owner, false);
      QVERIFY(backend->requests[0]->cancelled);
      backend->succeed(0);
      QCOMPARE(manager.state(), IdleInhibitor::State::Inactive);
      QCOMPARE(backend->counts->released, 1);
      manager.setRequested(&owner, true);
    }
    QVERIFY(backend->requests[1]->cancelled);
    backend->succeed(1);
    QCOMPARE(backend->counts->released, 2);
  }
  void olderReplyCannotReplaceNewRequest() {
    auto backend = std::make_shared<Backend>();
    IdleInhibitor manager(backend);
    QObject owner;
    manager.setRequested(&owner, true);
    manager.setRequested(&owner, false);
    manager.setRequested(&owner, true);
    backend->succeed(0);
    QCOMPARE(manager.state(), IdleInhibitor::State::Acquiring);
    QCOMPARE(backend->counts->released, 1);
    backend->succeed(1);
    QCOMPARE(manager.state(), IdleInhibitor::State::Active);
    QCOMPARE(manager.description(), QString("自動消灯を抑制中：Test backend"));
  }
  void backendLossAndRetry() {
    auto backend = std::make_shared<Backend>();
    IdleInhibitor manager(backend);
    QObject owner;
    manager.setRequested(&owner, true);
    backend->fail(0);
    QCOMPARE(manager.state(), IdleInhibitor::State::Failed);
    QVERIFY(manager.description().contains("test API unavailable"));
    QTRY_COMPARE_WITH_TIMEOUT(backend->requests.size(), size_t(2), 6500);
    backend->succeed(1);
    QCOMPARE(manager.state(), IdleInhibitor::State::Active);
    const auto lost = backend->last->lost;
    lost();
    QTRY_COMPARE(backend->requests.size(), size_t(3));
    QCOMPARE(backend->counts->released, 1);
    backend->succeed(2);
    QCOMPARE(manager.state(), IdleInhibitor::State::Active);
  }
  void cancelFromStatusNotification() {
    auto backend = std::make_shared<Backend>();
    IdleInhibitor manager(backend);
    QObject owner;
    connect(&manager, &IdleInhibitor::statusChanged, &owner, [&] {
      if (manager.state() == IdleInhibitor::State::Acquiring)
        manager.setRequested(&owner, false);
    });
    manager.setRequested(&owner, true);
    QCOMPARE(manager.state(), IdleInhibitor::State::Inactive);
    QVERIFY(backend->requests.empty());
  }
  void portalAcquireAndRelease() {
    auto bus = std::make_shared<Bus>();
    auto request = std::make_shared<IdleInhibitRequest>();
    std::unique_ptr<IdleInhibitLease> lease;
    QString error;
    acquireLinuxIdleInhibit(bus, request, [&](auto value, QString message) {
      lease = std::move(value);
      error = message;
    });
    QCOMPARE(bus->calls.size(), 1);
    const auto call = bus->calls.first();
    QCOMPARE(call.interface, QString("org.freedesktop.portal.Inhibit"));
    QCOMPARE(call.arguments[1].toUInt(), uint(8));
    const auto token =
        call.arguments[2].toMap().value("handle_token").toString();
    QVERIFY(token.startsWith("komamimic_"));
    QVERIFY(!token.contains('-'));
    bus->answer(0, {"/org/freedesktop/portal/desktop/request/1_7/token"});
    QVERIFY(error.isEmpty());
    QVERIFY(lease);
    QCOMPARE(bus->watched, call.service);
    lease.reset();
    QCOMPARE(bus->sent.size(), 1);
    QCOMPARE(bus->sent[0].method, QString("Close"));
    QCOMPARE(bus->sent[0].service, QString(":1.42"));
    QCOMPARE(bus->closes, 1);
  }
  void portalDenialDoesNotFallBack() {
    auto bus = std::make_shared<Bus>();
    auto request = std::make_shared<IdleInhibitRequest>();
    bool completed = false;
    QString error;
    acquireLinuxIdleInhibit(bus, request, [&](auto lease, QString message) {
      QVERIFY(!lease);
      completed = true;
      error = message;
    });
    auto done = std::move(bus->replies[0]);
    done({{}, "permission denied", ":1.42", true});
    QVERIFY(completed);
    QCOMPARE(error, QString("permission denied"));
    QCOMPARE(bus->calls.size(), 1);
    QCOMPARE(bus->closes, 1);
  }
  void standardPathAndZeroCookie() {
    auto bus = std::make_shared<Bus>();
    auto request = std::make_shared<IdleInhibitRequest>();
    std::unique_ptr<IdleInhibitLease> lease;
    acquireLinuxIdleInhibit(
        bus, request, [&](auto value, QString) { lease = std::move(value); });
    bus->answer(0, {}, "portal not installed");
    QCOMPARE(bus->calls.size(), 2);
    QCOMPARE(bus->calls[1].path, QString("/org/freedesktop/ScreenSaver"));
    bus->answer(1, {uint(0)});
    QVERIFY(lease);
    lease.reset();
    QCOMPARE(bus->sent[0].method, QString("UnInhibit"));
    QCOMPARE(bus->sent[0].arguments[0].toUInt(), uint(0));
  }
  void legacyPathAndMalformedReplies() {
    auto bus = std::make_shared<Bus>();
    auto request = std::make_shared<IdleInhibitRequest>();
    std::unique_ptr<IdleInhibitLease> lease;
    QString error;
    acquireLinuxIdleInhibit(bus, request, [&](auto value, QString message) {
      lease = std::move(value);
      error = message;
    });
    bus->answer(0, {"/"});
    bus->answer(1, {"12"});
    QCOMPARE(bus->calls[2].path, QString("/ScreenSaver"));
    bus->answer(2, {}, "unavailable");
    QVERIFY(!lease);
    QCOMPARE(error, QString("unavailable"));
    QCOMPARE(bus->closes, 1);
  }
  void cancelBusAndWatchLifetime() {
    auto bus = std::make_shared<Bus>();
    auto request = std::make_shared<IdleInhibitRequest>();
    int completed = 0;
    acquireLinuxIdleInhibit(bus, request, [&](auto, QString) { ++completed; });
    request->cancel();
    QCOMPARE(bus->closes, 1);
    bus->answer(0, {"/request/late"});
    QCOMPARE(completed, 0);
    QCOMPARE(bus->calls.size(), 1);
    auto next = std::make_shared<IdleInhibitRequest>();
    std::unique_ptr<IdleInhibitLease> lease;
    acquireLinuxIdleInhibit(
        bus, next, [&](auto value, QString) { lease = std::move(value); });
    bus->answer(1, {"/request/current"});
    QVERIFY(lease);
    int losses = 0;
    lease->lost = [&] { ++losses; };
    bus->loss->callback();
    QCOMPARE(losses, 1);
    lease.reset();
    QVERIFY(!bus->loss->callback);
  }
  void generalPreferencesSurviveProcessRestart() {
    QTemporaryDir d;
    const auto config = d.filePath("settings.ini");
    for (const bool enabled : {true, false, true}) {
      auto backend = std::make_shared<Backend>();
      IdleInhibitor manager(backend);
      {
        MainWindow w(config, nullptr, &manager);
        QTimer::singleShot(0, &w, [&] {
          auto *dialog = w.findChild<QDialog *>("preferencesDialog");
          QVERIFY(dialog);
          dialog->findChild<QCheckBox *>("General/inhibit")->setChecked(enabled);
          dialog->findChild<QCheckBox *>("General/singleInstance")->setChecked(!enabled);
          dialog->findChild<QCheckBox *>("General/watch")->setChecked(enabled);
          dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        w.command("preferences")->trigger();
        w.close();
      }
      // A fresh QSettings in this process can share Qt's cached key map and
      // conceal INI spelling changes. Read the dialog in another process.
      QProcess child;
      child.start(QCoreApplication::applicationFilePath(), {"--probe-preferences", config});
      QVERIFY(child.waitForFinished(30000));
      QCOMPARE(child.exitStatus(), QProcess::NormalExit);
      QCOMPARE(child.exitCode(), 0);
      const auto actual = QJsonDocument::fromJson(child.readAllStandardOutput()).object();
      QVERIFY2(!actual.isEmpty(), child.readAllStandardError().constData());
      QCOMPARE(actual.value("inhibit").toBool(), enabled);
      QCOMPARE(actual.value("singleInstance").toBool(), !enabled);
      QCOMPARE(actual.value("watch").toBool(), enabled);
    }
  }
  void existingEscapedGeneralSectionIsReadAfterRestart() {
    QTemporaryDir d;
    const auto config = d.filePath("settings.ini");
    QVERIFY(TestData::write(config,
        "[%General]\ninhibit=true\nsingleInstance=false\nwatch=true\n[cache]\nthumbnailsEnabled=false\n"));
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(), {"--probe-preferences", config});
    QVERIFY(child.waitForFinished(30000));
    QCOMPARE(child.exitStatus(), QProcess::NormalExit);
    QCOMPARE(child.exitCode(), 0);
    const auto actual = QJsonDocument::fromJson(child.readAllStandardOutput()).object();
    QCOMPARE(actual.value("inhibit").toBool(), true);
    QCOMPARE(actual.value("singleInstance").toBool(), false);
    QCOMPARE(actual.value("watch").toBool(), true);
  }
  void windowLifecycleAndCommonPreference() {
    QTemporaryDir d;
    QVERIFY(TestData::imageFolder(d.filePath("pages"), 2));
    const auto config = d.filePath("settings.ini");
    {
      QSettings s(config, QSettings::IniFormat);
      s.setValue("General/inhibit", true);
    }
    auto backend = std::make_shared<Backend>();
    backend->immediate = true;
    IdleInhibitor manager(backend);
    MainWindow w(config, nullptr, &manager);
    w.show();
    w.openBook(d.filePath("pages/1.png"));
    QTRY_VERIFY_WITH_TIMEOUT(w.renderReady(), 10000);
    w.activateWindow();
    QTRY_COMPARE(manager.state(), IdleInhibitor::State::Active);
    auto *indicator = w.findChild<QLabel *>("inhibitIndicator");
    QVERIFY(indicator);
    QVERIFY(indicator->isVisible());
    QCOMPARE(indicator->text(), QString("消灯抑制中"));
    w.showCovers();
    QCOMPARE(manager.state(), IdleInhibitor::State::Inactive);
    w.openBook(d.filePath("pages/1.png"));
    QTRY_VERIFY(w.renderReady());
    w.activateWindow();
    QTRY_COMPARE(manager.state(), IdleInhibitor::State::Active);
    w.showMinimized();
    QTRY_COMPARE(manager.state(), IdleInhibitor::State::Inactive);
    w.showNormal();
    w.activateWindow();
    QTRY_COMPARE(manager.state(), IdleInhibitor::State::Active);
    QWidget other;
    other.show();
    other.activateWindow();
    QTRY_COMPARE(manager.state(), IdleInhibitor::State::Inactive);
    w.activateWindow();
    QTRY_COMPARE(manager.state(), IdleInhibitor::State::Active);
    QTimer::singleShot(0, &w, [&] {
      auto *dialog = w.findChild<QDialog *>("preferencesDialog");
      QVERIFY(dialog);
      auto *box = dialog->findChild<QCheckBox *>("General/inhibit");
      QVERIFY(box);
      QVERIFY(box->isChecked());
      QVERIFY(dialog->findChild<QLabel *>("inhibitStatus"));
      bool common = false;
      for (auto *label : dialog->findChildren<QLabel *>())
        if (label->text() == "読書中のスクリーンセーバー・自動消灯を抑制")
          common = true;
      QVERIFY(common);
      box->setChecked(false);
      dialog->findChild<QDialogButtonBox *>()
          ->button(QDialogButtonBox::Ok)
          ->click();
    });
    w.command("preferences")->trigger();
    QTRY_COMPARE(manager.state(), IdleInhibitor::State::Inactive);
    {
      QSettings s(config, QSettings::IniFormat);
      QVERIFY(!s.value("General/inhibit").toBool());
    }
    QCOMPARE(backend->counts->acquired, backend->counts->released);
    QVERIFY(!indicator->isVisible());
    QTimer::singleShot(0, &w, [&] {
      auto *dialog = w.findChild<QDialog *>("preferencesDialog");
      QVERIFY(dialog);
      dialog->findChild<QCheckBox *>("General/inhibit")->setChecked(true);
      dialog->findChild<QDialogButtonBox *>()
          ->button(QDialogButtonBox::Ok)
          ->click();
    });
    w.command("preferences")->trigger();
    w.activateWindow();
    QTRY_COMPARE(manager.state(), IdleInhibitor::State::Active);
    w.close();
    QCOMPARE(manager.state(), IdleInhibitor::State::Inactive);
    QCOMPARE(backend->counts->acquired, backend->counts->released);
  }
};
int main(int argc, char **argv) {
  if (argc == 3 && QByteArray(argv[1]) == "--probe-preferences") {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("KomaMimicTests");
    QCoreApplication::setApplicationName("Regression");
    QThreadPool::globalInstance()->setMaxThreadCount(3);
    QJsonObject checked;
    {
      auto backend = std::make_shared<Backend>();
      IdleInhibitor manager(backend);
      MainWindow w(QString::fromLocal8Bit(argv[2]), nullptr, &manager);
      QTimer::singleShot(0, &w, [&] {
        auto *dialog = w.findChild<QDialog *>("preferencesDialog");
        if (!dialog) return;
        for (const auto &name : {"inhibit", "singleInstance", "watch"}) {
          auto *box = dialog->findChild<QCheckBox *>(QString("General/") + name);
          if (box) checked[name] = box->isChecked();
        }
        dialog->reject();
      });
      w.command("preferences")->trigger();
      w.close();
    }
    QThreadPool::globalInstance()->waitForDone();
    QTextStream(stdout) << QJsonDocument(checked).toJson(QJsonDocument::Compact) << '\n';
    return checked.size() == 3 ? 0 : 2;
  }
  return isolatedTestMain<PowerTests>(argc, argv);
}
#include "power_tests.moc"
