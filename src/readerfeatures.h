// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "book.h"
#include "bookexport.h"
#include "librarystore.h"
#include "shortcuts.h"
#include <QElapsedTimer>
#include <QIcon>
#include <QJsonArray>
#include <QObject>
#include <QPointer>
#include <memory>
class MainWindow;
class IdleInhibitor;
class QLabel;
class QDockWidget;
class QListWidget;
class QTabWidget;
class QTimer;
class QUndoStack;
class QPrinter;
class QFileSystemWatcher;
class ReaderInput;
QVector<ShortcutSpec> featureShortcutSpecs();
QString legacyCommandId(int command);
class ReaderFeatures : public QObject {
public:
  explicit ReaderFeatures(MainWindow *window, IdleInhibitor *inhibitor = nullptr);
  ~ReaderFeatures() override;
  OpenOptions openOptions() const;
  void closingBook();
  void openedBook();
  void refreshDocumentToc();
  void openFailed();
  void prepareOpen(const QString &path);
  bool usesPlaylist() const;
  QString collectionNeighbor(int direction);
  QString currentPositionKey() const;
  int resolvePosition(const QString &key, int fallback) const;
  void requestCollectionCover();
  void showCollection(const QString &kind, int layout);
  void beforeRender();
  void rendered(const QVector<int> &indices,
                const QVector<ImageResult> &originals);
  bool navigate(int direction, bool onePage);
  void refresh();
  void saveState();
  QVector<int> selectedPages() const;
  QVector<int> visiblePagesForView() const;
  void replacePages(const QVector<BookPage> &pages, int anchor);
  void openCollection(const QString &id, int intent = 0);
  void executeLegacy(int command);
  void namesChanged(const NamePolicy &policy);
  void updateInhibit();
  IdleInhibitor *idleInhibitor() const;
  void runRequest(const QJsonObject &request);
  void loadList(bool save, const QString &provided = {});
  void settingsFile(bool import, const QString &path = {}, bool mangaMeeyaOnly = false);
  QString collectionPath() const;
  int advance(int direction, bool onePage) const;
  bool autoSpread() const;
  void preferences(const QString &page = {});
  void fullscreenChanging(bool fullscreen);
  void fullscreenMouse(const QPoint &position);

protected:
  bool eventFilter(QObject *object, QEvent *event) override;

private:
  void initializeInhibit(IdleInhibitor *inhibitor);
  void updateInhibitStatus();
  void build();
  void bind(const QString &id, const std::function<void()> &function);
  void message(const QString &text);
  void rebuildLists();
  void addCollection(const QString &kind);
  void editMetadata(const QString &id);
  void editPages(const QString &operation);
  void applySort(int mode);
  void splitPages(int axis);
  void changeBook(std::shared_ptr<const Book> book, int anchor);
  void output(ExportKind kind, bool all = false, const QString &path = {},
              bool spread = false);
  void clipboard(bool paste);
  void importMangaMeeyaSettings(const QString &text);
  void profile(int slot, bool save);
  void settingsSlot(int slot, bool save);
  void cacheInformation();
  void slideshow(int direction);
  void timedList(const QString &path = {});
  void print(bool setup);
  void openUrl();
  void external(int slot, bool configure);
  void search();
  void listDetails();
  void registerBookImage(bool spine);
  void trashPages();
  void zipMetadata(bool save);
  void addToc();
  void mark(bool jump);
  void navigateToc(int direction);
  void exportThumbnail();
  void toolbarSettings();
  void restoreEditState(const QJsonObject &state);
  MainWindow *w;
  ReaderInput *m_input = nullptr;
  std::unique_ptr<LibraryStore> m_library;
  QDockWidget *m_listsDock = nullptr, *m_tocDock = nullptr,
              *m_shelfDock = nullptr;
  QTabWidget *m_tabs = nullptr;
  QHash<QString, QListWidget *> m_lists;
  QListWidget *m_toc = nullptr, *m_shelf = nullptr;
  QUndoStack *m_undo = nullptr;
  QTimer *m_slideshow = nullptr, *m_saveTimer = nullptr,
         *m_watchReloadTimer = nullptr;
  QFileSystemWatcher *m_watcher = nullptr;
  std::unique_ptr<QPrinter> m_printer;
  QString m_activeId, m_pendingId;
  int m_intent = 1, m_selectionFirst = -1, m_selectionLast = -1,
      m_slideDirection = 0;
  struct TimedPage {
    qint64 time;
    int page;
    QPointF pan;
    bool hasPan;
    QString text;
  };
  QVector<TimedPage> m_timed;
  QElapsedTimer m_slideClock;
  qint64 m_timedOffset = 0;
  int m_timedIndex = 0;
  QVector<BookPage> m_pageClipboard;
  std::shared_ptr<const Book> m_sourceBook;
  QJsonArray m_tocEntries;
  bool m_restoring = false, m_prefetching = false;
  bool m_navigationBusy = false;
  int m_lastDirection = 1;
  int m_previousAnchor = 0;
  QVector<int> m_visibleIndices;
  QHash<int, QSize> m_dimensions;
  QHash<QString, QPointF> m_panPositions;
  QString m_lastPositionKey;
  QPointF m_previousPan, m_restoredPan;
  bool m_restorePan = false, m_restoreSourceOrigin = false;
  QPointer<IdleInhibitor> m_inhibitor;
  QLabel *m_inhibitIndicator = nullptr;
  QHash<QDockWidget *, bool> m_dockVisibility;
  QByteArray m_normalDocks;
  QHash<QString, QIcon> m_collectionIcons;
  QStringList m_coverQueue, m_collectionIconOrder;
  bool m_coverBusy = false;
};
