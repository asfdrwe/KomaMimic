// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QSet>
#include "reading.h"
#include "book.h"
#include "imageprocessing.h"
#include "thumbnailgrid.h"
#include <QCache>
#include <QHash>
#include <QKeySequence>
#include <QMainWindow>
#include <QSettings>
#include <atomic>
#include <memory>
class QJsonObject;
class QAction;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QSlider;
class QSplitter;
class QTimer;
class QToolBar;
class QStackedWidget;
class QMenu;
class FolderTree;
class PageView;
class DocumentView;
class ReaderFeatures;
class IdleInhibitor;
class ThumbnailDiskCache;
class ReaderToolBar;
class QFileSystemWatcher;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    enum class ViewMode { Covers, Pages, Reader };
    explicit MainWindow(const QString &settingsPath, QWidget *parent = nullptr, IdleInhibitor *inhibitor = nullptr);
    ~MainWindow() override;
    void runRequest(const QJsonObject &request);
    void openDirectory(const QString &path);
    void openBook(const QString &path);
    void nextPage();
    void previousPage();
    void moveBook(int delta);
    void toggleFullscreen();
    void setFullscreen(bool enabled);
    void editShortcuts();
    void editArchiveNames();
    void setNamePolicy(const NamePolicy &policy);
    ViewMode viewMode() const { return m_mode; }
    void showCovers();
    void showPageThumbnails();
    void readFolder();
    void parentFolder();
    void moveFolder(int delta);
    void historyFolder(int delta);
    void setImageOptions(const ImageOptions &options, bool save = true);
    ImageOptions imageOptions() const { return m_imageOptions; }
    void setResizeAlgorithm(ResizeAlgorithm algorithm);
    void editImageOptions();
    QString currentPageName() const { return m_book && m_anchor >= 0 && m_anchor < m_book->pages.size() ? m_book->pages[m_anchor].name : QString{}; }
    bool applyBindings(const QHash<QString, QKeySequence> &bindings);
    bool applyBindings(const QHash<QString, QKeySequence> &bindings,
                        const QHash<QString, QKeySequence> &document);
    QHash<QString, QKeySequence> bindings() const;
    QHash<QString, QKeySequence> documentBindings() const { return m_documentBindings; }
    int pageIndex() const { return m_anchor; }
    int pageCount() const { return m_book ? m_book->pages.size() : 0; }
    bool isLoading() const { return m_loading; }
    bool renderReady() const { return m_renderReady; }
    QString bookPath() const { return m_currentPath; }
    QString directory() const { return m_directory; }
    QString lastError() const { return m_lastError; }
    ReadingOptions options() const { return m_options; }
    QAction *command(const QString &id) const { return m_actions.value(id); }
protected:
    QMenu *createPopupMenu() override;
    void changeEvent(QEvent *) override;
    void closeEvent(QCloseEvent *) override;
    void dragEnterEvent(QDragEnterEvent *) override;
    void dropEvent(QDropEvent *) override;
    bool eventFilter(QObject *, QEvent *) override;
private:
    friend class ReaderFeatures;
    friend class ReaderInput;
    void buildDocumentActions();
    void documentSettings();
    void updateDocumentActions();
    void openDocumentView();
    void clearDocumentView();
    void documentLaidOut(const QVector<DocumentLocation> &locations);
    void saveDocumentLocation();
    DocumentLocation currentDocumentLocation() const;
    int resolveDocumentLocation(const DocumentLocation &location) const;
    bool documentViewActive() const { return m_book && bool(m_book->publication); }
    void buildActions();
    void buildUi();
    void buildMenus();
    void restoreToolbarSettings();
    void updateNavigationIcons();
    void setFullscreenChromeVisible(bool top, bool bottom);
    void updateFullscreenChrome(const QPoint &position);
    void updateFullscreenState(bool enabled, bool changeWindowState);
    Qt::Edges fullscreenRevealEdges(const QPoint &position) const;
    bool fullscreenChromeHovered(const QPoint &position) const;
    void render();
    void jump(int anchor);
    void stepPage(int direction, bool onePage = false);
    QString adjacentArchive(int direction) const;
    void refreshState();
    void saveSettings();
    void activateLibraryItem(QListWidgetItem *item);
    void startNextThumbnail(quint64 generation);
    void requestThumbnails(ThumbnailGrid *grid);
    void populatePages();
    void populateDirectory(const QString &path, bool navigate, bool history, bool persist = true);
    void initializeDirectoryWatcher();
    void refreshDirectoryListing();
    void populateArchiveDirectory(const std::shared_ptr<const Book> &book, bool navigate = true);
    void setMode(ViewMode mode);
    void focusCurrentView();
    ThumbnailGrid *activeGrid() const;
    void acceptThumbnail();
    void cancelThumbnail();
    void refreshThumbnails();
    void loadBook(const QString &path, ViewMode mode, bool startAtLastPage = false);
    void buildNavigationActions();
    void attachScopedShortcuts();
    void updateShortcutBindings();
    void setDocumentOption(const QString &key, const QVariant &value);
    void setThumbnailLayout(ThumbnailGrid::Layout layout, int columns);
    void restoreThumbnailSettings();
    void buildImageActions();
    void restoreImageSettings();
    void saveImageSettings();
    void cancelBackgroundWork();
    void updateImageActionChecks();
    void highlightBook();
    QStringList zipPaths() const;
    NamePolicy namePolicyFor(const QString &path) const;
    QString nameSettingsKey(const QString &path) const;
    std::unique_ptr<QSettings> m_settings;
    std::unique_ptr<ReaderFeatures> m_features;
    std::shared_ptr<ThumbnailDiskCache> m_thumbnailDiskCache;
    QHash<QString, QAction *> m_actions;
    ReadingOptions m_options;
    std::shared_ptr<const Book> m_book;
    std::shared_ptr<const Book> m_archiveDirectory;
    QHash<QString, QString> m_archiveSelections;
    QString m_directory, m_currentPath, m_lastError;
    int m_anchor = 0, m_thumbnailActive = 0;
    quint64 m_bookGeneration = 0, m_renderGeneration = 0, m_libraryGeneration = 0;
    std::shared_ptr<std::atomic<quint64>> m_thumbnailToken;
    std::shared_ptr<std::atomic<quint64>> m_renderToken;
    std::shared_ptr<std::atomic<quint64>> m_openToken;
    std::shared_ptr<std::atomic<quint64>> m_pageThumbnailToken;
    std::shared_ptr<std::atomic<bool>> m_backgroundCancel=std::make_shared<std::atomic<bool>>(false);
    quint64 m_pageGeneration = 0;
    bool m_loading = false, m_renderReady = false;
    bool m_toolbarBeforeFullscreen = false;
    bool m_topChromeVisible = false, m_bottomChromeVisible = false;
    bool m_fullscreenTransition = false;
    bool m_treeBeforeFullscreen = true, m_wasMaximized = false;
    ViewMode m_mode = ViewMode::Covers, m_beforeThumbnails = ViewMode::Reader;
    ImageOptions m_imageOptions;
    bool m_restoring = true;
    QStringList m_history, m_siblingFolders;
    int m_historyIndex = -1, m_siblingIndex = -1;
    QHash<QString, QString> m_directorySelections;
    QFileSystemWatcher *m_directoryWatcher = nullptr;
    QTimer *m_directoryRefreshTimer = nullptr;
    QCache<int, ImageResult> m_imageCache;
    QWidget *m_browser = nullptr;
    ThumbnailGrid *m_library;
    ThumbnailGrid *m_pages;
    QWidget *m_treePanel;
    FolderTree *m_tree;
    QStackedWidget *m_stack;
    QLabel *m_pageTitle;
    QLabel *m_libraryTitle;
    QSplitter *m_splitter;
    PageView *m_view;
    DocumentView *m_documentView = nullptr;
    DocumentLocation m_pendingDocumentLocation;
    bool m_restoreDocumentLocation = false;
    QSet<int> m_documentWidePages, m_documentSoloPages;
    QHash<int,int> m_documentSides;
    QHash<QString, QKeySequence> m_baseBindings, m_documentBindings;
    QLabel *m_status;
    QSlider *m_slider;
    ReaderToolBar *m_toolbar;
    QTimer *m_chromeHideTimer;
};
