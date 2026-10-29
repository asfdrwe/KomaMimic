// SPDX-License-Identifier: GPL-3.0-only
#include "mainwindow.h"
#include "defaultpreferences.h"
#include "archivepath.h"
#include <QCoreApplication>
#include "readertoolbar.h"
#include "thumbnailcache.h"
#include "appearance.h"
#include "scriptreader.h"
#include "expression.h"
#include "pageview.h"
#include "shortcuts.h"
#include "namesdialog.h"
#include "foldertree.h"
#include "readerfeatures.h"
#include "documentsettings.h"
#include "publication.h"
#include "documentview.h"
#include <QAction>
#include <QActionGroup>
#include <QAbstractItemView>
#include <QApplication>
#include <QCloseEvent>
#include <QCursor>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QShortcutEvent>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QToolBar>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindowStateChangeEvent>
#include <QtConcurrent>
#include <algorithm>

namespace {
struct RenderResult { QVector<ImageResult> media, originals; QVector<int> indices; };
bool isOwnedBy(const QObject *object, const QObject *owner) {
    // QWidget::isAncestorOf stops at popup window boundaries; QObject ownership
    // also includes the menus and dialogs belonging to this reader window.
    for (; object; object = object->parent()) if (object == owner) return true;
    return false;
}

QImage errorPage(const QString &message) {
    QImage image(640, 900, QImage::Format_RGB32);
    image.fill(QColor("#f2e7e5"));
    QPainter p(&image);
    p.setPen(QColor("#8c2929"));
    QFont font = p.font(); font.setPixelSize(24); p.setFont(font);
    p.drawText(image.rect().adjusted(35, 35, -35, -35), Qt::AlignCenter | Qt::TextWordWrap, message);
    return image;
}

}

MainWindow::MainWindow(const QString &settingsPath, QWidget *parent, IdleInhibitor *inhibitor)
    : QMainWindow(parent), m_settings(std::make_unique<QSettings>(settingsPath, QSettings::IniFormat)),
      m_thumbnailToken(std::make_shared<std::atomic<quint64>>(0)),
      m_renderToken(std::make_shared<std::atomic<quint64>>(0)),
      m_openToken(std::make_shared<std::atomic<quint64>>(0)),
      m_pageThumbnailToken(std::make_shared<std::atomic<quint64>>(0)), m_imageCache(128 * 1024) {
    initializeMangaPreferences(*m_settings);
    m_imageCache.setMaxCost(std::clamp(m_settings->value("cache/megabytes",128).toInt(),16,2048)*1024);
    setApplicationAppearance(m_settings->value("display/appearance",0).toInt());
    m_thumbnailDiskCache=std::make_shared<ThumbnailDiskCache>();
    m_thumbnailDiskCache->configure(m_settings->value("cache/thumbnailsEnabled",true).toBool(),m_settings->value("cache/thumbnailMegabytes",512).toInt());
    (void)QtConcurrent::run([cache=m_thumbnailDiskCache,stop=m_backgroundCancel]{cache->maintain([stop]{return stop->load();});});
    setObjectName("mainWindow");
    setWindowTitle("KomaMimic");
    setAcceptDrops(true);
    resize(1180, 800);
    m_options.spread = m_settings->value("reading/spread", true).toBool();
    m_options.rightToLeft = m_settings->value("reading/rtl", true).toBool();
    m_options.coverAlone = m_settings->value("reading/cover", true).toBool();
    m_options.leadingBlank = m_settings->value("reading/leadingBlank", false).toBool();
    const int boundary = m_settings->value("reading/boundaryMode", 0).toInt();
    if (boundary >= 0 && boundary <= 2) m_options.boundaryMode = BoundaryMode(boundary);
    buildActions();
    buildUi();
    initializeDirectoryWatcher();
    buildDocumentActions();
    const QByteArray geometry = m_settings->value("window/geometry").toByteArray();
    if (!geometry.isEmpty()) restoreGeometry(geometry);
    const QByteArray splitter = m_settings->value("window/splitter").toByteArray();
    if (!splitter.isEmpty()) m_splitter->restoreState(splitter);
    m_treePanel->setVisible(m_settings->value("window/tree", true).toBool());
    { const QSignalBlocker blocker(m_actions["tree"]); m_actions["tree"]->setChecked(!m_treePanel->isHidden()); }
    restoreImageSettings();
    m_features = std::make_unique<ReaderFeatures>(this, inhibitor);
    buildMenus();
    setThumbnailLayout(ThumbnailGrid::Layout(std::clamp(m_settings->value("thumbnails/layout", 0).toInt(), 0, 2)),
                       m_settings->value("thumbnails/columns", 5).toInt());
    restoreThumbnailSettings();
    // Mouse tracking is needed to reveal the fullscreen controls without a click.
    setMouseTracking(true);
    for (auto *widget : findChildren<QWidget *>()) widget->setMouseTracking(true);
    qApp->installEventFilter(this);
    connect(qApp, &QApplication::focusChanged, this, [this] { updateShortcutBindings(); });
    m_restoring = false;
    refreshState();
}
MainWindow::~MainWindow() {
    cancelBackgroundWork();
    disconnect(qApp, nullptr, this, nullptr);
    if (m_features) m_features->closingBook();
    m_features.reset(); qApp->removeEventFilter(this);
}

void MainWindow::changeEvent(QEvent *event) {
    QMainWindow::changeEvent(event);
    // Native window controls can also change fullscreen.
    if (event->type() == QEvent::WindowStateChange && !m_fullscreenTransition) {
        const auto oldState = static_cast<QWindowStateChangeEvent *>(event)->oldState();
        if (oldState.testFlag(Qt::WindowFullScreen) != isFullScreen()) {
            updateFullscreenState(isFullScreen(), false);
            if (isFullScreen()) m_wasMaximized = oldState.testFlag(Qt::WindowMaximized);
        }
    }
    if(event->type()==QEvent::PaletteChange || event->type()==QEvent::StyleChange) updateNavigationIcons();
}
void MainWindow::updateNavigationIcons() {
    const QVector<QPair<QString,QStyle::StandardPixmap>> icons={{"folderBack",QStyle::SP_ArrowBack},{"folderForward",QStyle::SP_ArrowForward},
        {"parentFolder",QStyle::SP_ArrowUp},{"previousFolder",QStyle::SP_MediaSkipBackward},{"nextFolder",QStyle::SP_MediaSkipForward},
        {"folder",QStyle::SP_DirOpenIcon},{"readFolder",QStyle::SP_MediaPlay},
        {"open",QStyle::SP_DialogOpenButton},{"first",QStyle::SP_MediaSkipBackward},{"last",QStyle::SP_MediaSkipForward},
        {"previousBook",QStyle::SP_MediaSeekBackward},{"nextBook",QStyle::SP_MediaSeekForward},
        {"previous",QStyle::SP_ArrowBack},{"next",QStyle::SP_ArrowForward},
        {"previousOne",QStyle::SP_ArrowUp},{"nextOne",QStyle::SP_ArrowDown}};
    for(const auto &[id,kind]:icons) {
        auto *action=m_actions.value(id);if(!action) continue;
        const auto source=style()->standardIcon(kind);
        if(kind==QStyle::SP_DirOpenIcon) {action->setIcon(source);continue;}
        QIcon icon;
        for(int size:{16,24,32,48,64}) for(auto mode:{QIcon::Normal,QIcon::Disabled}) {
            auto pixels=source.pixmap(size,size);
            {QPainter painter(&pixels);painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
                painter.fillRect(pixels.rect(),palette().color(mode==QIcon::Disabled?QPalette::Disabled:QPalette::Active,QPalette::ButtonText));}
            icon.addPixmap(pixels,mode);
        }
        action->setIcon(icon);
    }
}

void MainWindow::buildActions() {
    for (const auto &spec : shortcutSpecs()) {
        auto *a = new QAction(spec.label, this);
        a->setObjectName("action_" + spec.id);
        // Assign native application-menu entries before adding any menus.
        if (spec.id == "preferences") a->setMenuRole(QAction::PreferencesRole);
        else if (spec.id == "about") a->setMenuRole(QAction::AboutRole);
        else if (spec.id == "quit") a->setMenuRole(QAction::QuitRole);
        else a->setMenuRole(QAction::NoRole);
        const QString key = "keys/" + spec.id;
        a->setShortcut(m_settings->contains(key)
            ? QKeySequence::fromString(m_settings->value(key).toString(), QKeySequence::PortableText) : QKeySequence{});
        a->setShortcutContext(Qt::WindowShortcut);
        addAction(a);
        m_actions.insert(spec.id, a);
    }
    if (m_settings->contains("keys/fullscreen")) {
        // Merge the old reader-only command into the single global command.
        // A custom reader key (e.g. V) takes priority over the old F11 default.
        const auto legacy = QKeySequence::fromString(m_settings->value("keys/fullscreen").toString(), QKeySequence::PortableText);
        QList<QKeySequence> candidates;
        if (!legacy.isEmpty() && legacy != QKeySequence("Return") && legacy != QKeySequence("Enter")) candidates.append(legacy);
        if (m_settings->contains("keys/fullscreenGlobal")) candidates.append(m_actions["fullscreenGlobal"]->shortcut());
        candidates.append(defaultFullscreenShortcut()); candidates.append(QKeySequence{});
        for (const auto &candidate : candidates) {
            if (candidate.count() > 1) continue;
            QHash<QString,QKeySequence> trial;
            for(auto it=m_actions.cbegin();it!=m_actions.cend();++it)trial[it.key()]=it.value()->shortcut();
            trial["fullscreenGlobal"]=candidate;
            if(!shortcutConflict(trial).isEmpty())continue;
            m_actions["fullscreenGlobal"]->setShortcut(candidate);
            m_settings->setValue("keys/fullscreenGlobal", candidate.toString(QKeySequence::PortableText));
            m_settings->remove("keys/fullscreen"); m_settings->sync();
            break;
        }
    }
    // Preserve saved assignments when adding commands in a newer version.
    // A conflicting default for a new command is left unassigned.
    QHash<QString, QKeySequence> accepted;
    for (const auto &spec : shortcutSpecs()) {
        auto candidate = accepted; candidate.insert(spec.id, m_actions[spec.id]->shortcut());
        if (!shortcutConflict(candidate).isEmpty()) m_actions[spec.id]->setShortcut(QKeySequence{});
        else accepted = candidate;
    }
    for (const auto &spec : shortcutSpecs()) {
        if (m_settings->contains("keys/" + spec.id)) continue;
        m_actions[spec.id]->setShortcut(spec.defaultKey);
        if (!shortcutConflict(bindings()).isEmpty()) m_actions[spec.id]->setShortcut(QKeySequence{});
    }
    m_baseBindings = bindings();
    m_documentBindings = documentKeyBindings(*m_settings);
    if (!documentShortcutConflict(m_baseBindings, m_documentBindings).isEmpty()) m_documentBindings.clear();
    connect(m_actions["open"], &QAction::triggered, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, QCoreApplication::translate("KomaMimic", "画像・書庫を開く"), m_directory, openFileFilter());
        if (!path.isEmpty()) openBook(path);
    });
    connect(m_actions["folder"], &QAction::triggered, this, [this] {
        const QString path = QFileDialog::getExistingDirectory(this, QCoreApplication::translate("KomaMimic", "フォルダを選択"), m_directory);
        if (!path.isEmpty()) openDirectory(path);
    });
    connect(m_actions["next"], &QAction::triggered, this, &MainWindow::nextPage);
    connect(m_actions["previous"], &QAction::triggered, this, &MainWindow::previousPage);
    connect(m_actions["nextOne"], &QAction::triggered, this, [this] { stepPage(1, true); });
    connect(m_actions["previousOne"], &QAction::triggered, this, [this] { stepPage(-1, true); });
    connect(m_actions["first"], &QAction::triggered, this, [this] { jump(0); });
    connect(m_actions["last"], &QAction::triggered, this, [this] { jump(pageCount() - 1); });
    connect(m_actions["nextBook"], &QAction::triggered, this, [this] { moveBook(1); });
    connect(m_actions["previousBook"], &QAction::triggered, this, [this] { moveBook(-1); });
    connect(m_actions["exitFullscreen"], &QAction::triggered, this, [this] { setFullscreen(false); });
    for (const QString &id : {QString("spread"), QString("rtl"), QString("cover"), QString("library")})
        m_actions[id]->setCheckable(true);
    m_actions["spread"]->setChecked(m_options.spread);
    m_actions["rtl"]->setChecked(m_options.rightToLeft);
    m_actions["cover"]->setChecked(m_options.coverAlone);
    connect(m_actions["spread"], &QAction::toggled, this, [this](bool on) {
        if (documentViewActive()) { setDocumentOption("pageLayout", on ? 1 : 0); return; }
        m_options.spread = on; render(); saveSettings();
    });
    connect(m_actions["rtl"], &QAction::toggled, this, [this](bool on) { m_options.rightToLeft = on; render(); saveSettings(); });
    connect(m_actions["cover"], &QAction::toggled, this, [this](bool on) { m_options.coverAlone = on; render(); saveSettings(); });
    connect(m_actions["library"], &QAction::triggered, this, [this] {
        if (m_mode == ViewMode::Covers && m_book) setMode(ViewMode::Reader); else showCovers();
    });
    auto *boundaryGroup = new QActionGroup(this);
    const QStringList boundaryIds{"boundaryStop", "boundaryRepeat", "boundaryAdvance"};
    for (int i = 0; i < boundaryIds.size(); ++i) {
        auto *action = m_actions[boundaryIds[i]];
        action->setCheckable(true);
        boundaryGroup->addAction(action);
        action->setChecked(int(m_options.boundaryMode) == i);
        connect(action, &QAction::triggered, this, [this, i] {
            m_options.boundaryMode = BoundaryMode(i); refreshState(); saveSettings();
        });
    }
    buildNavigationActions();
    connect(m_actions["shortcuts"], &QAction::triggered, this, &MainWindow::editShortcuts);
    connect(m_actions["archiveNames"], &QAction::triggered, this, &MainWindow::editArchiveNames);
    connect(m_actions["quit"], &QAction::triggered, this, &MainWindow::close);
}

void MainWindow::buildUi() {
    const QHash<QString, QString> labels{{"library", QCoreApplication::translate("KomaMimic", "表紙一覧")}, {"thumbnails", QCoreApplication::translate("KomaMimic", "ページ一覧")},
        {"spread", QCoreApplication::translate("KomaMimic", "見開き")}, {"documentSettings", QCoreApplication::translate("KomaMimic", "文書設定")}, {"textSizeIncrease", QCoreApplication::translate("KomaMimic", "文字＋")},
        {"textSizeDecrease", QCoreApplication::translate("KomaMimic", "文字－")}, {"documentWritingMode", QCoreApplication::translate("KomaMimic", "縦／横")},
        {"documentBack", QCoreApplication::translate("KomaMimic", "リンク元")}, {"findDocument", QCoreApplication::translate("KomaMimic", "本文検索")}};
    for (auto it = labels.cbegin(); it != labels.cend(); ++it) m_actions[it.key()]->setIconText(it.value());
    m_toolbar = new ReaderToolBar(m_actions, this);
    addToolBar(Qt::BottomToolBarArea, m_toolbar);
    m_slider = m_toolbar->slider();
    const auto visibility = [this](QToolBar *bar, const QString &id, const QString &key,
                                    bool initial, bool *normalState) {
        auto *action = m_actions[id];
        const bool enabled = m_settings->value(key, initial).toBool();
        *normalState = enabled;
        action->setCheckable(true); action->setChecked(enabled); bar->setVisible(enabled);
        connect(action, &QAction::toggled, this, [this, bar, key, normalState](bool on) {
            *normalState = on;
            m_settings->setValue(key, on);
            if (isFullScreen()) setFullscreenChromeVisible(m_topChromeVisible, m_bottomChromeVisible);
            else bar->setVisible(on);
        });
    };
    visibility(m_toolbar, "toolbarToggle", "window/readerToolbar", false, &m_toolbarBeforeFullscreen);
    updateNavigationIcons();
    m_splitter = new QSplitter(this);
    m_treePanel = new QWidget(m_splitter); m_treePanel->setObjectName("folderTreePanel"); m_treePanel->setMinimumWidth(150);
    auto *treeLayout = new QVBoxLayout(m_treePanel); treeLayout->setContentsMargins(4, 4, 4, 4);
    treeLayout->addWidget(new QLabel(QCoreApplication::translate("KomaMimic", "フォルダ"), m_treePanel)); m_tree = new FolderTree(m_treePanel); treeLayout->addWidget(m_tree);
    m_stack = new QStackedWidget(m_splitter); m_stack->setObjectName("contentStack");
    m_browser = new QWidget(m_stack); m_browser->setObjectName("libraryPanel");
    auto *browserLayout = new QVBoxLayout(m_browser); browserLayout->setContentsMargins(6, 6, 6, 6);
    m_libraryTitle = new QLabel(QCoreApplication::translate("KomaMimic", "表紙サムネイル — フォルダ・書庫・画像を選択して開く"), m_browser);
    m_libraryTitle->setObjectName("libraryTitle"); browserLayout->addWidget(m_libraryTitle);
    m_library = new ThumbnailGrid(m_browser); m_library->setObjectName("zipLibrary"); m_library->setAccessibleName(QCoreApplication::translate("KomaMimic", "表紙サムネイル")); browserLayout->addWidget(m_library);
    auto *pagePanel = new QWidget(m_stack); pagePanel->setObjectName("pageThumbnailsPanel");
    auto *pagesLayout = new QVBoxLayout(pagePanel); pagesLayout->setContentsMargins(6, 6, 6, 6);
    m_pageTitle = new QLabel(QCoreApplication::translate("KomaMimic", "ページサムネイル"), pagePanel); pagesLayout->addWidget(m_pageTitle);
    m_pages = new ThumbnailGrid(pagePanel); m_pages->setObjectName("pageThumbnails"); m_pages->setAccessibleName(QCoreApplication::translate("KomaMimic", "画像・書庫内のページサムネイル")); pagesLayout->addWidget(m_pages);
    m_view = new PageView(m_stack);
    auto *pdfTimer = new QTimer(this); pdfTimer->setSingleShot(true); pdfTimer->setInterval(160);
    connect(m_view, &PageView::scaleChanged, pdfTimer, [pdfTimer] { pdfTimer->start(); });
    connect(pdfTimer, &QTimer::timeout, this, [this] {
      if (!m_book || m_loading || documentViewActive()) return;
      const auto indices=visiblePages(pageCount(),m_anchor,m_options);
      for(int i:indices) if(m_book->pages[i].document && !m_book->pages[i].pdfResource &&
          QFileInfo(m_book->pages[i].filePath).suffix().compare("pdf",Qt::CaseInsensitive)==0) { render(); break; }
    });
    m_stack->addWidget(m_browser); m_stack->addWidget(pagePanel); m_stack->addWidget(m_view);
    m_splitter->addWidget(m_treePanel); m_splitter->addWidget(m_stack);
    m_splitter->setStretchFactor(0, 0); m_splitter->setStretchFactor(1, 1); m_splitter->setSizes({230, 950});
    setCentralWidget(m_splitter);
    m_status = new QLabel(QCoreApplication::translate("KomaMimic", "フォルダ・画像・書庫を開いてください"), this); m_status->setObjectName("readerStatus"); m_status->setMinimumWidth(100);
    statusBar()->addWidget(m_status, 1);
    connect(m_slider, &QSlider::valueChanged, this, &MainWindow::jump);
    connect(m_tree, &FolderTree::directoryActivated, this, &MainWindow::openDirectory);
    connect(m_library, &QListWidget::itemActivated, this, &MainWindow::activateLibraryItem);
    connect(m_pages, &QListWidget::itemActivated, this, [this] { acceptThumbnail(); });
    for (auto *grid : {m_library, m_pages}) {
        connect(grid, &ThumbnailGrid::visibleRowsChanged, this, [this, grid] { requestThumbnails(grid); });
        connect(grid, &QListWidget::currentRowChanged, this, [this] { refreshState(); });
        grid->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(grid, &QWidget::customContextMenuRequested, this, [this, grid](const QPoint &position) {
            if (auto *item = grid->itemAt(position)) grid->setCurrentItem(item);
            grid->setFocus(); QMenu menu(grid); menu.addAction(m_actions["thumbAccept"]);
            if (grid == m_library) { menu.addAction(m_actions["thumbnails"]); menu.addAction(m_actions["readFolder"]); menu.addAction(m_actions["parentFolder"]); }
            else menu.addAction(m_actions["thumbCancel"]);
            menu.addSeparator(); menu.addAction(m_actions["thumbRefresh"]); menu.exec(grid->viewport()->mapToGlobal(position));
        });
    }
    connect(m_view, &PageView::nextRequested, this, &MainWindow::nextPage);
    connect(m_view, &PageView::previousRequested, this, &MainWindow::previousPage);
    buildImageActions(); attachScopedShortcuts();
    m_actions["mediaPause"]->setCheckable(true); m_actions["mediaRepeat"]->setCheckable(true);
    m_actions["mediaPause"]->setEnabled(false); m_actions["mediaStop"]->setEnabled(false); m_actions["mediaRestart"]->setEnabled(false);
    connect(m_actions["mediaPause"], &QAction::toggled, m_view, &PageView::setAnimationPaused);
    connect(m_actions["mediaStop"], &QAction::triggered, m_view, &PageView::stopAnimations);
    connect(m_actions["mediaRestart"], &QAction::triggered, m_view, &PageView::restartAnimations);
    connect(m_actions["mediaRepeat"], &QAction::toggled, this, [this](bool enabled) { m_view->setAnimationRepeat(enabled); saveSettings(); });
    connect(m_view, &PageView::animationChanged, this, [this](bool available, bool paused) {
        const QSignalBlocker blocker(m_actions["mediaPause"]);
        m_actions["mediaPause"]->setEnabled(available); m_actions["mediaPause"]->setChecked(paused);
        m_actions["mediaStop"]->setEnabled(available); m_actions["mediaRestart"]->setEnabled(available);
    });
    {
        const QSignalBlocker blocker(m_actions["mediaRepeat"]);
        m_actions["mediaRepeat"]->setChecked(m_settings->value("reading/animationRepeat", false).toBool());
    }
    m_view->setAnimationRepeat(m_actions["mediaRepeat"]->isChecked());
    m_chromeHideTimer = new QTimer(this);
    m_chromeHideTimer->setSingleShot(true);
    m_chromeHideTimer->setInterval(300);
    connect(m_chromeHideTimer, &QTimer::timeout, this, [this] {
        if (!isFullScreen()) return;
        // A popup may extend well below the toolbar. Keep its parent controls
        // visible until the menu/dialog or an in-progress mouse operation ends.
        if (QApplication::activePopupWidget() || QApplication::activeModalWidget()
            || QApplication::mouseButtons() != Qt::NoButton) {
            m_chromeHideTimer->start();
            return;
        }
        setFullscreenChromeVisible(false, false);
    });

}

void MainWindow::activateLibraryItem(QListWidgetItem *item) {
    const QString path = item->data(PathRole).toString();
    if (item->data(DirectoryRole).toBool()) openDirectory(path); else openBook(path);
}

void MainWindow::render() {
    if (!m_book) return;
    if (documentViewActive()) {
        if (m_documentView && m_documentView->ready()) m_documentView->jump(m_anchor);
        refreshState(); return;
    }
    if (m_features) m_features->beforeRender();
    const quint64 generation = ++m_renderGeneration;
    m_renderToken->store(generation);
    const auto book = m_book;
    const QVector<int> indices = visiblePages(pageCount(), m_anchor, m_options);
    QVector<ImageResult> cached;
    QVector<QSize> targets;
    for (int i : indices) {
      const auto &p=book->pages[i];
      QSize target;
      if(p.document && !p.pdfResource && QFileInfo(p.filePath).suffix().compare("pdf",Qt::CaseInsensitive)==0) {
        int edge=std::max(m_view->width(),m_view->height())*m_view->devicePixelRatioF();
        if(m_view->scaleMode()==ScaleMode::Actual) edge=2400;
        else if(m_view->scaleMode()==ScaleMode::Zoom) edge=qRound(2400*m_view->zoomFactor());
        // Quantized resolution, at most 36 MP / 144 MiB per page.
        edge=std::clamp(((edge+255)/256)*256,256,6144);
        target={edge,edge};
      }
      targets.append(target);
      auto original=m_imageCache.contains(i) ? *m_imageCache.object(i) : ImageResult{};
      if(!target.isEmpty() && !original.image.isNull() &&
          std::abs(original.image.devicePixelRatio()-target.width()/2400.0)>.001) original={};
      cached.append(original);
    }
    m_renderReady = false;
    refreshState();
    auto *watcher = new QFutureWatcher<RenderResult>(this);
    connect(watcher, &QFutureWatcher<RenderResult>::finished, this, [this, watcher, generation] {
        const auto result = watcher->result(); watcher->deleteLater();
        if (generation != m_renderGeneration || !m_book) return;
        QStringList names;
        for (int n = 0; n < result.indices.size(); ++n) {
            const auto &asset = result.media[n];
            if (asset.error.isEmpty()) {
                const qint64 kb = (result.originals[n].memoryCost() + 1023) / 1024;
                if (kb <= m_imageCache.maxCost()) m_imageCache.insert(result.indices[n], new ImageResult(result.originals[n]), int(kb));
            }
            const auto &page = m_book->pages[result.indices[n]];
            names.append(page.name + (page.documentPage ? QString(" [%1]").arg(page.documentPage + 1) : QString{}));
        }
        m_lastError.clear();
        for (const auto &asset : result.media) if (!asset.error.isEmpty()) m_lastError += asset.error + " ";
        m_view->setLeadingBlank(m_options.leadingBlank && m_options.spread && m_anchor == 0);
        m_view->setTransition(m_settings->value("effects/transition",0).toInt(),m_settings->value("effects/duration",180).toInt());
        m_view->setMedia(result.media, m_options.rightToLeft, names.join("   /   "));
        m_renderReady = true;
        saveDocumentLocation();
        if (m_features) m_features->rendered(result.indices, result.originals);
        m_view->applyScanEntry();
        refreshState();
    });
    const bool automatic = m_features && m_features->autoSpread();
    watcher->setFuture(QtConcurrent::run([book, indices, cached, targets, automatic, filter=configuredScriptFilter(m_settings->value("scripts/filter")), condition=m_settings->value("reading/spreadCondition","@height>@width").toString(), options = m_imageOptions, token = m_renderToken, generation] {
        RenderResult result;
        for (int n = 0; n < indices.size(); ++n) {
            if (token->load() != generation) return RenderResult{};
            auto original = !cached[n].image.isNull() ? cached[n] : book->image(indices[n],targets[n],[token,generation]{return token->load()!=generation;});
            const double rasterScale=targets[n].isEmpty() ? 1 : targets[n].width()/2400.0;
            original.image.setDevicePixelRatio(rasterScale);
            if (automatic && n > 0 && !evaluateExpression(condition,{{"width",original.image.width()/original.image.devicePixelRatio()},{"height",original.image.height()/original.image.devicePixelRatio()},{"page",double(indices[n]+1)},{"totalpage",double(book->pages.size())}}).value) break;
            result.indices.append(indices[n]);
            result.originals.append(original);
            auto adjusted=options; adjusted.scriptFilter=filter;
            if(!filter.isEmpty())adjusted.scriptSettings=book->openOptions.scriptSettings;
            if(rasterScale!=1) {
              const auto margins=[rasterScale](QMargins m){return QMargins(qRound(m.left()*rasterScale),qRound(m.top()*rasterScale),qRound(m.right()*rasterScale),qRound(m.bottom()*rasterScale));};
              adjusted.crop=margins(adjusted.crop); adjusted.border=margins(adjusted.border);
              if(!adjusted.overlay.isEmpty())adjusted.textSize=qRound(adjusted.textSize*rasterScale);
            }
            auto image = processMedia(original, adjusted, [token, generation] { return token->load() != generation; });
            if (!image.image.isNull()) image.image.setDevicePixelRatio(options.resize.isValid() ? 1 : rasterScale);
            if (image.image.isNull()) image.image = errorPage(image.error);
            result.media.append(std::move(image));
            if (automatic && !evaluateExpression(condition,{{"width",original.image.width()/original.image.devicePixelRatio()},{"height",original.image.height()/original.image.devicePixelRatio()},{"page",double(indices[n]+1)},{"totalpage",double(book->pages.size())}}).value) break;
        }
        return result;
    }));
}

void MainWindow::jump(int anchor) {
    if (!m_book || anchor < 0 || anchor >= pageCount() || anchor == m_anchor) return;
    m_anchor = anchor; render();
}
void MainWindow::nextPage() { stepPage(1); }
void MainWindow::previousPage() { stepPage(-1); }
void MainWindow::stepPage(int direction, bool onePage) {
    if (!m_book || pageCount() == 0) return;
    if(documentViewActive() && m_documentView && m_documentView->auxiliaryActive()) {m_documentView->stepNote(direction);return;}
    if (m_features && m_features->navigate(direction, onePage)) return;
    setMode(ViewMode::Reader);
    const int target = m_features ? m_features->advance(direction, onePage) : onePage ? std::clamp(m_anchor + direction, 0, pageCount() - 1)
        : direction > 0 ? nextAnchor(pageCount(), m_anchor, m_options)
                        : previousAnchor(pageCount(), m_anchor, m_options);
    if (target != m_anchor) { jump(target); return; }
    switch (m_options.boundaryMode) {
    case BoundaryMode::Stop: break;
    case BoundaryMode::RepeatSource: jump(direction > 0 ? 0 : pageCount() - 1); break;
    case BoundaryMode::AdvanceSource:
        if (const auto path = adjacentArchive(direction); !path.isEmpty())
            loadBook(path, ViewMode::Reader, direction < 0);
        break;
    }
}
QString MainWindow::adjacentArchive(int direction) const {
    if(m_features&&m_features->usesPlaylist())return m_features->collectionNeighbor(direction);
    if (!m_book || m_book->filesystem) return {};
    // The browser may show another folder while this book remains open.
    // Always resolve adjacent volumes relative to the actual source archive.
    if (parseArchiveEntryPath(m_currentPath).valid() && m_archiveDirectory) {
        const auto paths = zipPaths();
        const auto active = isImagePath(m_currentPath) && m_anchor >= 0 && m_anchor < pageCount() ? m_book->entryPath(m_anchor) : m_currentPath;
        const int current = paths.indexOf(active), target = current + direction;
        return current >= 0 && target >= 0 && target < paths.size() ? paths[target] : QString{};
    }
    const QFileInfo source(physicalBookPath(m_currentPath));
    auto files = QDir(source.absolutePath()).entryInfoList(QDir::Files, QDir::NoSort);
    files.erase(std::remove_if(files.begin(), files.end(), [](const QFileInfo &file) {
        return !isArchivePath(file.fileName())&&!isDocumentPath(file.fileName());
    }), files.end());
    std::sort(files.begin(), files.end(), [this](const QFileInfo &a, const QFileInfo &b) {
        return browserLess(a,b,m_settings->value("covers/sort",0).toInt(),m_settings->value("covers/reverse",false).toBool());
    });
    for (int i = 0; i < files.size(); ++i) {
        if (files[i].absoluteFilePath() != source.absoluteFilePath()) continue;
        const int target = i + direction;
        return target >= 0 && target < files.size() ? files[target].absoluteFilePath() : QString{};
    }
    return {};
}
QStringList MainWindow::zipPaths() const {
    QStringList paths;
    for (int i = 0; i < m_library->count(); ++i) {
        auto *item = m_library->item(i);
        if (!item->data(DirectoryRole).toBool() && (isArchivePath(item->data(PathRole).toString())||isDocumentPath(item->data(PathRole).toString())||(m_archiveDirectory && isImagePath(item->data(PathRole).toString())))) paths.append(item->data(PathRole).toString());
    }
    return paths;
}
void MainWindow::moveBook(int delta) {
    const auto mode = m_mode == ViewMode::Pages ? ViewMode::Pages : ViewMode::Reader;
    if(m_features&&m_features->usesPlaylist()){const auto neighbor=m_features->collectionNeighbor(delta);if(!neighbor.isEmpty())loadBook(neighbor, mode);return;}
    const auto paths = zipPaths();
    const int current = paths.indexOf(m_currentPath);
    const int target = current < 0 ? (delta > 0 ? 0 : paths.size() - 1) : current + delta;
    if (target >= 0 && target < paths.size()) loadBook(paths[target], mode);
}
void MainWindow::highlightBook() {
    for (int i = 0; i < m_library->count(); ++i) {
        auto *item = m_library->item(i);
        if (item->data(PathRole).toString() == m_currentPath) { m_library->setCurrentItem(item); return; }
    }
}
void MainWindow::refreshState() {
    updateShortcutBindings();
    const bool loaded = bool(m_book) && pageCount() > 0;
    const int count = pageCount();
    const auto indices = m_features?m_features->visiblePagesForView():visiblePages(count, m_anchor, m_options);
    const bool auxiliary=documentViewActive() && m_documentView && m_documentView->auxiliaryActive();
    const bool canNext = auxiliary || (loaded && (m_features ? m_features->advance(1, false) : nextAnchor(count, m_anchor, m_options)) != m_anchor);
    const bool canPrevious = auxiliary || (loaded && m_anchor > 0);
    const bool repeat = loaded && m_options.boundaryMode == BoundaryMode::RepeatSource;
    const bool advance = loaded && m_options.boundaryMode == BoundaryMode::AdvanceSource;
    const bool crossNext = repeat || (advance && !canNext && !adjacentArchive(1).isEmpty());
    const bool crossPrevious = repeat || (advance && !canPrevious && !adjacentArchive(-1).isEmpty());
    m_actions["next"]->setEnabled(canNext || crossNext);
    m_actions["previous"]->setEnabled(canPrevious || crossPrevious);
    m_actions["nextOne"]->setEnabled((loaded && m_anchor + 1 < count) || crossNext);
    m_actions["previousOne"]->setEnabled(canPrevious || crossPrevious);
    m_actions["first"]->setEnabled(canPrevious);
    m_actions["last"]->setEnabled(loaded && m_anchor + 1 < count);
    const auto paths = zipPaths();
    const int current = paths.indexOf(m_currentPath);
    m_actions["nextBook"]->setEnabled(m_features&&m_features->usesPlaylist()?!m_features->collectionNeighbor(1).isEmpty():(!paths.isEmpty() && (current < 0 || current + 1 < paths.size())));
    m_actions["previousBook"]->setEnabled(m_features&&m_features->usesPlaylist()?!m_features->collectionNeighbor(-1).isEmpty():(!paths.isEmpty() && (current < 0 || current > 0)));
    m_actions["exitFullscreen"]->setEnabled(isFullScreen());
    m_actions["archiveNames"]->setEnabled(!m_loading && ((m_mode == ViewMode::Covers && m_archiveDirectory && m_archiveDirectory->canReinterpret) || (loaded && m_book->canReinterpret)));
    m_actions["parentFolder"]->setEnabled(!m_directory.isEmpty() && (m_mode != ViewMode::Covers || !QDir(m_directory).isRoot()));
    m_actions["folderBack"]->setEnabled(m_historyIndex > 0);
    m_actions["folderForward"]->setEnabled(m_historyIndex >= 0 && m_historyIndex + 1 < m_history.size());
    m_actions["previousFolder"]->setEnabled(m_siblingIndex > 0);
    m_actions["nextFolder"]->setEnabled(m_siblingIndex >= 0 && m_siblingIndex + 1 < m_siblingFolders.size());
    m_actions["readFolder"]->setEnabled(!m_directory.isEmpty() && !m_archiveDirectory);
    { const QSignalBlocker blocker(m_actions["library"]); m_actions["library"]->setChecked(m_mode == ViewMode::Covers); }
    { const QSignalBlocker blocker(m_actions["thumbnails"]); m_actions["thumbnails"]->setChecked(m_mode == ViewMode::Pages); }
    m_toolbar->setPosition(count, m_anchor, loaded && m_mode == ViewMode::Reader);
    QString text;
    if (m_mode == ViewMode::Covers) text = QCoreApplication::translate("KomaMimic", "%1    %2項目    Enter: 開く").arg(m_archiveDirectory ? m_archiveDirectory->path : m_directory).arg(m_library->count());
    else if (m_mode == ViewMode::Pages && loaded) text = QCoreApplication::translate("KomaMimic", "%1    %2 / %3    Enter: 決定 / Escape: 戻る").arg(bookDisplayName(m_currentPath)).arg(m_pages->currentRow() + 1).arg(m_pages->count());
    else if (loaded) {
        QString pages = QString::number(m_anchor + 1);
        if (indices.size() == 2) pages += "–" + QString::number(indices.last() + 1);
        const auto &t = m_book->openOptions.text;
        const bool rtl = documentViewActive() && m_book->publication
            ? (m_book->publication->epub ? m_book->publication->rightToLeft
                : t.writingMode == 2 || (t.writingMode == 0 && t.vertical))
            : m_options.rightToLeft;
        const QString layout = documentViewActive()
            ? (t.pageLayout == 1 ? QCoreApplication::translate("KomaMimic", "文書・見開き") : QCoreApplication::translate("KomaMimic", "文書・単ページ"))
            : isTextDocumentPath(m_currentPath)
                ? (m_options.spread ? QCoreApplication::translate("KomaMimic", "見開き") : QCoreApplication::translate("KomaMimic", "単ページ"))
                : (m_options.spread ? QCoreApplication::translate("KomaMimic", "見開き") : QCoreApplication::translate("KomaMimic", "単ページ"));
        text = QString("%1    %2 / %3    %4・%5%6")
            .arg(bookDisplayName(m_currentPath), pages).arg(count)
            .arg(layout, rtl ? QCoreApplication::translate("KomaMimic", "右開き") : QCoreApplication::translate("KomaMimic", "左開き"),
                 m_renderReady ? "" : QCoreApplication::translate("KomaMimic", "    読み込み中…"));
        if (m_book->canReinterpret) {
            const auto &name = m_book->pages[m_anchor].decodedName;
            text += QCoreApplication::translate("KomaMimic", "    名前: ") + name.encoding + (name.assumed ? QCoreApplication::translate("KomaMimic", "（仮定）") : QString{});
            if (!name.warning.isEmpty()) text += QCoreApplication::translate("KomaMimic", "  ※名前の解釈を確認");
        }
    } else text = m_loading ? QCoreApplication::translate("KomaMimic", "画像・書庫を読み込み中…") : QCoreApplication::translate("KomaMimic", "フォルダの書庫・画像をダブルクリックして開きます");
    if (!m_lastError.isEmpty()) text = m_lastError.simplified();
    m_status->setText(m_status->fontMetrics().elidedText(text, Qt::ElideMiddle, std::max(200, width() - 300)));
    QString details = text;
    if (loaded) for (int index : indices) {
        const auto &name = m_book->pages[index].decodedName;
        details += "\n" + name.text + "\n" + name.origin + " / " + name.encoding + "\n" + name.warning;
    }
    m_status->setToolTip(details);
    m_slider->setInvertedAppearance(m_settings->value("display/sliderDirection",false).toBool()&&m_options.rightToLeft);
    if(loaded&&m_book->format!="生成画像")m_settings->setValue("lastBook",m_currentPath);
    if (m_features) m_features->refresh();
    updateDocumentActions();
    setWindowTitle(loaded ? QString("%1 [%2/%3] — KomaMimic").arg(bookDisplayName(m_currentPath)).arg(m_anchor + 1).arg(count) : "KomaMimic");
}

void MainWindow::restoreToolbarSettings() { m_toolbar->restore(*m_settings); }

void MainWindow::toggleFullscreen() { setFullscreen(!isFullScreen()); }
void MainWindow::setFullscreenChromeVisible(bool top, bool bottom) {
    m_topChromeVisible = top; m_bottomChromeVisible = bottom;
    const auto show = [this](const QString &key, bool revealed) {
        const int mode = m_settings->value(key, 0).toInt();
        return !isFullScreen() || mode == 1 || (mode == 0 && revealed);
    };
    menuBar()->setVisible(show("fullscreen/menu", top));
    m_toolbar->setVisible(m_toolbarBeforeFullscreen && show("fullscreen/toolbar", bottom));
    statusBar()->setVisible(!isFullScreen() || (m_toolbarBeforeFullscreen && show("fullscreen/toolbar", bottom)));
}
Qt::Edges MainWindow::fullscreenRevealEdges(const QPoint &position) const {
    const bool withinWidth = position.x() >= 0 && position.x() < width();
    const bool topEdge = withinWidth && position.y() >= 0 && position.y() < 8;
    const bool bottomEdge = withinWidth && position.y() >= height() - 8 && position.y() < height();
    Qt::Edges edges;
    if (topEdge) edges |= Qt::TopEdge;
    if (bottomEdge) edges |= Qt::BottomEdge;
    return edges;
}
bool MainWindow::fullscreenChromeHovered(const QPoint &position) const {
    if (fullscreenRevealEdges(position)) return true;
    for (QWidget *widget : {static_cast<QWidget *>(menuBar()), static_cast<QWidget *>(m_toolbar),
                           static_cast<QWidget *>(statusBar())}) {
        if (widget->isVisible() && QRect(widget->mapTo(this, QPoint()), widget->size())
                                      .adjusted(-8, -8, 8, 8).contains(position)) return true;
    }
    return false;
}
void MainWindow::updateFullscreenChrome(const QPoint &position) {
    if (!isFullScreen()) return;
    const auto edges = fullscreenRevealEdges(position);
    if (edges) setFullscreenChromeVisible(m_topChromeVisible || edges.testFlag(Qt::TopEdge),
                                        m_bottomChromeVisible || edges.testFlag(Qt::BottomEdge));
    if (fullscreenChromeHovered(position)) m_chromeHideTimer->stop();
    else if (!m_chromeHideTimer->isActive()) m_chromeHideTimer->start();
}
void MainWindow::setFullscreen(bool enabled) {
    if (enabled == isFullScreen()) return;
    updateFullscreenState(enabled, true);
}
void MainWindow::updateFullscreenState(bool enabled, bool changeWindowState) {
    const QScopedValueRollback<bool> transition(m_fullscreenTransition, true);
    m_chromeHideTimer->stop();
    if (enabled) {
        if (changeWindowState) m_settings->setValue("window/geometry", saveGeometry());
        m_toolbarBeforeFullscreen = m_actions["toolbarToggle"]->isChecked();
        m_treeBeforeFullscreen = !m_treePanel->isHidden();
        m_wasMaximized = isMaximized();
        if (m_features) m_features->fullscreenChanging(true);
        if (m_settings->value("fullscreen/panes", 2).toInt() != 1) m_treePanel->hide();
        if (changeWindowState) showFullScreen();
        setFullscreenChromeVisible(false, false);
    } else {
        if (changeWindowState) {
            if (m_wasMaximized) showMaximized(); else showNormal();
        }
        m_treePanel->setVisible(m_treeBeforeFullscreen);
        if (m_features) m_features->fullscreenChanging(false);
        setFullscreenChromeVisible(true, true);
    }
    { const QSignalBlocker blocker(m_actions["tree"]); m_actions["tree"]->setChecked(!m_treePanel->isHidden()); }
    focusCurrentView(); refreshState();
}
QHash<QString, QKeySequence> MainWindow::bindings() const {
    if (!m_baseBindings.isEmpty()) return m_baseBindings;
    QHash<QString, QKeySequence> result;
    for (auto it = m_actions.cbegin(); it != m_actions.cend(); ++it) result.insert(it.key(), it.value()->shortcut());
    return result;
}
bool MainWindow::applyBindings(const QHash<QString, QKeySequence> &bindings) {
    return applyBindings(bindings, m_documentBindings);
}
bool MainWindow::applyBindings(const QHash<QString, QKeySequence> &bindings,
                                const QHash<QString, QKeySequence> &document) {
    if (!shortcutConflict(bindings).isEmpty()) return false;
    if (!documentShortcutConflict(bindings, document).isEmpty()) return false;
    for (const auto &spec : shortcutSpecs()) {
        if (!bindings.contains(spec.id)) return false;
    }
    m_baseBindings = bindings; m_documentBindings = document;
    // Preserve commands unavailable in this build (e.g. extended-only CSS).
    for (const auto &spec : shortcutSpecs())
        if (spec.scope == ShortcutScope::Reader) m_settings->remove("documentKeys/" + spec.id);
    for (auto it = document.cbegin(); it != document.cend(); ++it)
        m_settings->setValue("documentKeys/" + it.key(), it.value().toString(QKeySequence::PortableText));
    updateShortcutBindings();
    saveSettings(); return m_settings->status() == QSettings::NoError;
}
void MainWindow::editShortcuts() {
    {
        ShortcutDialog dialog(bindings(), this, m_documentBindings);
        if (dialog.exec() == QDialog::Accepted) applyBindings(dialog.bindings(), dialog.documentBindings());
    }
    focusCurrentView();
}
void MainWindow::updateShortcutBindings() {
    const bool document = m_book && isTextDocumentPath(m_currentPath) && m_mode == ViewMode::Reader;
    const auto *focus = QApplication::focusWidget();
    ShortcutScope scope = ShortcutScope::Global;
    if (focus && isOwnedBy(focus, m_tree)) scope = ShortcutScope::Tree;
    else if (focus && m_mode != ViewMode::Reader && isOwnedBy(focus, activeGrid())) scope = ShortcutScope::Thumbnails;
    else if (focus && m_mode == ViewMode::Reader && (isOwnedBy(focus, m_view)
        || (m_documentView && isOwnedBy(focus, m_documentView)))) scope = ShortcutScope::Reader;
    const bool documentSpace=document && m_baseBindings.value("spread").isEmpty()
        && m_baseBindings.value("autoSpread")==QKeySequence("Space")
        && !m_documentBindings.contains("spread") && !m_documentBindings.contains("autoSpread");
    for (const auto &spec : shortcutSpecs()) {
        auto key = document && spec.scope == ShortcutScope::Reader && m_documentBindings.contains(spec.id)
            ? m_documentBindings.value(spec.id) : m_baseBindings.value(spec.id);
        if(documentSpace && spec.id=="spread")key=QKeySequence("Space");
        if(documentSpace && spec.id=="autoSpread")key={};
        // Native macOS menu key equivalents can run before widget shortcuts.
        // Remove inactive scopes from the action itself, retaining saved keys.
        if (spec.scope != ShortcutScope::Global && spec.scope != scope) key = {};
        // Compatibility for saved/custom Return fullscreen keys: keep text and
        // thumbnail confirmation usable. New defaults use F11 (macOS: Ctrl+Meta+F).
        if (spec.id=="fullscreenGlobal" && key==QKeySequence("Return") && scope!=ShortcutScope::Reader) key={};
        if (auto *action = m_actions.value(spec.id); action && action->shortcut() != key) action->setShortcut(key);
    }
}
QString MainWindow::nameSettingsKey(const QString &path) const {
    const QFileInfo info(physicalBookPath(path));
    const QByteArray identity = info.absoluteFilePath().toUtf8() + '\0' + QByteArray::number(info.size())
        + ':' + QByteArray::number(info.lastModified().toMSecsSinceEpoch());
    return "archiveNames/" + QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
}
NamePolicy MainWindow::namePolicyFor(const QString &path) const {
    const QString key = nameSettingsKey(path);
    NamePolicy policy;
    policy.legacyEncoding = m_settings->value(key + "/encoding", "CP932").toString();
    policy.force = m_settings->value(key + "/force", false).toBool();
    if (!legacyEncodings().contains(policy.legacyEncoding)) return {};
    return policy;
}
void MainWindow::editArchiveNames() {
    const auto source = m_mode == ViewMode::Covers && m_archiveDirectory ? m_archiveDirectory : m_book;
    if (!source || !source->canReinterpret) return;
    NamesDialog dialog(source, this);
    if (dialog.exec() == QDialog::Accepted) setNamePolicy(dialog.policy());
    focusCurrentView();
}
void MainWindow::setNamePolicy(const NamePolicy &policy) {
    if (m_mode == ViewMode::Covers && m_archiveDirectory && m_archiveDirectory->canReinterpret && legacyEncodings().contains(policy.legacyEncoding)) {
        const auto key = nameSettingsKey(m_archiveDirectory->path);
        m_settings->setValue(key + "/encoding", policy.legacyEncoding); m_settings->setValue(key + "/force", policy.force);
        const auto renamed = m_archiveDirectory->reinterpretNames(policy); populateArchiveDirectory(renamed); saveSettings(); return;
    }
    if (!m_book || !m_book->canReinterpret || !legacyEncodings().contains(policy.legacyEncoding)) return;
    const auto current = m_book->pages[m_anchor];
    qint64 selectedEntry = -1;
    int selectedDocument = 0, selectedRow = -1;
    if (m_mode == ViewMode::Pages && m_pages->currentRow() >= 0 && m_pages->currentRow() < m_book->pages.size()) {
        const auto &selected = m_book->pages[m_pages->currentRow()]; selectedEntry = selected.archiveIndex; selectedDocument = selected.documentPage;
    }
    QHash<QString, ImageResult> cached;
    const auto identity = [](const BookPage &page) { return page.identity+QString("[%1,%2,%3,%4]").arg(page.portion.x()).arg(page.portion.y()).arg(page.portion.width()).arg(page.portion.height()); };
    for (int index : m_imageCache.keys()) cached.insert(identity(m_book->pages[index]), *m_imageCache.object(index));
    m_book = m_book->reinterpretNames(policy);
    m_imageCache.clear();
    for (int i = 0; i < m_book->pages.size(); ++i) {
        const auto &page = m_book->pages[i];
        if (identity(page)==identity(current)) m_anchor = i;
        if (page.archiveIndex == selectedEntry && page.documentPage == selectedDocument) selectedRow = i;
        const auto found = cached.constFind(identity(page));
        if (found != cached.cend()) {
            const qint64 kb = (found->memoryCost() + 1023) / 1024;
            if (kb <= m_imageCache.maxCost()) m_imageCache.insert(i, new ImageResult(*found), int(kb));
        }
    }
    const QString key = nameSettingsKey(m_currentPath);
    m_settings->setValue(key + "/encoding", policy.legacyEncoding);
    m_settings->setValue(key + "/force", policy.force);
    populateDirectory(m_directory, false, false);
    populatePages();
    if (selectedRow >= 0) { m_pages->setCurrentRow(selectedRow); m_pages->scrollToItem(m_pages->currentItem()); }
    if(m_features)m_features->namesChanged(policy);
    render(); saveSettings();
}
void MainWindow::saveSettings() {
    if (m_restoring || !m_browser) return;
    m_settings->setValue("reading/spread", m_options.spread);
    m_settings->setValue("reading/rtl", m_options.rightToLeft);
    m_settings->setValue("reading/cover", m_options.coverAlone);
    m_settings->setValue("reading/leadingBlank", m_options.leadingBlank);
    m_settings->setValue("reading/boundaryMode", int(m_options.boundaryMode));
    m_settings->setValue("reading/animationRepeat", m_actions["mediaRepeat"]->isChecked());
    m_settings->setValue("window/tree", isFullScreen() ? m_treeBeforeFullscreen : !m_treePanel->isHidden());
    if (!isFullScreen()) {
        m_settings->setValue("window/geometry", saveGeometry());
        m_settings->setValue("window/splitter", m_splitter->saveState());
    }
    if (!m_directory.isEmpty()) m_settings->setValue("lastDirectory", m_directory);
    for (const auto &spec : shortcutSpecs())
        m_settings->setValue("keys/" + spec.id, bindings().value(spec.id).toString(QKeySequence::PortableText));
    m_settings->sync();
    if (m_settings->status() != QSettings::NoError)
        statusBar()->showMessage(QCoreApplication::translate("KomaMimic", "設定ファイルに書き込めません: ") + m_settings->fileName(), 15000);
}
void MainWindow::cancelBackgroundWork() {
    m_backgroundCancel->store(true);
    m_thumbnailToken->store(++m_libraryGeneration);
    m_pageThumbnailToken->store(++m_pageGeneration);
    m_renderToken->fetch_add(1); ++m_renderGeneration;
    m_openToken->fetch_add(1); ++m_bookGeneration;
}
void MainWindow::closeEvent(QCloseEvent *event) {
    cancelBackgroundWork();
    if (m_features) {m_features->setProperty("closing",true);m_features->updateInhibit();m_features->closingBook();}
    saveSettings(); QMainWindow::closeEvent(event);
}
void MainWindow::dragEnterEvent(QDragEnterEvent *event) {
    if (event->mimeData()->hasUrls() && event->mimeData()->urls().first().isLocalFile()) event->acceptProposedAction();
}
void MainWindow::dropEvent(QDropEvent *event) {
    const auto urls = event->mimeData()->urls();
    if (!urls.isEmpty() && urls.first().isLocalFile()) {
        const QString path = urls.first().toLocalFile();
        if (QFileInfo(path).isDir()) openDirectory(path); else openBook(path);
        event->acceptProposedAction();
    }
}
bool MainWindow::eventFilter(QObject *object, QEvent *event) {
    if(object==this&&(event->type()==QEvent::WindowStateChange||event->type()==QEvent::ActivationChange||event->type()==QEvent::Show||event->type()==QEvent::Hide)&&m_features)QTimer::singleShot(0,this,[this]{if(m_features)m_features->updateInhibit();});
    if (isFullScreen() && event->type() == QEvent::Hide
        && (qobject_cast<QMenu *>(object) || qobject_cast<QDialog *>(object))) {
        // Popup mouse grabs can keep movement from reaching the reader. Recheck
        // the cursor after Qt has released the popup, including keyboard closes.
        if (isOwnedBy(object, this)) QTimer::singleShot(0, this, [this] {
            updateFullscreenChrome(mapFromGlobal(QCursor::pos()));
        });
    }
    if (isFullScreen() && event->type() == QEvent::MouseMove) {
        auto *widget = qobject_cast<QWidget *>(object);
        if (widget && isOwnedBy(widget, this)) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            updateFullscreenChrome(mapFromGlobal(mouse->globalPosition().toPoint()));
            if (m_features) m_features->fullscreenMouse(mapFromGlobal(mouse->globalPosition().toPoint()));
        }
    }
    // Dispatch scoped keys before item views consume their native arrows/Enter.
    // Shortcuts attached to reader/thumbnail/tree widgets also support custom
    // keys; editable fields retain Qt's usual text-editing shortcuts.
    if (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) {
        const auto *widget = qobject_cast<QWidget *>(object);
        ShortcutScope scope = ShortcutScope::Global;
        if (widget && isOwnedBy(widget, m_tree)) scope = ShortcutScope::Tree;
        else if (widget && (isOwnedBy(widget, m_library) || isOwnedBy(widget, m_pages))) scope = ShortcutScope::Thumbnails;
        else if (widget && (isOwnedBy(widget, m_view)
            || (m_documentView && isOwnedBy(widget, m_documentView))
            )) scope = ShortcutScope::Reader;
        if (scope == ShortcutScope::Global) {
            bool itemView = false;
            for (auto *parent = widget; parent && parent != this; parent = parent->parentWidget())
                if (qobject_cast<const QAbstractItemView *>(parent)) { itemView = true; break; }
            if (!widget || !isOwnedBy(widget, this) || !itemView || qobject_cast<const QLineEdit *>(widget) || QApplication::activeModalWidget())
                return QMainWindow::eventFilter(object, event);
        }
        auto *key = static_cast<QKeyEvent *>(event);
        const QKeySequence pressed(key->modifiers() | Qt::Key(key->key() == Qt::Key_Enter ? Qt::Key_Return : key->key()));
        for (const auto &spec : shortcutSpecs()) {
            auto *action = m_actions[spec.id];
            if ((spec.scope == scope || spec.scope == ShortcutScope::Global) && action->isEnabled() && !action->shortcut().isEmpty() && action->shortcut().matches(pressed) == QKeySequence::ExactMatch) {
                key->accept(); if (event->type() == QEvent::KeyPress) action->trigger(); return true;
            }
        }
    }
    return QMainWindow::eventFilter(object, event);
}
