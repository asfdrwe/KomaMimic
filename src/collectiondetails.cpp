// SPDX-License-Identifier: GPL-3.0-only
#include "mainwindow.h"
#include <QCoreApplication>
#include "pageview.h"
#include "readerfeatures.h"
#include "zipmetadata.h"
#include <QAction>
#include <QBuffer>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent>
namespace {
class Row : public QTreeWidgetItem {
public:
  using QTreeWidgetItem::QTreeWidgetItem;
  bool operator<(const QTreeWidgetItem &other) const override {
    const auto column = treeWidget()->sortColumn();
    if (column >= 2 && column <= 4)
      return data(column, Qt::UserRole).toInt() <
             other.data(column, Qt::UserRole).toInt();
    return naturalLess(text(column), other.text(column));
  }
};
} // namespace
void ReaderFeatures::listDetails() {
  const auto kind = m_lists.key(
      qobject_cast<QListWidget *>(m_tabs->currentWidget()), "playlist");
  QDialog dialog(w);
  dialog.setWindowTitle(QCoreApplication::translate("KomaMimic", "ブックリストの詳細"));
  dialog.resize(1000, 600);
  auto *layout = new QVBoxLayout(&dialog);
  auto *table = new QTreeWidget(&dialog);
  table->setRootIsDecorated(false);
  table->setAlternatingRowColors(true);
  table->setHeaderLabels({QCoreApplication::translate("KomaMimic", "タイトル"), QCoreApplication::translate("KomaMimic", "パス"), QCoreApplication::translate("KomaMimic", "しおり"), QCoreApplication::translate("KomaMimic", "総頁"), QCoreApplication::translate("KomaMimic", "マーカー"),
                          QCoreApplication::translate("KomaMimic", "著者"), QCoreApplication::translate("KomaMimic", "出版社"), QCoreApplication::translate("KomaMimic", "巻"), QCoreApplication::translate("KomaMimic", "発売日"), QCoreApplication::translate("KomaMimic", "価格"), QCoreApplication::translate("KomaMimic", "備考")});
  table->header()->setSectionsMovable(true);
  table->header()->setContextMenuPolicy(Qt::CustomContextMenu);
  layout->addWidget(table);
  connect(table->header(), &QWidget::customContextMenuRequested, &dialog,
          [table](const QPoint &point) {
            QMenu menu;
            for (int i = 0; i < table->columnCount(); ++i) {
              auto *action = menu.addAction(table->headerItem()->text(i));
              action->setCheckable(true);
              action->setChecked(!table->isColumnHidden(i));
              QObject::connect(action, &QAction::toggled, table,
                               [table, i](bool shown) {
                                 table->setColumnHidden(i, !shown);
                               });
            }
            menu.exec(table->header()->mapToGlobal(point));
          });
  for (const auto &item : m_library->items(kind)) {
    auto *row = new Row(table);
    row->setData(0, Qt::UserRole, item.id);
    row->setText(0, item.title);
    row->setText(1, item.path);
    for (int col = 2; col <= 4; ++col) {
      const int value =
          item.state.value(col == 2   ? "anchor"
                           : col == 3 ? "total"
                                      : "marker")
              .toInt() +
          (col == 3 ? 0 : 1);
      row->setText(col, QString::number(value));
      row->setData(col, Qt::UserRole, value);
    }
    int col = 5;
    for (const auto &key :
         {"author", "publisher", "volume", "date", "price", "notes"})
      row->setText(col++, item.metadata.value(key).toVariant().toString());
  }
  table->setSortingEnabled(true);
  table->header()->restoreState(
      w->m_settings->value("lists/" + kind + "/columns").toByteArray());
  connect(table, &QTreeWidget::itemActivated, &dialog,
          [this, &dialog](QTreeWidgetItem *row, int) {
            const auto id = row->data(0, Qt::UserRole).toString();
            dialog.accept();
            QTimer::singleShot(0, this, [this, id] { openCollection(id); });
          });
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  dialog.exec();
  w->m_settings->setValue("lists/" + kind + "/columns",
                          table->header()->saveState());
}
void ReaderFeatures::zipMetadata(bool save) {
  const auto source = w->bookPath();
  const auto suffix = QFileInfo(source).suffix().toLower();
  if (suffix != "zip" && suffix != "cbz") {
    message(QCoreApplication::translate("KomaMimic", "ZIP/CBZを開いてください。"));
    return;
  }
  auto item = m_library->item(m_activeId);
  if (item.id.isEmpty())
    return;
  if (!save) {
    const auto metadata = readZipMetadata(source);
    if (!metadata.error.isEmpty()) {
      message(metadata.error);
      return;
    }
    if (metadata.data.value("format").toString() != "KomaMimic") {
      message(QCoreApplication::translate("KomaMimic", "KomaMimic形式のメタデータがありません。既存の独自コメントは保持"
              "します。"));
      return;
    }
    item.metadata = metadata.data.value("metadata").toObject();
    const auto state = metadata.data.value("state").toObject();
    for (const auto &key : {"toc", "marker", "markerKey"})
      if (state.contains(key))
        item.state[key] = state.value(key);
    m_library->update(item.id, item.metadata, item.state);
    m_collectionIcons.remove(item.id);
    m_tocEntries = item.state.value("toc").toArray();
    rebuildLists();
    message(QCoreApplication::translate("KomaMimic", "メタデータを取り込みました。目次の一覧は開き直すと反映します。"));
    return;
  }
  const auto target = QFileDialog::getSaveFileName(
      w, QCoreApplication::translate("KomaMimic", "書誌情報付きZIPを保存"),
      QFileInfo(source).absolutePath() + "/" +
          QFileInfo(source).completeBaseName() + "-metadata.zip",
      "ZIP (*.zip *.cbz)");
  if (target.isEmpty())
    return;
  QJsonObject metadata = item.metadata;
  metadata.remove("coverPng");
  metadata.remove("spinePng");
  metadata.remove("legacyColumns");
  metadata.remove("legacyLine");
  const QJsonObject data{
      {"format", "KomaMimic"},
      {"version", 1},
      {"metadata", metadata},
      {"state", QJsonObject{{"toc", m_tocEntries},
                            {"marker", item.state.value("marker")},
                            {"markerKey", item.state.value("markerKey")}}}};
  auto *watcher = new QFutureWatcher<QString>(this);
  connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
    const auto error = watcher->result();
    watcher->deleteLater();
    message(error.isEmpty() ? QCoreApplication::translate("KomaMimic", "メタデータ付きZIPを保存しました。") : error);
  });
  watcher->setFuture(QtConcurrent::run([source, target, data] {
    return writeZipMetadata(source, target, data);
  }));
}
void ReaderFeatures::registerBookImage(bool spine) {
  const auto images = w->m_view->images();
  if (images.isEmpty() || m_activeId.isEmpty())
    return;
  const auto image = images.first();
  const QString field = spine ? "spine" : "cover";
  QDialog dialog(w);
  dialog.setWindowTitle(spine ? QCoreApplication::translate("KomaMimic", "背表紙の範囲を登録") : QCoreApplication::translate("KomaMimic", "表紙の範囲を登録"));
  dialog.resize(500, 600);
  auto *layout = new QVBoxLayout(&dialog);
  auto *form = new QFormLayout;
  layout->addLayout(form);
  auto *x = new QSpinBox(&dialog), *y = new QSpinBox(&dialog),
       *width = new QSpinBox(&dialog), *height = new QSpinBox(&dialog);
  x->setRange(0, image.width() - 1);
  y->setRange(0, image.height() - 1);
  width->setRange(1, image.width());
  height->setRange(1, image.height());
  width->setValue(spine ? std::max(1, image.width() / 12) : image.width());
  height->setValue(image.height());
  x->setValue(spine && w->m_options.rightToLeft ? image.width() - width->value()
                                                : 0);
  form->addRow(QCoreApplication::translate("KomaMimic", "左からの位置（px）"), x);
  form->addRow(QCoreApplication::translate("KomaMimic", "上からの位置（px）"), y);
  form->addRow(QCoreApplication::translate("KomaMimic", "幅（px）"), width);
  form->addRow(QCoreApplication::translate("KomaMimic", "高さ（px）"), height);
  auto *preview = new QLabel(&dialog);
  preview->setAlignment(Qt::AlignCenter);
  layout->addWidget(preview, 1);
  const auto rectangle = [=] {
    return QRect(x->value(), y->value(), width->value(), height->value())
        .intersected(image.rect());
  };
  const auto refresh = [=] {
    preview->setPixmap(
        QPixmap::fromImage(image.copy(rectangle())
                               .scaled({300, 360}, Qt::KeepAspectRatio,
                                       Qt::SmoothTransformation)));
  };
  for (auto *spin : {x, y, width, height})
    connect(spin, &QSpinBox::valueChanged, &dialog, refresh);
  refresh();
  auto *buttons = new QDialogButtonBox(
      QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  if (dialog.exec() != QDialog::Accepted)
    return;
  auto item = m_library->item(m_activeId);
  const auto crop = rectangle();
  QByteArray bytes;
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::WriteOnly);
  image.copy(crop)
      .scaled({320, 480}, Qt::KeepAspectRatio, Qt::SmoothTransformation)
      .save(&buffer, "PNG");
  item.metadata[field] = w->m_anchor;
  item.metadata[field + "Key"] = currentPositionKey();
  item.metadata[field + "Png"] = QString::fromLatin1(bytes.toBase64());
  item.metadata[field + "X"] = crop.x();
  item.metadata[field + "Y"] = crop.y();
  item.metadata[field + "Width"] = crop.width();
  item.metadata[field + "Height"] = crop.height();
  m_library->update(item.id, item.metadata, item.state);
  m_collectionIcons.remove(item.id);
  rebuildLists();
}
void ReaderFeatures::trashPages() {
  if (!w->m_book || !w->m_book->filesystem) {
    message(QCoreApplication::translate("KomaMimic", "画像フォルダの実ファイルだけが対象です。書庫のページ削除にはページ"
            "編集を使ってください。"));
    return;
  }
  QStringList paths;
  for (int index : selectedPages()) {
    const auto &page = w->m_book->pages[index];
    if (!page.inlineData.isEmpty() || page.document ||
        !isImagePath(page.filePath))
      continue;
    const QFileInfo file(page.filePath);
    if (file.size() != page.encodedSize ||
        file.lastModified().toMSecsSinceEpoch() != page.modifiedMs) {
      message(QCoreApplication::translate("KomaMimic", "変更された画像があるため中止しました。開き直してください。"));
      return;
    }
    if (!paths.contains(page.filePath))
      paths.append(page.filePath);
  }
  if (paths.isEmpty())
    return;
  QMessageBox question(QMessageBox::Question, QCoreApplication::translate("KomaMimic", "元画像をごみ箱へ"),
                       QCoreApplication::translate("KomaMimic", "次の実ファイルをごみ箱へ移動します。\n編集のUndoでは戻"
                       "せません。OSのごみ箱から復元できます。"),
                       QMessageBox::Yes | QMessageBox::Cancel, w);
  question.setDetailedText(paths.join('\n'));
  question.setDefaultButton(QMessageBox::Cancel);
  if (question.exec() != QMessageBox::Yes)
    return;
  QStringList errors;
  for (const auto &path : paths)
    if (!QFile::moveToTrash(path))
      errors.append(path);
  w->loadBook(w->bookPath(), MainWindow::ViewMode::Reader);
  if (!errors.isEmpty())
    message(QCoreApplication::translate("KomaMimic", "ごみ箱へ移せなかった画像:\n") + errors.join('\n'));
}
