// SPDX-License-Identifier: GPL-3.0-only
#include "readerfeatures.h"
#include "archivepath.h"
#include <QCoreApplication>
#include "readertoolbar.h"
#include "i18n.h"
#include "thumbnailsource.h"
#include "scriptreader.h"
#include "idleinhibitor.h"
#include "documentsettings.h"
#include "documentview.h"
#include "expression.h"
#include "foldertree.h"
#include "mainwindow.h"
#include "pageview.h"
#include "readerinput.h"
#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPrinter>
#include <QProgressDialog>
#include <QRandomGenerator>
#include <QScreen>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QUndoStack>
#include <QUuid>
#include <QtConcurrent>
#include <algorithm>
#include <numeric>
#include <random>
namespace {
QString pageKey(const BookPage &page) {
  if (!page.identity.isEmpty())
    return page.identity;
  return page.name + QChar(0x1f) + QString::number(page.archiveIndex) +
         QChar(0x1f) + QString::number(page.documentPage);
}
QString positionKey(const BookPage &page) {
  return pageKey(page) + QString("[%1,%2,%3,%4]")
                             .arg(page.portion.x(), 0, 'g', 12)
                             .arg(page.portion.y(), 0, 'g', 12)
                             .arg(page.portion.width(), 0, 'g', 12)
                             .arg(page.portion.height(), 0, 'g', 12);
}
class PageEdit : public QUndoCommand {
public:
  PageEdit(ReaderFeatures *owner, QVector<BookPage> before,
           QVector<BookPage> after, int oldAnchor, int newAnchor,
           const QString &label)
      : QUndoCommand(label), f(owner), a(std::move(before)),
        b(std::move(after)), old(oldAnchor), next(newAnchor) {}
  void redo() override { f->replacePages(b, next); }
  void undo() override { f->replacePages(a, old); }

private:
  ReaderFeatures *f;
  QVector<BookPage> a, b;
  int old, next;
};
} // namespace
ReaderFeatures::ReaderFeatures(MainWindow *window, IdleInhibitor *inhibitor)
    : QObject(window), w(window) {
  initializeInhibit(inhibitor);
  m_library = std::make_unique<LibraryStore>(collectionPath());
  m_undo = new QUndoStack(this);
  m_saveTimer = new QTimer(this);
  m_saveTimer->setSingleShot(true);
  m_saveTimer->setInterval(400);
  connect(m_saveTimer, &QTimer::timeout, this, &ReaderFeatures::saveState);
  m_slideshow = new QTimer(this);
  m_slideshow->setObjectName("slideshowTimer");
  connect(m_slideshow, &QTimer::timeout, this, [this] {
    if (!w->renderReady() || QApplication::activeModalWidget() ||
        !w->isVisible() || w->viewMode() != MainWindow::ViewMode::Reader ||
        w->isMinimized())
      return;
    if (!m_timed.isEmpty()) {
      const auto now = m_timedOffset + m_slideClock.elapsed();
      if (m_timedIndex < m_timed.size() && m_timed[m_timedIndex].time <= now) {
        const auto event = m_timed[m_timedIndex++];
        if (event.page >= 0)
          w->jump(event.page);
        else
          w->nextPage();
        if (event.hasPan) {
          m_restoreSourceOrigin = true;
          m_restoredPan = event.pan;
          if (w->renderReady()) {
            w->m_view->setSourceOrigin(event.pan);
            m_restoreSourceOrigin = false;
          }
        }
        if (!event.text.isEmpty())
          message(event.text);
      }
      if (m_timedIndex >= m_timed.size())
        m_slideshow->stop();
      return;
    }
    const auto previous = w->bookPath();
    const auto page = w->pageIndex();
    if(w->m_settings->value("slideshow/scroll",false).toBool() && !w->documentViewActive()) {
      if(w->m_view->scan(m_slideDirection,w->m_options.rightToLeft,w->m_settings->value("reading/scrollOverlap",15).toInt()))return;
    }
    if (m_slideDirection > 0)
      w->nextPage();
    else
      w->previousPage();
    if(w->m_settings->value("slideshow/scroll",false).toBool() &&
       (previous!=w->bookPath() || page!=w->pageIndex()))w->m_view->enterScan(m_slideDirection,w->m_options.rightToLeft);
    if (previous == w->bookPath() && page == w->pageIndex() &&
        !w->isLoading()) {
      m_slideshow->stop();
      message(QCoreApplication::translate("KomaMimic", "自動再生は端で停止しました。"));
    }
  });
  m_watcher = new QFileSystemWatcher(this);
  m_watchReloadTimer = new QTimer(this);
  m_watchReloadTimer->setSingleShot(true);
  m_watchReloadTimer->setInterval(250);
  const auto updated = [this](const QString &) {
    if (w->m_settings->value("General/watch", false).toBool())
      m_watchReloadTimer->start();
  };
  connect(m_watcher, &QFileSystemWatcher::fileChanged, this, updated);
  connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, updated);
  connect(m_watchReloadTimer, &QTimer::timeout, this, [this] {
    if (!w->m_book)
      return;
    if (!bookPathExists(w->bookPath())) {
      message(QCoreApplication::translate("KomaMimic", "更新監視: 閲覧中の本が見つかりません。"));
      return;
    }
    m_pendingId = m_activeId;
    m_intent = 0;
    w->loadBook(w->bookPath(), w->viewMode());
  });
  build();
  m_input = new ReaderInput(w, this);
  bind("mouseSettings", [this] { m_input->configure(false); });
  bind("gamepadSettings", [this] { m_input->configure(true); });
  rebuildLists();
  if (!m_library->error().isEmpty())
    message(QCoreApplication::translate("KomaMimic", "ブックリストを開けません: ") + m_library->error());
}
ReaderFeatures::~ReaderFeatures() {
  setProperty("closing", true);
  updateInhibit();
}
QString ReaderFeatures::collectionPath() const {
  return QFileInfo(w->m_settings->fileName()).absolutePath() +
         "/library.sqlite3";
}
void ReaderFeatures::bind(const QString &id,
                          const std::function<void()> &function) {
  connect(w->m_actions[id], &QAction::triggered, this, function);
}
void ReaderFeatures::message(const QString &text) {
  w->statusBar()->showMessage(text, 10000);
}
OpenOptions ReaderFeatures::openOptions() const {
  OpenOptions result;
  auto &s = *w->m_settings;
  result.fileCacheMiB = s.value("cache/fileMegabytes", 32).toInt();
  result.recursive = s.value("source/recursive", false).toBool();
  if(!m_pendingId.isEmpty()) {
    const auto value=m_library->item(m_pendingId).metadata.value("recursive").toString().toInt();
    if(value==1 || value==2)result.recursive=value==1;
  }
  result.nestedArchives = s.value("source/nested", false).toBool();
  result.folderContainers = s.value("source/folderContainers", false).toBool();
  result.pdfResources = s.value("source/pdfResources", false).toBool();
  result.password = property("password").toString();
  const auto schemas=scriptSettingsSchemas();
  for(auto i=schemas.cbegin();i!=schemas.cend();++i) {
    QVariantMap values;const auto fields=i.value().toMap();
    for(auto field=fields.cbegin();field!=fields.cend();++field)
      values[field.key()]=s.value("scripts/"+i.key()+"/"+field.key(),field.value().toMap().value("default"));
    result.scriptSettings[i.key()]=values;
  }
  result.text = textOptions(documentOptions(s, w->m_currentPath));
  return result;
}
void ReaderFeatures::build() {
  for (const QString kind : {"playlist", "favorite", "history"})
    for (int layout = 0; layout < 3; ++layout)
      bind(kind + QString::number(layout),
           [this, kind, layout] { showCollection(kind, layout); });
  bind("scaleFill", [this] { w->m_view->setScaleMode(ScaleMode::Fill); });
  bind("boundaryCycle",[this] {
    const int mode=(int(w->m_options.boundaryMode)+1)%3;
    w->m_actions[QStringList{"boundaryStop","boundaryRepeat","boundaryAdvance"}[mode]]->trigger();
  });
  bind("scaleEqualHeightToggle",[this] { w->m_view->setScaleMode(w->m_view->scaleMode()==ScaleMode::EqualHeight?ScaleMode::Actual:ScaleMode::EqualHeight); });
  bind("scaleFitToggle",[this] { w->m_view->setScaleMode(w->m_view->scaleMode()==ScaleMode::Fit?ScaleMode::Actual:ScaleMode::Fit); });
  for(int direction:{-1,1})bind(direction<0?"wheelScrollUp":"wheelScrollDown",[this,direction] {
    double height=w->m_view->height();
    if(w->m_settings->value("input/wheelImageUnits",false).toBool()) {
      const auto rects=w->m_view->pageRects();if(!rects.isEmpty())height=0;
      for(const auto &rect:rects)height=std::max(height,rect.height());
    }
    const double distance=height*std::clamp(w->m_settings->value("input/wheelPercent",25).toInt(),1,100)/100.0;
    const int speed=w->m_settings->value("input/smoothScroll",false).toBool()?w->m_settings->value("input/scrollSpeed",4000).toInt():0;
    if(!w->m_view->scrollBy({0,-direction*distance},speed) && w->m_settings->value("reading/scrollTurnsPage",true).toBool()) {
      const int before=w->m_anchor;const auto path=w->bookPath();
      if(direction>0)w->nextPage();else w->previousPage();
      if(before!=w->m_anchor || path!=w->bookPath())w->m_view->enterScan(direction,w->m_options.rightToLeft);
    }
  });
  bind("scaleEqualHeight",
       [this] { w->m_view->setScaleMode(ScaleMode::EqualHeight); });
  bind("scaleFixed", [this] {
    const auto rects = w->m_view->pageRects();
    const auto images = w->m_view->images();
    if (!rects.isEmpty() && !images.isEmpty()) {
      const double factor =
          rects[0].width() * w->devicePixelRatioF() / images[0].width();
      w->m_view->setScaleMode(ScaleMode::Actual);
      w->m_view->zoomBy(factor);
    }
  });
  bind("swapMouseButtons", [this] {
    w->m_settings->setValue(
        "input/swapButtons",
        !w->m_settings->value("input/swapButtons", false).toBool());
  });
  for (int direction : {-1, 1})
    for (bool percent : {false, true})
      bind(QString(percent ? "jumpPercent" : "jumpFixed") +
               (direction > 0 ? "Next" : "Previous"),
           [this, direction, percent] {
             const int step =
                 percent
                     ? std::max(1, qRound(w->pageCount() *
                                          w->m_settings
                                              ->value("navigation/percent", 10)
                                              .toDouble() /
                                          100))
                     : w->m_settings->value("navigation/count", 10).toInt();
             w->jump(std::clamp(w->m_anchor + direction * step, 0,
                                std::max(0, w->pageCount() - 1)));
           });
  bind("readerContextMenu", [this] {
    QMenu menu(w);
    for (const auto &id :
         {"next", "previous", "thumbnails", "library", "fullscreenGlobal",
          "filterSettings", "saveImage", "imageCopy", "imageInfo"})
      menu.addAction(w->m_actions[id]);
    menu.exec(QCursor::pos());
  });
  for (int i = 1; i <= 20; ++i) {
    bind(QString("profile%1").arg(i), [this, i] { profile(i, false); });
    bind(QString("saveProfile%1").arg(i), [this, i] { profile(i, true); });
  }
  for (int i = 1; i <= 10; ++i) {
    bind(QString("settingsLoad%1").arg(i), [this, i] { settingsSlot(i, false); });
    bind(QString("settingsSave%1").arg(i), [this, i] { settingsSlot(i, true); });
  }
  for (int i = 1; i <= 8; ++i)
    bind(QString("userCommand%1").arg(i), [this, i] { external(i, false); });
  m_listsDock = new QDockWidget(QCoreApplication::translate("KomaMimic", "ブックリスト"), w);
  m_listsDock->setObjectName("bookListDock");
  m_tabs = new QTabWidget(m_listsDock);
  for (const auto &[kind, label] :
       QVector<QPair<QString, QString>>{{"playlist", QCoreApplication::translate("KomaMimic", "プレイリスト")},
                                        {"favorite", QCoreApplication::translate("KomaMimic", "お気に入り")},
                                        {"history", QCoreApplication::translate("KomaMimic", "履歴")},
                                        {"shelf", QCoreApplication::translate("KomaMimic", "本棚")}}) {
    auto *list = new QListWidget(m_tabs);
    list->setProperty("coverLayout",
                      w->m_settings->value("lists/" + kind + "/layout", 2));
    list->setObjectName("collection_" + kind);
    installThumbnailDelegate(list);
    list->installEventFilter(this);
    connect(list->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this] { requestCollectionCover(); });
    m_lists[kind] = list;
    list->setDragDropMode(QAbstractItemView::InternalMove);
    list->setIconSize({64, 88});
    list->setWordWrap(true);
    connect(list->model(), &QAbstractItemModel::rowsMoved, this,
            [this, list, kind] {
              for (int i = 0; i < list->count(); ++i) {
                auto value = m_library->item(
                    list->item(i)->data(Qt::UserRole).toString());
                value.metadata["order"] = i;
                m_library->update(value.id, value.metadata, value.state);
              }
            });
    m_tabs->addTab(list, label);
    connect(list, &QListWidget::itemActivated, this,
            [this](QListWidgetItem *item) {
              openCollection(item->data(Qt::UserRole).toString());
            });
    list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(list, &QWidget::customContextMenuRequested, this,
            [this, list](QPoint point) {
              auto *item = list->itemAt(point);
              const auto id =
                  item ? item->data(Qt::UserRole).toString() : QString{};
              QMenu context(w);
              context.addAction(QCoreApplication::translate("KomaMimic", "つづきから"), this,
                                [this, id] { openCollection(id, 0); });
              context.addAction(QCoreApplication::translate("KomaMimic", "最初から"), this,
                                [this, id] { openCollection(id, 1); });
              context.addAction(QCoreApplication::translate("KomaMimic", "マーカーから"), this,
                                [this, id] { openCollection(id, 2); });
              context.addAction(QCoreApplication::translate("KomaMimic", "表紙一覧へ"), this, [this, id] {
                const auto value = m_library->item(id);
                if (parseArchiveEntryPath(value.path).valid()) w->openBook(physicalBookPath(value.path));
                else w->openDirectory(QFileInfo(value.path).isDir() ? value.path : QFileInfo(value.path).absolutePath());
              });
              context.addAction(QCoreApplication::translate("KomaMimic", "プロパティ"), this,
                                [this, id] { editMetadata(id); });
              context.addAction(QCoreApplication::translate("KomaMimic", "リストから削除"), this, [this, id] {
                m_library->remove(id);
                rebuildLists();
              });
              context.addSeparator();
              context.addAction(QCoreApplication::translate("KomaMimic", "一覧表示"), this, [this, list] {
                list->setViewMode(QListView::ListMode);
                w->m_settings->setValue("lists/" + m_lists.key(list) + "/icons",
                                        false);
              });
              context.addAction(QCoreApplication::translate("KomaMimic", "表紙サムネイル表示"), this, [this, list] {
                showCollection(m_lists.key(list), 2);
              });
              auto *sort = context.addMenu(QCoreApplication::translate("KomaMimic", "並べ替え"));
              for (const auto &[key, label] :
                   QVector<QPair<QString, QString>>{{"order", QCoreApplication::translate("KomaMimic", "登録順／手動順")},
                                                    {"title", QCoreApplication::translate("KomaMimic", "タイトル")},
                                                    {"author", QCoreApplication::translate("KomaMimic", "著者")},
                                                    {"publisher", QCoreApplication::translate("KomaMimic", "出版社")},
                                                    {"volume", QCoreApplication::translate("KomaMimic", "巻")},
                                                    {"path", QCoreApplication::translate("KomaMimic", "パス")}})
                sort->addAction(label, this, [this, list, key] {
                  w->m_settings->setValue(
                      "lists/" + m_lists.key(list) + "/sort", key);
                  rebuildLists();
                });
              context.addAction(QCoreApplication::translate("KomaMimic", "全項目をリストから削除"), this, [this, list] {
                if (QMessageBox::question(
                        w, QCoreApplication::translate("KomaMimic", "リストのクリア"),
                        QCoreApplication::translate("KomaMimic", "登録項目をすべて削除します。続けますか？")) ==
                    QMessageBox::Yes) {
                  m_library->clear(m_lists.key(list));
                  rebuildLists();
                }
              });
              context.exec(list->viewport()->mapToGlobal(point));
            });
  }
  connect(m_tabs, &QTabWidget::currentChanged, this,
          [this] { requestCollectionCover(); });
  connect(m_listsDock, &QDockWidget::visibilityChanged, this,
          [this](bool visible) {
            if (visible)
              requestCollectionCover();
          });
  m_listsDock->setWidget(m_tabs);
  w->addDockWidget(Qt::RightDockWidgetArea, m_listsDock);
  m_listsDock->hide();
  m_tocDock = new QDockWidget(QCoreApplication::translate("KomaMimic", "目次"), w);
  m_tocDock->setObjectName("tocDock");
  m_toc = new QListWidget(m_tocDock);
  m_toc->setObjectName("tocList");
  m_tocDock->setWidget(m_toc);
  w->addDockWidget(Qt::LeftDockWidgetArea, m_tocDock);
  m_tocDock->hide();
  connect(m_toc, &QListWidget::itemActivated, this,
          [this](QListWidgetItem *item) {
            w->setMode(MainWindow::ViewMode::Reader);
            const auto location=item->data(Qt::UserRole+1).toMap();
            if(w->documentViewActive() && !location.isEmpty()) {
              w->m_documentView->navigate(location.value("section").toString(),location.value("fragment").toString(),location.value("offset").toInt());return;
            }
            w->jump(item->data(Qt::UserRole).toInt());
          });
  m_shelfDock = new QDockWidget(QCoreApplication::translate("KomaMimic", "本棚"), w);
  m_shelfDock->setObjectName("bookshelfDock");
  m_shelf = new QListWidget(m_shelfDock);
  m_shelf->setObjectName("bookshelfList");
  installThumbnailDelegate(m_shelf);
  m_shelf->setGridSize({100, 160});
  m_shelf->setViewMode(QListView::IconMode);
  connect(m_shelf->verticalScrollBar(), &QScrollBar::valueChanged, this,
          [this] { requestCollectionCover(); });
  m_shelf->setIconSize(QSize(72, 108));
  m_shelf->setWordWrap(true);
  m_shelfDock->setWidget(m_shelf);
  w->addDockWidget(Qt::BottomDockWidgetArea, m_shelfDock);
  m_shelfDock->hide();
  connect(m_shelfDock, &QDockWidget::visibilityChanged, this,
          [this](bool visible) {
            if (visible)
              requestCollectionCover();
          });
  connect(m_shelf, &QListWidget::itemActivated, this,
          [this](QListWidgetItem *item) {
            openCollection(item->data(Qt::UserRole).toString());
          });
  for (const auto &[id, dock] :
       QVector<QPair<QString, QDockWidget *>>{{"bookLists", m_listsDock},
                                              {"toc", m_tocDock},
                                              {"bookshelf", m_shelfDock}}) {
    w->m_actions[id]->setCheckable(true);
    bind(id, [dock] { dock->setVisible(!dock->isVisible()); });
    connect(dock, &QDockWidget::visibilityChanged, this,
            [this, id](bool visible) {
              QSignalBlocker block(w->m_actions[id]);
              w->m_actions[id]->setChecked(visible);
            });
  }
  bind("closeBook", [this] {
    w->clearDocumentView();
    m_slideshow->stop();
    closingBook();
    w->m_openToken->store(++w->m_bookGeneration);
    w->m_renderToken->store(++w->m_renderGeneration);
    w->m_loading = false;
    w->m_book.reset();
    w->m_imageCache.clear();
    w->m_pages->clearItems();
    w->m_view->setMessage(QCoreApplication::translate("KomaMimic", "本を閉じました。"));
    w->showCovers();
  });
  bind("reloadBook", [this] {
    w->m_pendingDocumentLocation=w->currentDocumentLocation();w->m_restoreDocumentLocation=isTextDocumentPath(w->bookPath());
    if (!w->bookPath().isEmpty())
      w->loadBook(w->bookPath(), w->viewMode());
  });
  bind("goPage", [this] {
    bool ok = false;
    int page = QInputDialog::getInt(w, QCoreApplication::translate("KomaMimic", "指定ページへ"), QCoreApplication::translate("KomaMimic", "ページ（1始まり）"),
                                    w->pageIndex() + 1, 1,
                                    std::max(1, w->pageCount()), 1, &ok);
    if (ok) {
      w->setMode(MainWindow::ViewMode::Reader);
      w->jump(page - 1);
    }
  });
  for (int count : {10, 20, 50})
    for (int direction : {-1, 1})
      bind((direction > 0 ? "jumpNext" : "jumpPrevious") +
               QString::number(count),
           [this, count, direction] {
             if (w->pageCount())
               w->jump(std::clamp(w->m_anchor + direction * count, 0,
                                  w->pageCount() - 1));
           });
  for (const QString operation :
       {"selectStart", "selectEnd", "selectRange", "editCopy", "editCut",
        "editPaste", "editDelete", "editClear", "insertBlank", "addPages"})
    bind(operation, [this, operation] { editPages(operation); });
  bind("editUndo", [this] { m_undo->undo(); });
  bind("editRedo", [this] { m_undo->redo(); });
  connect(m_undo, &QUndoStack::canUndoChanged, this,
          [this](bool) { refresh(); });
  connect(m_undo, &QUndoStack::canRedoChanged, this,
          [this](bool) { refresh(); });
  for (int i = 0; i < 9; ++i)
    bind(QStringList{"sortNatural", "sortName", "sortType", "sortSize",
                     "sortDate", "sortEntry", "sortUnicode", "sortReverse",
                     "sortRandom"}[i],
         [this, i] { applySort(i); });
  for (const auto &[id, axis] : QVector<QPair<QString, int>>{
           {"splitVertical", 1}, {"splitHorizontal", 2}, {"splitReset", 0}})
    bind(id, [this, axis] { splitPages(axis); });
  w->m_actions["autoSpread"]->setCheckable(true);
  w->m_actions["autoSpread"]->setChecked(autoSpread());
  bind("autoSpread", [this] {
    w->m_settings->setValue("reading/autoSpread",
                            w->m_actions["autoSpread"]->isChecked());
    if (w->m_actions["autoSpread"]->isChecked())
      w->m_actions["spread"]->setChecked(true);
    w->render();
  });
  w->m_actions["coverBlank"]->setCheckable(true);
  w->m_actions["coverBlank"]->setChecked(w->m_options.leadingBlank);
  bind("coverBlank", [this] {
    w->m_options.leadingBlank = w->m_actions["coverBlank"]->isChecked();
    w->saveSettings();
    w->render();
  });
  for (const auto &[id, delta] :
       QVector<QPair<QString, QPointF>>{{"scrollLeft", {120, 0}},
                                        {"scrollRight", {-120, 0}},
                                        {"scrollUp", {0, 120}},
                                        {"scrollDown", {0, -120}}})
    bind(id, [this, delta] {
      auto movement=delta;
      if(delta.y()!=0 && w->m_settings->contains("input/keyVerticalPercent")) {
        double height=0;for(const auto &rect:w->m_view->pageRects())height=std::max(height,rect.height());
        if(height<=0)height=w->m_view->height();
        movement.setY((delta.y()>0?1:-1)*height*w->m_settings->value("input/keyVerticalPercent").toInt()/100.0);
      }
      w->m_view->scrollBy(movement,w->m_settings->value("input/smoothScroll",false).toBool()?w->m_settings->value("input/scrollSpeed",4000).toInt():0);
    });
  bind("scrollNext", [this] {
    if(w->documentViewActive()){w->nextPage();return;}
    if (!w->m_view->scan(1, w->m_options.rightToLeft, w->m_settings->value("reading/scrollOverlap", 15).toInt())) {
      if (!w->m_settings->value("reading/scrollTurnsPage", true).toBool()) return;
      const int old = w->m_anchor; const auto oldPath=w->bookPath(); w->nextPage();
      if (old != w->m_anchor || oldPath!=w->bookPath()) w->m_view->enterScan(1, w->m_options.rightToLeft);
      else if(w->m_options.boundaryMode==BoundaryMode::RepeatSource) {w->m_view->enterScan(1,w->m_options.rightToLeft);w->m_view->applyScanEntry();}
    }
  });
  bind("scrollPrevious", [this] {
    if(w->documentViewActive()){w->previousPage();return;}
    if (!w->m_view->scan(-1, w->m_options.rightToLeft, w->m_settings->value("reading/scrollOverlap", 15).toInt())) {
      if (!w->m_settings->value("reading/scrollTurnsPage", true).toBool()) return;
      const int old = w->m_anchor; const auto oldPath=w->bookPath(); w->previousPage();
      if (old != w->m_anchor || oldPath!=w->bookPath()) w->m_view->enterScan(-1, w->m_options.rightToLeft);
      else if(w->m_options.boundaryMode==BoundaryMode::RepeatSource) {w->m_view->enterScan(-1,w->m_options.rightToLeft);w->m_view->applyScanEntry();}
    }
  });
  w->m_actions["loupe"]->setCheckable(true);
  const auto loupe = [this] {
    w->m_view->setLoupe(w->m_actions["loupe"]->isChecked(),
                        w->m_settings->value("loupe/radius", 100).toInt(),
                        w->m_settings->value("loupe/zoom", 2).toDouble(),
                        w->m_settings->value("loupe/round", true).toBool());
  };
  bind("loupe", loupe);
  for (const auto &[id, key, factor] :
       QVector<std::tuple<QString, QString, double>>{
           {"loupeLarger", "radius", 1.2},
           {"loupeSmaller", "radius", 1 / 1.2},
           {"loupeZoomIn", "zoom", 1.25},
           {"loupeZoomOut", "zoom", 0.8}})
    bind(id, [this, key, factor, loupe] {
      w->m_settings->setValue(
          "loupe/" + key,
          w->m_settings->value("loupe/" + key, key == "radius" ? 100 : 2)
                  .toDouble() *
              factor);
      loupe();
    });
  bind("scaleSpecified", [this] {
    bool ok = false;
    double value =
        QInputDialog::getDouble(w, QCoreApplication::translate("KomaMimic", "倍率を指定"), QCoreApplication::translate("KomaMimic", "倍率（%）"),
                                w->m_view->zoomFactor() * 100, 5, 1600, 1, &ok);
    if (ok) {
      w->m_view->setScaleMode(ScaleMode::Actual);
      w->m_view->zoomBy(value / 100);
    }
  });
  bind("scaleCycle", [this] {
    w->m_view->setScaleMode(ScaleMode((int(w->m_view->scaleMode()) + 1) % 4));
  });
  bind("fitWindow", [this] {
    const auto images = w->m_view->images();
    if (!images.isEmpty() && !w->isFullScreen())
      w->resize(
          (images.first().size() / w->devicePixelRatioF() + QSize(280, 150))
              .boundedTo(w->screen()->availableGeometry().size()));
  });
  for (const auto &[id, kind] :
       QVector<QPair<QString, QString>>{{"addFavorite", "favorite"},
                                        {"addPlaylist", "playlist"},
                                        {"addShelf", "shelf"}})
    bind(id, [this, kind] { addCollection(kind); });
  bind("addToc", [this] { addToc(); });
  bind("marker", [this] { mark(false); });
  bind("gotoMarker", [this] { mark(true); });
  bind("nextToc", [this] { navigateToc(1); });
  bind("previousToc", [this] { navigateToc(-1); });
  bind("registerCover", [this] { registerBookImage(false); });
  bind("registerSpine", [this] { registerBookImage(true); });
  bind("trashPages", [this] { trashPages(); });
  bind("saveThumbnail", [this] { exportThumbnail(); });
  bind("listDetails", [this] { listDetails(); });
  bind("zipMetadataImport", [this] { zipMetadata(false); });
  bind("zipMetadataExport", [this] { zipMetadata(true); });
  bind("listOpen", [this] { loadList(false); });
  bind("listSave", [this] { loadList(true); });
  bind("saveSpread", [this] { output(ExportKind::Image, false, {}, true); });
  bind("saveImage", [this] { output(ExportKind::Image); });
  bind("saveOriginal", [this] { output(ExportKind::Original); });
  bind("saveAll", [this] { output(ExportKind::Directory, true); });
  bind("saveZip", [this] { output(ExportKind::Zip, true); });
  bind("savePdf", [this] { output(ExportKind::Pdf, true); });
  bind("saveGif", [this] { output(ExportKind::Gif, true); });
  for (int i = 1; i <= 4; ++i)
    bind(QString("quickSave%1").arg(i), [this, i] {
      auto path =
          w->m_settings->value(QString("quickSave/%1").arg(i)).toString();
      if (path.isEmpty()) {
        message(QCoreApplication::translate("KomaMimic", "先にクイックセーブの保存先を設定してください。"));
        return;
      }
      QDir().mkpath(path);
      output(ExportKind::Image, false,
             QDir(path).filePath(
                 QFileInfo(w->bookPath()).completeBaseName() +
                 QString("-%1.png").arg(w->m_anchor + 1, 6, 10, QChar('0'))));
    });
  bind("quickSaveSettings", [this] {
    for (int i = 1; i <= 4; ++i) {
      const auto path = QFileDialog::getExistingDirectory(
          w, QCoreApplication::translate("KomaMimic", "クイックセーブ%1の保存先").arg(i),
          w->m_settings->value(QString("quickSave/%1").arg(i)).toString());
      if (!path.isEmpty())
        w->m_settings->setValue(QString("quickSave/%1").arg(i), path);
    }
  });
  bind("imageCopy", [this] { clipboard(false); });
  bind("imagePaste", [this] { clipboard(true); });
  bind("print", [this] { print(false); });
  bind("pageSetup", [this] { print(true); });
  bind("timedList", [this] { timedList(); });
  bind("slideForward", [this] { slideshow(1); });
  bind("slideReverse", [this] { slideshow(-1); });
  bind("slidePause", [this] {
    if (m_slideshow->isActive()) {
      if (!m_timed.isEmpty())
        m_timedOffset += m_slideClock.elapsed();
      m_slideshow->stop();
    } else if (!m_timed.isEmpty()) {
      m_slideClock.restart();
      m_slideshow->start(20);
    } else if (m_slideDirection)
      slideshow(m_slideDirection);
  });
  bind("slideStop", [this] {
    m_slideshow->stop();
    m_slideDirection = 0;
    m_timed.clear();
    m_timedIndex = 0;
  });
  bind("clearCache", [this] {
    w->m_imageCache.clear();
    if (w->m_book)
      w->m_book->clearEncodedCache();
    message(QCoreApplication::translate("KomaMimic", "画像キャッシュを解放しました。"));
  });
  bind("preloadAll", [this] {
    if (!w->m_book || m_prefetching)
      return;
    m_prefetching = true;
    auto book = w->m_book;
    auto generation = w->m_bookGeneration;
    auto *watcher = new QFutureWatcher<QVector<QPair<int, ImageResult>>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this,
            [this, watcher, generation] {
              auto images = watcher->result();
              watcher->deleteLater();
              m_prefetching = false;
              if (generation != w->m_bookGeneration)
                return;
              for (auto &[index, image] : images)
                w->m_imageCache.insert(index, new ImageResult(image),
                                       int((image.memoryCost() + 1023) / 1024));
              message(QCoreApplication::translate("KomaMimic", "設定した容量内の画像をキャッシュしました。"));
            });
    watcher->setFuture(QtConcurrent::run(
        [book, budget = qint64(w->m_imageCache.maxCost()) * 1024] {
          QVector<QPair<int, ImageResult>> values;
          qint64 used = 0;
          for (int i = 0; i < book->pages.size(); ++i) {
            auto image = book->image(i);
            if (!image.error.isEmpty())
              continue;
            if (used + image.memoryCost() > budget)
              break;
            used += image.memoryCost();
            values.append({i, std::move(image)});
          }
          return values;
        }));
  });
  bind("importMangaMeeyaSettings", [this] { settingsFile(true, {}, true); });
  bind("importSettings", [this] { settingsFile(true); });
  bind("exportSettings", [this] { settingsFile(false); });
  bind("preferences", [this] { preferences(); });
  bind("toolbarSettings", [this] { toolbarSettings(); });
  bind("openUrl", [this] { openUrl(); });
  bind("external", [this] { external(0, false); });
  bind("externalSettings", [this] { external(0, true); });
  bind("findPages", [this] { if(isTextDocumentPath(w->bookPath()))w->command("findDocument")->trigger();else search(); });
  bind("renameFile", [this] {
    bool ok = false;
    const QFileInfo old(w->bookPath());
    const auto name =
        QInputDialog::getText(w, QCoreApplication::translate("KomaMimic", "ファイル名を変更"), QCoreApplication::translate("KomaMimic", "新しい名前"),
                              QLineEdit::Normal, old.fileName(), &ok);
    if (!ok || name.isEmpty() || name.contains('/') || name.contains('\\'))
      return;
    saveState(); w->saveDocumentLocation();
    if(w->m_book)w->m_book->clearEncodedCache();
    QFile file(old.absoluteFilePath());
    const auto target = old.dir().filePath(name);
    if (!file.rename(target)) {
      message(file.errorString());
      return;
    }
    if(!m_library->movePath(old.absoluteFilePath(), target)) {
      const bool rolledBack=QFile::rename(target,old.absoluteFilePath());
      message(rolledBack ? QCoreApplication::translate("KomaMimic", "読書履歴の移行に失敗したため、改名を取り消しました。") : QCoreApplication::translate("KomaMimic", "履歴の移行と改名の取り消しに失敗しました：")+target);
      return;
    }
    moveDocumentSettings(*w->m_settings, old.absoluteFilePath(), target);
    m_pendingId = m_activeId;
    m_intent = 0;
    w->openBook(target);
    // loadBook saves the closing document once more before switching paths.
    // Remove that final old-location write after the new settings snapshot exists.
    w->m_settings->remove(documentBookKey(old.absoluteFilePath()).chopped(1));
    w->m_settings->sync();
  });
  bind("newWindow", [this] {
    auto *window = new MainWindow(w->m_settings->fileName());
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->show();
    if (!w->bookPath().isEmpty())
      window->openBook(w->bookPath());
    else
      window->openDirectory(w->directory());
  });
  bind("minimize", [this] { w->showMinimized(); });
  bind("maximize",
       [this] { w->isMaximized() ? w->showNormal() : w->showMaximized(); });
  bind("tray", [this] {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
      message(QCoreApplication::translate("KomaMimic", "このデスクトップにはシステムトレイがありません。"));
      return;
    }
    auto *tray = findChild<QSystemTrayIcon *>();
    if (!tray) {
      tray = new QSystemTrayIcon(
          w->style()->standardIcon(QStyle::SP_FileDialogContentsView), this);
      tray->setToolTip("KomaMimic");
      auto *menu = new QMenu(w);
      menu->addAction(QCoreApplication::translate("KomaMimic", "ウィンドウを表示"), w, [this] {
        w->show();
        w->raise();
        w->activateWindow();
      });
      menu->addAction(QCoreApplication::translate("KomaMimic", "終了"), w, &QWidget::close);
      tray->setContextMenu(menu);
      connect(tray, &QSystemTrayIcon::activated, w,
              [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger ||
                    reason == QSystemTrayIcon::DoubleClick) {
                  w->show();
                  w->raise();
                  w->activateWindow();
                }
              });
    }
    tray->show();
    w->hide();
  });
  bind("imageInfo", [this] {
    if (!w->m_book)
      return;
    const auto images = w->m_view->images();
    QString text = QCoreApplication::translate("KomaMimic", "%1\n形式: %2\nページ: %3 / %4")
                       .arg(w->currentPageName(), I18n::formatLabel(w->m_book->format))
                       .arg(w->m_anchor + 1)
                       .arg(w->pageCount());
    if (!images.isEmpty())
      text += QCoreApplication::translate("KomaMimic", "\n寸法: %1 × %2\nメモリ: %3 KiB")
                  .arg(images.first().width())
                  .arg(images.first().height())
                  .arg(images.first().sizeInBytes() / 1024);
    QMessageBox::information(w, QCoreApplication::translate("KomaMimic", "画像情報"), text);
  });
  bind("cacheInfo", [this] { cacheInformation(); });
  bind("about", [this] {
    QMessageBox::about(
        w, QCoreApplication::translate("KomaMimic", "KomaMimicについて"),
        "KomaMimic " KOMAMIMIC_VERSION + QCoreApplication::translate("KomaMimic", "\nKomaMimic 画像ビューア\nライセンス: GNU GPL version 3\nQt・SDL・libarchive・ICU・zlibなどは各ライセンスに従います"
        "。"));
  });
  bind("help", [this] {
    const auto key = [this](const QString &id) {
      const auto value =
          w->command(id)->shortcut().toString(QKeySequence::NativeText);
      return value.isEmpty() ? QCoreApplication::translate("KomaMimic", "未割当") : value;
    };
    QMessageBox::information(
        w, QCoreApplication::translate("KomaMimic", "操作説明"),
        QCoreApplication::translate("KomaMimic", "%1: 本を開く（表紙一覧）\n%2: 表紙一覧へ戻る／上のフォルダ\n%3: "
            "ページ一覧\n%4: 全画面切替\n%5: "
            "キー設定\n\n各メニューの操作をキー設定で変更できます。詳細はREADME"
            ".mdを参照してください。")
            .arg(key("thumbAccept"), key("parentFolder"), key("thumbnails"),
                 key("fullscreenGlobal"), key("shortcuts")));
  });
  w->restoreToolbarSettings();
}
void ReaderFeatures::refresh() {
  updateInhibit();
  const QStringList sortIds{"sortNatural","sortDate","sortType","sortSize","sortEntry","sortUnicode","sortRandom","sortName"};
  for(int i=0;i<sortIds.size();++i) {auto *action=w->m_actions[sortIds[i]];action->setCheckable(true);action->setChecked(i==w->m_settings->value("covers/sort",0).toInt());}
  w->m_actions["sortReverse"]->setCheckable(true);w->m_actions["sortReverse"]->setChecked(w->m_settings->value("covers/reverse",false).toBool());
  const bool loaded = w->pageCount() > 0;
  for (const auto &spec : featureShortcutSpecs())
    if (spec.scope == ShortcutScope::Reader)
      w->m_actions[spec.id]->setEnabled(loaded || (spec.id.startsWith("sort") && w->m_mode==MainWindow::ViewMode::Covers));
  for (const auto &id : {"insertBlank", "addPages", "editPaste"})
    w->m_actions[id]->setEnabled(bool(w->m_book));
  if (w->m_book && w->m_book->format == "生成画像")
    for (const auto &id : {"addFavorite", "addPlaylist", "addShelf"})
      w->m_actions[id]->setEnabled(false);
  w->m_actions["editUndo"]->setEnabled(m_undo->canUndo());
  w->m_actions["editRedo"]->setEnabled(m_undo->canRedo());
  w->m_actions["saveOriginal"]->setEnabled(
      loaded && !w->m_book->pages[w->m_anchor].document);
  w->m_actions["reloadBook"]->setEnabled(bookPathExists(w->bookPath()));
  w->m_actions["renameFile"]->setEnabled(QFileInfo(w->bookPath()).isFile());
  w->m_actions["closeBook"]->setEnabled(bool(w->m_book) || w->isLoading());
  if (m_navigationBusy || (autoSpread() && !w->renderReady())) {
    w->m_actions["next"]->setEnabled(false);
    w->m_actions["previous"]->setEnabled(false);
  }
}
void ReaderFeatures::rebuildLists() {
  for (auto it = m_lists.begin(); it != m_lists.end(); ++it) {
    const auto selected =
        it.value()->currentItem()
            ? it.value()->currentItem()->data(Qt::UserRole).toString()
            : QString{};
    it.value()->clear();
    auto entries = m_library->items(it.key());
    const auto sorting =
        w->m_settings->value("lists/" + it.key() + "/sort", "order").toString();
    if (sorting != "order")
      std::stable_sort(entries.begin(), entries.end(),
                       [sorting](const auto &a, const auto &b) {
                         const auto text = [sorting](const auto &v) {
                           return sorting == "title" ? v.title
                                  : sorting == "path"
                                      ? v.path
                                      : v.metadata.value(sorting).toString();
                         };
                         return naturalLess(text(a), text(b));
                       });
    const bool icons =
        w->m_settings->value("lists/" + it.key() + "/icons", false).toBool();
    it.value()->setViewMode(icons ? QListView::IconMode : QListView::ListMode);
    for (const auto &entry : entries) {
      auto *item = new QListWidgetItem(entry.title, it.value());
      item->setData(Qt::UserRole, entry.id);
      item->setToolTip(entry.path + "\n" +
                       entry.metadata.value("author").toString() + " " +
                       entry.metadata.value("publisher").toString() +
                       QCoreApplication::translate("KomaMimic", "\n読書位置 %1 / %2")
                           .arg(entry.state.value("anchor").toInt() + 1)
                           .arg(entry.state.value("total").toInt()));
      if (icons)
        item->setIcon(m_collectionIcons.value(
            entry.id,
            ThumbnailGrid::thumbnailIcon({}, QFileInfo(entry.path).isDir())));
      if (entry.id == selected)
        it.value()->setCurrentItem(item);
    }
  }
  m_shelf->clear();
  for (const auto &entry : m_library->items("shelf")) {
    auto *item = new QListWidgetItem(
        m_collectionIcons.value(
            entry.id,
            ThumbnailGrid::thumbnailIcon({}, QFileInfo(entry.path).isDir())),
        entry.title, m_shelf);
    item->setData(Qt::UserRole, entry.id);
    item->setToolTip(entry.path);
  }
  requestCollectionCover();
}
void ReaderFeatures::closingBook() {
  if (m_inhibitor) m_inhibitor->setRequested(this, false);
  if (property("closing").toBool())
    m_slideshow->stop();
  m_watchReloadTimer->stop();
  if (!m_watcher->files().isEmpty())
    m_watcher->removePaths(m_watcher->files());
  if (!m_watcher->directories().isEmpty())
    m_watcher->removePaths(m_watcher->directories());
  if (!m_timed.isEmpty())
    m_slideshow->stop();
  m_timed.clear();
  if (!m_lastPositionKey.isEmpty())
    m_panPositions[m_lastPositionKey] = w->m_view->pan();
  m_saveTimer->stop();
  saveState();
  w->m_settings->setValue("window/docks",
                          w->isFullScreen() && !m_normalDocks.isEmpty()
                              ? m_normalDocks
                              : w->saveState());
  m_activeId.clear();
  m_sourceBook.reset();
  m_tocEntries = {};
  m_lastPositionKey.clear();
  m_panPositions.clear();
  m_restorePan = false;
  m_undo->clear();
}
void ReaderFeatures::openedBook() {
  m_visibleIndices.clear();
  m_dimensions.clear();
  m_navigationBusy = false;
  m_previousAnchor = 0;
  m_sourceBook = w->m_book;
  m_activeId = w->m_book->format == "生成画像" ? QString{}
               : m_pendingId.isEmpty()
                   ? m_library->add("history", w->bookPath())
                   : m_pendingId;
  m_pendingId.clear();
  m_selectionFirst = m_selectionLast = -1;
  auto item = m_library->item(m_activeId);
  m_tocEntries = item.state.value("toc").toArray();
  if (!w->documentViewActive()) restoreEditState(item.state);
  if(!w->documentViewActive() && !item.state.contains("pages") && item.state.value("legacySort").toInt()>0) {
    const int legacy=item.state.value("legacySort").toInt();
    if(legacy<=10) {
      auto pages=w->m_book->pages;
      const int mode=(legacy-1)/2;
      std::stable_sort(pages.begin(),pages.end(),[mode](const BookPage &a,const BookPage &b) {
        if(mode==1)return naturalLess(QFileInfo(a.name).fileName(),QFileInfo(b.name).fileName());
        if(mode==2 && QFileInfo(a.name).suffix()!=QFileInfo(b.name).suffix())return QFileInfo(a.name).suffix()<QFileInfo(b.name).suffix();
        if(mode==3 && a.encodedSize!=b.encodedSize)return a.encodedSize<b.encodedSize;
        if(mode==4 && a.modifiedMs!=b.modifiedMs)return a.modifiedMs<b.modifiedMs;
        return naturalLess(a.name,b.name);
      });
      if(legacy%2==0)std::reverse(pages.begin(),pages.end());
      w->m_book=w->m_book->withPages(pages);
    }
  }
  const bool resume = m_intent == 0;
  if (m_intent == 0)
    w->m_anchor = std::clamp(item.state.value("anchor").toInt(), 0,
                             std::max(0, w->pageCount() - 1));
  else if (m_intent == 2) {
    w->m_anchor = resolvePosition(item.state.value("markerKey").toString(),item.state.value("marker").toInt());
    if(isTextDocumentPath(w->bookPath()) && item.state.contains("markerLocation")) {
      w->m_pendingDocumentLocation=DocumentLocation::fromJson(item.state.value("markerLocation").toObject());
      w->m_restoreDocumentLocation=true;
    }
  }
  m_intent = 1;
  if (resume && isTextDocumentPath(w->bookPath())) {
    w->m_pendingDocumentLocation = DocumentLocation::fromJson(item.state.value("documentLocation").toObject());
    w->m_restoreDocumentLocation = !w->m_pendingDocumentLocation.section.isEmpty();
  }
  if (resume) {
    if (item.state.contains("rtl"))
      w->m_actions["rtl"]->setChecked(item.state.value("rtl").toBool());
    if (item.state.contains("spread"))
      w->m_actions["spread"]->setChecked(item.state.value("spread").toBool());
    const auto key = item.state.value("positionKey").toString();
    for (int i = 0; i < w->pageCount(); ++i)
      if (positionKey(w->m_book->pages[i]) == key) {
        w->m_anchor = i;
        break;
      }
    m_restorePan = true;
    m_restoredPan = {item.state.value("panX").toDouble(),
                     item.state.value("panY").toDouble()};
    if (item.state.contains("scale")) {
      const auto mode =
          ScaleMode(std::clamp(item.state.value("scale").toInt(), 0, 6));
      w->m_view->setScaleMode(mode == ScaleMode::Zoom ? ScaleMode::Actual
                                                      : mode);
      if (mode == ScaleMode::Zoom)
        w->m_view->zoomBy(item.state.value("zoom").toDouble(1));
    }
  }
  const auto pans = item.state.value("pans").toObject();
  for (auto it = pans.begin(); it != pans.end(); ++it) {
    const auto xy = it.value().toArray();
    if (xy.size() == 2)
      m_panPositions[it.key()] = {xy[0].toDouble(), xy[1].toDouble()};
  }
  m_toc->clear();
  for (const auto &value : m_tocEntries) {
    auto entry = value.toObject();
    auto *row = new QListWidgetItem(entry.value("label").toString(), m_toc);
    row->setData(Qt::UserRole, resolvePosition(entry.value("key").toString(),entry.value("page").toInt()));
  }
  if (!m_watcher->files().isEmpty())
    m_watcher->removePaths(m_watcher->files());
  if (w->m_settings->value("General/watch", false).toBool() &&
      bookPathExists(w->bookPath()))
    m_watcher->addPath(physicalBookPath(w->bookPath()));
  refreshDocumentToc();
  rebuildLists();
}
void ReaderFeatures::rendered(const QVector<int> &indices,
                              const QVector<ImageResult> &originals) {
  m_saveTimer->start();
  if (w->documentViewActive()) return;
  if (w->pageCount()) {
    const auto key = positionKey(w->m_book->pages[w->m_anchor]);
    const int origin = w->m_settings->value("display/origin", 0).toInt();
    if (m_restoreSourceOrigin) {
      w->m_view->setSourceOrigin(m_restoredPan);
      m_restoreSourceOrigin = false;
      m_restorePan = false;
    } else if (m_restorePan) {
      w->m_view->setPan(m_restoredPan);
      m_restorePan = false;
    } else if ((w->m_settings->value("display/rememberPan", false).toBool() &&
                m_panPositions.contains(key)))
      w->m_view->setPan(m_panPositions.value(key));
    else if (key == m_lastPositionKey || origin == 4)
      w->m_view->setPan(m_previousPan);
    else if (origin >= 1 && origin <= 3)
      w->m_view->setPan({origin == 1 ? 1e8 : origin == 2 ? -1e8 : 0, 1e8});
    m_lastPositionKey = key;
  }
  if (w->m_settings->value("General/watch", false).toBool() && w->m_book &&
      w->m_book->filesystem) {
    QStringList keep;
    if (QFileInfo(w->bookPath()).isFile())
      keep.append(w->bookPath());
    for (int index : indices)
      keep.append(w->m_book->pages[index].filePath);
    QStringList old;
    for (const auto &file : m_watcher->files())
      if (!keep.contains(file))
        old.append(file);
    if (!old.isEmpty())
      m_watcher->removePaths(old);
    QStringList paths;
    for (int index : indices) {
      const auto &file = w->m_book->pages[index].filePath;
      if (!file.isEmpty() && !m_watcher->files().contains(file))
        paths.append(file);
    }
    if (!paths.isEmpty())
      m_watcher->addPaths(paths);
  }
  m_visibleIndices = indices;
  for (int i = 0; i < indices.size(); ++i)
    m_dimensions[indices[i]] = originals[i].image.size()/originals[i].image.devicePixelRatio();
  if (w->m_anchor != m_previousAnchor) {
    m_lastDirection = w->m_anchor > m_previousAnchor ? 1 : -1;
    if (m_input)
      m_input->pageTurned();
  }
  m_previousAnchor = w->m_anchor;
  if (m_prefetching || !w->m_book)
    return;
  QVector<int> wanted;
  for (int sign : {m_lastDirection, -m_lastDirection}) {
    int anchor = w->m_anchor;
    const int count = std::clamp(
        w->m_settings
            ->value(sign == m_lastDirection ? "cache/ahead" : "cache/behind",
                    sign == m_lastDirection ? 2 : 1)
            .toInt(),
        0, 20);
    for (int unit = 0; unit < count; ++unit) {
      const int next =
          sign > 0 ? nextAnchor(w->pageCount(), anchor, w->m_options)
                   : previousAnchor(w->pageCount(), anchor, w->m_options);
      if (next == anchor)
        break;
      anchor = next;
      for (int page : visiblePages(w->pageCount(), anchor, w->m_options))
        if (!w->m_imageCache.contains(page) && !wanted.contains(page))
          wanted.append(page);
    }
  }
  if (wanted.isEmpty())
    return;
  m_prefetching = true;
  const auto book = w->m_book;
  const auto generation = w->m_bookGeneration;
  auto *watcher = new QFutureWatcher<QVector<QPair<int, ImageResult>>>(this);
  connect(watcher, &QFutureWatcherBase::finished, this,
          [this, watcher, generation, book] {
            const auto images = watcher->result();
            watcher->deleteLater();
            m_prefetching = false;
            if (generation != w->m_bookGeneration || book != w->m_book)
              return;
            for (const auto &[index, image] : images) {
              m_dimensions[index] = image.image.size()/image.image.devicePixelRatio();
              const auto cost = (image.memoryCost() + 1023) / 1024;
              if (cost <= w->m_imageCache.maxCost())
                w->m_imageCache.insert(index, new ImageResult(image),
                                       int(cost));
            }
          });
  const auto budget = qint64(w->m_imageCache.maxCost()) * 1024;
  watcher->setFuture(QtConcurrent::run([book, wanted, budget] {
    QVector<QPair<int, ImageResult>> values;
    qint64 used = 0;
    for (int page : wanted) {
      auto image = book->image(page);
      if (!image.error.isEmpty())
        continue;
      if (used + image.memoryCost() > budget)
        break;
      used += image.memoryCost();
      values.append({page, std::move(image)});
    }
    return values;
  }));
}
void ReaderFeatures::saveState() {
  if (m_activeId.isEmpty() || !w->m_book || m_restoring)
    return;
  auto item = m_library->item(m_activeId);
  auto state = item.state;
  state["anchor"] = w->m_anchor;
  if (isTextDocumentPath(w->bookPath())) { state["documentLocation"] = w->currentDocumentLocation().json(); w->saveDocumentLocation(); }
  state["name"] = w->currentPageName();
  state["toc"] = m_tocEntries;
  state["total"] = w->pageCount();
  state["panX"] = w->m_view->pan().x();
  state["panY"] = w->m_view->pan().y();
  state["rtl"] = w->m_options.rightToLeft;
  state["spread"] = w->m_options.spread;
  state["scale"] = int(w->m_view->scaleMode());
  state["zoom"] = w->m_view->zoomFactor();
  if (w->pageCount())
    state["positionKey"] = positionKey(w->m_book->pages[w->m_anchor]);
  if (w->m_settings->value("display/rememberPan", false).toBool()) {
    QJsonObject pans;
    for (auto it = m_panPositions.begin(); it != m_panPositions.end(); ++it)
      pans[it.key()] = QJsonArray{it.value().x(), it.value().y()};
    state["pans"] = pans;
  }
  if (!w->documentViewActive() && m_sourceBook && w->m_book != m_sourceBook) {
    QSet<QString> sourceKeys;
    for (const auto &page : m_sourceBook->pages)
      sourceKeys.insert(pageKey(page));
    QJsonArray pages;
    for (const auto &page : w->m_book->pages) {
      QJsonObject value{{"key", pageKey(page)}};
      if (!page.inlineData.isEmpty() && !sourceKeys.contains(pageKey(page)))
        value["data"] = QString::fromLatin1(page.inlineData.toBase64());
      if (!page.portion.isEmpty())
        value["portion"] =
            QJsonArray{page.portion.x(), page.portion.y(), page.portion.width(),
                       page.portion.height()};
      value["name"] = page.name;
      value["identity"] = page.identity;
      pages.append(value);
    }
    state["pages"] = pages;
  }
  m_library->update(item.id, item.metadata, state);
}
void ReaderFeatures::restoreEditState(const QJsonObject &state) {
  if (!state.contains("pages") || !m_sourceBook)
    return;
  QHash<QString, BookPage> original;
  for (const auto &page : m_sourceBook->pages)
    original[pageKey(page)] = page;
  const auto saved = state.value("pages").toArray();
  if (saved.size() > 100000)
    return;
  QVector<BookPage> pages;
  qint64 bytes = 0;
  for (const auto &entry : saved) {
    auto value = entry.toObject();
    BookPage page;
    if (original.contains(value.value("key").toString()))
      page = original.value(value.value("key").toString());
    else if (value.contains("data")) {
      page.name = value.value("name").toString();
      page.identity = value.value("identity").toString();
      page.inlineData =
          QByteArray::fromBase64(value.value("data").toString().toLatin1());
      bytes += page.inlineData.size();
      if (bytes > 128 * 1024 * 1024)
        return;
    } else
      continue;
    auto rect = value.value("portion").toArray();
    if (rect.size() == 4)
      page.portion = QRectF(rect[0].toDouble(), rect[1].toDouble(),
                            rect[2].toDouble(), rect[3].toDouble());
    pages.append(page);
  }
  w->m_book = w->m_book->withPages(pages);
}
void ReaderFeatures::openCollection(const QString &id, int intent) {
  const auto item = m_library->item(id);
  if (item.id.isEmpty())
    return;
  m_pendingId = id;
  m_intent = intent;
  w->loadBook(item.path, MainWindow::ViewMode::Reader);
}
void ReaderFeatures::addCollection(const QString &kind) {
  if (!w->m_book)
    return;
  saveState();
  const auto old = m_library->item(m_activeId);
  const auto id = m_library->add(kind, w->bookPath());
  const auto existing = m_library->item(id);
  if (existing.state.isEmpty())
    m_library->update(id, old.metadata, old.state);
  rebuildLists();
  m_listsDock->show();
  m_tabs->setCurrentWidget(m_lists[kind]);
  message(QCoreApplication::translate("KomaMimic", "リストに登録しました。"));
}
QVector<int> ReaderFeatures::selectedPages() const {
  QVector<int> result;
  if (m_selectionFirst >= 0 && m_selectionLast >= 0) {
    for (int i = std::min(m_selectionFirst, m_selectionLast);
         i <= std::max(m_selectionFirst, m_selectionLast) && i < w->pageCount();
         ++i)
      result.append(i);
  } else if (w->viewMode() == MainWindow::ViewMode::Pages &&
             w->m_pages->currentRow() >= 0)
    result = {w->m_pages->currentRow()};
  else
    result = visiblePagesForView();
  return result;
}
void ReaderFeatures::replacePages(const QVector<BookPage> &pages, int anchor) {
  changeBook(w->m_book->withPages(pages), anchor);
}
void ReaderFeatures::changeBook(std::shared_ptr<const Book> book, int anchor) {
  w->m_renderToken->store(++w->m_renderGeneration);
  w->m_book = std::move(book);
  for (int i = 0; i < m_tocEntries.size(); ++i) {
    auto entry = m_tocEntries[i].toObject();
    const int page = resolvePosition(entry.value("key").toString(),
                                     entry.value("page").toInt());
    entry["page"] = page;
    m_tocEntries[i] = entry;
    if (i < m_toc->count())
      m_toc->item(i)->setData(Qt::UserRole, page);
  }
  w->m_imageCache.clear();
  m_selectionFirst = m_selectionLast = -1;
  w->m_anchor = std::clamp(anchor, 0, std::max(0, w->pageCount() - 1));
  w->populatePages();
  w->render();
  m_saveTimer->start();
  refresh();
}
void ReaderFeatures::editPages(const QString &operation) {
  if (!w->m_book)
    return;
  if (operation == "selectStart") {
    m_selectionFirst = w->m_anchor;
    m_selectionLast = -1;
    message(QCoreApplication::translate("KomaMimic", "選択の始点を設定しました。"));
    return;
  }
  if (operation == "selectEnd") {
    m_selectionLast = w->m_anchor;
    if (m_selectionFirst < 0)
      m_selectionFirst = m_selectionLast;
    message(QCoreApplication::translate("KomaMimic", "%1〜%2ページを選択しました。")
                .arg(m_selectionFirst + 1)
                .arg(m_selectionLast + 1));
    return;
  }
  if (operation == "selectRange") {
    bool ok = false;
    const auto text = QInputDialog::getText(
        w, QCoreApplication::translate("KomaMimic", "ページ範囲"), QCoreApplication::translate("KomaMimic", "先頭,末尾（例: 1,10）"), QLineEdit::Normal,
        QString("%1,%1").arg(w->m_anchor + 1), &ok);
    auto values = text.split(',');
    if (ok && values.size() == 2) {
      m_selectionFirst =
          std::clamp(values[0].toInt() - 1, 0, w->pageCount() - 1);
      m_selectionLast =
          std::clamp(values[1].toInt() - 1, 0, w->pageCount() - 1);
    }
    return;
  }
  const auto before = w->m_book->pages;
  auto after = before;
  auto selected = selectedPages();
  int anchor = w->m_anchor;
  if (operation == "editCopy" || operation == "editCut") {
    m_pageClipboard.clear();
    qint64 bytes = 0;
    for (int i : selected) {
      auto page = before[i];
      const auto data = w->m_book->encoded(i);
      if (!data.error.isEmpty()) {
        message(data.error);
        return;
      }
      bytes += data.bytes.size();
      if (bytes > 128 * 1024 * 1024) {
        m_pageClipboard.clear();
        message(QCoreApplication::translate("KomaMimic", "コピーする画像が128 MiBを超えています。"));
        return;
      }
      page.inlineData = data.bytes;
      page.identity = QUuid::createUuid().toString(QUuid::WithoutBraces);
      if (page.document) {
        page.document = false;
        page.archivedText.reset();
        page.documentPage = 0;
        page.portion = {};
        page.name += ".png";
      }
      m_pageClipboard.append(page);
    }
    if (operation == "editCopy") {
      clipboard(false);
      return;
    }
  }
  if (operation == "editDelete" || operation == "editCut") {
    std::sort(selected.begin(), selected.end(), std::greater<int>());
    for (int i : selected)
      after.removeAt(i);
  } else if (operation == "editClear")
    after.clear();
  else if (operation == "editPaste") {
    if (m_pageClipboard.isEmpty()) {
      clipboard(true);
      return;
    }
    for (int i = 0; i < m_pageClipboard.size(); ++i) {
      auto page = m_pageClipboard[i];
      page.identity = QUuid::createUuid().toString(QUuid::WithoutBraces);
      after.insert(std::min(anchor + 1 + i, int(after.size())), page);
    }
    ++anchor;
  } else if (operation == "insertBlank") {
    QImage image(1200, 1600, QImage::Format_RGB32);
    image.fill(Qt::white);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    BookPage page;
    page.name = QCoreApplication::translate("KomaMimic", "白紙.png");
    page.identity = QUuid::createUuid().toString(QUuid::WithoutBraces);
    page.inlineData = bytes;
    after.insert(std::min(anchor, int(after.size())), page);
  } else if (operation == "addPages") {
    const auto paths = QFileDialog::getOpenFileNames(
        w, QCoreApplication::translate("KomaMimic", "画像を追加"), w->directory(), openFileFilter());
    qint64 bytes = 0;
    for (const auto &path : paths) {
      if (!isImagePath(path))
        continue;
      QFile file(path);
      if (!file.open(QIODevice::ReadOnly) || file.size() > MaxEncodedBytes) {
        message(QCoreApplication::translate("KomaMimic", "追加する画像を読めません。"));
        return;
      }
      BookPage page;
      page.name = QFileInfo(path).fileName();
      page.identity = QUuid::createUuid().toString(QUuid::WithoutBraces);
      page.inlineData = file.readAll();
      bytes += page.inlineData.size();
      if (bytes > 128 * 1024 * 1024) {
        message(QCoreApplication::translate("KomaMimic", "追加する画像が128 MiBを超えています。"));
        return;
      }
      after.append(page);
    }
  } else
    return;
  m_selectionFirst = m_selectionLast = -1;
  m_undo->push(new PageEdit(this, before, after, w->m_anchor, anchor,
                            w->m_actions[operation]->text()));
}
void ReaderFeatures::beforeRender() {
  m_previousPan = w->m_view->pan();
  if (!m_lastPositionKey.isEmpty())
    m_panPositions[m_lastPositionKey] = m_previousPan;
}
void ReaderFeatures::applySavedSort() {
  // A saved structural edit (blank insertion, deletion, split, pasted page)
  // takes precedence over automatic defaults. Explicit sorting still works.
  if(m_sourceBook && w->m_book) {
    if(m_sourceBook->pages.size()!=w->m_book->pages.size())return;
    QSet<QString> source;
    for(const auto &page:m_sourceBook->pages)source.insert(positionKey(page));
    for(const auto &page:w->m_book->pages)if(!source.contains(positionKey(page)))return;
  }
  const int modes[]{0,4,2,3,5,6,8,1};
  applySort(modes[std::clamp(w->m_settings->value("covers/sort",0).toInt(),0,7)],false);
}
void ReaderFeatures::applySort(int mode, bool savePreference) {
  if(savePreference) {
    if(mode==8)w->m_coverSortSeed=QRandomGenerator::global()->generate()|1u;
    if(mode==7)w->m_settings->setValue("covers/reverse",!w->m_settings->value("covers/reverse",false).toBool());
    else {const int mapping[]{0,7,2,3,1,4,5,0,6};w->m_settings->setValue("covers/sort",mapping[std::clamp(mode,0,8)]);}
    w->m_settings->sync();
  }
  if(w->m_mode==MainWindow::ViewMode::Covers && savePreference) {
    if(w->m_archiveDirectory)w->populateArchiveDirectory(w->m_archiveDirectory,false);
    else if(!w->m_directory.isEmpty())w->populateDirectory(w->m_directory,false,false);
    applySavedSort();
    w->refreshState();return;
  }
  if (!w->m_book || !w->pageCount() || w->documentViewActive())
    return;
  auto pages = w->m_book->pages;
  const auto current = positionKey(pages[w->m_anchor]);
  if (mode == 7)
    std::reverse(pages.begin(), pages.end());
  else if (mode == 8) {
    std::mt19937 generator(QRandomGenerator::global()->generate());
    std::shuffle(pages.begin(), pages.end(), generator);
  } else
    std::stable_sort(pages.begin(), pages.end(),
                     [mode](const BookPage &a, const BookPage &b) {
                       if (mode == 1)
                         return naturalLess(QFileInfo(a.name).fileName(),
                                            QFileInfo(b.name).fileName());
                       if (mode == 2 && QFileInfo(a.name).suffix() !=
                                            QFileInfo(b.name).suffix())
                         return QFileInfo(a.name).suffix() <
                                QFileInfo(b.name).suffix();
                       if (mode == 3 && a.encodedSize != b.encodedSize)
                         return a.encodedSize < b.encodedSize;
                       if (mode == 4 && a.modifiedMs != b.modifiedMs)
                         return a.modifiedMs < b.modifiedMs;
                       if (mode == 5 && a.archiveIndex != b.archiveIndex)
                         return a.archiveIndex < b.archiveIndex;
                       if (mode == 6)
                         return a.name < b.name;
                       return naturalLess(a.name, b.name);
                     });
  if(mode!=7 && w->m_settings->value("covers/reverse",false).toBool())std::reverse(pages.begin(),pages.end());
  bool same=true;
  for(int i=0;i<pages.size();++i)if(positionKey(pages[i])!=positionKey(w->m_book->pages[i])) {same=false;break;}
  if(same) {w->refreshState();return;}
  int anchor = 0;
  for (int i = 0; i < pages.size(); ++i)
    if (positionKey(pages[i]) == current) {
      anchor = i;
      break;
    }
  if(savePreference)m_undo->push(new PageEdit(this, w->m_book->pages, pages, w->m_anchor, anchor,
                            QCoreApplication::translate("KomaMimic", "並び順を変更")));
  else {
    changeBook(w->m_book->withPages(pages),anchor);
  }
  w->refreshState();
}
void ReaderFeatures::splitPages(int axis) {
  if (!w->m_book)
    return;
  if (axis == 0) {
    const auto current = pageKey(w->m_book->pages[w->m_anchor]);
    auto pages = w->m_book->pages;
    QSet<QString> seen;
    QVector<BookPage> merged;
    for (auto page : pages) {
      if (!page.portion.isNull()) {
        if (seen.contains(pageKey(page)))
          continue;
        seen.insert(pageKey(page));
        page.portion = {};
      }
      merged.append(page);
    }
    int target = 0;
    for (int i = 0; i < merged.size(); ++i)
      if (pageKey(merged[i]) == current) {
        target = i;
        break;
      }
    m_undo->push(new PageEdit(this, w->m_book->pages, merged, w->m_anchor,
                              target, QCoreApplication::translate("KomaMimic", "分割を解除")));
    return;
  }
  const auto book = w->m_book;
  const auto generation = w->m_bookGeneration;
  auto *watcher = new QFutureWatcher<QVector<BookPage>>(this);
  connect(watcher, &QFutureWatcherBase::finished, this,
          [this, watcher, generation, book] {
            const auto pages = watcher->result();
            watcher->deleteLater();
            if (generation != w->m_bookGeneration || book != w->m_book)
              return;
            w->m_actions["spread"]->setChecked(false);
            m_undo->push(new PageEdit(this, w->m_book->pages, pages,
                                      w->m_anchor, 0, QCoreApplication::translate("KomaMimic", "ページを分割")));
          });
  watcher->setFuture(QtConcurrent::run(
      [book, axis, rtl = w->m_options.rightToLeft,
       condition = w->m_settings
                       ->value(axis == 1 ? "reading/splitVCondition"
                                         : "reading/splitHCondition",
                               axis == 1 ? "@width>@height" : "@height>@width")
                       .toString(),
       position = w->m_settings
                      ->value(axis == 1 ? "reading/splitVPosition"
                                        : "reading/splitHPosition",
                              axis == 1 ? "@width/2" : "@height/2")
                      .toString(),
       overlap = w->m_settings
                     ->value(axis == 1 ? "reading/splitVOverlap"
                                       : "reading/splitHOverlap",
                             0)
                     .toInt()] {
        QVector<BookPage> pages;
        for (int i = 0; i < book->pages.size(); ++i) {
          auto page = book->pages[i];
          const auto image = book->image(i).image;
          const QHash<QString, double> variables{
              {"width", double(image.width())},
              {"height", double(image.height())},
              {"page", double(i + 1)},
              {"totalpage", double(book->pages.size())}};
          const auto conditionValue = evaluateExpression(condition, variables);
          const auto positionValue = evaluateExpression(position, variables);
          const bool split = !image.isNull() && page.portion.isNull() &&
                             conditionValue.error.isEmpty() &&
                             positionValue.error.isEmpty() &&
                             conditionValue.value;
          if (!split) {
            pages.append(page);
            continue;
          }
          auto a = page, b = page;
          const double size = axis == 1 ? image.width() : image.height();
          if (size < 2) {
            pages.append(page);
            continue;
          }
          const double point =
              std::clamp(positionValue.value, 1.0, size - 1) / size;
          const double allowance =
              std::clamp(overlap / size, 0.0, std::min(point, 1 - point));
          if (axis == 1) {
            a.portion =
                rtl ? QRectF(point - allowance, 0, 1 - point + allowance, 1)
                    : QRectF(0, 0, point + allowance, 1);
            b.portion =
                rtl ? QRectF(0, 0, point + allowance, 1)
                    : QRectF(point - allowance, 0, 1 - point + allowance, 1);
          } else {
            a.portion = QRectF(0, 0, 1, point + allowance);
            b.portion = QRectF(0, point - allowance, 1, 1 - point + allowance);
          }
          pages.append(a);
          pages.append(b);
        }
        return pages;
      }));
}
bool ReaderFeatures::autoSpread() const {
  return w->m_settings->value("reading/autoSpread", false).toBool();
}
int ReaderFeatures::advance(int direction, bool onePage) const {
  if (!w->pageCount())
    return 0;
  if (w->documentViewActive())
    return advanceDocumentPage(w->pageCount(), w->m_anchor, direction,
                                w->m_book->openOptions.text.pageLayout, onePage, w->m_documentWidePages,w->m_documentSides,w->m_documentSoloPages,w->m_book->publication->rightToLeft);
  if (onePage)
    return std::clamp(w->m_anchor + direction, 0, w->pageCount() - 1);
  if (autoSpread() && w->m_options.spread && direction > 0 &&
      !m_visibleIndices.isEmpty() && m_visibleIndices.first() == w->m_anchor) {
    const int next = w->m_anchor + m_visibleIndices.size();
    return next < w->pageCount() ? next : w->m_anchor;
  }
  return direction > 0
             ? nextAnchor(w->pageCount(), w->m_anchor, w->m_options)
             : previousAnchor(w->pageCount(), w->m_anchor, w->m_options);
}
bool ReaderFeatures::navigate(int direction, bool onePage) {
  if (w->documentViewActive()) return false;
  if (!autoSpread() || !w->m_options.spread || onePage)
    return false;
  if (m_navigationBusy || !w->renderReady())
    return true;
  if (direction > 0) {
    const int target = advance(direction, false);
    if (target == w->m_anchor)
      return false;
    w->jump(target);
    return true;
  }
  if (w->m_anchor <= 0)
    return false;
  const int before = w->m_anchor;
  const auto book = w->m_book;
  if (before == 1 ||
      ((w->m_options.coverAlone || w->m_options.leadingBlank) && before == 2)) {
    w->jump(before == 1 ? 0 : 1);
    return true;
  }
  m_navigationBusy = true;
  w->refreshState();
  auto *watcher = new QFutureWatcher<int>(this);
  connect(watcher, &QFutureWatcherBase::finished, this,
          [this, watcher, book, before] {
            const int target = watcher->result();
            watcher->deleteLater();
            m_navigationBusy = false;
            if (book == w->m_book && before == w->m_anchor)
              w->jump(target);
            w->refreshState();
          });
  watcher->setFuture(QtConcurrent::run(
      [book, before,
       condition =
           w->m_settings->value("reading/spreadCondition", "@height>@width")
               .toString()] {
        const auto previous = book->image(before - 1, QSize(128, 128)).image;
        const auto earlier = book->image(before - 2, QSize(128, 128)).image;
        return previous.isNull() || earlier.isNull() ||
                       !evaluateExpression(
                            condition,
                            {{"width", double(previous.width())},
                             {"height", double(previous.height())},
                             {"page", double(before)},
                             {"totalpage", double(book->pages.size())}})
                            .value ||
                       !evaluateExpression(
                            condition,
                            {{"width", double(earlier.width())},
                             {"height", double(earlier.height())},
                             {"page", double(before - 1)},
                             {"totalpage", double(book->pages.size())}})
                            .value
                   ? before - 1
                   : before - 2;
      }));
  return true;
}
void ReaderFeatures::fullscreenChanging(bool fullscreen) {
  if (fullscreen) {
    m_normalDocks = w->saveState();
    for (auto *dock : {m_listsDock, m_shelfDock, m_tocDock}) {
      m_dockVisibility[dock] = dock->isVisible();
      if (w->m_settings->value("fullscreen/panes", 2).toInt() != 1)
        dock->hide();
    }
  } else {
    for (auto it = m_dockVisibility.cbegin(); it != m_dockVisibility.cend();
         ++it)
      it.key()->setVisible(it.value());
    m_dockVisibility.clear();
  }
}
void ReaderFeatures::fullscreenMouse(const QPoint &position) {
  if (!w->isFullScreen() ||
      w->m_settings->value("fullscreen/panes", 2).toInt() != 0 ||
      QApplication::activePopupWidget())
    return;
  const auto retain = [&](QWidget *widget) {
    return widget->isVisible() &&
           QRect(widget->mapTo(w, QPoint()), widget->size())
               .adjusted(-12, -12, 12, 12)
               .contains(position);
  };
  w->m_treePanel->setVisible(w->m_treeBeforeFullscreen &&
                             (position.x() < 8 || retain(w->m_treePanel)));
  m_tocDock->setVisible(m_dockVisibility.value(m_tocDock) &&
                        (position.x() < 8 || retain(m_tocDock)));
  m_listsDock->setVisible(
      m_dockVisibility.value(m_listsDock) &&
      (position.x() >= w->width() - 8 || retain(m_listsDock)));
  m_shelfDock->setVisible(
      m_dockVisibility.value(m_shelfDock) &&
      (position.y() >= w->height() - 8 || retain(m_shelfDock)));
}

void ReaderFeatures::showCollection(const QString &kind, int layout) {
  auto *list = m_lists.value(kind);
  if (!list)
    return;
  w->m_settings->setValue("lists/" + kind + "/icons", true);
  list->setViewMode(QListView::IconMode);
  list->setProperty("coverLayout", layout);
  w->m_settings->setValue("lists/" + kind + "/layout", layout);
  const int columns =
      layout == 0   ? std::max(1, int(std::ceil(std::sqrt(
                                      double(std::max(1, list->count()))))))
      : layout == 1 ? 8
                    : w->m_settings->value("lists/columns", 4).toInt();
  const int cell =
      std::max(16, list->viewport()->width() / std::max(1, columns));
  list->setIconSize(
      {std::max(40, cell - 16), std::max(60, (cell - 16) * 3 / 2)});
  list->setGridSize({cell, cell * 3 / 2 + 48});
  m_tabs->setCurrentWidget(list);
  m_listsDock->show();
  if (layout == 1)
    w->resizeDocks({m_listsDock}, {std::min(800, w->width() * 2 / 3)},
                   Qt::Horizontal);
  list->setFocus();
  rebuildLists();
}
void ReaderFeatures::requestCollectionCover() {
  if (w->m_backgroundCancel->load() || m_coverBusy || !m_shelf || !m_tabs)
    return;
  QListWidget *target = nullptr;
  LibraryItem chosen;
  for (auto *list :
       {m_shelf, qobject_cast<QListWidget *>(m_tabs->currentWidget())}) {
    if (!list || !list->isVisible())
      continue;
    for (int i = 0; i < list->count(); ++i) {
      const auto id = list->item(i)->data(Qt::UserRole).toString();
      if (!m_collectionIcons.contains(id) &&
          list->visualItemRect(list->item(i))
              .intersects(list->viewport()->rect())) {
        chosen = m_library->item(id);
        target = list;
        break;
      }
    }
    if (!chosen.id.isEmpty())
      break;
  }
  if (chosen.id.isEmpty())
    return;
  m_coverBusy = true;
  auto *watcher = new QFutureWatcher<QImage>(this);
  connect(watcher, &QFutureWatcherBase::finished, this,
          [this, watcher, id = chosen.id] {
            const auto image = watcher->result();
            watcher->deleteLater();
            m_coverBusy = false;
            if (w->m_backgroundCancel->load()) return;
            if (m_collectionIcons.size() >= 512 &&
                !m_collectionIconOrder.isEmpty()) {
              const auto stale = m_collectionIconOrder.takeFirst();
              m_collectionIcons.remove(stale);
              for (auto *list : m_lists)
                for (int i = 0; i < list->count(); ++i)
                  if (list->item(i)->data(Qt::UserRole).toString() == stale)
                    list->item(i)->setIcon(ThumbnailGrid::thumbnailIcon());
              for (int i = 0; i < m_shelf->count(); ++i)
                if (m_shelf->item(i)->data(Qt::UserRole).toString() == stale)
                  m_shelf->item(i)->setIcon(ThumbnailGrid::thumbnailIcon());
            }
            m_collectionIconOrder.removeAll(id);
            m_collectionIconOrder.append(id);
            m_collectionIcons[id] = ThumbnailGrid::thumbnailIcon(image, false);
            for (auto *list : m_lists)
              for (int i = 0; i < list->count(); ++i)
                if (list->item(i)->data(Qt::UserRole).toString() == id)
                  list->item(i)->setIcon(m_collectionIcons[id]);
            for (int i = 0; i < m_shelf->count(); ++i)
              if (m_shelf->item(i)->data(Qt::UserRole).toString() == id)
                m_shelf->item(i)->setIcon(m_collectionIcons[id]);
            QTimer::singleShot(0, this,
                               &ReaderFeatures::requestCollectionCover);
          });
  const auto policy = w->namePolicyFor(chosen.path);
  const bool spine = target == m_shelf;
  watcher->setFuture(QtConcurrent::run([chosen, policy, spine, cache=w->m_thumbnailDiskCache,stop=w->m_backgroundCancel] {
    return cachedCollectionCover(chosen.path,policy,chosen.metadata,spine,cache,[stop]{return stop->load();});
  }));
}
QString ReaderFeatures::currentPositionKey() const {
  return w->pageCount() ? positionKey(w->m_book->pages[w->m_anchor])
                        : QString{};
}
int ReaderFeatures::resolvePosition(const QString &key, int fallback) const {
  if (isTextDocumentPath(w->bookPath()) && key.startsWith("text:")) {
    const auto start=key.lastIndexOf(':');
    const auto end=key.indexOf('[',start);
    bool ok=false;const int offset=key.mid(start+1,end<0?-1:end-start-1).toInt(&ok);
    if(ok)return w->resolveDocumentLocation({key.mid(5,start-5),offset,{}});
  }
  if (!key.isEmpty())
    for (int i = 0; i < w->pageCount(); ++i)
      if (positionKey(w->m_book->pages[i]) == key)
        return i;
  return std::clamp(fallback, 0, std::max(0, w->pageCount() - 1));
}
void ReaderFeatures::openFailed() {
  m_pendingId.clear();
  m_intent = 1;
}
QString ReaderFeatures::collectionNeighbor(int direction) {
  const auto active = m_library->item(m_activeId);
  if (active.kind != "playlist")
    return {};
  const auto entries = m_library->items("playlist");
  for (int i = 0; i < entries.size(); ++i)
    if (entries[i].id == active.id) {
      const int n = i + direction;
      if (n >= 0 && n < entries.size()) {
        return entries[n].path;
      }
      break;
    }
  return {};
}

void ReaderFeatures::prepareOpen(const QString &path) {
  if (!m_pendingId.isEmpty())
    return;
  const auto active = m_library->item(m_activeId);
  if (active.kind == "playlist")
    for (const auto &item : m_library->items("playlist"))
      if (item.path == QFileInfo(path).absoluteFilePath()) {
        m_pendingId = item.id;
        m_intent = 1;
        break;
      }
  if (m_pendingId.isEmpty() && parseArchiveEntryPath(path).valid())
    for (const auto &item : m_library->items("history"))
      if (bookIdentityPath(item.path) == bookIdentityPath(path) && item.state.contains("anchor")) {
        m_pendingId = item.id; m_intent = 0; break;
      }
}

void ReaderFeatures::namesChanged(const NamePolicy &policy) {
  if (m_sourceBook)
    m_sourceBook = m_sourceBook->reinterpretNames(policy);
  for (int i = 0; i < m_tocEntries.size(); ++i) {
    auto entry = m_tocEntries[i].toObject();
    entry["page"] = resolvePosition(entry.value("key").toString(),
                                    entry.value("page").toInt());
    m_tocEntries[i] = entry;
    if (i < m_toc->count())
      m_toc->item(i)->setData(Qt::UserRole, entry["page"].toInt());
  }
  saveState();
}

bool ReaderFeatures::usesPlaylist() const {
  return m_library->item(m_activeId).kind == "playlist" &&
         m_library->items("playlist").size() > 1;
}

QVector<int> ReaderFeatures::visiblePagesForView() const {
  if (w->documentViewActive())
    return documentVisiblePages(w->pageCount(), w->m_anchor,
                                  w->m_book->openOptions.text.pageLayout, w->m_documentWidePages,w->m_documentSides,w->m_documentSoloPages,w->m_book->publication->rightToLeft);
  if (w->renderReady() && !m_visibleIndices.isEmpty() &&
      m_visibleIndices.first() == w->m_anchor)
    return m_visibleIndices;
  return visiblePages(w->pageCount(), w->m_anchor, w->m_options);
}

void ReaderFeatures::refreshDocumentToc() {
  if (!w->m_book || !isTextDocumentPath(w->bookPath())) return;
  m_toc->clear();
  for(const auto &value:m_tocEntries) {
    const auto e=value.toObject();auto *row=new QListWidgetItem(e.value("label").toString(),m_toc);
    row->setData(Qt::UserRole,e.contains("location")?w->resolveDocumentLocation(DocumentLocation::fromJson(e.value("location").toObject())):resolvePosition(e.value("key").toString(),e.value("page").toInt()));
  }
  auto publication=w->m_book->publication;
  if (!publication) publication=readPublication(w->bookPath(),w->m_book->openOptions.text);
  if (!publication || !publication->error.isEmpty()) return;
  for (const auto &entry:publication->toc) {
    auto *row=new QListWidgetItem(entry.title,m_toc);
    row->setData(Qt::UserRole,w->resolveDocumentLocation({entry.section,entry.offset,{}}));
    row->setData(Qt::UserRole+1,QJsonObject{{"section",entry.section},{"offset",entry.offset},{"fragment",entry.fragment}}.toVariantMap());
  }
}
