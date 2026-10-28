// SPDX-License-Identifier: GPL-3.0-only
#include "archivepath.h"
#include "legacysettings.h"
#include "mainwindow.h"
#include "readerfeatures.h"
#include "readertoolbar.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileSystemWatcher>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QTemporaryFile>
#include <QTreeWidget>
#include <QVBoxLayout>

void ReaderFeatures::importMangaMeeyaSettings(const QString &text) {
  const auto plan =
      planLegacySettings(text, w->bindings(), w->documentBindings());
  if (!plan.error.isEmpty()) {
    QMessageBox::warning(
        w, QCoreApplication::translate("KomaMimic", "設定を適用できません"),
        plan.error);
    return;
  }
  QDialog dialog(w);
  dialog.setObjectName("mangaMeeyaImportDialog");
  dialog.setWindowTitle(
      QCoreApplication::translate("KomaMimic", "MangaMeeyaの設定を取り込む"));
  dialog.resize(1050, 720);
  auto *layout = new QVBoxLayout(&dialog);
  auto *note = new QLabel(
      QCoreApplication::translate(
          "KomaMimic",
          "取り込む項目を選んでください。選択した設定だけを上書きします。\n"
          "適用前に現在のKomaMimic設定をバックアップします。旧プラグイン・スク"
          "リプトは実行しません。"),
      &dialog);
  note->setWordWrap(true);
  layout->addWidget(note);
  auto *selection = new QComboBox(&dialog);
  selection->setObjectName("mangaMeeyaImportScope");
  selection->addItem(
      QCoreApplication::translate("KomaMimic", "すべての対応項目"), -1);
  selection->addItem(
      QCoreApplication::translate("KomaMimic", "キーボードショートカットのみ"),
      int(LegacySettingsCategory::Keyboard));
  selection->addItem(
      QCoreApplication::translate("KomaMimic", "読書・読み込み設定のみ"),
      int(LegacySettingsCategory::Reading));
  selection->addItem(QCoreApplication::translate("KomaMimic", "先読み設定のみ"),
                     int(LegacySettingsCategory::Cache));
  selection->addItem(
      QCoreApplication::translate("KomaMimic", "読書ツールバーのみ"),
      int(LegacySettingsCategory::Toolbar));
  selection->addItem(QCoreApplication::translate("KomaMimic", "個別に選択"),
                     -2);
  auto *scopeForm = new QFormLayout;
  scopeForm->addRow(QCoreApplication::translate("KomaMimic", "取り込み対象"),
                    selection);
  layout->addLayout(scopeForm);
  auto *tree = new QTreeWidget(&dialog);
  tree->setObjectName("mangaMeeyaImportChanges");
  tree->setHeaderLabels(
      {QCoreApplication::translate("KomaMimic", "項目"),
       QCoreApplication::translate("KomaMimic", "保存先"),
       QCoreApplication::translate("KomaMimic", "現在の値"),
       QCoreApplication::translate("KomaMimic", "取り込む値")});
  layout->addWidget(tree, 1);
  QStringList report = plan.report;
  const auto display = [](const QVariant &value) {
    return value.metaType().id() == QMetaType::QStringList
               ? value.toStringList().join(", ")
               : value.toString();
  };
  const QVariantMap defaults{{"cache/ahead", 2},
                             {"cache/behind", 1},
                             {"General/watch", false},
                             {"source/recursive", false},
                             {"reading/scrollTurnsPage", true},
                             {"reading/autoSpread", false},
                             {"reading/spreadCondition", "@height>@width"},
                             {"reading/splitVCondition", "@width>@height"},
                             {"reading/splitHCondition", "@height>@width"},
                             {"reading/splitVPosition", "@width/2"},
                             {"reading/splitHPosition", "@height/2"},
                             {"reading/splitVOverlap", 0},
                             {"reading/splitHOverlap", 0}};
  const auto currentValue = [&](const QString &key) -> QVariant {
    if (key.startsWith("keys/"))
      return w->bindings()
          .value(key.mid(5))
          .toString(QKeySequence::PortableText);
    if (key == "reading/spread")
      return w->m_options.spread;
    if (key == "toolbar/leftActions")
      return w->m_toolbar->leftCommands();
    if (key == "toolbar/rightActions")
      return w->m_toolbar->rightCommands();
    if (key == "toolbar/showPageBar")
      return w->m_toolbar->pageBarVisible();
    return w->m_settings->value(key, defaults.value(key));
  };
  for (const auto &change : plan.changes) {
    auto *item = new QTreeWidgetItem(tree, {change.label});
    item->setCheckState(0, Qt::Checked);
    item->setToolTip(0, change.sources.join('\n'));
    for (const auto &source : change.sources)
      report << source + " = " + plan.originals.value(source);
    for (auto it = change.values.cbegin(); it != change.values.cend(); ++it) {
      const bool keyboard = it.key().startsWith("keys/");
      auto label = change.sources.value(0);
      if (keyboard)
        if (auto *action = w->command(it.key().mid(5))) {
          label = action->text();
          label.remove('&');
        }
      const auto valueLabel = [&](const QVariant &value) {
        return keyboard && value.toString().isEmpty()
                   ? QCoreApplication::translate("KomaMimic", "割り当てなし")
                   : display(value);
      };
      new QTreeWidgetItem(item,
                          {label, it.key(), valueLabel(currentValue(it.key())),
                           valueLabel(it.value())});
      report << " → " + it.key() + " = " + display(it.value());
    }
    if (change.values.size() <= 2)
      item->setExpanded(true);
  }
  tree->setColumnWidth(0, 350);
  tree->setColumnWidth(1, 250);
  auto *details = new QPlainTextEdit(report.join('\n'), &dialog);
  details->setObjectName("mangaMeeyaImportDetails");
  details->setReadOnly(true);
  details->setMaximumHeight(180);
  layout->addWidget(details);
  auto *buttons = new QDialogButtonBox(
      QDialogButtonBox::Apply | QDialogButtonBox::Cancel, &dialog);
  layout->addWidget(buttons);
  auto *apply = buttons->button(QDialogButtonBox::Apply);
  apply->setObjectName("applyMangaMeeyaImport");
  const auto update = [=] {
    bool any = false;
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
      any |= tree->topLevelItem(i)->checkState(0) == Qt::Checked;
    apply->setEnabled(any);
  };
  connect(tree, &QTreeWidget::itemChanged, &dialog, [=] {
    const QSignalBlocker blocker(selection);
    selection->setCurrentIndex(selection->findData(-2));
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
      tree->topLevelItem(i)->setHidden(false);
    update();
  });
  connect(selection, &QComboBox::currentIndexChanged, &dialog,
          [&, tree, selection, update] {
            const int scope = selection->currentData().toInt();
            const QSignalBlocker blocker(tree);
            for (int i = 0; i < plan.changes.size(); ++i) {
              auto *item = tree->topLevelItem(i);
              const bool matches =
                  scope < 0 || int(plan.changes[i].category) == scope;
              item->setHidden(!matches);
              if (scope != -2)
                item->setCheckState(0, matches ? Qt::Checked : Qt::Unchecked);
              if (scope >= 0 && matches)
                item->setExpanded(true);
            }
            update();
          });
  update();
  connect(apply, &QPushButton::clicked, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  if (dialog.exec() != QDialog::Accepted)
    return;

  w->saveImageSettings();
  w->saveSettings();
  w->m_settings->sync();
  QFile previous(w->m_settings->fileName());
  QTemporaryFile backup(QFileInfo(previous).absoluteDir().filePath(
      "KomaMimic-before-MangaMeeya-XXXXXX.ini"));
  if (w->m_settings->status() != QSettings::NoError ||
      !previous.open(QIODevice::ReadOnly) || !backup.open()) {
    QMessageBox::warning(
        w, QCoreApplication::translate("KomaMimic", "設定を適用できません"),
        QCoreApplication::translate(
            "KomaMimic",
            "現在の設定をバックアップできません。取り込みを中止しました。"));
    return;
  }
  const auto bytes = previous.readAll();
  if (previous.error() != QFileDevice::NoError ||
      backup.write(bytes) != bytes.size() || !backup.flush()) {
    QMessageBox::warning(
        w, QCoreApplication::translate("KomaMimic", "設定を適用できません"),
        QCoreApplication::translate(
            "KomaMimic",
            "現在の設定をバックアップできません。取り込みを中止しました。"));
    return;
  }
  backup.setAutoRemove(false);
  backup.close();
  QVariantMap selected;
  QSet<LegacySettingsCategory> categories;
  for (int i = 0; i < plan.changes.size(); ++i) {
    const auto &change = plan.changes[i];
    const bool checked = tree->topLevelItem(i)->checkState(0) == Qt::Checked;
    if (checked)
      categories.insert(change.category);
    for (auto it = change.values.cbegin(); it != change.values.cend(); ++it) {
      report << (checked
                     ? QCoreApplication::translate("KomaMimic", "適用：")
                     : QCoreApplication::translate("KomaMimic", "選択せず：")) +
                    it.key();
      if (checked)
        selected[it.key()] = it.value();
    }
  }
  const bool everything = selection->currentData().toInt() == -1;
  for (auto it = plan.originals.cbegin(); it != plan.originals.cend(); ++it) {
    auto category = LegacySettingsCategory::Reading;
    if (it.key().startsWith("KeyBoardKeyConfig/", Qt::CaseInsensitive))
      category = LegacySettingsCategory::Keyboard;
    else if (it.key().startsWith("ToolBar/", Qt::CaseInsensitive))
      category = LegacySettingsCategory::Toolbar;
    else if (it.key().startsWith("Config/Prepage", Qt::CaseInsensitive) ||
             it.key().contains("Cache", Qt::CaseInsensitive))
      category = LegacySettingsCategory::Cache;
    const bool categorized =
        it.key().startsWith("Config/", Qt::CaseInsensitive) ||
        it.key().startsWith("AutoDisp/", Qt::CaseInsensitive) ||
        it.key().startsWith("KeyBoardKeyConfig/", Qt::CaseInsensitive) ||
        it.key().startsWith("ToolBar/", Qt::CaseInsensitive);
    if (everything || (categorized && categories.contains(category)))
      w->m_settings->setValue("legacy/" + it.key(), it.value());
  }
  for (auto it = selected.cbegin(); it != selected.cend(); ++it)
    w->m_settings->setValue(it.key(), it.value());
  w->m_settings->setValue("migration/lastIniReport", report);
  w->m_settings->setValue("migration/lastIniBackup", backup.fileName());
  w->m_settings->sync();
  if (w->m_settings->status() != QSettings::NoError) {
    QMessageBox::warning(
        w, QCoreApplication::translate("KomaMimic", "設定を適用できません"),
        QCoreApplication::translate(
            "KomaMimic", "設定の保存に失敗しました。バックアップ：%1")
            .arg(backup.fileName()));
    return;
  }
  auto keys = w->bindings();
  for (auto it = selected.cbegin(); it != selected.cend(); ++it)
    if (it.key().startsWith("keys/"))
      keys[it.key().mid(5)] = QKeySequence::fromString(
          it.value().toString(), QKeySequence::PortableText);
  // Block reader actions: importing image layout must not change the open
  // WebEngine document's independent pageLayout setting.
  w->m_options.spread = w->m_settings->value("reading/spread", true).toBool();
  {
    const QSignalBlocker spread(w->m_actions["spread"]),
        automatic(w->m_actions["autoSpread"]);
    w->m_actions["spread"]->setChecked(w->m_options.spread);
    w->m_actions["autoSpread"]->setChecked(autoSpread());
  }
  w->applyBindings(keys, w->documentBindings());
  w->restoreToolbarSettings();
  if (!m_watcher->files().isEmpty())
    m_watcher->removePaths(m_watcher->files());
  if (w->m_settings->value("General/watch", false).toBool() &&
      bookPathExists(w->bookPath()))
    m_watcher->addPath(physicalBookPath(w->bookPath()));
  w->m_imageCache.clear();
  w->render();
  w->refreshState();
  w->saveSettings();
  QMessageBox result(
      QMessageBox::Information,
      QCoreApplication::translate("KomaMimic", "設定インポートの結果"),
      QCoreApplication::translate("KomaMimic",
                                  "%1項目を取り込みました。読み込み設定は次に本"
                                  "を開くときに適用します。\nバックアップ：%2")
          .arg(selected.size())
          .arg(backup.fileName()),
      QMessageBox::Ok, w);
  result.setDetailedText(report.join('\n'));
  result.exec();
}
