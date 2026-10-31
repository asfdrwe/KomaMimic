// SPDX-License-Identifier: GPL-3.0-only
#include "expression.h"
#include "defaultpreferences.h"
#include <QCoreApplication>
#include "scriptreader.h"
#include "thumbnailcache.h"
#include "appearance.h"
#include <QDoubleSpinBox>
#include "mainwindow.h"
#include "foldertree.h"
#include "pageview.h"
#include "readerfeatures.h"
#include "readertoolbar.h"
#include "i18n.h"
#include "idleinhibitor.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QMouseEvent>
#include <QSaveFile>
#include <QScrollArea>
#include <QSpinBox>
#include <QStringDecoder>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTabBar>
#include <QHBoxLayout>
#include <QTemporaryFile>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <cmath>
#include <unicode/ucnv.h>
namespace {
QString legacyAction(int command) {
  static const QHash<int, QString> ids{{1, "next"},
                                       {2, "previous"},
                                       {3, "nextOne"},
                                       {4, "previousOne"},
                                       {5, "scrollNext"},
                                       {6, "scrollPrevious"},
                                       {11, "last"},
                                       {12, "first"},
                                       {13, "goPage"},
                                       {14, "thumbnails"},
                                       {15, "spread"},
                                       {16, "scrollLeft"},
                                       {17, "scrollUp"},
                                       {18, "scrollRight"},
                                       {19, "scrollDown"},
                                       {20, "zoomIn"},
                                       {21, "zoomOut"},
                                       {22, "fitWindow"},
                                       {23, "fullscreenGlobal"},
                                       {24, "scaleFixed"},
                                       {25, "scaleFill"},
                                       {26, "scaleFit"},
                                       {27, "scaleActual"},
                                       {28, "scaleSpecified"},
                                       {29, "loupe"},
                                       {30, "loupeLarger"},
                                       {31, "loupeSmaller"},
                                       {32, "loupeZoomIn"},
                                       {33, "loupeZoomOut"},
                                       {34, "thumbFit"},
                                       {35, "thumbEight"},
                                       {36, "thumbAccept"},
                                       {37, "thumbCancel"},
                                       {38, "thumbLeft"},
                                       {39, "thumbUp"},
                                       {40, "thumbRight"},
                                       {41, "thumbDown"},
                                       {42, "filterEnabled"},
                                       {43, "filterSettings"},
                                       {64, "open"},
                                       {65, "saveImage"},
                                       {66, "imagePaste"},
                                       {67, "imageCopy"},
                                       {68, "print"},
                                       {69, "pageSetup"},
                                       {70, "quit"},
                                       {71, "imageInfo"},
                                       {72, "fitWindow"},
                                       {73, "tree"},
                                       {74, "nextBook"},
                                       {75, "previousBook"},
                                       {76, "nextFolder"},
                                       {77, "previousFolder"},
                                       {78, "bookLists"},
                                       {79, "marker"},
                                       {80, "gotoMarker"},
                                       {81, "addFavorite"},
                                       {83, "rtl"},
                                       {104, "importSettings"},
                                       {105, "exportSettings"},
                                       {107, "noUpscale"},
                                       {108, "boundaryAdvance"},
                                       {109, "boundaryRepeat"},
                                       {110, "slideForward"},
                                       {111, "slidePause"},
                                       {112, "slideReverse"},
                                       {113, "slidePause"},
                                       {114, "thumbCustom"},
                                       {124, "slideStop"},
                                       {125, "minimize"},
                                       {126, "maximize"},
                                       {127, "coverBlank"},
                                       {128, "scrollNext"},
                                       {129, "preferences"},
                                       {130, "scrollPrevious"},
                                       {131, "bookshelf"},
                                       {132, "toc"},
                                       {133, "newWindow"},
                                       {134, "library"},
                                       {135, "nextToc"},
                                       {136, "previousToc"},
                                       {137, "autoSpread"},
                                       {138, "splitHorizontal"},
                                       {139, "splitVertical"},
                                       {140, "parentFolder"},
                                       {141, "thumbRefresh"},
                                       {142, "folder"},
                                       {143, "closeBook"},
                                       {144, "reloadBook"},
                                       {145, "addToc"},
                                       {146, "tray"},
                                       {147, "boundaryStop"},
                                       {148, "selectStart"},
                                       {149, "selectEnd"},
                                       {150, "editCopy"},
                                       {151, "editPaste"},
                                       {152, "editCut"},
                                       {153, "editDelete"},
                                       {154, "editUndo"},
                                       {155, "editRedo"},
                                       {164, "insertBlank"},
                                       {165, "openUrl"},
                                       {166, "toolbarToggle"},
                                       {167, "scaleCycle"},
                                       {168, "external"},
                                       {169, "findPages"},
                                       {170, "addPages"},
                                       {171, "editClear"}};
  if (command >= 44 && command <= 63)
    return QString("profile%1").arg(command - 43);
  if (command >= 84 && command <= 93)
    return QString("settingsSave%1").arg(command - 83);
  if (command >= 94 && command <= 103)
    return QString("settingsLoad%1").arg(command - 93);
  if (command >= 156 && command <= 163)
    return QString("userCommand%1").arg(command - 155);
  if (command >= 172 && command <= 175)
    return QString("quickSave%1").arg(command - 171);
  if (command >= 7 && command <= 10)
    return QStringList{"jumpFixedNext", "jumpFixedPrevious", "jumpPercentNext",
                       "jumpPercentPrevious"}[command - 7];
  if (command == 82)
    return "swapMouseButtons";
  if (command == 106)
    return "scaleEqualHeight";
  if (command >= 115 && command <= 123)
    return QStringList{"playlist", "favorite", "history"}[(command - 115) / 3] +
           QString::number((command - 115) % 3);
  return ids.value(command);
}
} // namespace
QString legacyCommandId(int command) { return legacyAction(command); }
void ReaderFeatures::executeLegacy(int command) {
  auto *action = w->command(legacyAction(command));
  if (action && action->isEnabled())
    action->trigger();
  else
    message(QCoreApplication::translate("KomaMimic", "このコマンドは現在の画面では実行できません。"));
}
void ReaderFeatures::settingsFile(bool import, const QString &provided, bool mangaMeeyaOnly) {
  QString path = provided;
  if (path.isEmpty())
    path = import
               ? QFileDialog::getOpenFileName(w, mangaMeeyaOnly ? QCoreApplication::translate("KomaMimic", "MangaMeeyaの設定を取り込む") : QCoreApplication::translate("KomaMimic", "設定をインポート"),
                                              w->directory(), QCoreApplication::translate("KomaMimic", "設定 (*.ini)"))
               : QFileDialog::getSaveFileName(w, QCoreApplication::translate("KomaMimic", "設定をエクスポート"),
                                              w->directory() + "/KomaMimic.ini",
                                              QCoreApplication::translate("KomaMimic", "設定 (*.ini)"));
  if (path.isEmpty())
    return;
  if (!import) {
    w->saveImageSettings();
    w->m_treePanel->saveSettings();
  w->saveSettings();
    QVariantMap snapshot;
    for (const auto &key : w->m_settings->allKeys())
      snapshot[key] = w->m_settings->value(key);
    QTemporaryFile temporary;
    if (!temporary.open()) {
      message(temporary.errorString());
      return;
    }
    const auto temporaryPath = temporary.fileName();
    temporary.close();
    {
      QSettings output(temporaryPath, QSettings::IniFormat);
      for (auto it = snapshot.begin(); it != snapshot.end(); ++it)
        output.setValue(it.key(), it.value());
      output.sync();
      if (output.status() != QSettings::NoError) {
        message(QCoreApplication::translate("KomaMimic", "設定を一時保存できません。"));
        return;
      }
    }
    QFile input(temporaryPath);
    QSaveFile output(path);
    if (!input.open(QIODevice::ReadOnly) ||
        !output.open(QIODevice::WriteOnly)) {
      message(QCoreApplication::translate("KomaMimic", "設定を書き出せません。"));
      return;
    }
    const auto bytes = input.readAll();
    if (output.write(bytes) != bytes.size() || !output.commit()) {
      message(output.errorString());
      return;
    }
    message(QCoreApplication::translate("KomaMimic", "設定を保存しました。"));
    return;
  }
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || file.size() > 8 * 1024 * 1024) {
    message(QCoreApplication::translate("KomaMimic", "設定ファイルを読めないか、サイズが上限を超えています。"));
    return;
  }
  const auto raw = file.readAll();
  const auto encoding = QStringConverter::encodingForData(raw).value_or(QStringConverter::Utf8);
  QStringDecoder utf8(encoding);
  QString text = utf8(raw);
  if (utf8.hasError() && encoding != QStringConverter::Utf8) {
    message(QCoreApplication::translate("KomaMimic", "設定の文字コードを解釈できません。")); return;
  }
  if (utf8.hasError()) {
    QByteArray converted(raw.size() * 4 + 16, '\0');
    UErrorCode error = U_ZERO_ERROR;
    const int size = ucnv_convert("UTF-8", "windows-932", converted.data(),
                                  int(converted.size()), raw.constData(),
                                  int(raw.size()), &error);
    if (U_FAILURE(error)) {
      message(QCoreApplication::translate("KomaMimic", "設定の文字コードを解釈できません。"));
      return;
    }
    text = QString::fromUtf8(converted.constData(), size);
  }
  QTemporaryFile temporary;
  if (!temporary.open()) {
    message(temporary.errorString());
    return;
  }
  temporary.write(text.toUtf8());
  temporary.flush();
  QSettings input(temporary.fileName(), QSettings::IniFormat);
  const auto inputKeys = input.allKeys();
  const bool own = std::any_of(inputKeys.cbegin(), inputKeys.cend(), [](const QString &key) {
    return key.startsWith("covers/") || key.startsWith("reading/") || key.startsWith("keys/") || key.startsWith("text/") ||
        key.startsWith("display/") || key.startsWith("ui/") || key.startsWith("documents/") || key.startsWith("cache/");
  });
  if (!own || mangaMeeyaOnly) { importMangaMeeyaSettings(text); return; }
  auto importedKeys = w->bindings();
  auto importedDocumentKeys = w->documentBindings();
  if (own) {
    const auto documentKeys = documentKeyBindings(input);
    for (auto it = documentKeys.cbegin(); it != documentKeys.cend(); ++it) importedDocumentKeys[it.key()] = it.value();
    for (const auto &spec : shortcutSpecs())
      if (input.contains("keys/" + spec.id))
        importedKeys[spec.id] =
            QKeySequence::fromString(input.value("keys/" + spec.id).toString(),
                                     QKeySequence::PortableText);
    auto conflict = shortcutConflict(importedKeys);
    if (conflict.isEmpty()) conflict = documentShortcutConflict(importedKeys, importedDocumentKeys);
    if (!conflict.isEmpty()) {
      QMessageBox::warning(w, QCoreApplication::translate("KomaMimic", "設定を適用できません"), conflict);
      return;
    }
  }
  int copied = 0;
  for (const auto &key : inputKeys) { w->m_settings->setValue(key, input.value(key)); ++copied; }
  if (own) {
    w->applyBindings(importedKeys, importedDocumentKeys);
    w->m_actions["spread"]->setChecked(
        w->m_settings->value("reading/spread", true).toBool());
    w->m_actions["rtl"]->setChecked(
        w->m_settings->value("reading/rtl", true).toBool());
    w->m_actions["cover"]->setChecked(
        w->m_settings->value("reading/cover", true).toBool());
    const int boundary =
        w->m_settings->value("reading/boundaryMode", 0).toInt();
    const QStringList modes{"boundaryStop", "boundaryRepeat",
                            "boundaryAdvance"};
    w->m_actions[modes[std::clamp(boundary, 0, 2)]]->trigger();
    w->m_options.leadingBlank =
        w->m_settings->value("reading/leadingBlank", false).toBool();
    w->m_actions["coverBlank"]->setChecked(w->m_options.leadingBlank);
    w->m_actions["autoSpread"]->setChecked(autoSpread());
    w->restoreImageSettings();
    w->m_view->setBackgroundColor(QColor(w->m_settings->value("display/background").toString()));
  w->restoreThumbnailSettings();
  w->m_treePanel->restoreSettings();
  applySavedSort();
  if(!w->m_directory.isEmpty())w->populateDirectory(w->m_directory,false,false);
  }
  w->restoreToolbarSettings();
  w->m_settings->sync();
  QMessageBox::information(w, QCoreApplication::translate("KomaMimic", "設定インポートの結果"),
      QCoreApplication::translate("KomaMimic", "KomaMimicの設定を%1件取り込みました。").arg(copied));
}
void ReaderFeatures::profile(int slot, bool save) {
  const auto prefix = QString("profiles/%1/").arg(slot);
  if (save) {
    w->saveImageSettings();
    for (const auto &key : w->m_settings->allKeys())
      if (key.startsWith("processing/") || key == "display/algorithm")
        w->m_settings->setValue(prefix + key, w->m_settings->value(key));
    message(QCoreApplication::translate("KomaMimic", "プロファイル%1を保存しました。").arg(slot));
    return;
  }
  bool found = false;
  for (const auto &key : w->m_settings->allKeys())
    if (key.startsWith(prefix)) {
      found = true;
      w->m_settings->setValue(key.mid(prefix.size()),
                              w->m_settings->value(key));
    }
  if (found) {
    w->restoreImageSettings();
    message(QCoreApplication::translate("KomaMimic", "プロファイル%1を読み込みました。").arg(slot));
  } else {
    ImageOptions o;
    switch (slot) {
    case 1: o.cropPercent=5;break;
    case 2: o.cropPercent=4;break; // Per-image approximation of spread clipping.
    case 3: o.rotation=1;break;
    case 4: o.rotation=2;break;
    case 5: o.rotation=3;break;
    case 6: o.resizeToHeight=1200;w->setResizeAlgorithm(ResizeAlgorithm::Lanczos3);break;
    case 7: o.autoCrop=true;break;
    case 8: o.autoCrop=true;w->m_view->setScaleMode(ScaleMode::EqualHeight);break;
    case 9: o.flipH=true;break;
    case 10: o.brightness=20;break;
    case 11: o.brightness=-16;break;
    case 12: o.sharpen=1;break;
    case 13: o.lineWidth=-1;break;
    case 14: o.lineWidth=1;break;
    case 15: o.gray=true;o.curveShadows=26;o.curveMidtones=153;o.curveHighlights=217;break;
    case 16: o.binary=true;o.threshold=200;break;
    case 17: o.cropPercent=0;break; // Binding-side margin is selected per page below.
    case 18: o.gray=true;o.invert=true;o.gamma=1.25;break;
    case 19: o.invert=true;break;
    case 20: o.gray=true;break;
    }
    if(slot==17 && !w->m_view->images().isEmpty()) {
      const int margin=int(w->m_view->images().first().width()*0.06);
      o.crop=w->m_options.rightToLeft?QMargins(0,0,margin,0):QMargins(margin,0,0,0);
    }
    w->setImageOptions(o);
    message(
        QCoreApplication::translate("KomaMimic", "標準プロファイル%"
            "1を適用しました。加工設定で調整し、同じスロットへ保存できます。")
            .arg(slot));
  }
}
void ReaderFeatures::settingsSlot(int slot, bool save) {
  const auto path =
      QFileInfo(w->m_settings->fileName())
          .absoluteDir()
          .filePath(QString("KomaMimic_%1.ini").arg(slot, 2, 10, QChar('0')));
  if (!save && !QFileInfo::exists(path)) {
    message(QCoreApplication::translate("KomaMimic", "この設定スロットはまだ保存されていません。"));
    return;
  }
  settingsFile(!save, path);
}
void ReaderFeatures::preferences(const QString &pageName) {
  const int oldCoverSort=w->m_settings->value("covers/sort",0).toInt();
  const bool oldCoverReverse=w->m_settings->value("covers/reverse",false).toBool();
  QDialog dialog(w);
  dialog.setObjectName("preferencesDialog");
  dialog.setWindowTitle(QCoreApplication::translate("KomaMimic", "環境設定"));
  dialog.resize(940, 720);
  auto *layout = new QVBoxLayout(&dialog);
  auto *tabs = new QTabWidget(&dialog);
  auto *categories = new QListWidget(&dialog); categories->setObjectName("preferencesCategories");
  auto *columns = new QHBoxLayout; layout->addLayout(columns, 1);
  columns->addWidget(categories); columns->addWidget(tabs, 1); tabs->tabBar()->hide();
  connect(categories, &QListWidget::currentRowChanged, tabs, &QTabWidget::setCurrentIndex);
  connect(tabs, &QTabWidget::currentChanged, categories, [categories](int row) { categories->setCurrentRow(row); });
  QHash<QString, QWidget *> editors;
  QVector<QFormLayout *> forms;
  const auto form = [&](const QString &id, const QString &name) {
    auto *scroll = new QScrollArea(tabs);
    scroll->setWidgetResizable(true);
    auto *page = new QWidget(scroll);
    auto *form = new QFormLayout(page);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    forms.append(form);
    scroll->setWidget(page);
    scroll->setProperty("settingsPageId", id);
    categories->addItem(name);
    tabs->addTab(scroll, name);
    return form;
  };
  const auto check = [&](QFormLayout *form, const QString &label,
                         const QString &key, bool initial) {
    auto *box = new QCheckBox(&dialog);
    box->setChecked(w->m_settings->value(key, initial).toBool());
    box->setObjectName(key);
    form->addRow(label, box);
    editors[key] = box;
  };
  const auto spin = [&](QFormLayout *form, const QString &label,
                        const QString &key, int initial, int low, int high) {
    auto *value = new QSpinBox(&dialog);
    value->setRange(low, high);
    value->setValue(w->m_settings->value(key, initial).toInt());
    value->setObjectName(key);
    form->addRow(label, value);
    editors[key] = value;
  };
  const auto choice = [&](QFormLayout *form, const QString &label,
                          const QString &key, const QStringList &choices,
                          int initial) {
    auto *box = new QComboBox(&dialog);
    box->addItems(choices);
    box->setCurrentIndex(w->m_settings->value(key, initial).toInt());
    box->setObjectName(key);
    form->addRow(label, box);
    editors[key] = box;
  };
  const auto line = [&](QFormLayout *form, const QString &label,
                        const QString &key,
                        const QString &initial = QString{}) {
    auto *edit =
        new QLineEdit(w->m_settings->value(key, initial).toString(), &dialog);
    form->addRow(label, edit);
    editors[key] = edit;
    return edit;
  };
  auto *general = form("general", QCoreApplication::translate("KomaMimic", "一般"));
  auto *language = new QComboBox(&dialog);
  language->setObjectName("ui/language");
  language->addItem(QCoreApplication::translate("KomaMimic", "システムに従う / System default"), "system");
  for (const auto &entry : I18n::languages()) language->addItem(entry.name, entry.code);
  language->setCurrentIndex(std::max(0, language->findData(I18n::savedLanguage(*w->m_settings))));
  const auto previousLanguage = language->currentData().toString();
  general->addRow(QCoreApplication::translate("KomaMimic", "表示言語 / UI language"), language);
  general->addRow(new QLabel(QCoreApplication::translate("KomaMimic", "言語の変更は次回起動時に反映します。"), &dialog));
  editors["ui/language"] = language;

  choice(general,QCoreApplication::translate("KomaMimic", "配色モード"),"display/appearance",{QCoreApplication::translate("KomaMimic", "システムに合わせる"),QCoreApplication::translate("KomaMimic", "ライト"),QCoreApplication::translate("KomaMimic", "ダーク")},0);
  editors["display/appearance"]->setToolTip(QCoreApplication::translate("KomaMimic", "メニュー・一覧・読書画面の周囲の配色を変更します。画像や本文の文字色・背景色は変更しません。"));
  check(general, QCoreApplication::translate("KomaMimic", "単一起動（次回起動から）"), "General/singleInstance", true);
  check(general, QCoreApplication::translate("KomaMimic", "読書中のスクリーンセーバー・自動消灯を抑制"), "General/inhibit", false);
  editors["General/inhibit"]->setToolTip(
    QCoreApplication::translate("KomaMimic", "KomaMimicの読書画面が前面の間だけOSへ抑制を要求します。\n"
    "最小化・他のアプリへの移動・本を閉じたときに解放します。\n"
    "macOSは自動消灯の抑制です。ロックやOSのセキュリティポリシーは変更しません。"));
  auto *inhibitStatus = new QLabel(&dialog);
  inhibitStatus->setObjectName("inhibitStatus");
  inhibitStatus->setWordWrap(true);
  inhibitStatus->setTextFormat(Qt::PlainText);
  const auto updateInhibitStatus = [this, inhibitStatus] {
    if (auto *inhibitor = idleInhibitor()) {
      auto text=inhibitor->description();
      const auto last=w->property("lastIdleInhibitionError").toString();
      if(!last.isEmpty() && inhibitor->state()!=IdleInhibitor::State::Failed)text+=QCoreApplication::translate("KomaMimic", "\n直近の失敗：")+last;
      inhibitStatus->setText(text);
    }
  };
  updateInhibitStatus();
  if (auto *inhibitor = idleInhibitor())
    connect(inhibitor, &IdleInhibitor::statusChanged, &dialog, updateInhibitStatus);
  general->addRow(QCoreApplication::translate("KomaMimic", "抑制の状態"), inhibitStatus);
  spin(general, QCoreApplication::translate("KomaMimic", "固定ページ移動の枚数"), "navigation/count", 10, 1, 100000);
  spin(general, QCoreApplication::translate("KomaMimic", "固定割合移動（%）"), "navigation/percent", 10, 1, 100);
  spin(general, QCoreApplication::translate("KomaMimic", "リストのカスタム列数"), "lists/columns", 4, 1, 20);
  auto *source = form("source", QCoreApplication::translate("KomaMimic", "読み込み"));
  check(source, QCoreApplication::translate("KomaMimic", "サブフォルダの画像も読む"), "source/recursive", false);
  check(source, QCoreApplication::translate("KomaMimic", "フォルダ内の画像書庫・PDFも連続して読む"), "source/folderContainers",
        false);
  check(source, QCoreApplication::translate("KomaMimic", "書庫内の画像書庫も連続して読む（3階層まで）"), "source/nested", false);
  check(source, QCoreApplication::translate("KomaMimic", "現在のファイルの更新を監視"), "General/watch", false);
  check(source, QCoreApplication::translate("KomaMimic", "PDFをページではなく内部画像単位で読む"), "source/pdfResources",
        false);
  auto *password = new QLineEdit(property("password").toString(), &dialog);
  password->setEchoMode(QLineEdit::Password);
  source->addRow(QCoreApplication::translate("KomaMimic", "書庫・PDFのパスワード（今回の起動中のみ）"), password);
  auto *display = form("display", QCoreApplication::translate("KomaMimic", "表示"));
  check(display, QCoreApplication::translate("KomaMimic", "読書ツールバーを表示（下部）"), "window/readerToolbar", false);
  check(display,QCoreApplication::translate("KomaMimic", "読書画面の下部にファイル名を表示"),"display/filename",false);
  check(display,QCoreApplication::translate("KomaMimic", "上下2ペインのフォルダブラウザーを使う"),"folders/twoPane",true);
  line(display,QCoreApplication::translate("KomaMimic", "画像の周囲の背景色（#RRGGBB）"),"display/background");
  check(display,QCoreApplication::translate("KomaMimic", "ホイール移動量を画像サイズの割合にする"),"input/wheelImageUnits",false);
  spin(display,QCoreApplication::translate("KomaMimic", "ホイールの移動量（%）"),"input/wheelPercent",25,1,100);
  check(display,QCoreApplication::translate("KomaMimic", "スムーズスクロール"),"input/smoothScroll",false);
  spin(display,QCoreApplication::translate("KomaMimic", "スクロール速度（px/秒）"),"input/scrollSpeed",4000,100,20000);
  spin(display,QCoreApplication::translate("KomaMimic", "キーの縦スクロール量（画像の%）"),"input/keyVerticalPercent",100,1,3000);
  check(display, QCoreApplication::translate("KomaMimic", "マウス移動でサムネイルを選択する"), "thumbnails/mouseSelection", true);
  editors["thumbnails/mouseSelection"]->setToolTip(
      QCoreApplication::translate("KomaMimic", "表紙・ページサムネイル共通。選択後のキー移動もその項目を基準にします。\n"
      "一覧にキーボードフォーカスがある間だけ追従し、開く操作や読書位置の確定は行いません。"));
  choice(display, QCoreApplication::translate("KomaMimic", "表紙一覧の並び順"), "covers/sort", {QCoreApplication::translate("KomaMimic", "名前（自然順）"), QCoreApplication::translate("KomaMimic", "更新日時"), QCoreApplication::translate("KomaMimic", "種類"), QCoreApplication::translate("KomaMimic", "サイズ"), QCoreApplication::translate("KomaMimic", "格納順"), QCoreApplication::translate("KomaMimic", "Unicode順"), QCoreApplication::translate("KomaMimic", "ランダム"), QCoreApplication::translate("KomaMimic", "名前順")}, 0);
  check(display, QCoreApplication::translate("KomaMimic", "表紙一覧を逆順にする"), "covers/reverse", false);
  check(display, QCoreApplication::translate("KomaMimic", "スクロールで端まで読んだらページをめくる"), "reading/scrollTurnsPage", true);
  spin(display, QCoreApplication::translate("KomaMimic", "スクロールの重複幅（%）"), "reading/scrollOverlap", 15, 0, 80);
  choice(display, QCoreApplication::translate("KomaMimic", "ページの表示開始位置"), "display/origin",
         {QCoreApplication::translate("KomaMimic", "中心"), QCoreApplication::translate("KomaMimic", "左上"), QCoreApplication::translate("KomaMimic", "右上"), QCoreApplication::translate("KomaMimic", "中央上"), QCoreApplication::translate("KomaMimic", "前の位置を保持")}, 0);
  check(display, QCoreApplication::translate("KomaMimic", "画像ごとのスクロール位置を記憶"), "display/rememberPan",
        false);
  choice(display, QCoreApplication::translate("KomaMimic", "ページ移動効果"), "effects/transition",
         {QCoreApplication::translate("KomaMimic", "なし"), QCoreApplication::translate("KomaMimic", "フェード"), QCoreApplication::translate("KomaMimic", "スライド")}, 0);
  spin(display, QCoreApplication::translate("KomaMimic", "移動効果の時間（ミリ秒）"), "effects/duration", 180, 50, 1500);
  check(display, QCoreApplication::translate("KomaMimic", "ページめくり音を鳴らす"), "effects/sound", false);
  line(display, QCoreApplication::translate("KomaMimic", "効果音WAV（空欄はシステム音）"), "effects/soundFile");
  check(display, QCoreApplication::translate("KomaMimic", "ページバーの方向を開き方向に合わせる"),
        "display/sliderDirection", false);
  spin(display, QCoreApplication::translate("KomaMimic", "ルーペの半径（DIP）"), "loupe/radius", 100, 20, 800);
  spin(display, QCoreApplication::translate("KomaMimic", "ルーペの倍率"), "loupe/zoom", 2, 1, 16);
  check(display, QCoreApplication::translate("KomaMimic", "丸いルーペ"), "loupe/round", true);
  choice(display, QCoreApplication::translate("KomaMimic", "全画面時のメニュー"), "fullscreen/menu",
         {QCoreApplication::translate("KomaMimic", "端でポップアップ"), QCoreApplication::translate("KomaMimic", "固定表示"), QCoreApplication::translate("KomaMimic", "非表示")}, 0);
  choice(display, QCoreApplication::translate("KomaMimic", "全画面時のツールバー"), "fullscreen/toolbar",
         {QCoreApplication::translate("KomaMimic", "端でポップアップ"), QCoreApplication::translate("KomaMimic", "固定表示"), QCoreApplication::translate("KomaMimic", "非表示")}, 0);
  choice(display, QCoreApplication::translate("KomaMimic", "全画面時のペイン"), "fullscreen/panes",
         {QCoreApplication::translate("KomaMimic", "端でポップアップ"), QCoreApplication::translate("KomaMimic", "固定表示"), QCoreApplication::translate("KomaMimic", "非表示")}, 2);
  check(display, QCoreApplication::translate("KomaMimic", "本と同名の時間付きリストを自動読込"), "slideshow/useSidecar",
        true);
  spin(display, QCoreApplication::translate("KomaMimic", "自動再生の間隔（ミリ秒）"), "slideshow/interval", 3000, 100,
       3600000);
  check(display,QCoreApplication::translate("KomaMimic", "自動再生で大きい画像をスクロールしてからめくる"),"slideshow/scroll",false);
  auto *thumbnails = form("thumbnails", QCoreApplication::translate("KomaMimic", "サムネイル"));
  check(thumbnails,QCoreApplication::translate("KomaMimic", "ページサムネイルに連番を表示"),"thumbnails/numbers",false);
  spin(thumbnails,QCoreApplication::translate("KomaMimic", "カスタム列数（0は自動）"),"thumbnails/columns",0,0,100);
  spin(thumbnails,QCoreApplication::translate("KomaMimic", "カスタムサムネイルの幅（DIP）"),"thumbnails/width",64,16,1024);
  spin(thumbnails,QCoreApplication::translate("KomaMimic", "カスタムサムネイルの高さ（DIP）"),"thumbnails/height",64,16,1024);
  spin(thumbnails,QCoreApplication::translate("KomaMimic", "横の間隔（DIP）"),"thumbnails/horizontalGap",0,0,200);
  spin(thumbnails,QCoreApplication::translate("KomaMimic", "縦の間隔（DIP）"),"thumbnails/verticalGap",0,0,200);
  spin(thumbnails,QCoreApplication::translate("KomaMimic", "ファイル名の行数"),"thumbnails/filenameLines",1,0,2);
  check(thumbnails,QCoreApplication::translate("KomaMimic", "右上から配置する"),"thumbnails/rightOrigin",false);
  check(thumbnails,QCoreApplication::translate("KomaMimic", "本・フォルダの装飾枠を表示"),"thumbnails/frames",false);
  thumbnails->addRow(new QLabel(QCoreApplication::translate("KomaMimic", "配置設定はカスタムモードの表紙・ページ一覧に共通です。連番の設定は全モードに適用します。"),&dialog));
  auto *automatic = form("automatic", QCoreApplication::translate("KomaMimic", "自動組版"));
  line(automatic, QCoreApplication::translate("KomaMimic", "見開き条件（数値式）"), "reading/spreadCondition",
       "@height>@width");
  line(automatic, QCoreApplication::translate("KomaMimic", "左右分割の条件"), "reading/splitVCondition",
       "@width>@height");
  line(automatic, QCoreApplication::translate("KomaMimic", "上下分割の条件"), "reading/splitHCondition",
       "@height>@width");
  line(automatic, QCoreApplication::translate("KomaMimic", "左右分割の位置（元画像px）"), "reading/splitVPosition",
       "@width/2");
  line(automatic, QCoreApplication::translate("KomaMimic", "上下分割の位置（元画像px）"), "reading/splitHPosition",
       "@height/2");
  spin(automatic, QCoreApplication::translate("KomaMimic", "左右分割の重複幅（px）"), "reading/splitVOverlap", 0, 0,
       4096);
  spin(automatic, QCoreApplication::translate("KomaMimic", "上下分割の重複幅（px）"), "reading/splitHOverlap", 0, 0,
       4096);
  automatic->addRow(new QLabel(
      QCoreApplication::translate("KomaMimic", "変数: @width @height @page "
      "@totalpage。四則・比較・論理・?:・min/max/abs/floor/ceil/round。"),
      &dialog));
  auto *cache = form("cache", QCoreApplication::translate("KomaMimic", "キャッシュ"));
  auto *memoryHelp=new QLabel(QCoreApplication::translate("KomaMimic", "メモリに残したデータを再利用し、読み直しを減らします。\nアプリ全体の使用メモリを制限する値ではありません。"),&dialog);
  memoryHelp->setWordWrap(true);cache->addRow(memoryHelp);
  spin(cache, QCoreApplication::translate("KomaMimic", "ファイルデータのキャッシュ容量（MiB）"), "cache/fileMegabytes", 32, 0,
       256);
  editors["cache/fileMegabytes"]->setToolTip(QCoreApplication::translate("KomaMimic", "JPEG・PNG等のファイルデータをメモリに保持します。書庫の場合は取り出した画像ファイルのデータです。0で無効。変更は次に本を開くときに適用します。"));
  spin(cache, QCoreApplication::translate("KomaMimic", "表示用画像のキャッシュ容量（MiB）"), "cache/megabytes", 128, 16, 2048);
  editors["cache/megabytes"]->setToolTip(QCoreApplication::translate("KomaMimic", "復号した画素データと先読み画像をメモリに保持します。3000×4000画素・32 bitなら約46 MiBです。容量を超えると古いキャッシュを解放して、必要なときに読み直します。"));
  spin(cache, QCoreApplication::translate("KomaMimic", "前方向の先読み（表示単位）"), "cache/ahead", 2, 0, 20);
  spin(cache, QCoreApplication::translate("KomaMimic", "逆方向の先読み（表示単位）"), "cache/behind", 1, 0, 20);
  editors["cache/ahead"]->setToolTip(QCoreApplication::translate("KomaMimic", "進んでいる方向のページを先に読み込みます。見開きなら1表示単位は通常2ページ。0で無効。"));
  editors["cache/behind"]->setToolTip(QCoreApplication::translate("KomaMimic", "進んでいる方向とは反対側のページを先に読み込みます。0で無効。"));
  check(cache,QCoreApplication::translate("KomaMimic", "サムネイルをディスクに保存する"),"cache/thumbnailsEnabled",true);
  editors["cache/thumbnailsEnabled"]->setToolTip(QCoreApplication::translate("KomaMimic", "表紙・ページサムネイルを保存し、次回起動時も再利用します。無効にしても保存済み画像は残ります。削除ボタンで消せます。"));
  spin(cache,QCoreApplication::translate("KomaMimic", "サムネイルのディスク使用量の上限（MiB）"),"cache/thumbnailMegabytes",512,16,16384);
  auto *diskLimit=editors["cache/thumbnailMegabytes"];
  auto *diskEnabled=qobject_cast<QCheckBox *>(editors["cache/thumbnailsEnabled"]);
  diskLimit->setEnabled(diskEnabled->isChecked());
  connect(diskEnabled,&QCheckBox::toggled,diskLimit,&QWidget::setEnabled);
  diskLimit->setToolTip(QCoreApplication::translate("KomaMimic", "保存したPNGの合計容量です。上限を超える前に、最近使っていないサムネイルから削除します。設定ファイルや本の原本は削除しません。"));
  auto *diskHelp=new QLabel(QCoreApplication::translate("KomaMimic", "表示した分だけ保存します。元ファイル変更時は作り直します。\nF6の再取得では保存済み画像を使わず再生成します。\nサムネイル画像はディスクに残るため、共有PCでは無効化・削除してください。"),&dialog);
  diskHelp->setWordWrap(true);cache->addRow(diskHelp);
  auto *diskPath=new QLabel(w->m_thumbnailDiskCache->directory(),&dialog);
  diskPath->setTextFormat(Qt::PlainText);
  diskPath->setWordWrap(true);diskPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
  cache->addRow(QCoreApplication::translate("KomaMimic", "サムネイルの保存先"),diskPath);
  auto *usage=new QLabel(QCoreApplication::translate("KomaMimic", "使用量を確認中…"),&dialog);usage->setObjectName("thumbnailCacheUsage");cache->addRow(QCoreApplication::translate("KomaMimic", "ディスク使用量"),usage);
  auto *clearDisk=new QPushButton(QCoreApplication::translate("KomaMimic", "保存済みサムネイルを削除"),&dialog);clearDisk->setObjectName("clearThumbnailDiskCache");cache->addRow(clearDisk);
  const auto refreshUsage=[&,disk=w->m_thumbnailDiskCache] {
    auto *watcher=new QFutureWatcher<ThumbnailDiskCache::Stats>(&dialog);
    connect(watcher,&QFutureWatcherBase::finished,&dialog,[watcher,usage] {
      const auto result=watcher->result();watcher->deleteLater();
      usage->setText(result.error.isEmpty()?QCoreApplication::translate("KomaMimic", "%1 MiB / %2枚").arg(result.bytes/(1024.0*1024),0,'f',1).arg(result.entries):result.error);
    });
    watcher->setFuture(QtConcurrent::run([disk]{return disk->stats();}));
  };
  refreshUsage();
  connect(clearDisk,&QPushButton::clicked,&dialog,[&,disk=w->m_thumbnailDiskCache,refreshUsage] {
    clearDisk->setEnabled(false);usage->setText(QCoreApplication::translate("KomaMimic", "削除中…"));
    auto *watcher=new QFutureWatcher<QString>(&dialog);
    connect(watcher,&QFutureWatcherBase::finished,&dialog,[watcher,clearDisk,usage,refreshUsage] {
      const auto error=watcher->result();watcher->deleteLater();clearDisk->setEnabled(true);
      if(error.isEmpty()) refreshUsage();else usage->setText(error);
    });
    watcher->setFuture(QtConcurrent::run([disk]{return disk->clear();}));
  });
  auto *scripts=form("scripts", QCoreApplication::translate("KomaMimic", "スクリプト"));
  auto *scriptHelp=new QLabel(QCoreApplication::translate("KomaMimic", "画像読込・複数ページ・画像フィルターはAPI 1を使います。\n登録フォルダの追加・削除は保存して再起動すると反映します。"),&dialog);
  scriptHelp->setWordWrap(true);scripts->addRow(scriptHelp);
  auto *directories=new QListWidget(&dialog);directories->setObjectName("scriptDirectories");directories->setMaximumHeight(96);
  directories->addItems(w->m_settings->value("scripts/directories").toStringList());
  scripts->addRow(QCoreApplication::translate("KomaMimic", "追加スクリプトのフォルダ"),directories);
  auto *packagePath=new QLineEdit(&dialog);packagePath->setObjectName("scriptPackagePath");
  scripts->addRow(QCoreApplication::translate("KomaMimic", "登録するフォルダ"),packagePath);
  auto *controls=new QWidget(&dialog);auto *row=new QHBoxLayout(controls);row->setContentsMargins(0,0,0,0);
  auto *browse=new QPushButton(QCoreApplication::translate("KomaMimic", "参照…"),controls);
  auto *addPackage=new QPushButton(QCoreApplication::translate("KomaMimic", "検査して追加"),controls);addPackage->setObjectName("addScriptDirectory");
  auto *removePackage=new QPushButton(QCoreApplication::translate("KomaMimic", "選択フォルダを削除"),controls);removePackage->setObjectName("removeScriptDirectory");
  row->addWidget(browse);row->addWidget(addPackage);row->addWidget(removePackage);scripts->addRow(controls);
  auto *registryError=new QLabel(&dialog);registryError->setObjectName("scriptRegistryError");registryError->setTextFormat(Qt::PlainText);registryError->setWordWrap(true);scripts->addRow(registryError);
  auto *packages=new QListWidget(&dialog);packages->setObjectName("scriptPackages");packages->setMinimumHeight(120);packages->setMaximumHeight(180);
  scripts->addRow(QCoreApplication::translate("KomaMimic", "登録内容（選択すると設定を表示）"),packages);
  auto *packageDetails=new QLabel(&dialog);packageDetails->setObjectName("scriptPackageDetails");packageDetails->setTextFormat(Qt::PlainText);packageDetails->setWordWrap(true);scripts->addRow(packageDetails);
  check(scripts,QCoreApplication::translate("KomaMimic", "画像加工を有効"),"processing/filterEnabled",true);
  auto *filterBox=new QComboBox(&dialog);filterBox->setObjectName("scripts/filter");
  scripts->addRow(QCoreApplication::translate("KomaMimic", "画像フィルター"),filterBox);editors["scripts/filter"]=filterBox;
  auto *parameterWidget=new QWidget(&dialog);auto *parameters=new QFormLayout(parameterWidget);scripts->addRow(parameterWidget);
  QVariantMap pendingScriptValues;
  QString selectedFilter=configuredScriptFilter(w->m_settings->value("scripts/filter"));
  const auto directoryPaths=[directories] {
    QStringList result;for(int i=0;i<directories->count();++i)result.append(directories->item(i)->text());return result;
  };
  const auto previewPaths=[&] { return directoryPaths()+qApp->property("scriptCommandLineDirectories").toStringList(); };
  const auto stashParameters=[&] {
    for(auto i=editors.cbegin();i!=editors.cend();++i) if(i.key().startsWith("scripts/") && i.key()!="scripts/filter") {
      if(auto *box=qobject_cast<QCheckBox *>(i.value()))pendingScriptValues[i.key()]=box->isChecked();
      else if(auto *number=qobject_cast<QDoubleSpinBox *>(i.value()))pendingScriptValues[i.key()]=number->value();
    }
  };
  const auto showParameters=[&] {
    stashParameters();
    for(auto i=editors.begin();i!=editors.end();) {
      if(i.key().startsWith("scripts/") && i.key()!="scripts/filter")i=editors.erase(i);else ++i;
    }
    while(parameters->rowCount())parameters->removeRow(0);
    auto *item=packages->currentItem();if(!item){packageDetails->clear();return;}
    const auto package=item->data(Qt::UserRole).toMap();
    packageDetails->setText(package.value("path").toString());
    const auto schema=package.value("settings").toMap();
    for(auto field=schema.cbegin();field!=schema.cend();++field) {
      const auto spec=field.value().toMap();const auto key="scripts/"+package.value("id").toString()+"/"+field.key();
      const auto value=pendingScriptValues.value(key,w->m_settings->value(key,spec.value("default")));
      if(spec.value("type")=="boolean") {
        auto *box=new QCheckBox(parameterWidget);box->setChecked(value.toBool());box->setObjectName(key);parameters->addRow(field.key(),box);editors[key]=box;
      } else {
        auto *number=new QDoubleSpinBox(parameterWidget);number->setDecimals(spec.value("type")=="integer"?0:6);
        number->setRange(spec.value("minimum",-1000000).toDouble(),spec.value("maximum",1000000).toDouble());
        number->setValue(value.toDouble());number->setObjectName(key);parameters->addRow(field.key(),number);editors[key]=number;
      }
    }
  };
  const auto rebuildPackages=[&] {
    stashParameters();
    QVariantList registry;const auto error=inspectScriptReaders(previewPaths(),registry);
    registryError->setText(error);
    if(!error.isEmpty())return false;
    const auto previous=packages->currentItem()?packages->currentItem()->data(Qt::UserRole).toMap().value("id").toString():QString{};
    selectedFilter=filterBox->currentIndex()<0?selectedFilter:filterBox->currentData().toString();
    {const QSignalBlocker blocker(packages);packages->clear();
      filterBox->clear();filterBox->addItem(QCoreApplication::translate("KomaMimic", "なし"),QString{});
      for(const auto &value:registry) {
        const auto package=value.toMap();const auto id=package.value("id").toString();
        auto *item=new QListWidgetItem(id+" — "+package.value("kind").toString()+" / API 1",packages);item->setData(Qt::UserRole,package);
        if(id==previous)packages->setCurrentItem(item);
        if(package.value("kind")=="filter")filterBox->addItem(id,id);
      }
      if(filterBox->findData(selectedFilter)<0)filterBox->addItem(QCoreApplication::translate("KomaMimic", "未登録：")+selectedFilter,selectedFilter);
      filterBox->setCurrentIndex(filterBox->findData(selectedFilter));
      if(!packages->currentItem() && packages->count())packages->setCurrentRow(0);
    }
    showParameters();return true;
  };
  connect(packages,&QListWidget::currentRowChanged,&dialog,[&](int){showParameters();});
  connect(browse,&QPushButton::clicked,&dialog,[&] {
    const auto path=QFileDialog::getExistingDirectory(&dialog,QCoreApplication::translate("KomaMimic", "スクリプトのフォルダ"));
    if(!path.isEmpty())packagePath->setText(path);
  });
  connect(addPackage,&QPushButton::clicked,&dialog,[&] {
    const auto path=QFileInfo(packagePath->text().trimmed()).absoluteFilePath();
    if(packagePath->text().trimmed().isEmpty() || !directories->findItems(path,Qt::MatchExactly).isEmpty())return;
    directories->addItem(path);
    if(!rebuildPackages())delete directories->takeItem(directories->count()-1);
  });
  connect(removePackage,&QPushButton::clicked,&dialog,[&] {
    delete directories->takeItem(directories->currentRow());rebuildPackages();
  });
  rebuildPackages();
  auto *namingForm = form("naming", QCoreApplication::translate("KomaMimic", "書き出し"));
  choice(namingForm,QCoreApplication::translate("KomaMimic", "一括出力名"),"output/naming",{QCoreApplication::translate("KomaMimic", "連番"),QCoreApplication::translate("KomaMimic", "元ファイル名"),QCoreApplication::translate("KomaMimic", "元ファイル名＋連番")},0);
  line(namingForm,QCoreApplication::translate("KomaMimic", "名前の先頭文字"),"output/prefix");
  spin(namingForm,QCoreApplication::translate("KomaMimic", "連番の桁数"),"output/digits",6,1,9);
  spin(namingForm,QCoreApplication::translate("KomaMimic", "連番の開始値"),"output/start",1,0,1000000000);
  choice(namingForm,QCoreApplication::translate("KomaMimic", "一括画像形式"),"output/format",{"PNG","JPEG","WebP","TIFF"},0);
  choice(namingForm,QCoreApplication::translate("KomaMimic", "同名ファイル"),"output/conflict",{QCoreApplication::translate("KomaMimic", "中止"),QCoreApplication::translate("KomaMimic", "スキップ"),QCoreApplication::translate("KomaMimic", "上書き（出力前に確認）")},0);
  check(namingForm,QCoreApplication::translate("KomaMimic", "加工画像のICC色プロファイルを保持"),"output/keepColorProfile",true);
  namingForm->addRow(new QLabel(QCoreApplication::translate("KomaMimic", "元画像の保存はメタデータを含め原本を保持。加工出力は文字・EXIF・XMPを引き継ぎません。"), &dialog));
  auto *text = form("text", QCoreApplication::translate("KomaMimic", "テキスト"));
  check(text, QCoreApplication::translate("KomaMimic", "青空文庫形式のルビ（｜漢字《よみ》）"), "text/ruby", true);
  check(text, QCoreApplication::translate("KomaMimic", "縦書き"), "text/vertical", false);
  spin(text, QCoreApplication::translate("KomaMimic", "文字サイズ（px）"), "text/size", 22, 8, 120);
  spin(text, QCoreApplication::translate("KomaMimic", "行間／縦書きの列間（px）"), "text/spacing", 8, 0, 80);
  line(text, QCoreApplication::translate("KomaMimic", "文字色（#RRGGBB）"), "text/foreground", "#000000");
  line(text, QCoreApplication::translate("KomaMimic", "背景色（#RRGGBB）"), "text/background", "#fffdf4");
  spin(text, QCoreApplication::translate("KomaMimic", "余白（px）"), "text/margin", 60, 0, 300);
  auto *font =
      new QLineEdit(w->m_settings->value("text/font").toString(), &dialog);
  text->addRow(QCoreApplication::translate("KomaMimic", "フォント名"), font);
  editors["text/font"] = font;
  auto *encoding = new QComboBox(&dialog);
  encoding->addItems({"UTF-8", "CP932", "CP437", "GBK", "CP949", "Big5"});
  encoding->setCurrentText(
      w->m_settings->value("text/encoding", "UTF-8").toString());
  text->addRow(QCoreApplication::translate("KomaMimic", "文字コード（UTF-8の不正列はCP932へ）"), encoding);
  auto *network = form("network", QCoreApplication::translate("KomaMimic", "通信"));
  spin(network, QCoreApplication::translate("KomaMimic", "ダウンロードの制限（MiB）"), "network/maxMiB", 2048, 1, 8192);
  spin(network, QCoreApplication::translate("KomaMimic", "タイムアウト（秒）"), "network/timeout", 30, 5, 600);
  line(network, "User-Agent", "network/userAgent", "KomaMimic/" KOMAMIMIC_VERSION);
  auto *output = form("output", QCoreApplication::translate("KomaMimic", "出力"));
  choice(output, QCoreApplication::translate("KomaMimic", "一括保存・ZIP/PDF/GIFの対象"), "output/range",
         {QCoreApplication::translate("KomaMimic", "全ページ"), QCoreApplication::translate("KomaMimic", "選択した範囲"), QCoreApplication::translate("KomaMimic", "現在表示中")}, 0);
  check(output, QCoreApplication::translate("KomaMimic", "一括出力は2頁を1枚に結合（アニメは先頭フレーム）"),
        "output/spread", false);
  spin(output, QCoreApplication::translate("KomaMimic", "JPEG/WebPの品質"), "output/quality", 90, 1, 100);
  auto *buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  layout->addWidget(buttons);
  auto *resetDefaults=buttons->addButton(QCoreApplication::translate("KomaMimic", "この画面のマンガ用初期値に戻す"),QDialogButtonBox::ResetRole);
  resetDefaults->setObjectName("resetMangaPreferences");
  connect(resetDefaults,&QPushButton::clicked,&dialog,[&] {
    const auto values=mangaPreferenceDefaults();
    for(auto i=values.cbegin();i!=values.cend();++i)if(auto *editor=editors.value(i.key())) {
      if(auto *box=qobject_cast<QCheckBox *>(editor))box->setChecked(i.value().toBool());
      else if(auto *number=qobject_cast<QSpinBox *>(editor))number->setValue(i.value().toInt());
      else if(auto *text=qobject_cast<QLineEdit *>(editor))text->setText(i.value().toString());
    }
  });
  connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
    QVariantList checked;const auto error=inspectScriptReaders(previewPaths(),checked);
    registryError->setText(error);
    if(error.isEmpty())dialog.accept();
    else for(int i=0;i<tabs->count();++i)if(tabs->widget(i)->property("settingsPageId")=="scripts")tabs->setCurrentIndex(i);
  });
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  for (auto *label : dialog.findChildren<QLabel *>()) label->setWordWrap(true);
  for (auto *form : forms) for (int row = 0; row < form->rowCount(); ++row) {
    auto *item = form->itemAt(row, QFormLayout::LabelRole);
    if (item) if (auto *label = qobject_cast<QLabel *>(item->widget())) {
      label->setFixedWidth(240);
      label->setMinimumHeight(label->fontMetrics().boundingRect(QRect(0, 0, 240, 2000), Qt::TextWordWrap, label->text()).height() + 2);
    }
  }
  int categoryWidth = 150;
  for (int i = 0; i < categories->count(); ++i)
    categoryWidth = std::max(categoryWidth, categories->fontMetrics().horizontalAdvance(categories->item(i)->text()) + 32);
  categories->setFixedWidth(std::min(categoryWidth, 240));
  categories->setCurrentRow(0);
  for (int i = 0; i < tabs->count(); ++i)
    if (tabs->widget(i)->property("settingsPageId").toString() == pageName) tabs->setCurrentIndex(i);
  if (dialog.exec() != QDialog::Accepted)
    return;
  for (const auto &key : {"reading/spreadCondition", "reading/splitVCondition",
                          "reading/splitHCondition", "reading/splitVPosition",
                          "reading/splitHPosition"}) {
    const auto expression = evaluateExpression(
        qobject_cast<QLineEdit *>(editors[key])->text(),
        {{"width", 1200}, {"height", 1600}, {"page", 1}, {"totalpage", 100}});
    if (!expression.error.isEmpty()) {
      QMessageBox::warning(w, QCoreApplication::translate("KomaMimic", "式が不正です"),
                           QString(key) + ": " + expression.error);
      return;
    }
  }
  QVariantList checkedScripts;
  const auto registrationError=inspectScriptReaders(previewPaths(),checkedScripts);
  if(!registrationError.isEmpty()) { message(registrationError); return; }
  stashParameters();
  for(auto i=pendingScriptValues.cbegin();i!=pendingScriptValues.cend();++i)w->m_settings->setValue(i.key(),i.value());
  w->m_settings->setValue("scripts/directories",directoryPaths());
  for (auto it = editors.cbegin(); it != editors.cend(); ++it) {
    QVariant value;
    if (auto *box = qobject_cast<QCheckBox *>(it.value()))
      value = box->isChecked();
    else if (auto *spin = qobject_cast<QSpinBox *>(it.value()))
      value = spin->value();
    else if (auto *number = qobject_cast<QDoubleSpinBox *>(it.value()))
      value = number->value();
    else if (auto *choice = qobject_cast<QComboBox *>(it.value()))
      value = (it.key()=="scripts/filter" || it.key()=="ui/language") ? choice->currentData() : QVariant(choice->currentIndex());
    else if (auto *edit = qobject_cast<QLineEdit *>(it.value()))
      value = edit->text();
    w->m_settings->setValue(it.key(), value);
  }
  w->m_settings->setValue("text/encoding", encoding->currentText());
  auto imageOptions=w->m_imageOptions;imageOptions.enabled=w->m_settings->value("processing/filterEnabled",true).toBool();
  w->setImageOptions(imageOptions);
  setProperty("password", password->text());
  w->m_settings->sync();
  setApplicationAppearance(w->m_settings->value("display/appearance",0).toInt());
  w->m_actions["toolbarToggle"]->setChecked(w->m_settings->value("window/readerToolbar", false).toBool());
  if (w->isFullScreen()) w->setFullscreenChromeVisible(w->m_topChromeVisible, w->m_bottomChromeVisible);
  w->m_thumbnailDiskCache->configure(w->m_settings->value("cache/thumbnailsEnabled",true).toBool(),w->m_settings->value("cache/thumbnailMegabytes",512).toInt());
  (void)QtConcurrent::run([disk=w->m_thumbnailDiskCache,stop=w->m_backgroundCancel]{disk->maintain([stop]{return stop->load();});});
  w->m_view->setBackgroundColor(QColor(w->m_settings->value("display/background").toString()));
  w->restoreThumbnailSettings();
  if (!w->m_directory.isEmpty() && (oldCoverSort!=w->m_settings->value("covers/sort",0).toInt()
      || oldCoverReverse!=w->m_settings->value("covers/reverse",false).toBool()))
    w->populateDirectory(w->m_directory, false, false);
  if(oldCoverSort!=w->m_settings->value("covers/sort",0).toInt() || oldCoverReverse!=w->m_settings->value("covers/reverse",false).toBool())applySavedSort();
  updateInhibit();
  QTimer::singleShot(0, this, [this] { updateInhibit(); });
  w->m_imageCache.setMaxCost(
      w->m_settings->value("cache/megabytes", 128).toInt() * 1024);
  message(
      QCoreApplication::translate("KomaMimic", "環境設定を保存しました。読み込み設定は次に本を開くときに適用します。"));
  if (language->currentData().toString() != previousLanguage) {
    QMessageBox notice(QMessageBox::Information,
        QCoreApplication::translate("KomaMimic", "再起動が必要です"),
        QCoreApplication::translate("KomaMimic", "表示言語の変更は次回起動時に反映されます。\nKomaMimicを終了し、起動し直してください。"),
        QMessageBox::Ok, w);
    notice.setObjectName("languageRestartNotice");
    notice.exec();
  }
}
void ReaderFeatures::toolbarSettings() { w->m_toolbar->configure(*w->m_settings); }
bool ReaderFeatures::eventFilter(QObject *object, QEvent *event) {
  if (event->type() == QEvent::Resize) {
    auto *list = qobject_cast<QListWidget *>(object);
    if (list && m_lists.values().contains(list) &&
        list->viewMode() == QListView::IconMode) {
      const int layout = list->property("coverLayout").toInt();
      const int columns =
          layout == 0   ? std::max(1, int(std::ceil(std::sqrt(
                                          double(std::max(1, list->count()))))))
          : layout == 1 ? 8
                        : w->m_settings->value("lists/columns", 4).toInt();
      const int width =
          std::max(16, list->viewport()->width() / std::max(1, columns));
      list->setGridSize({width, width * 3 / 2 + 48});
      list->setIconSize(
          {std::max(16, width - 16), std::max(24, width * 3 / 2)});
      QTimer::singleShot(0, this, &ReaderFeatures::requestCollectionCover);
    }
  }

  return QObject::eventFilter(object, event);
}
