// SPDX-License-Identifier: GPL-3.0-only
#include "mainwindow.h"
#include <QCoreApplication>
#include "scriptreader.h"
#include "pageview.h"
#include "readerfeatures.h"
#include "thumbnailcache.h"
#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPageSetupDialog>
#include <QPainter>
#include <QPrintDialog>
#include <QPrinter>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringDecoder>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QUndoStack>
#include <QUuid>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>
#include <numeric>
#include <unicode/ucnv.h>
void ReaderFeatures::cacheInformation() {
  QDialog dialog(w);dialog.setObjectName("cacheInfoDialog");dialog.setWindowTitle(QCoreApplication::translate("KomaMimic", "キャッシュ情報"));dialog.resize(820,460);
  auto *layout=new QVBoxLayout(&dialog);
  auto *heading=new QLabel(QCoreApplication::translate("KomaMimic", "メモリはこのウィンドウ、ディスクはアプリ共通の保存分です。"),&dialog);
  heading->setWordWrap(true);layout->addWidget(heading);
  auto *table=new QTableWidget(4,5,&dialog);table->setObjectName("cacheInfoTable");
  table->setHorizontalHeaderLabels({QCoreApplication::translate("KomaMimic", "キャッシュの種類"),QCoreApplication::translate("KomaMimic", "使用量"),QCoreApplication::translate("KomaMimic", "上限"),QCoreApplication::translate("KomaMimic", "件数"),QCoreApplication::translate("KomaMimic", "状態")});
  table->verticalHeader()->hide();table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  for(int column=0;column<4;++column) table->horizontalHeader()->setSectionResizeMode(column,QHeaderView::ResizeToContents);
  table->horizontalHeader()->setSectionResizeMode(4,QHeaderView::Stretch);
  const QStringList names{QCoreApplication::translate("KomaMimic", "メモリ：ファイルデータ"),QCoreApplication::translate("KomaMimic", "メモリ：表示用画像"),QCoreApplication::translate("KomaMimic", "メモリ：拡大縮小画像"),QCoreApplication::translate("KomaMimic", "ディスク：サムネイル")};
  const QStringList details{
    QCoreApplication::translate("KomaMimic", "現在の本のJPEG・PNG等のファイルデータ。書庫から取り出したデータも含みます。変更した上限は本を開き直すと適用します。"),
    QCoreApplication::translate("KomaMimic", "復号済み画像と先読み分。件数はページ数で、アニメーションのフレームも容量に含みます。"),
    QCoreApplication::translate("KomaMimic", "画面表示用の拡大・縮小結果。表示用画像のキャッシュとは別で、上限は32 MiB固定です。"),
    QCoreApplication::translate("KomaMimic", "保存済みサムネイルのPNGの合計。無効化しても保存済みファイルは残るため、残存分も表示します。")};
  for(int row=0;row<4;++row) for(int column=0;column<5;++column) {
    auto *item=new QTableWidgetItem(column==0?names[row]:QString{});item->setToolTip(details[row]);table->setItem(row,column,item);
  }
  layout->addWidget(table);
  auto *path=new QLabel(QCoreApplication::translate("KomaMimic", "サムネイルの保存先：")+w->m_thumbnailDiskCache->directory(),&dialog);
  path->setObjectName("cacheInfoPath");path->setTextFormat(Qt::PlainText);path->setWordWrap(true);
  path->setTextInteractionFlags(Qt::TextSelectableByMouse);layout->addWidget(path);
  auto *note=new QLabel(QCoreApplication::translate("KomaMimic", "取得時点の値です。メモリ容量はKiB単位で集計した概算です。\n"
      "表示・加工中の画像、一時バッファ、一覧アイコン、WebEngine・GPU・ライブラリ内部のメモリは含みません。アプリ全体の使用量ではなく、各行を足しても実メモリ使用量にはなりません。\n"
      "容量の変更・サムネイルの削除は「環境設定 → キャッシュ」で行えます。"),&dialog);
  note->setObjectName("cacheInfoNote");note->setWordWrap(true);layout->addWidget(note);
  auto *buttons=new QDialogButtonBox(QDialogButtonBox::Close,&dialog);
  buttons->button(QDialogButtonBox::Close)->setText(QCoreApplication::translate("KomaMimic", "閉じる"));
  auto *refresh=buttons->addButton(QCoreApplication::translate("KomaMimic", "更新"),QDialogButtonBox::ActionRole);refresh->setObjectName("refreshCacheInfo");
  connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);layout->addWidget(buttons);
  const auto format=[](qint64 bytes) {
    return bytes>=1024*1024?QString("%1 MiB").arg(bytes/(1024.0*1024),0,'f',1):QString("%1 KiB").arg(bytes/1024.0,0,'f',bytes?1:0);
  };
  const auto rowText=[table](int row,const QString &used,const QString &limit,const QString &count,const QString &state) {
    const QStringList values{used,limit,count,state};
    for(int i=0;i<4;++i) {table->item(row,i+1)->setText(values[i]);table->item(row,i+1)->setToolTip(values[i]);}
  };
  const auto memoryRow=[rowText,format](int row,const MemoryCacheInfo &info,const QString &unit,const QString &state) {
    rowText(row,format(info.bytes),format(info.limitBytes),unit.arg(info.entries),state);
  };
  const auto update=[&,disk=w->m_thumbnailDiskCache] {
    refresh->setEnabled(false);
    const auto book=w->m_book;
    const bool document=w->documentViewActive();
    const QString state=document?QCoreApplication::translate("KomaMimic", "文書の本文は対象外"):QCoreApplication::translate("KomaMimic", "有効");
    memoryRow(1,{int(w->m_imageCache.size()),qint64(w->m_imageCache.totalCost())*1024,qint64(w->m_imageCache.maxCost())*1024},QCoreApplication::translate("KomaMimic", "%1ページ"),state);
    memoryRow(2,w->m_view->resizeCacheInfo(),QCoreApplication::translate("KomaMimic", "%1枚"),document?state:QCoreApplication::translate("KomaMimic", "有効（固定上限）"));
    auto pending=std::make_shared<int>(book?2:1);
    const auto finished=[refresh,pending] {if(--*pending==0) refresh->setEnabled(true);};
    if(book) {
      rowText(0,QCoreApplication::translate("KomaMimic", "取得中…"),"—","—",QCoreApplication::translate("KomaMimic", "取得中…"));
      auto *watcher=new QFutureWatcher<MemoryCacheInfo>(&dialog);
      connect(watcher,&QFutureWatcherBase::finished,&dialog,[watcher,memoryRow,finished,document] {
        const auto info=watcher->result();watcher->deleteLater();
        memoryRow(0,info,QCoreApplication::translate("KomaMimic", "%1件"),info.limitBytes==0?QCoreApplication::translate("KomaMimic", "無効"):document?QCoreApplication::translate("KomaMimic", "文書の本文は対象外"):QCoreApplication::translate("KomaMimic", "有効（現在の本）"));finished();
      });
      watcher->setFuture(QtConcurrent::run([book]{return book->encodedCacheInfo();}));
    } else rowText(0,"—",format(qint64(std::clamp(w->m_settings->value("cache/fileMegabytes",32).toInt(),0,256))*1024*1024)+QCoreApplication::translate("KomaMimic", "（設定値）"),"—",QCoreApplication::translate("KomaMimic", "本を開いていません"));
    const auto limit=format(disk->limitBytes());
    const QString diskState=disk->enabled()?QCoreApplication::translate("KomaMimic", "有効"):QCoreApplication::translate("KomaMimic", "無効（保存済み画像は残ります）");
    rowText(3,QCoreApplication::translate("KomaMimic", "取得中…"),limit,"—",diskState);
    auto *watcher=new QFutureWatcher<ThumbnailDiskCache::Stats>(&dialog);
    connect(watcher,&QFutureWatcherBase::finished,&dialog,[watcher,rowText,format,limit,diskState,finished] {
      const auto info=watcher->result();watcher->deleteLater();
      if(info.error.isEmpty()) rowText(3,format(info.bytes),limit,QCoreApplication::translate("KomaMimic", "%1枚").arg(info.entries),diskState);
      else rowText(3,QCoreApplication::translate("KomaMimic", "取得できません"),limit,"—",info.error);
      finished();
    });
    watcher->setFuture(QtConcurrent::run([disk]{return disk->stats();}));
  };
  connect(refresh,&QPushButton::clicked,&dialog,update);update();dialog.exec();
}
void ReaderFeatures::output(ExportKind kind, bool all, const QString &provided,
                            bool spread) {
  if (!w->m_book || !w->pageCount())
    return;
  QString path = provided;
  if (path.isEmpty()) {
    if (kind == ExportKind::Directory)
      path = QFileDialog::getExistingDirectory(w, QCoreApplication::translate("KomaMimic", "一括保存先"), w->directory());
    else {
      const QString extension = kind == ExportKind::Zip   ? "zip"
                                : kind == ExportKind::Pdf ? "pdf"
                                : kind == ExportKind::Gif ? "gif"
                                : kind == ExportKind::Original
                                    ? QFileInfo(w->currentPageName()).suffix()
                                    : "png";
      path = QFileDialog::getSaveFileName(
          w, QCoreApplication::translate("KomaMimic", "保存"),
          QDir(w->directory())
              .filePath(QFileInfo(w->bookPath()).completeBaseName() + "." +
                        extension),
          kind == ExportKind::Image ? QCoreApplication::translate("KomaMimic", "画像 (*.png *.jpg *.webp *.bmp *.tiff)")
                                    : QCoreApplication::translate("KomaMimic", "出力 (*.") + extension + ")");
    }
  }
  if (path.isEmpty())
    return;
  QVector<int> pages;
  if (all) {
    pages.resize(w->pageCount());
    std::iota(pages.begin(), pages.end(), 0);
    const int range = w->m_settings->value("output/range", 0).toInt();
    if (range == 1)
      pages = selectedPages();
    else if (range == 2)
      pages = m_visibleIndices;
    spread = w->m_settings->value("output/spread", false).toBool();
  } else
    pages = spread
                ? m_visibleIndices
                : QVector<int>{w->viewMode() == MainWindow::ViewMode::Pages &&
                                       w->m_pages->currentRow() >= 0
                                   ? w->m_pages->currentRow()
                                   : w->m_anchor};
  ExportNaming naming;
  naming.mode=w->m_settings->value("output/naming",0).toInt();
  naming.prefix=w->m_settings->value("output/prefix").toString();
  naming.digits=w->m_settings->value("output/digits",6).toInt();
  naming.start=w->m_settings->value("output/start",1).toInt();
  naming.format=QStringList{"png","jpg","webp","tiff"}.value(w->m_settings->value("output/format",0).toInt(),"png");
  naming.conflict=w->m_settings->value("output/conflict",0).toInt();
  naming.keepColorProfile=w->m_settings->value("output/keepColorProfile",true).toBool();
  if(kind==ExportKind::Directory || kind==ExportKind::Zip) {
    const auto plan=planExport(*w->m_book,pages,kind==ExportKind::Directory ? path : QString{},naming,spread);
    if(!plan.error.isEmpty()) {message(plan.error);return;}
    if(naming.conflict==0 && !plan.conflicts.isEmpty()) {message(QCoreApplication::translate("KomaMimic", "同名ファイルがあります：")+plan.conflicts.join(", "));return;}
    if(provided.isEmpty()) {
      QMessageBox preview(QMessageBox::Information,QCoreApplication::translate("KomaMimic", "一括出力の確認"),
          QCoreApplication::translate("KomaMimic", "%1件を出力します。同名%2件：%3\n%4").arg(plan.names.size()).arg(plan.conflicts.size())
          .arg(QStringList{QCoreApplication::translate("KomaMimic", "中止"),QCoreApplication::translate("KomaMimic", "スキップ"),QCoreApplication::translate("KomaMimic", "上書き")}.value(naming.conflict)).arg(plan.names.mid(0,12).join('\n')),
          QMessageBox::Ok|QMessageBox::Cancel,w);
      preview.setDetailedText(plan.names.join('\n')); if(preview.exec()!=QMessageBox::Ok)return;
    }
  }
  auto *dialog = new QProgressDialog(QCoreApplication::translate("KomaMimic", "ページを書き出しています…"), QCoreApplication::translate("KomaMimic", "キャンセル"),
                                     0, pages.size(), w);
  dialog->setWindowModality(Qt::WindowModal);
  dialog->setMinimumDuration(0);
  dialog->show();
  auto cancel = std::make_shared<std::atomic_bool>(false);
  auto completed = std::make_shared<std::atomic_int>(0);
  connect(dialog, &QProgressDialog::canceled, this,
          [cancel] { cancel->store(true); });
  auto *timer = new QTimer(dialog);
  timer->setInterval(60);
  connect(timer, &QTimer::timeout, dialog,
          [dialog, completed] { dialog->setValue(completed->load()); });
  timer->start();
  auto *watcher = new QFutureWatcher<QString>(this);
  connect(watcher, &QFutureWatcherBase::finished, this,
          [this, watcher, dialog] {
            const auto error = watcher->result();
            watcher->deleteLater();
            dialog->close();
            dialog->deleteLater();
            message(error.isEmpty() ? QCoreApplication::translate("KomaMimic", "書き出しが完了しました。") : error);
          });
  auto processingOptions=w->m_imageOptions;
  processingOptions.scriptFilter=configuredScriptFilter(w->m_settings->value("scripts/filter"));
  if(!processingOptions.scriptFilter.isEmpty())processingOptions.scriptSettings=w->m_book->openOptions.scriptSettings;
  watcher->setFuture(QtConcurrent::run(
      [book = w->m_book, pages, path, kind, options = processingOptions, cancel,
       spread, naming, rtl = w->m_options.rightToLeft, completed,
       quality = w->m_settings->value("output/quality", 90).toInt(),
       delay = w->m_settings->value("slideshow/interval", 3000).toInt()] {
        return exportPages(
            book, pages, path, kind, options, quality, delay,
            [cancel, completed](int done, int) {
              completed->store(done);
              return !cancel->load();
            },
            spread, rtl, naming);
      }));
}
void ReaderFeatures::clipboard(bool paste) {
  if (!paste) {
    const auto images = w->m_view->images();
    if (images.isEmpty())
      return;
    QImage image = images.first();
    if (images.size() == 2) {
      image = composeSpread(images[0], images[1], w->m_options.rightToLeft);
      if (image.isNull()) {
        message(QCoreApplication::translate("KomaMimic", "結合画像が大きすぎます。"));
        return;
      }
    }
    QApplication::clipboard()->setImage(image);
    message(QCoreApplication::translate("KomaMimic", "画像をクリップボードへコピーしました。"));
    return;
  }
  const auto image = QApplication::clipboard()->image();
  if (image.isNull()) {
    message(QCoreApplication::translate("KomaMimic", "クリップボードに画像がありません。"));
    return;
  }
  if (image.sizeInBytes() > 128 * 1024 * 1024) {
    message(QCoreApplication::translate("KomaMimic", "クリップボード画像が大きすぎます。"));
    return;
  }
  QByteArray bytes;
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::WriteOnly);
  image.save(&buffer, "PNG");
  BookPage page;
  page.name = QCoreApplication::translate("KomaMimic", "クリップボード.png");
  page.identity = QUuid::createUuid().toString(QUuid::WithoutBraces);
  page.inlineData = bytes;
  if (!w->m_book) {
    const auto generated = Book::fromPages({page});
    if (!generated.book) {
      message(generated.error);
      return;
    }
    closingBook();
    w->m_openToken->store(++w->m_bookGeneration);
    w->m_renderToken->store(++w->m_renderGeneration);
    w->m_loading = false;
    w->m_book = generated.book;
    w->m_currentPath = generated.book->path;
    w->m_anchor = 0;
    openedBook();
    w->populatePages();
    w->setMode(MainWindow::ViewMode::Reader);
    w->render();
    message(
        QCoreApplication::translate("KomaMimic", "クリップボードを開きました。保存・書き出しでファイルへ保存できます。"));
    return;
  }
  m_pageClipboard = {page};
  editPages("editPaste");
}
void ReaderFeatures::print(bool setup) {
  if (!m_printer)
    m_printer = std::make_unique<QPrinter>(QPrinter::HighResolution);
  if (setup) {
    QPageSetupDialog dialog(m_printer.get(), w);
    dialog.exec();
    return;
  }
  if (!w->m_book || !w->pageCount())
    return;
  QPrintDialog dialog(m_printer.get(), w);
  dialog.setMinMax(1, w->pageCount());
  dialog.setFromTo(w->m_anchor + 1, w->m_anchor + 1);
  dialog.setOption(QAbstractPrintDialog::PrintPageRange, true);
  if (dialog.exec() != QDialog::Accepted)
    return;
  QPainter painter(m_printer.get());
  if (!painter.isActive()) {
    message(QCoreApplication::translate("KomaMimic", "プリンタへ描画できません。"));
    return;
  }
  int first = m_printer->fromPage() ? m_printer->fromPage() - 1 : 0,
      last = m_printer->toPage() ? m_printer->toPage() - 1 : w->pageCount() - 1;
  QProgressDialog progress(QCoreApplication::translate("KomaMimic", "印刷しています…"), QCoreApplication::translate("KomaMimic", "キャンセル"), first, last + 1, w);
  progress.setWindowModality(Qt::WindowModal);
  for (int i = first; i <= last && i < w->pageCount(); ++i) {
    progress.setValue(i);
    QApplication::processEvents();
    if (progress.wasCanceled())
      break;
    QFutureWatcher<ImageResult> watcher;
    QEventLoop loop;
    connect(&watcher, &QFutureWatcherBase::finished, &loop, &QEventLoop::quit);
    connect(&progress, &QProgressDialog::canceled, &loop, &QEventLoop::quit);
    watcher.setFuture(
        QtConcurrent::run([book = w->m_book, options = w->m_imageOptions, i] {
          return processMedia(book->image(i), options);
        }));
    loop.exec();
    if (progress.wasCanceled()) {
      m_printer->abort();
      break;
    }
    const auto media = watcher.result();
    if (media.image.isNull()) {
      message(media.error);
      break;
    }
    if (i > first && !m_printer->newPage())
      break;
    const auto rect =
        m_printer->pageLayout().paintRectPixels(m_printer->resolution());
    const auto size =
        media.image.size().scaled(rect.size(), Qt::KeepAspectRatio);
    painter.drawImage(
        QRect(rect.center() - QPoint(size.width() / 2, size.height() / 2),
              size),
        media.image);
  }
  painter.end();
  progress.setValue(last + 1);
}
void ReaderFeatures::slideshow(int direction) {
  if(m_slideshow->isActive() && m_slideDirection==direction) {m_slideshow->stop();return;}
  if (direction > 0 &&
      w->m_settings->value("slideshow/useSidecar", true).toBool()) {
    const QFileInfo source(w->bookPath());
    const auto sidecar =
        source.isDir()
            ? QDir(w->bookPath()).filePath(source.fileName() + ".txt")
            : w->bookPath() + ".txt";
    if (QFileInfo::exists(sidecar)) {
      timedList(sidecar);
      return;
    }
  }
  m_timed.clear();
  m_slideDirection = direction;
  m_slideshow->start(std::clamp(
      w->m_settings->value("slideshow/interval", 3000).toInt(), 100, 3600000));
  message(direction > 0 ? QCoreApplication::translate("KomaMimic", "自動再生を開始しました。")
                        : QCoreApplication::translate("KomaMimic", "逆向きの自動再生を開始しました。"));
}
void ReaderFeatures::mark(bool jump) {
  if (m_activeId.isEmpty())
    return;
  auto item = m_library->item(m_activeId);
  if (jump) {
    if (item.state.contains("marker")) {
      if(isTextDocumentPath(w->bookPath()) && item.state.contains("markerLocation"))w->jump(w->resolveDocumentLocation(DocumentLocation::fromJson(item.state.value("markerLocation").toObject())));
      else w->jump(resolvePosition(item.state.value("markerKey").toString(),item.state.value("marker").toInt()));
    }
    else
      message(QCoreApplication::translate("KomaMimic", "マーカーは登録されていません。"));
  } else {
    item.state["marker"] = w->m_anchor;
    item.state["markerKey"] = currentPositionKey();
    if(isTextDocumentPath(w->bookPath()))item.state["markerLocation"]=w->currentDocumentLocation().json();
    m_library->update(item.id, item.metadata, item.state);
    message(QCoreApplication::translate("KomaMimic", "マーカーを登録しました。"));
  }
}
void ReaderFeatures::addToc() {
  if (!w->m_book)
    return;
  for (int i = 0; i < m_tocEntries.size(); ++i)
    if (resolvePosition(m_tocEntries[i].toObject().value("key").toString(),m_tocEntries[i].toObject().value("page").toInt()) == w->m_anchor) {
      m_tocEntries.removeAt(i);
      delete m_toc->takeItem(i);
      refreshDocumentToc();
      saveState();
      return;
    }
  bool ok = false;
  const auto label = QInputDialog::getText(
      w, QCoreApplication::translate("KomaMimic", "目次に追加"), QCoreApplication::translate("KomaMimic", "見出し"), QLineEdit::Normal, w->currentPageName(), &ok);
  if (!ok || label.isEmpty())
    return;
  QJsonObject entry{{"label",label},{"page",w->m_anchor},{"key",currentPositionKey()}};
  if(isTextDocumentPath(w->bookPath()))entry["location"]=w->currentDocumentLocation().json();
  m_tocEntries.append(entry);
  auto *item = new QListWidgetItem(label, m_toc);
  item->setData(Qt::UserRole, w->m_anchor);
  refreshDocumentToc();
  saveState();
  m_tocDock->show();
}
void ReaderFeatures::navigateToc(int direction) {
  int best = direction > 0 ? w->pageCount() : -1;
  for (int i=0;i<m_toc->count();++i) {
    const int page=m_toc->item(i)->data(Qt::UserRole).toInt();
    if (direction > 0 && page > w->m_anchor)
      best = std::min(best, page);
    if (direction < 0 && page < w->m_anchor)
      best = std::max(best, page);
  }
  if (best >= 0 && best < w->pageCount())
    w->jump(best);
}
void ReaderFeatures::editMetadata(const QString &id) {
  auto item = m_library->item(id);
  if (item.id.isEmpty())
    return;
  QDialog dialog(w);
  dialog.setWindowTitle(QCoreApplication::translate("KomaMimic", "書籍のプロパティ"));
  auto *layout = new QVBoxLayout(&dialog);
  auto *form = new QFormLayout;
  layout->addLayout(form);
  QHash<QString, QLineEdit *> editors;
  for (const auto &[key, label] :
       QVector<QPair<QString, QString>>{{"title", QCoreApplication::translate("KomaMimic", "タイトル")},
                                        {"author", QCoreApplication::translate("KomaMimic", "著者")},
                                        {"publisher", QCoreApplication::translate("KomaMimic", "出版社")},
                                        {"volume", QCoreApplication::translate("KomaMimic", "巻")},
                                        {"date", QCoreApplication::translate("KomaMimic", "発売日")},
                                        {"price", QCoreApplication::translate("KomaMimic", "価格")},
                                        {"notes", QCoreApplication::translate("KomaMimic", "備考")},
                                        {"notes2", QCoreApplication::translate("KomaMimic", "備考2")},
                                        {"notes3", QCoreApplication::translate("KomaMimic", "備考3")},
                                        {"titleKana", QCoreApplication::translate("KomaMimic", "タイトル読み")},
                                        {"authorKana", QCoreApplication::translate("KomaMimic", "著者読み")}}) {
    auto *edit = new QLineEdit(
        key == "title" ? item.title : item.metadata.value(key).toString(),
        &dialog);
    editors[key] = edit;
    form->addRow(label, edit);
  }
  auto *buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  if (dialog.exec() == QDialog::Accepted) {
    for (auto it = editors.cbegin(); it != editors.cend(); ++it)
      item.metadata[it.key()] = it.value()->text();
    m_library->update(id, item.metadata, item.state);
    rebuildLists();
  }
}
void ReaderFeatures::exportThumbnail() {
  if (!w->m_book)
    return;
  const auto path = QFileDialog::getSaveFileName(w, QCoreApplication::translate("KomaMimic", "表紙サムネイルを保存"),
                                                 w->directory() + "/cover.png",
                                                 QCoreApplication::translate("KomaMimic", "PNG画像 (*.png)"));
  if (path.isEmpty())
    return;
  auto book = w->m_book;
  auto *watcher = new QFutureWatcher<ImageResult>(this);
  const int page = w->m_anchor;
  connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, path] {
    const auto image = watcher->result();
    watcher->deleteLater();
    if (!image.error.isEmpty() || !image.image.save(path, "PNG"))
      message(QCoreApplication::translate("KomaMimic", "サムネイルを保存できません。"));
    else
      message(QCoreApplication::translate("KomaMimic", "サムネイルを保存しました。"));
  });
  watcher->setFuture(QtConcurrent::run(
      [book, page] { return book->image(page, QSize(600, 800)); }));
}
void ReaderFeatures::openUrl() {
  bool ok = false;
  const auto text = QInputDialog::getText(w, QCoreApplication::translate("KomaMimic", "URLを開く"), "HTTP / HTTPS URL",
                                          QLineEdit::Normal, {}, &ok);
  if (!ok)
    return;
  const QUrl url = QUrl::fromUserInput(text);
  if (url.scheme() != "https" && url.scheme() != "http") {
    message(QCoreApplication::translate("KomaMimic", "HTTPまたはHTTPSのURLを指定してください。"));
    return;
  }
  QString name = QFileInfo(url.path()).fileName();
  if (name.isEmpty())
    name = "download.zip";
  const auto path = QFileDialog::getSaveFileName(
      w, QCoreApplication::translate("KomaMimic", "ダウンロード先"), QDir(w->directory()).filePath(name),
      openFileFilter());
  if (path.isEmpty())
    return;
  auto file = std::make_shared<QSaveFile>(path);
  if (!file->open(QIODevice::WriteOnly)) {
    message(file->errorString());
    return;
  }
  auto *manager = new QNetworkAccessManager(this);
  QNetworkRequest request(url);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::NoLessSafeRedirectPolicy);
  request.setTransferTimeout(
      std::clamp(w->m_settings->value("network/timeout", 30).toInt(), 5, 600) *
      1000);
  request.setRawHeader(
      "User-Agent",
      w->m_settings->value("network/userAgent", "KomaMimic/" KOMAMIMIC_VERSION).toByteArray());
  auto *reply = manager->get(request);
  auto *progress =
      new QProgressDialog(QCoreApplication::translate("KomaMimic", "ダウンロードしています…"), QCoreApplication::translate("KomaMimic", "キャンセル"), 0, 100, w);
  progress->setWindowModality(Qt::WindowModal);
  progress->setMinimumDuration(0);
  connect(progress, &QProgressDialog::canceled, reply, &QNetworkReply::abort);
  connect(reply, &QNetworkReply::downloadProgress, progress,
          [progress](qint64 done, qint64 total) {
            if (total > 0) {
              progress->setMaximum(1000);
              progress->setValue(int(done * 1000 / total));
            } else
              progress->setRange(0, 0);
          });
  auto bytes = std::make_shared<qint64>(0);
  auto writeError = std::make_shared<QString>();
  connect(
      reply, &QIODevice::readyRead, this,
      [reply, file, bytes, writeError,
       limit = qint64(w->m_settings->value("network/maxMiB", 2048).toInt()) *
               1024 * 1024] {
        const auto data = reply->readAll();
        *bytes += data.size();
        if (*bytes > limit) {
          *writeError = QCoreApplication::translate("KomaMimic", "ダウンロードが設定された容量を超えました。");
          reply->abort();
          return;
        }
        if (file->write(data) != data.size()) {
          *writeError = file->errorString();
          reply->abort();
        }
      });
  connect(
      reply, &QNetworkReply::finished, this,
      [this, reply, manager, progress, file, path, writeError] {
        progress->close();
        progress->deleteLater();
        const auto tail = reply->readAll();
        if (!tail.isEmpty() && file->write(tail) != tail.size())
          *writeError = file->errorString();
        if (reply->error() != QNetworkReply::NoError || !writeError->isEmpty())
          message(writeError->isEmpty() ? reply->errorString() : *writeError);
        else if (!file->commit())
          message(file->errorString());
        else
          w->openBook(path);
        reply->deleteLater();
        manager->deleteLater();
      });
}
void ReaderFeatures::external(int slot, bool configure) {
  const auto key = QString("external/%1").arg(slot);
  QString command = w->m_settings->value(key).toString();
  if (configure || command.isEmpty()) {
    bool ok = false;
    command = QInputDialog::getText(
        w, QCoreApplication::translate("KomaMimic", "外部コマンド%1").arg(slot),
        QCoreApplication::translate("KomaMimic", "プログラムと引数。{file} / {directory} / {page} を利用できます。"),
        QLineEdit::Normal, command, &ok);
    if (!ok || command.isEmpty())
      return;
    w->m_settings->setValue(key, command);
    if (configure)
      return;
  }
  QStringList args = QProcess::splitCommand(command);
  if (args.isEmpty())
    return;
  const auto program = args.takeFirst();
  for (auto &arg : args) {
    arg.replace("{file}", w->bookPath());
    arg.replace("{directory}", w->directory());
    arg.replace("{page}", QString::number(w->m_anchor + 1));
  }
  if (!QProcess::startDetached(program, args, w->directory()))
    message(QCoreApplication::translate("KomaMimic", "外部プログラムを起動できません。"));
}
void ReaderFeatures::search() {
  bool ok = false;
  const auto text = QInputDialog::getText(
      w, QCoreApplication::translate("KomaMimic", "ファイル名・ページ名の検索"), QCoreApplication::translate("KomaMimic", "正規表現"), QLineEdit::Normal, {}, &ok);
  if (!ok || text.isEmpty())
    return;
  QRegularExpression regex(text, QRegularExpression::CaseInsensitiveOption);
  if (!regex.isValid()) {
    message(regex.errorString());
    return;
  }
  QDialog dialog(w);
  dialog.setWindowTitle(QCoreApplication::translate("KomaMimic", "検索結果"));
  dialog.resize(650, 500);
  auto *layout = new QVBoxLayout(&dialog);
  auto *list = new QListWidget(&dialog);
  layout->addWidget(list);
  const bool pages =
      w->viewMode() != MainWindow::ViewMode::Covers && bool(w->m_book);
  if (pages) {
    for (int i = 0; i < w->pageCount(); ++i)
      if (regex.match(w->m_book->pages[i].name).hasMatch()) {
        auto *item = new QListWidgetItem(
            QString("%1  %2").arg(i + 1).arg(w->m_book->pages[i].name), list);
        item->setData(Qt::UserRole, i);
      }
  } else
    for (int i = 0; i < w->m_library->count(); ++i) {
      const auto *source = w->m_library->item(i);
      if (regex.match(source->text()).hasMatch()) {
        auto *item = new QListWidgetItem(source->text(), list);
        item->setData(Qt::UserRole, i);
      }
    }
  connect(list, &QListWidget::itemActivated, &dialog,
          [&dialog] { dialog.accept(); });
  auto *buttons = new QDialogButtonBox(
      QDialogButtonBox::Open | QDialogButtonBox::Cancel, &dialog);
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  if (dialog.exec() == QDialog::Accepted && list->currentItem()) {
    const int index = list->currentItem()->data(Qt::UserRole).toInt();
    if (pages) {
      w->setMode(MainWindow::ViewMode::Reader);
      w->jump(index);
    } else
      w->activateLibraryItem(w->m_library->item(index));
  }
}
namespace {
QStringList csvFields(const QString &line) {
  QStringList result;
  QString field;
  bool quoted = false;
  for (int i = 0; i < line.size(); ++i) {
    const auto c = line[i];
    if (c == '"') {
      if (quoted && i + 1 < line.size() && line[i + 1] == '"') {
        field += '"';
        ++i;
      } else
        quoted = !quoted;
    } else if (c == ',' && !quoted) {
      result.append(field);
      field.clear();
    } else
      field += c;
  }
  result.append(field);
  return result;
}
} // namespace
void ReaderFeatures::loadList(bool save, const QString &provided) {
  const auto path = !provided.isEmpty() ? provided :
      save ? QFileDialog::getSaveFileName(w, QCoreApplication::translate("KomaMimic", "ブックリストを保存"),
                                          w->directory() + "/books.json",
                                          QCoreApplication::translate("KomaMimic", "KomaMimicブックリスト (*.json)"))
           : QFileDialog::getOpenFileName(w, QCoreApplication::translate("KomaMimic", "ブックリストを読み込む"),
                                          w->directory(),
                                          QCoreApplication::translate("KomaMimic", "ブックリスト (*.json *.lst)"));
  if (path.isEmpty())
    return;
  const QString kind = m_lists.key(
      qobject_cast<QListWidget *>(m_tabs->currentWidget()), "playlist");
  if (save) {
    QJsonArray items;
    for (const auto &item : m_library->items(kind))
      items.append(QJsonObject{{"path", item.path},
                               {"title", item.title},
                               {"metadata", item.metadata},
                               {"state", item.state}});
    QSaveFile file(path);
    const auto bytes = QJsonDocument(QJsonObject{{"format", "KomaMimic"},
                                                 {"version", 1},
                                                 {"kind", kind},
                                                 {"items", items}})
                           .toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
        !file.commit())
      message(file.errorString());
    return;
  }
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || file.size() > 128 * 1024 * 1024) {
    message(QCoreApplication::translate("KomaMimic", "リストを読めないか、サイズが上限を超えています。"));
    return;
  }
  const auto bytes = file.readAll();
  QJsonArray items;
  if (QFileInfo(path).suffix().toLower() == "json") {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError ||
        document.object().value("format").toString() != "KomaMimic" ||
        document.object().value("version").toInt() != 1) {
      message(QCoreApplication::translate("KomaMimic", "KomaMimicのリスト形式ではありません。"));
      return;
    }
    items = document.object().value("items").toArray();
  } else {
    QStringDecoder decoder(QStringDecoder::Utf8);
    QString text = decoder(bytes);
    if (decoder.hasError()) {
      QByteArray utf8(bytes.size() * 4 + 16, '\0');
      UErrorCode error = U_ZERO_ERROR;
      const int length =
          ucnv_convert("UTF-8", "windows-932", utf8.data(), int(utf8.size()),
                       bytes.constData(), int(bytes.size()), &error);
      if (U_FAILURE(error)) {
        message(QCoreApplication::translate("KomaMimic", "旧LSTの文字コードを解釈できません。"));
        return;
      }
      text = QString::fromUtf8(utf8.constData(), length);
    }
    if (text.startsWith(QChar(0xfeff)))
      text.remove(0, 1);
    QStringList legacyHeader;
    QString windowsPrefix, mappedDirectory;
    bool mappingAsked = false;
    for (const auto &line : text.split('\n')) {
      if (!line.trimmed().startsWith('"')) {
        legacyHeader.append(line);
        continue;
      }
      auto columns = csvFields(line.trimmed());
      if (columns.size() > 30)
        columns[30] = "";
      if (columns.isEmpty())
        continue;
      QJsonObject metadata;
      const QVector<QPair<int, QString>> fields = {
          {4, "legacySize"}, {5, "lastAccess"},  {6, "registered"},
          {7, "sort"},       {8, "recursive"},   {9, "spine"},
          {10, "spineX"},    {11, "spineWidth"}, {12, "author"},
          {13, "publisher"}, {14, "notes"},      {15, "notes2"},
          {16, "notes3"},    {17, "title"},      {18, "entryMode"},
          {19, "legacyToc"}, {20, "volume"},     {21, "date"},
          {22, "titleKana"}, {23, "authorKana"}, {24, "price"},
          {25, "cover"},     {26, "coverX"},     {27, "coverWidth"},
          {28, "rtl"},       {29, "pageRange"}};
      for (const auto &[column, key] : fields)
        if (column < columns.size())
          metadata[key] = columns[column];
      for (const auto &key : {"spine", "cover"})
        if (metadata.contains(key))
          metadata[key] = std::max(0, metadata[key].toString().toInt() - 1);
      QJsonArray original;
      for (const auto &value : columns)
        original.append(value);
      metadata["legacyColumns"] = original;
      const auto source = columns[0];
      QString resolved = source;
#ifndef Q_OS_WIN
      if (QRegularExpression("^[A-Za-z]:[\\\\/]|^\\\\\\\\")
              .match(source)
              .hasMatch()) {
        if (!mappingAsked) {
          mappingAsked = true;
          bool ok = false;
          windowsPrefix = QInputDialog::getText(
              w, QCoreApplication::translate("KomaMimic", "Windowsパスの対応付け"),
              QCoreApplication::translate("KomaMimic", "置換する旧フォルダの先頭部分（空欄なら元のパスを保持）"),
              QLineEdit::Normal, source.left(3), &ok);
          if (ok && !windowsPrefix.isEmpty())
            mappedDirectory = QFileDialog::getExistingDirectory(
                w, QCoreApplication::translate("KomaMimic", "対応する現在のフォルダ"), w->directory());
        }
        if (!windowsPrefix.isEmpty() && !mappedDirectory.isEmpty() &&
            source.startsWith(windowsPrefix, Qt::CaseInsensitive)) {
          QString relative = source.mid(windowsPrefix.size());
          relative.replace('\\', '/');
          while (relative.startsWith('/'))
            relative.remove(0, 1);
          resolved = QDir(mappedDirectory).filePath(relative);
        }
      } else
#endif
          if (!QFileInfo(source).isAbsolute())
        resolved = QDir(QFileInfo(path).absolutePath()).filePath(source);
      metadata["originalPath"] = source;
      items.append(QJsonObject{
          {"path", resolved},
          {"metadata", metadata},
          {"state",
           QJsonObject{{"anchor", std::max(0, columns.value(1).toInt() - 1)},
                       {"marker", std::max(0, columns.value(3).toInt() - 1)},
                       {"total", columns.value(2).toInt()},
                       {"legacySort",columns.value(7).toInt()},
                       {"rtl",columns.value(28,"1").toInt()!=0}}}});
    }
    w->m_settings->setValue("lists/" + kind + "/legacyHeader", legacyHeader);
  }
  if (items.size() > 100000) {
    message(QCoreApplication::translate("KomaMimic", "リストの項目数が上限を超えています。"));
    return;
  }
  for (const auto &value : items) {
    const auto item = value.toObject();
    if (item.value("path").toString().isEmpty())
      continue;
    const auto id = m_library->add(kind, item.value("path").toString(),
                                   item.value("title").toString());
    m_library->update(id, item.value("metadata").toObject(),
                      item.value("state").toObject());
  }
  rebuildLists();
  m_listsDock->show();
}

void ReaderFeatures::timedList(const QString &provided) {
  const auto path =
      provided.isEmpty()
          ? QFileDialog::getOpenFileName(
                w, QCoreApplication::translate("KomaMimic", "時間付きページリスト"),
                QFileInfo(w->bookPath()).absolutePath(), QCoreApplication::translate("KomaMimic", "テキスト (*.txt)"))
          : provided;
  if (path.isEmpty())
    return;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) {
    message(QCoreApplication::translate("KomaMimic", "時間リストを読めないか1 MiBを超えています。"));
    return;
  }
  const auto raw = file.readAll();
  QStringDecoder decoder(QStringDecoder::Utf8);
  QString text = decoder(raw);
  if (decoder.hasError()) {
    QByteArray utf8(raw.size() * 4 + 16, '\0');
    UErrorCode status = U_ZERO_ERROR;
    const int len = ucnv_convert("UTF-8", "windows-932", utf8.data(),
                                 utf8.size(), raw.data(), raw.size(), &status);
    if (U_FAILURE(status)) {
      message(QCoreApplication::translate("KomaMimic", "時間リストの文字コードが不正です。"));
      return;
    }
    text = QString::fromUtf8(utf8.constData(), len);
  }
  const QRegularExpression expression(
      R"(^\s*\[(\d+):(\d{1,2})(?::(\d{1,2}))?(?:\s*/\s*(\d*)(?:\s*/\s*(-?\d+))?(?:\s*/\s*(-?\d+))?)?\](.*)$)");
  QVector<TimedPage> events;
  for (const auto &line : text.split('\n')) {
    if (line.trimmed().isEmpty() ||
        (line.trimmed().startsWith('#') || line.trimmed().startsWith("//")))
      continue;
    const auto match = expression.match(line);
    if (!match.hasMatch()) {
      message(QCoreApplication::translate("KomaMimic", "不正な時間リスト行: ") + line.left(100));
      return;
    }
    const qint64 time = match.captured(1).toLongLong() * 60000 +
                        match.captured(2).toInt() * 1000 +
                        match.captured(3).toInt() * 10;
    if (time > 24LL * 3600000 || match.captured(2).toInt() > 59 ||
        match.captured(3).toInt() > 99 ||
        (!events.isEmpty() && time < events.last().time)) {
      message(QCoreApplication::translate("KomaMimic", "時刻が範囲外または逆順です。"));
      return;
    }
    events.append(
        {time,
         match.captured(4).isEmpty()
             ? -1
             : std::clamp(match.captured(4).toInt() - 1, 0, w->pageCount() - 1),
         {match.captured(5).toDouble(), match.captured(6).toDouble()},
         !match.captured(5).isEmpty(),
         match.captured(7)});
  }
  if (events.isEmpty()) {
    message(QCoreApplication::translate("KomaMimic", "再生項目がありません。"));
    return;
  }
  m_slideshow->stop();
  m_timed = events;
  m_timedIndex = 0;
  m_timedOffset = 0;
  m_slideClock.restart();
  m_slideshow->start(20);
  message(QCoreApplication::translate("KomaMimic", "時間リストの再生を開始しました。"));
}
