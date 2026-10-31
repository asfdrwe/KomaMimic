// SPDX-License-Identifier: GPL-3.0-only
#include "mainwindow.h"
#include "archivepath.h"
#include <QCoreApplication>
#include "foldertree.h"
#include "pageview.h"
#include "shortcuts.h"
#include "readerfeatures.h"
#include "documentsettings.h"
#include <QMessageBox>
#include "documentview.h"
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTimer>
#include <QtConcurrent>
#include <algorithm>

#include "thumbnailsource.h"

ThumbnailGrid *MainWindow::activeGrid() const { return m_mode == ViewMode::Pages ? m_pages : m_library; }
void MainWindow::restoreThumbnailSettings() {
    const bool follow = m_settings->value("thumbnails/mouseSelection", true).toBool();
    CustomThumbnailSettings custom;
    custom.columns=m_settings->value("thumbnails/columns",0).toInt();
    custom.width=m_settings->value("thumbnails/width",64).toInt();
    custom.height=m_settings->value("thumbnails/height",64).toInt();
    custom.horizontalGap=m_settings->value("thumbnails/horizontalGap",0).toInt();
    custom.verticalGap=m_settings->value("thumbnails/verticalGap",0).toInt();
    custom.filenameLines=m_settings->value("thumbnails/filenameLines",1).toInt();
    custom.rightOrigin=m_settings->value("thumbnails/rightOrigin",false).toBool();
    custom.frames=m_settings->value("thumbnails/frames",false).toBool();
    for (auto *grid : {m_library, m_pages}) {grid->setMouseSelectionEnabled(follow);grid->setCustomSettings(custom);}
    setThumbnailLayout(ThumbnailGrid::Layout(std::clamp(m_settings->value("thumbnails/layout",0).toInt(),0,2)),custom.columns);
    m_view->setFilenameVisible(m_settings->value("display/filename",false).toBool());
    if (m_treePanel->twoPane()!=m_settings->value("folders/twoPane",true).toBool())
        m_treePanel->setTwoPane(m_settings->value("folders/twoPane",true).toBool());
    for(int row=0;row<m_pages->count();++row) {
        auto *item=m_pages->item(row);const auto name=item->data(ThumbnailPageNameRole).toString();
        if(!name.isEmpty())item->setText((m_settings->value("thumbnails/numbers",false).toBool()?QString::number(row+1)+"  ":QString{})+name);
    }
}
void MainWindow::focusCurrentView() {
    if (m_mode == ViewMode::Reader && documentViewActive() && m_documentView) { m_documentView->setFocus(); return; }
    if (m_mode == ViewMode::Reader) m_view->setFocus(); else activeGrid()->setFocus();
}
void MainWindow::setMode(ViewMode mode) {
    m_mode = mode; m_stack->setCurrentIndex(mode == ViewMode::Covers ? 0 : mode == ViewMode::Pages ? 1 : 2);
    if (mode == ViewMode::Reader && documentViewActive() && m_documentView) m_stack->setCurrentWidget(m_documentView);
    if (mode != ViewMode::Reader) activeGrid()->requestVisible();
    focusCurrentView(); refreshState();
}
void MainWindow::showCovers() {
    const auto entry = parseArchiveEntryPath(m_currentPath);
    if (entry.valid()) {
        m_archiveSelections[entry.archive] = m_currentPath;
        if (m_archiveDirectory && m_archiveDirectory->path == entry.archive) populateArchiveDirectory(m_archiveDirectory);
        else loadBook(entry.archive, ViewMode::Covers);
    } else if (m_archiveDirectory) populateArchiveDirectory(m_archiveDirectory);
    else setMode(ViewMode::Covers);
}
void MainWindow::showPageThumbnails() {
    if (m_mode == ViewMode::Pages) { setMode(m_beforeThumbnails); return; }
    m_beforeThumbnails = m_mode;
    if (m_mode == ViewMode::Reader && m_book) {
        m_pages->setCurrentRow(m_anchor); m_pages->scrollToItem(m_pages->currentItem()); setMode(ViewMode::Pages); return;
    }
    const auto *item = m_library->currentItem();
    if (m_archiveDirectory) { if (item) openBook(item->data(PathRole).toString()); return; }
    if (item) loadBook(item->data(PathRole).toString(), ViewMode::Pages);
    else if (!m_directory.isEmpty()) loadBook(m_directory, ViewMode::Pages);
}
void MainWindow::readFolder() { if (!m_directory.isEmpty()) loadBook(m_directory, ViewMode::Reader); }
void MainWindow::acceptThumbnail() {
    auto *grid = activeGrid(); auto *item = grid->currentItem(); if (!item) return;
    if (m_mode == ViewMode::Pages) {
        const int page = item->data(PageRole).toInt(); setMode(ViewMode::Reader); jump(page);
    } else activateLibraryItem(item);
}
void MainWindow::cancelThumbnail() {
    if (m_mode == ViewMode::Pages) setMode(m_beforeThumbnails);
    else if (isFullScreen()) setFullscreen(false);
    else if (m_book) setMode(ViewMode::Reader);
}
void MainWindow::buildNavigationActions() {
    m_actions["tree"]->setCheckable(true); m_actions["thumbnails"]->setCheckable(true);
    connect(m_actions["tree"], &QAction::toggled, this, [this](bool visible) {
        m_treePanel->setVisible(visible); if (!visible && (m_treePanel->hasFocus() || m_treePanel->isAncestorOf(QApplication::focusWidget()))) focusCurrentView(); saveSettings();
    });
    connect(m_actions["focusTree"], &QAction::triggered, this, [this] { m_actions["tree"]->setChecked(true); m_tree->selectPath(m_directory); m_treePanel->focusTree(); });
    connect(m_actions["focusThumbnails"], &QAction::triggered, this, [this] { if (m_mode == ViewMode::Reader) showCovers(); else activeGrid()->setFocus(); });
    connect(m_actions["parentFolder"], &QAction::triggered, this, &MainWindow::parentFolder);
    connect(m_actions["folderBack"], &QAction::triggered, this, [this] { historyFolder(-1); });
    connect(m_actions["folderForward"], &QAction::triggered, this, [this] { historyFolder(1); });
    connect(m_actions["previousFolder"], &QAction::triggered, this, [this] { moveFolder(-1); });
    connect(m_actions["nextFolder"], &QAction::triggered, this, [this] { moveFolder(1); });
    connect(m_actions["readFolder"], &QAction::triggered, this, &MainWindow::readFolder);
    connect(m_actions["thumbnails"], &QAction::triggered, this, &MainWindow::showPageThumbnails);
    connect(m_actions["fullscreenGlobal"], &QAction::triggered, this, &MainWindow::toggleFullscreen);
    connect(m_actions["thumbAccept"], &QAction::triggered, this, &MainWindow::acceptThumbnail);
    connect(m_actions["thumbCancel"], &QAction::triggered, this, &MainWindow::cancelThumbnail);
    for (const auto &[id, move] : QVector<QPair<QString, QPoint>>{{"thumbLeft", {-1,0}}, {"thumbRight", {1,0}}, {"thumbUp", {0,-1}}, {"thumbDown", {0,1}}})
        connect(m_actions[id], &QAction::triggered, this, [this, move] { activeGrid()->moveSelection(move.x(), move.y()); });
    connect(m_actions["thumbFirst"], &QAction::triggered, this, [this] { activeGrid()->selectFirst(); });
    connect(m_actions["thumbLast"], &QAction::triggered, this, [this] { activeGrid()->selectLast(); });
    for (const auto &[id, sign] : QVector<QPair<QString, int>>{{"thumbPageUp", -1}, {"thumbPageDown", 1}})
        connect(m_actions[id], &QAction::triggered, this, [this, sign] { auto *grid = activeGrid(); grid->moveSelection(0, sign * std::max(1, grid->viewport()->height() / grid->gridSize().height())); });
    connect(m_actions["thumbRefresh"], &QAction::triggered, this, &MainWindow::refreshThumbnails);
    for (const auto &[id, key] : QVector<QPair<QString, int>>{{"treeUp", Qt::Key_Up}, {"treeDown", Qt::Key_Down}, {"treeLeft", Qt::Key_Left}, {"treeRight", Qt::Key_Right}})
        connect(m_actions[id], &QAction::triggered, this, [this, key] { m_treePanel->navigate(key); });
    connect(m_actions["treeAccept"], &QAction::triggered, this, [this] { m_treePanel->activateSelection(); });
    connect(m_actions["treeCancel"], &QAction::triggered, this, [this] { if (isFullScreen()) setFullscreen(false); else showCovers(); });
    auto *layouts = new QActionGroup(this); layouts->setExclusive(true);
    for (const auto &id : {"thumbFit", "thumbEight", "thumbCustom"}) { m_actions[id]->setCheckable(true); layouts->addAction(m_actions[id]); }
    connect(m_actions["thumbFit"], &QAction::triggered, this, [this] { setThumbnailLayout(ThumbnailGrid::Layout::Fit, m_library->customColumns()); });
    connect(m_actions["thumbEight"], &QAction::triggered, this, [this] { setThumbnailLayout(ThumbnailGrid::Layout::EightColumns, m_library->customColumns()); });
    connect(m_actions["thumbCustom"], &QAction::triggered, this, [this] {
        setThumbnailLayout(ThumbnailGrid::Layout::Custom,m_library->customColumns());
    });
}
void MainWindow::attachScopedShortcuts() {
    for (const auto &spec : shortcutSpecs()) {
        if (spec.scope == ShortcutScope::Global) continue;
        auto *action = m_actions[spec.id]; removeAction(action); action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        if (spec.scope == ShortcutScope::Reader) m_view->addAction(action);
        else if (spec.scope == ShortcutScope::Tree) m_treePanel->addAction(action);
        else { m_library->addAction(action); m_pages->addAction(action); }
    }
}
void MainWindow::setThumbnailLayout(ThumbnailGrid::Layout layout, int columns) {
    m_library->setLayoutMode(layout, columns); m_pages->setLayoutMode(layout, columns);
    m_actions[layout == ThumbnailGrid::Layout::Fit ? "thumbFit" : layout == ThumbnailGrid::Layout::EightColumns ? "thumbEight" : "thumbCustom"]->setChecked(true);
    if (!m_restoring) {
        m_settings->setValue("thumbnails/layout", int(layout)); m_settings->setValue("thumbnails/columns", m_library->customColumns()); m_settings->sync();
    }
}
void MainWindow::initializeDirectoryWatcher() {
    m_directoryWatcher = new QFileSystemWatcher(this);
    m_directoryWatcher->setObjectName("browserDirectoryWatcher");
    m_directoryRefreshTimer = new QTimer(this);
    m_directoryRefreshTimer->setSingleShot(true);
    m_directoryRefreshTimer->setInterval(250);
    connect(m_directoryWatcher, &QFileSystemWatcher::directoryChanged,
        this, [this] { m_directoryRefreshTimer->start(); });
    connect(m_directoryRefreshTimer, &QTimer::timeout, this, [this] {
        // Do not replace entries underneath an open context menu or dialog.
        if (QApplication::activePopupWidget() || QApplication::activeModalWidget()) {
            m_directoryRefreshTimer->start(); return;
        }
        refreshDirectoryListing();
    });
}
void MainWindow::refreshDirectoryListing() {
    if (m_directory.isEmpty() || m_archiveDirectory) return;
    const int row = m_library->currentRow(), scroll = m_library->verticalScrollBar()->value();
    const auto *item = m_library->currentItem();
    const QString selected = item ? item->data(PathRole).toString() : QString{};
    const QSignalBlocker blocker(m_library);
    // Directory notifications must not reload the book, move focus or write
    // settings: the settings file can itself be inside the watched directory.
    populateDirectory(m_directory, false, false, false);
    if (!selected.isEmpty() && m_library->currentItem()
        && m_library->currentItem()->data(PathRole).toString() != selected)
        m_library->setCurrentRow(std::clamp(row, 0, m_library->count() - 1));
    m_library->verticalScrollBar()->setValue(scroll);
}
void MainWindow::openDirectory(const QString &path) { populateDirectory(path, true, true); }
void MainWindow::populateDirectory(const QString &path, bool navigate, bool history, bool persist) {
    if (!navigate && m_archiveDirectory) { populateArchiveDirectory(m_archiveDirectory, false); return; }
    const QFileInfo info(path);
    if (!info.isDir() || !info.isReadable()) { m_status->setText(QCoreApplication::translate("KomaMimic", "フォルダを開けません: ") + path); return; }
    if (const auto *current = m_library->currentItem()) {
        if (m_archiveDirectory) m_archiveSelections[m_archiveDirectory->path] = current->data(PathRole).toString();
        else m_directorySelections[m_directory] = current->data(PathRole).toString();
    }
    m_archiveDirectory.reset();
    m_libraryTitle->setText(QCoreApplication::translate("KomaMimic", "表紙サムネイル — フォルダ・書庫・画像を選択して開く"));
    const auto absolute = info.absoluteFilePath();
    if (history && (m_historyIndex < 0 || m_history[m_historyIndex] != absolute)) {
        while (m_history.size() > m_historyIndex + 1) m_history.removeLast();
        m_history.append(absolute); m_historyIndex = m_history.size() - 1;
    }
    if (navigate && m_loading) {
        m_openToken->store(++m_bookGeneration); m_loading = false;
        m_renderToken->store(++m_renderGeneration);
    }
    m_directory = absolute;
    // Background refresh preserves the tree's keyboard selection.
    if (navigate || persist) m_tree->selectPath(m_directory);
    const auto watched = m_directoryWatcher->directories();
    if (watched != QStringList{m_directory}) {
        m_directoryRefreshTimer->stop();
        if (!watched.isEmpty()) m_directoryWatcher->removePaths(watched);
        m_directoryWatcher->addPath(m_directory);
    }
    const auto generation = ++m_libraryGeneration; m_thumbnailToken->store(generation); m_library->clearItems();
    auto entries = QDir(m_directory).entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Readable, QDir::NoSort);
    std::stable_sort(entries.begin(), entries.end(), [sort=m_settings->value("covers/sort",0).toInt(), reverse=m_settings->value("covers/reverse",false).toBool()](const QFileInfo &a, const QFileInfo &b) {
        return browserLess(a,b,sort,reverse);
    });
    const QString selected = m_directorySelections.value(m_directory, m_currentPath);
    for (const auto &entry : entries) {
        if (!entry.isDir() && !isArchivePath(entry.fileName()) && !isImagePath(entry.fileName()) && !isDocumentPath(entry.fileName())) continue;
        auto *item = new QListWidgetItem(m_library->placeholderIcon(entry.isDir()), entry.fileName());
        item->setData(PathRole, entry.absoluteFilePath()); item->setData(DirectoryRole, entry.isDir()); item->setData(ThumbnailRoleReady, false); item->setToolTip(entry.absoluteFilePath());
        m_library->addItem(item); if (entry.absoluteFilePath() == selected) m_library->setCurrentItem(item);
    }
    if (m_library->currentRow() < 0 && m_library->count()) m_library->setCurrentRow(0);
    if (m_library->currentItem()) m_library->scrollToItem(m_library->currentItem());
    m_siblingFolders.clear();
    QDir parent(m_directory);
    if (parent.cdUp()) {
        auto siblings = parent.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable, QDir::NoSort);
        std::sort(siblings.begin(), siblings.end(), [](const QFileInfo &a, const QFileInfo &b) { return naturalLess(a.fileName(), b.fileName()); });
        for (const auto &sibling : siblings) m_siblingFolders.append(sibling.absoluteFilePath());
    }
    m_siblingIndex = m_siblingFolders.indexOf(m_directory);
    if (navigate) { m_lastError.clear(); setMode(ViewMode::Covers); }
    refreshState(); m_library->requestVisible();
    if (persist) saveSettings();
}
void MainWindow::populateArchiveDirectory(const std::shared_ptr<const Book> &book, bool navigate) {
    if (!book || !book->archiveDirectory) return;
    QString selected = m_archiveSelections.value(book->path);
    if (m_mode == ViewMode::Covers && m_archiveDirectory && m_archiveDirectory->path == book->path && m_library->currentItem())
        selected = m_library->currentItem()->data(PathRole).toString();
    else if (parseArchiveEntryPath(m_currentPath).archive == book->path) selected = m_currentPath;
    m_archiveDirectory = book;
    m_directorySelections[m_directory] = book->path;
    m_libraryTitle->setText(QCoreApplication::translate("KomaMimic", "書庫内ファイル — %1（%2項目）").arg(QFileInfo(book->path).fileName()).arg(book->pages.size()));
    m_thumbnailToken->store(++m_libraryGeneration); m_library->clearItems();
    QVector<int> order; for (int i = 0; i < book->pages.size(); ++i) order.append(i);
    const int sort = m_settings->value("covers/sort", 0).toInt();
    const bool reverse = m_settings->value("covers/reverse", false).toBool();
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const auto &x = book->pages[a], &y = book->pages[b];
        int result = sort == 2 ? QFileInfo(x.name).suffix().compare(QFileInfo(y.name).suffix(), Qt::CaseInsensitive)
            : sort == 3 && x.encodedSize != y.encodedSize ? (x.encodedSize < y.encodedSize ? -1 : 1) : 0;
        if (!result) result = naturalLess(x.name, y.name) ? -1 : naturalLess(y.name, x.name) ? 1 : 0;
        return reverse ? result > 0 : result < 0;
    });
    for (int index : order) {
        const auto &entry = book->pages[index]; const auto path = book->entryPath(index);
        auto *item = new QListWidgetItem(m_library->placeholderIcon(), entry.name);
        item->setData(PathRole, path); item->setData(DirectoryRole, false); item->setData(ThumbnailRoleReady, false);
        item->setToolTip(book->path + " / " + entry.name); m_library->addItem(item);
        if (path == selected || (!parseArchiveEntryPath(selected).id.isEmpty() && parseArchiveEntryPath(selected).id == entry.archiveEntryId)) m_library->setCurrentItem(item);
    }
    if (m_library->currentRow() < 0 && m_library->count()) m_library->setCurrentRow(0);
    if (m_library->currentItem()) { m_archiveSelections[book->path] = m_library->currentItem()->data(PathRole).toString(); m_library->scrollToItem(m_library->currentItem()); }
    if (navigate) setMode(ViewMode::Covers);
    else refreshState();
    m_library->requestVisible();
}
void MainWindow::parentFolder() {
    if (m_mode != ViewMode::Covers && parseArchiveEntryPath(m_currentPath).valid()) { showCovers(); return; }
    if (m_mode == ViewMode::Covers && m_archiveDirectory) {
        const auto archive = m_archiveDirectory->path; const auto parent = QFileInfo(archive).absolutePath();
        m_directorySelections[parent] = archive; openDirectory(parent); return;
    }
    if (m_mode != ViewMode::Covers) {
        const QFileInfo source(m_currentPath);
        const QString directory = m_currentPath.isEmpty() ? m_directory
            : source.isDir() ? source.absoluteFilePath() : source.absolutePath();
        if (directory != m_directory) openDirectory(directory);
        else showCovers();
        highlightBook();
        if (auto *item = m_library->currentItem()) m_library->scrollToItem(item);
        return;
    }
    QDir parent(m_directory);
    if (parent.cdUp()) { m_directorySelections[parent.absolutePath()] = m_directory; openDirectory(parent.absolutePath()); }
}
void MainWindow::moveFolder(int delta) {
    const int target = m_siblingIndex + delta;
    if (m_siblingIndex >= 0 && target >= 0 && target < m_siblingFolders.size()) openDirectory(m_siblingFolders[target]);
}
void MainWindow::historyFolder(int delta) {
    const int target = m_historyIndex + delta;
    if (target < 0 || target >= m_history.size() || !QFileInfo(m_history[target]).isDir()) return;
    m_historyIndex = target; populateDirectory(m_history[target], true, false);
}
void MainWindow::loadBook(const QString &path, ViewMode mode, bool startAtLastPage) {
    saveDocumentLocation();
    clearDocumentView();
    if(QFileInfo(path).absoluteFilePath()!=m_currentPath)m_restoreDocumentLocation=false;
    if (m_features){m_features->prepareOpen(path);m_features->closingBook();}
    const QFileInfo info(path);
    const auto physical = QFileInfo(physicalBookPath(path));
    const auto directory = physical.isDir() ? physical.absoluteFilePath() : physical.absolutePath();
    const auto address = parseArchiveEntryPath(path);
    const bool keepArchive = m_archiveDirectory && (m_archiveDirectory->path == address.archive || m_archiveDirectory->path == info.absoluteFilePath());
    const bool leavingArchive = m_archiveDirectory && !keepArchive;
    if (leavingArchive) {
        if (m_library->currentItem()) m_archiveSelections[m_archiveDirectory->path] = m_library->currentItem()->data(PathRole).toString();
        m_archiveDirectory.reset();
    }
    if (directory != m_directory) populateDirectory(directory, true, true);
    else if (leavingArchive) populateDirectory(directory, false, false);
    m_currentPath = info.absoluteFilePath(); m_book.reset(); m_imageCache.clear(); m_anchor = 0;
    m_loading = true; m_renderReady = false; m_lastError.clear(); m_renderToken->store(++m_renderGeneration);
    const auto generation = ++m_bookGeneration; m_openToken->store(generation);
    m_pageThumbnailToken->store(++m_pageGeneration); m_pages->clearItems();
    m_pageTitle->setText(QCoreApplication::translate("KomaMimic", "画像・書庫を読み込んでいます… ") + bookDisplayName(m_currentPath));
    m_view->setMessage(QCoreApplication::translate("KomaMimic", "画像・書庫を読み込んでいます…\n") + bookDisplayName(m_currentPath)); setMode(mode); highlightBook(); refreshState();
    auto *watcher = new QFutureWatcher<Book::OpenResult>(this);
    connect(watcher, &QFutureWatcher<Book::OpenResult>::finished, this, [this, watcher, generation, startAtLastPage] {
        const auto result = watcher->result(); watcher->deleteLater();
        if (generation != m_bookGeneration) return;
        m_loading = false;
        if (!result.book) { m_restoreDocumentLocation=false; m_lastError = result.error; if(m_features)m_features->openFailed(); m_pageTitle->setText(result.error); m_view->setMessage(result.error); refreshState(); return; }
        if (result.book->archiveDirectory) {
            m_book.reset(); m_restoreDocumentLocation = false; m_renderReady = true;
            if (m_features) m_features->openFailed();
            populateArchiveDirectory(result.book); return;
        }
        m_book = result.book; m_anchor = startAtLastPage ? pageCount() - 1 : m_book->initialPage;
        if (m_book->archiveParent) populateArchiveDirectory(m_book->archiveParent, false);
        if (m_features) m_features->openedBook();
        if (m_restoreDocumentLocation && !documentViewActive()) {
            m_anchor=resolveDocumentLocation(m_pendingDocumentLocation);m_restoreDocumentLocation=false;
        }
        if (documentViewActive() && m_mode == ViewMode::Pages) setMode(ViewMode::Reader);
        populatePages();
        if (documentViewActive()) openDocumentView(); else render();
    });
    const auto policy = namePolicyFor(path);
    const auto options = m_features ? m_features->openOptions() : OpenOptions{};
    watcher->setFuture(QtConcurrent::run([path = m_currentPath, policy, options, token = m_openToken, generation] {
        if (token->load() != generation) return Book::OpenResult{};
        auto request=options;request.cancel=[token,generation]{return token->load()!=generation;};return Book::open(path, policy, request);
    }));
}
void MainWindow::openBook(const QString &path) { if (QFileInfo(path).isDir()) openDirectory(path); else loadBook(path, ViewMode::Reader); }
void MainWindow::populatePages() {
    m_pageThumbnailToken->store(++m_pageGeneration); m_pages->clearItems();
    if (!m_book) return;
    m_pageTitle->setText(QCoreApplication::translate("KomaMimic", "ページサムネイル — %1（%2ページ）").arg(bookDisplayName(m_currentPath)).arg(pageCount()));
    for (int index = 0; index < m_book->pages.size(); ++index) {
        const auto &page = m_book->pages[index];
        const QString name = page.name + (page.documentPage ? QString(" [%1]").arg(page.documentPage+1) : QString{});
        const QString title = (m_settings->value("thumbnails/numbers",false).toBool()?QString::number(index+1)+"  ":QString{})+name;
        auto *item = new QListWidgetItem(m_pages->placeholderIcon(), title);
        item->setData(PageRole, index); item->setData(ThumbnailPageNameRole,name); item->setData(ThumbnailRoleReady, false);
        item->setToolTip(title + '\n' + page.decodedName.encoding + " / " + page.decodedName.origin + '\n' + page.decodedName.warning);
        m_pages->addItem(item);
    }
    m_pages->setCurrentRow(m_anchor); m_pages->scrollToItem(m_pages->currentItem()); m_pages->requestVisible();
}
void MainWindow::startNextThumbnail(quint64 generation) { if (generation == m_libraryGeneration) requestThumbnails(m_library); }
void MainWindow::requestThumbnails(ThumbnailGrid *grid) {
    if (m_backgroundCancel->load() || !grid->isVisible()) return;
    const bool pages = grid == m_pages;
    if (pages && !m_book) return;
    for (int row : grid->visibleRows()) {
        if (m_thumbnailActive >= 2) break;
        auto *item = grid->item(row);
        if (item->data(ThumbnailRoleReady).toBool() || item->data(PendingRole).toBool()) continue;
        item->setData(PendingRole, true); ++m_thumbnailActive;
        const quint64 generation = pages ? m_pageGeneration : m_libraryGeneration;
        const auto token = pages ? m_pageThumbnailToken : m_thumbnailToken;
        const QString path = item->data(PathRole).toString(); const int page = item->data(PageRole).toInt();
        const auto book = m_book; const auto policy = namePolicyFor(path);
        const bool refresh=item->data(ThumbnailRefreshRole).toBool();
        auto *watcher = new QFutureWatcher<ImageResult>(this);
        connect(watcher, &QFutureWatcher<ImageResult>::finished, this, [this, watcher, grid, pages, generation, row] {
            const auto result = watcher->result(); watcher->deleteLater(); --m_thumbnailActive;
            if (generation == (pages ? m_pageGeneration : m_libraryGeneration)) grid->setThumbnail(row, result.image, result.error);
            requestThumbnails(m_library); requestThumbnails(m_pages);
        });
        watcher->setFuture(QtConcurrent::run([path, page, pages, book, policy, token, generation, cache=m_thumbnailDiskCache, refresh] {
            const auto cancel = [token, generation] { return token->load() != generation; };
            if (cancel()) return ImageResult{};
            if (pages) return cachedPageThumbnail(book,page,cache,cancel,refresh);
            return cachedCover(path,policy,cache,cancel,refresh);
        }));
    }
}
void MainWindow::refreshThumbnails() {
    if (m_mode == ViewMode::Covers && m_archiveDirectory) { loadBook(m_archiveDirectory->path, ViewMode::Covers); return; }
    if (m_mode == ViewMode::Covers) refreshDirectoryListing();
    auto *grid = activeGrid();
    if (grid == m_library) m_thumbnailToken->store(++m_libraryGeneration); else m_pageThumbnailToken->store(++m_pageGeneration);
    grid->invalidateThumbnails();
}
