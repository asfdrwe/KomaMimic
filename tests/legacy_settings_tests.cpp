// SPDX-License-Identifier: GPL-3.0-only
#include "legacysettings.h"
#include "readerfeatures.h"
#include "readertoolbar.h"
#include "test_support.h"
#include "shortcuts.h"
#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>

namespace {
QVariantMap allChanges(const LegacySettingsPlan &plan) {
  QVariantMap values;
  for (const auto &change : plan.changes)
    for (auto it = change.values.cbegin(); it != change.values.cend(); ++it)
      values[it.key()] = it.value();
  return values;
}
ReaderFeatures *features(MainWindow &window) {
  for (auto *child : window.children())
    if (auto *candidate = dynamic_cast<ReaderFeatures *>(child))
      return candidate;
  return nullptr;
}
QVariantMap snapshot(const QString &path) {
  QSettings s(path, QSettings::IniFormat);
  QVariantMap values;
  for (const auto &key : s.allKeys())
    values[key] = s.value(key);
  return values;
}
} // namespace
class LegacySettingsTests : public QObject {
  Q_OBJECT
private slots:
  void returnAcceptBindingsCanBeImported() {
    QHash<QString,QKeySequence> keys;
    for(const auto &spec:shortcutSpecs())keys[spec.id]=spec.defaultKey;
    keys["fullscreenGlobal"]=QKeySequence("F11");
    const auto plan=planLegacySettings("[KeyBoardKeyConfig]\nCMD23=13\nCMD36=13\n",keys,{});
    QVERIFY(plan.error.isEmpty());const auto values=allChanges(plan);
    QCOMPARE(values.value("keys/fullscreenGlobal").toString(),QString("Return"));
    QCOMPARE(values.value("keys/thumbAccept").toString(),QString("Return"));
  }
  void supportedValuesAndUnsupportedSettings() {
    const auto plan = planLegacySettings(
        "[Config]\nPageMode=0\nPrepageForwardNum=4\nAutoUpdate="
        "0\nAutoPageChange=1\nSubFolder=2\nScaleMode=2\n[AutoDisp]\nTwoPage=@"
        "height>@width\nHalfPageVPos=@width/"
        "2\n[AvisynthPlugin]\nScript1=anything()\n",
        {}, {});
    QVERIFY(plan.error.isEmpty());
    const auto values = allChanges(plan);
    QCOMPARE(values.value("cache/ahead").toInt(), 4);
    QCOMPARE(values.value("General/watch").toBool(), false);
    QCOMPARE(values.value("source/recursive").toBool(), false);
    QCOMPARE(values.value("reading/autoSpread").toBool(), true);
    QCOMPARE(values.value("reading/spread").toBool(), true);
    QCOMPARE(values.value("reading/spreadCondition").toString(),
             "@height>@width");
    QVERIFY(!values.contains("display/scaleMode"));
    QVERIFY(plan.report.join('\n').contains("ScaleMode"));
    QCOMPARE(plan.originals.value("AvisynthPlugin/Script1"), "anything()");
  }
  void rejectsInvalidValuesAndExpressions() {
    const auto plan = planLegacySettings(
        "[Config]\nPageMode=99\nAutoUpdate=yes\nPrepageForwardNum=-"
        "1\nPrepageBackwardNum=999999999999999\nSubFolder=0\n[AutoDisp]"
        "\nHalfPageVAllowance=1.5\nTwoPage=exec(1)\nHalfPageH=\n",
        {}, {});
    QVERIFY(plan.error.isEmpty());
    QVERIFY(plan.changes.isEmpty());
    QVERIFY(plan.report.size() >= 8);
    QVERIFY(!planLegacySettings("garbage", {}, {}).error.isEmpty());
    QVERIFY(!planLegacySettings("[Other]\nx=1", {}, {}).error.isEmpty());
    QVERIFY(!planLegacySettings(
                 QString::fromLatin1("[Config]\nx=bad\0data", 19), {}, {})
                 .error.isEmpty());
  }
  void preservesRawStringsAndHandlesCaseAndDuplicates() {
    const auto plan =
        planLegacySettings("[config]\nprepageforwardnum=2\nPrepageForwardNum="
                           "5\n[AutoDisp]\nTwoPage=\"@height>@width\"\n["
                           "Unknown]\nPath=C:\\folder\\日本語,a=b\n",
                           {}, {});
    QVERIFY(plan.error.isEmpty());
    QCOMPARE(allChanges(plan).value("cache/ahead").toInt(), 5);
    QCOMPARE(plan.originals.value("Unknown/Path"),
             QString("C:\\folder\\日本語,a=b"));
    QVERIFY(plan.report.join('\n').contains("重複"));
  }
  void swapsKeysAsASet() {
    QHash<QString, QKeySequence> keys{{"next", QKeySequence("Left")},
                                      {"previous", QKeySequence("Right")}};
    const auto values = allChanges(planLegacySettings(
        "[KeyBoardKeyConfig]\nCMD1=39\nCMD2=37\n", keys, {}));
    QCOMPARE(values.value("keys/next").toString(), "Right");
    QCOMPARE(values.value("keys/previous").toString(), "Left");
  }
  void duplicateCommandAliasesPreferAnAssignedKey() {
    const auto values = allChanges(planLegacySettings(
        "[KeyBoardKeyConfig]\nCMD22=0\nCMD72=78\n", {{"fitWindow", {}}}, {}));
    QCOMPARE(values.value("keys/fitWindow").toString(), "N");
  }
  void denseKeyDefinitionsStayConflictFree() {
    QTemporaryDir d;
    MainWindow w(d.filePath("settings.ini"));
    QString ini = "[KeyBoardKeyConfig]\n";
    for (int command = 1; command <= 175; ++command)
      ini += QString("CMD%1=0\n").arg(command);
    ini += "CMD1=39\nCMD2=37\n";
    auto keys = w.bindings();
    const auto plan = planLegacySettings(ini, keys, {});
    const auto values = allChanges(plan);
    QVERIFY(values.size() > 100);
    for (auto it = values.cbegin(); it != values.cend(); ++it)
      if (it.key().startsWith("keys/"))
        keys[it.key().mid(5)] = QKeySequence::fromString(
            it.value().toString(), QKeySequence::PortableText);
    QVERIFY(shortcutConflict(keys).isEmpty());
    QCOMPARE(keys.value("next"), QKeySequence("Right"));
    QCOMPARE(keys.value("previous"), QKeySequence("Left"));
    w.close();
  }
  void rejectsConflictsAndKeepsUnassignment() {
    QHash<QString, QKeySequence> keys{{"next", QKeySequence("Left")},
                                      {"previous", QKeySequence("Right")},
                                      {"first", QKeySequence("Home")}};
    auto plan = planLegacySettings(
        "[KeyBoardKeyConfig]\nCMD1=39\nCMD12=0\nCMD9=444\nCMD999=65\n", keys,
        {});
    auto values = allChanges(plan);
    QVERIFY(!values.contains("keys/next"));
    QVERIFY(values.contains("keys/first"));
    QVERIFY(values["keys/first"].toString().isEmpty());
    QVERIFY(plan.report.join('\n').contains("CMD9"));
    QVERIFY(plan.report.join('\n').contains("CMD999"));
    plan = planLegacySettings("[KeyBoardKeyConfig]\nCMD1=65\n", keys,
                              {{"previous", QKeySequence("A")}});
    QVERIFY(!allChanges(plan).contains("keys/next"));
  }
  void retainsToolbarSidesAndPageBar() {
    QHash<QString, QKeySequence> keys{
        {"open", {}}, {"next", {}}, {"previous", {}}, {"fullscreenGlobal", {}}};
    auto values = allChanges(planLegacySettings(
        "[ToolBar]\nLeftButtonNum=3\nRightButtonNum=3\nButton1=64\nButton2="
        "0\nButton3=1\nButton4=2\nButton5=0\nButton6=23\nTrackBar=0\n",
        keys, {}));
    QCOMPARE(values["toolbar/leftActions"].toStringList(),
             (QStringList{"open", "|", "next"}));
    QCOMPARE(values["toolbar/rightActions"].toStringList(),
             (QStringList{"previous", "|", "fullscreenGlobal"}));
    QCOMPARE(values["toolbar/showPageBar"].toBool(), false);
    QVERIFY(!values.contains("window/readerToolbar"));
    values = allChanges(planLegacySettings(
        "[ToolBar]\nLeftButtonNum=1\nRightButtonNum=0\nButton1=999\n", keys,
        {}));
    QVERIFY(!values.contains("toolbar/leftActions"));
    values =
        allChanges(planLegacySettings("[ToolBar]\nButton1=64\n", keys, {}));
    QVERIFY(!values.contains("toolbar/leftActions"));
  }
  void categoryPresetsImportOnlyTheirOwnSettings() {
    for (const auto category :
         {LegacySettingsCategory::Keyboard, LegacySettingsCategory::Reading,
          LegacySettingsCategory::Cache, LegacySettingsCategory::Toolbar}) {
      QTemporaryDir d;
      const auto config = d.filePath("settings.ini"),
                 fixture = d.filePath("MangaMeeya.ini");
      {
        QSettings settings(config, QSettings::IniFormat);
        settings.setValue("reading/spread", true);
        settings.setValue("cache/ahead", 2);
        settings.setValue("toolbar/showPageBar", true);
      }
      QVERIFY(TestData::write(
          fixture, "[Config]\nPageMode=1\nPrepageForwardNum=9\n[ToolBar]"
                   "\nTrackBar=0\n[KeyBoardKeyConfig]\nCMD1=0\n[AvisynthPlugin]"
                   "\nScript1=never_execute()\n"));
      MainWindow w(config);
      w.show();
      auto *f = features(w);
      QVERIFY(f);
      const auto oldNext = w.bindings().value("next");
      bool selected = false;
      QTimer timer;
      timer.setInterval(10);
      connect(&timer, &QTimer::timeout, &w, [&] {
        if (auto *box =
                qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
          box->accept();
        else if (auto *dialog =
                     qobject_cast<QDialog *>(QApplication::activeModalWidget());
                 dialog && dialog->objectName() == "mangaMeeyaImportDialog") {
          auto *scope = dialog->findChild<QComboBox *>("mangaMeeyaImportScope");
          scope->setCurrentIndex(scope->findData(int(category)));
          selected = true;
          dialog->findChild<QPushButton *>("applyMangaMeeyaImport")->click();
        }
      });
      timer.start();
      f->settingsFile(true, fixture, true);
      timer.stop();
      QVERIFY(selected);
      QSettings settings(config, QSettings::IniFormat);
      QCOMPARE(w.options().spread, category != LegacySettingsCategory::Reading);
      QCOMPARE(settings.value("cache/ahead").toInt(),
               category == LegacySettingsCategory::Cache ? 9 : 2);
      QCOMPARE(settings.value("toolbar/showPageBar").toBool(),
               category != LegacySettingsCategory::Toolbar);
      QVERIFY(!settings.contains("legacy/AvisynthPlugin/Script1"));
      QCOMPARE(w.bindings().value("next"),
               category == LegacySettingsCategory::Keyboard ? QKeySequence{}
                                                            : oldNext);
      if (category == LegacySettingsCategory::Keyboard) {
        QVERIFY(settings.contains("legacy/KeyBoardKeyConfig/CMD1"));
        QVERIFY(!settings.contains("legacy/Config/PageMode"));
        QVERIFY(!settings.contains("legacy/Config/PrepageForwardNum"));
        QVERIFY(!settings.contains("legacy/ToolBar/TrackBar"));
        QVERIFY(!settings.contains("legacy/AvisynthPlugin/Script1"));
      }
      w.close();
    }
  }
  void cancelDoesNotWriteOrBackUp() {
    QTemporaryDir d;
    const auto config = d.filePath("settings.ini"),
               fixture = d.filePath("MangaMeeya.ini");
    QVERIFY(TestData::write(fixture, "[Config]\nPrepageForwardNum=7\n"));
    MainWindow w(config);
    w.show();
    auto *f = features(w);
    QVERIFY(f);
    const auto before = snapshot(config);
    bool previewed = false;
    QTimer timer;
    timer.setInterval(10);
    connect(&timer, &QTimer::timeout, &w, [&] {
      if (auto *dialog =
              qobject_cast<QDialog *>(QApplication::activeModalWidget());
          dialog && dialog->objectName() == "mangaMeeyaImportDialog") {
        previewed = true;
        dialog->reject();
      }
    });
    timer.start();
    f->settingsFile(true, fixture, true);
    timer.stop();
    QVERIFY(previewed);
    QCOMPARE(snapshot(config), before);
    QCOMPARE(
        QDir(d.path()).entryList({"KomaMimic-before-*"}, QDir::Files).size(),
        0);
    w.close();
  }
  void selectiveImportBacksUpAndAppliesImmediately() {
    QTemporaryDir d;
    const auto config = d.filePath("settings.ini"),
               fixture = d.filePath("MangaMeeya.ini");
    {
      QSettings s(config, QSettings::IniFormat);
      s.setValue("reading/spread", false);
      s.setValue("cache/ahead", 2);
    }
    const QByteArray original(
        "[Config]\nPageMode=2\nPrepageForwardNum=7\n[ToolBar]\nTrackBar="
        "0\nLeftButtonNum=1\nRightButtonNum=1\nButton1=64\nButton2=23\n["
        "AvisynthPlugin]\nScript1=do_not_execute()\n");
    QVERIFY(TestData::write(fixture, original));
    MainWindow w(config);
    w.show();
    auto *f = features(w);
    QVERIFY(f);
    bool previewed = false, result = false;
    QTimer timer;
    timer.setInterval(10);
    connect(&timer, &QTimer::timeout, &w, [&] {
      if (auto *box =
              qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
        result = true;
        box->accept();
      } else if (auto *dialog =
                     qobject_cast<QDialog *>(QApplication::activeModalWidget());
                 dialog && dialog->objectName() == "mangaMeeyaImportDialog") {
        previewed = true;
        auto *tree =
            dialog->findChild<QTreeWidget *>("mangaMeeyaImportChanges");
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
          for (int j = 0; j < tree->topLevelItem(i)->childCount(); ++j)
            if (tree->topLevelItem(i)->child(j)->text(1) == "reading/spread")
              tree->topLevelItem(i)->setCheckState(0, Qt::Unchecked);
        dialog->findChild<QPushButton *>("applyMangaMeeyaImport")->click();
      }
    });
    timer.start();
    f->settingsFile(true, fixture);
    timer.stop();
    QVERIFY(previewed && result);
    QSettings s(config, QSettings::IniFormat);
    QCOMPARE(s.value("cache/ahead").toInt(), 7);
    QCOMPARE(w.options().spread, false);
    QVERIFY(!s.contains("legacy/AvisynthPlugin/Script1"));
    QVERIFY(!s.contains("legacy/Config/PageMode"));
    const auto backup = s.value("migration/lastIniBackup").toString();
    QVERIFY(QFileInfo::exists(backup));
    const auto previous = snapshot(backup);
    QCOMPARE(previous.value("cache/ahead").toInt(), 2);
    QVERIFY(!previous.contains("legacy/Config/PageMode"));
    auto *bar =
        dynamic_cast<ReaderToolBar *>(w.findChild<QToolBar *>("readerToolbar"));
    QVERIFY(bar);
    QCOMPARE(bar->leftCommands(), (QStringList{"open"}));
    QCOMPARE(bar->rightCommands(), (QStringList{"fullscreenGlobal"}));
    QVERIFY(!s.value("toolbar/showPageBar").toBool());
    QFile input(fixture);
    QVERIFY(input.open(QIODevice::ReadOnly));
    QCOMPARE(input.readAll(), original);
    w.close();
  }
  void importedPageModeSurvivesRuntimeSettingsSave() {
    for (int mode : {0, 1, 2}) {
      QTemporaryDir d;
      const auto config = d.filePath("settings.ini"),
                 fixture = d.filePath("MangaMeeya.ini");
      {
        QSettings settings(config, QSettings::IniFormat);
        settings.setValue("reading/spread", mode == 1);
      }
      QVERIFY(TestData::write(
          fixture, QString("[Config]\nPageMode=%1\n").arg(mode).toUtf8()));
      MainWindow w(config);
      w.show();
      auto *f = features(w);
      QVERIFY(f);
      QTimer timer;
      timer.setInterval(10);
      connect(&timer, &QTimer::timeout, &w, [&] {
        if (auto *box =
                qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
          box->accept();
        else if (auto *dialog =
                     qobject_cast<QDialog *>(QApplication::activeModalWidget());
                 dialog && dialog->objectName() == "mangaMeeyaImportDialog")
          dialog->findChild<QPushButton *>("applyMangaMeeyaImport")->click();
      });
      timer.start();
      f->settingsFile(true, fixture);
      timer.stop();
      QCOMPARE(w.options().spread, mode != 1);
      w.close();
      QSettings settings(config, QSettings::IniFormat);
      QCOMPARE(settings.value("reading/spread").toBool(), mode != 1);
      QCOMPARE(settings.value("reading/autoSpread").toBool(), mode == 0);
    }
  }
  void decodesCp932AndUtf16() {
    for (bool utf16 : {false, true}) {
      QTemporaryDir d;
      const auto config = d.filePath("settings.ini"),
                 fixture = d.filePath("MangaMeeya.ini");
      const QString text =
          "[Config]\nPrepageForwardNum=3\n[Unknown]\nLabel=日本語\n";
      QByteArray bytes;
      if (utf16) {
        bytes = QByteArray::fromHex("fffe");
        for (auto c : text) {
          bytes.append(char(c.unicode() & 255));
          bytes.append(char(c.unicode() >> 8));
        }
      } else
        bytes = QByteArray("[Config]\nPrepageForwardNum=3\n[Unknown]\nLabel=") +
                QByteArray::fromHex("93fa967b8cea") + '\n';
      QVERIFY(TestData::write(fixture, bytes));
      MainWindow w(config);
      w.show();
      auto *f = features(w);
      QVERIFY(f);
      QTimer timer;
      timer.setInterval(10);
      connect(&timer, &QTimer::timeout, &w, [&] {
        if (auto *box =
                qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
          box->accept();
        else if (auto *dialog =
                     qobject_cast<QDialog *>(QApplication::activeModalWidget());
                 dialog && dialog->objectName() == "mangaMeeyaImportDialog")
          dialog->findChild<QPushButton *>("applyMangaMeeyaImport")->click();
      });
      timer.start();
      f->settingsFile(true, fixture);
      timer.stop();
      QSettings s(config, QSettings::IniFormat);
      QCOMPARE(s.value("cache/ahead").toInt(), 3);
      QCOMPARE(s.value("legacy/Unknown/Label").toString(), QString("日本語"));
      w.close();
    }
  }
};
ISOLATED_TEST_MAIN(LegacySettingsTests)
#include "legacy_settings_tests.moc"
