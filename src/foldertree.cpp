// SPDX-License-Identifier: GPL-3.0-only
#include "foldertree.h"
#include "book.h"
#include <QApplication>
#include <QCollator>
#include <QDateTime>
#include <QDir>
#include <QFutureWatcher>
#include <QFileSystemWatcher>
#include <QTimer>
#include <QScrollBar>
#include <QSet>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QPersistentModelIndex>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QStyle>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <QtEndian>
#include <algorithm>
#include <functional>
#ifdef Q_OS_WIN
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>
#endif

namespace {
enum { LocationRole = Qt::UserRole+110, PhysicalPathRole, IsFolderRole, LoadedRole, LoadingRole, RefreshRole };
struct Entry { QString location, path, name, type; qint64 size = 0; QDateTime modified; bool folder = true; };
struct Listing { QList<Entry> entries; QString error; };
class BrowserList final : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;
    std::function<void()> accept;
protected:
    void keyPressEvent(QKeyEvent *event) override {
        if(event->key()==Qt::Key_Return || event->key()==Qt::Key_Enter) {
            if(accept)accept();
            event->accept();
        } else QTreeWidget::keyPressEvent(event);
    }
};
bool virtualLocation(const QString &s) { return s.startsWith("km:") || s.startsWith("shell:"); }
Entry directoryEntry(const QString &path, const QString &name = {}) {
    const QFileInfo f(path);
    return {f.absoluteFilePath(),f.absoluteFilePath(),name.isEmpty() ? f.fileName() : name,{},0,{},true};
}
#ifdef Q_OS_WIN
using Microsoft::WRL::ComPtr;
struct ComApartment {
    HRESULT status = CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    ~ComApartment() { if(SUCCEEDED(status))CoUninitialize(); }
};
QString shellText(IShellItem *item,SIGDN kind) {
    PWSTR value = nullptr;
    if(FAILED(item->GetDisplayName(kind,&value)))return {};
    const auto text = QString::fromWCharArray(value); CoTaskMemFree(value); return text;
}
Entry shellEntry(IShellItem *item) {
    Entry entry; entry.name = shellText(item,SIGDN_NORMALDISPLAY);
    entry.path = shellText(item,SIGDN_FILESYSPATH);
    SFGAOF attributes = 0; item->GetAttributes(SFGAO_FOLDER | SFGAO_HIDDEN,&attributes);
    entry.folder = !entry.path.isEmpty() ? QFileInfo(entry.path).isDir() : bool(attributes & SFGAO_FOLDER);
    if (!entry.path.isEmpty()) {
        entry.location = entry.path;
        const QFileInfo info(entry.path); entry.size=info.size(); entry.modified=info.lastModified();entry.type=info.suffix();
    } else {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if(SUCCEEDED(SHGetIDListFromObject(item,&pidl))) {
            entry.location="shell:"+QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(pidl),int(ILGetSize(pidl))).toBase64());
            CoTaskMemFree(pidl);
        }
    }
    return entry;
}
ComPtr<IShellItem> shellItem(const QString &location) {
    ComPtr<IShellItem> item;
    if(location=="km:desktop") {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if(SUCCEEDED(SHGetSpecialFolderLocation(nullptr,CSIDL_DESKTOP,&pidl))) {
            SHCreateItemFromIDList(pidl,IID_PPV_ARGS(item.GetAddressOf())); CoTaskMemFree(pidl);
        }
    } else if(location=="km:computer") {
        SHGetKnownFolderItem(FOLDERID_ComputerFolder,KF_FLAG_DEFAULT,nullptr,IID_PPV_ARGS(item.GetAddressOf()));
    } else {
        const auto decoded=QByteArray::fromBase64Encoding(location.mid(6).toLatin1(),QByteArray::AbortOnBase64DecodingErrors);
        if(!decoded)return item;
        const auto &bytes=decoded.decoded;
        if(bytes.size()>1024*1024)return item;
        // Saved settings may be truncated. Validate every ITEMID before passing
        // an opaque native ID list to the Shell, including its final terminator.
        qsizetype offset=0;bool valid=false;
        while(offset+2<=bytes.size()) {
            const auto size=qFromLittleEndian<quint16>(bytes.constData()+offset);
            if(!size) {valid=offset+2==bytes.size();break;}
            if(size<2 || offset+size>bytes.size())break;
            offset+=size;
        }
        if(valid)SHCreateItemFromIDList(reinterpret_cast<PCIDLIST_ABSOLUTE>(bytes.constData()),IID_PPV_ARGS(item.GetAddressOf()));
    }
    return item;
}
#endif
Listing listLocation(const QString &location) {
    Listing result;
#ifdef Q_OS_WIN
    if(location=="km:desktop" || location.startsWith("shell:")) {
        ComApartment com;
        auto parent=shellItem(location);
        ComPtr<IShellFolder> folder;
        ComPtr<IEnumIDList> items;
        if(!parent || FAILED(parent->BindToHandler(nullptr,BHID_SFObject,IID_PPV_ARGS(folder.GetAddressOf())))
            || FAILED(folder->EnumObjects(nullptr,SHCONTF(SHCONTF_FOLDERS|SHCONTF_NONFOLDERS|SHCONTF_NAVIGATION_ENUM),items.GetAddressOf()))) {
            result.error=QCoreApplication::translate("KomaMimic","フォルダを開けません: ")+FolderTree::locationName(location); return result;
        }
        PIDLIST_ABSOLUTE parentId=nullptr;
        if(FAILED(SHGetIDListFromObject(parent.Get(),&parentId))) {
            result.error=QCoreApplication::translate("KomaMimic","フォルダを開けません: ")+FolderTree::locationName(location);return result;
        }
        PITEMID_CHILD child=nullptr;
        while(items && items->Next(1,&child,nullptr)==S_OK) {
            PIDLIST_ABSOLUTE combined=ILCombine(parentId,child);
            ComPtr<IShellItem> item;
            if(combined && SUCCEEDED(SHCreateItemFromIDList(combined,IID_PPV_ARGS(item.GetAddressOf())))) {
                SFGAOF hidden=0; item->GetAttributes(SFGAO_HIDDEN,&hidden);
                if(!(hidden & SFGAO_HIDDEN)) {
                    auto entry=shellEntry(item.Get());
                    if(!entry.location.isEmpty())result.entries.append(entry);
                }
            }
            CoTaskMemFree(combined);CoTaskMemFree(child);child=nullptr;
        }
        CoTaskMemFree(parentId);
        if(location=="km:desktop") {
            // Known folders supply native, redirected paths; never substitute C:.
            for(const auto *id : {&FOLDERID_Profile,&FOLDERID_Downloads,&FOLDERID_ComputerFolder,&FOLDERID_NetworkFolder}) {
                ComPtr<IShellItem> known;
                if(SUCCEEDED(SHGetKnownFolderItem(*id,KF_FLAG_DEFAULT,nullptr,IID_PPV_ARGS(known.GetAddressOf())))) {
                    auto entry=shellEntry(known.Get());
                    if(id==&FOLDERID_ComputerFolder) {
                        for(auto &listed:result.entries)if(listed.location==entry.location)listed.location="km:computer";
                        entry.location="km:computer";
                    }
                    if(!entry.location.isEmpty() && std::none_of(result.entries.cbegin(),result.entries.cend(),[&](const Entry &e){return e.location==entry.location || (!e.path.isEmpty() && e.path==entry.path);}))
                        result.entries.append(entry);
                }
            }
        }
    } else
#endif
    if(location=="km:places") {
        for(const auto &[kind,name] : QList<QPair<QStandardPaths::StandardLocation,QString>>{
            {QStandardPaths::HomeLocation,QCoreApplication::translate("KomaMimic","ホーム")},{QStandardPaths::DesktopLocation,QCoreApplication::translate("KomaMimic","デスクトップ")},
            {QStandardPaths::DownloadLocation,QCoreApplication::translate("KomaMimic","ダウンロード")},{QStandardPaths::DocumentsLocation,QCoreApplication::translate("KomaMimic","ドキュメント")},
            {QStandardPaths::PicturesLocation,QCoreApplication::translate("KomaMimic","画像")}}) {
            const auto path=QStandardPaths::writableLocation(kind);
            if(!path.isEmpty() && QFileInfo(path).isDir())result.entries.append(directoryEntry(path,name));
        }
#ifdef Q_OS_MACOS
        result.entries.append({"km:computer",{},QCoreApplication::translate("KomaMimic","コンピュータ"),{},0,{},true});
#endif
        result.entries.append(directoryEntry(QDir::rootPath(),QCoreApplication::translate("KomaMimic","ファイルシステム")));
        for(const auto &volume:QStorageInfo::mountedVolumes()) {
            if(!volume.isValid() || !volume.isReady())continue;
            const auto path=volume.rootPath();
            if(!QFileInfo(path).isDir())continue;
            if(path=="/" || path.startsWith("/proc") || path.startsWith("/sys") || path.startsWith("/dev") || path.startsWith("/run/user/"))continue;
            if(std::none_of(result.entries.cbegin(),result.entries.cend(),[&](const Entry &e){return e.path==path;}))
                result.entries.append(directoryEntry(path,volume.displayName()));
        }
    } else if(location=="km:computer") {
#ifdef Q_OS_WIN
        for(const auto &drive:QDir::drives()) {
            const auto volume=QStorageInfo(drive.absoluteFilePath());
            const auto name=volume.name();
            const auto label=name.isEmpty() ? QDir::toNativeSeparators(drive.absoluteFilePath())
                : name+" ("+QDir::toNativeSeparators(drive.absoluteFilePath())+")";
            result.entries.append(directoryEntry(drive.absoluteFilePath(),label));
        }
#else
        for(const auto &volume:QStorageInfo::mountedVolumes())
            if(volume.isValid() && volume.isReady() && (volume.rootPath()=="/" || volume.rootPath().startsWith("/Volumes/")))
                result.entries.append(directoryEntry(volume.rootPath(),volume.displayName()));
#endif
    } else {
        const QFileInfo info(location);
        if(!info.isDir() || !info.isReadable()) { result.error=QCoreApplication::translate("KomaMimic","フォルダを開けません: ")+location; return result; }
        // Enumerate just one level; no archive reads, hashes or recursive walk.
        const auto files=QDir(location).entryInfoList(QDir::AllDirs|QDir::Files|QDir::NoDotAndDotDot,QDir::NoSort);
        for(const auto &file:files) {
            if(!file.isDir() && !isImagePath(file.fileName()) && !isArchivePath(file.fileName()) && !isDocumentPath(file.fileName()))continue;
            result.entries.append({file.absoluteFilePath(),file.absoluteFilePath(),file.fileName(),file.suffix(),file.size(),file.lastModified(),file.isDir()});
        }
    }
    QCollator order; order.setNumericMode(true); order.setCaseSensitivity(Qt::CaseInsensitive);
    std::stable_sort(result.entries.begin(),result.entries.end(),[&](const Entry &a,const Entry &b){
        return a.folder!=b.folder ? a.folder : order.compare(a.name,b.name)<0;
    });
    return result;
}
QStandardItem *node(const Entry &entry) {
    auto *item=new QStandardItem(entry.name);
    item->setEditable(false); item->setData(entry.location,LocationRole); item->setData(entry.path,PhysicalPathRole);
    item->setData(entry.folder,IsFolderRole); item->setToolTip(entry.path.isEmpty()?entry.name:QDir::toNativeSeparators(entry.path));
    item->setIcon(QApplication::style()->standardIcon(entry.folder?QStyle::SP_DirIcon:QStyle::SP_FileIcon));
    return item;
}
}
class FolderModel final : public QStandardItemModel {
public:
    explicit FolderModel(QObject *parent):QStandardItemModel(parent),m_watch(new QFileSystemWatcher(this)),m_timer(new QTimer(this)) {
        m_timer->setSingleShot(true);m_timer->setInterval(250);
        connect(m_watch,&QFileSystemWatcher::directoryChanged,this,[this](const QString &path){m_pending.insert(path);m_timer->start();});
        connect(m_timer,&QTimer::timeout,this,[this] {
            const auto pending=m_pending;m_pending.clear();
            for(const auto &path:pending) {
                QList<QStandardItem *> nodes;
                for(int row=0;row<rowCount();++row)nodes.append(item(row));
                while(!nodes.isEmpty()) {
                    auto *entry=nodes.takeLast();
                    if(entry->data(PhysicalPathRole).toString()==path) {
                        if(entry->data(LoadingRole).toBool()) {m_pending.insert(path);break;}
                        entry->setData(true,RefreshRole);entry->setData(false,LoadedRole);fetchMore(entry->index());break;
                    }
                    for(int row=0;row<entry->rowCount();++row)nodes.append(entry->child(row));
                }
            }
            if(!m_pending.isEmpty())m_timer->start();
        });
    }
    std::function<void(const QString &)> changed;
    void resetRoot() {
        m_timer->stop();m_pending.clear();
        const auto paths=m_watch->directories();if(!paths.isEmpty())m_watch->removePaths(paths);
        clear();
    }
    bool hasChildren(const QModelIndex &index = {}) const override {
        return !index.isValid() ? rowCount()>0 : index.data(IsFolderRole).toBool() && (!index.data(LoadedRole).toBool() || rowCount(index)>0);
    }
    bool canFetchMore(const QModelIndex &index) const override {
        return index.isValid() && index.data(IsFolderRole).toBool() && !index.data(LoadedRole).toBool() && !index.data(LoadingRole).toBool();
    }
    void fetchMore(const QModelIndex &index) override {
        if(!canFetchMore(index))return;
        setData(index,true,LoadingRole);
        const auto location=index.data(LocationRole).toString();
        const QPersistentModelIndex saved(index);
        auto *watcher=new QFutureWatcher<Listing>(this);
        QObject::connect(watcher,&QFutureWatcher<Listing>::finished,this,[this,watcher,saved] {
            const auto result=watcher->result(); watcher->deleteLater(); if(!saved.isValid())return;
            auto *parent=itemFromIndex(saved); parent->setData(true,LoadedRole); parent->setData(false,LoadingRole);
            if(!result.error.isEmpty()) {parent->setToolTip(result.error);return;}
            QSet<QString> locations;
            for(const auto &entry:result.entries)if(entry.folder)locations.insert(entry.location);
            for(int row=parent->rowCount()-1;row>=0;--row)
                if(!locations.contains(parent->child(row)->data(LocationRole).toString()))parent->removeRow(row);
            QCollator order;order.setNumericMode(true);order.setCaseSensitivity(Qt::CaseInsensitive);
            for(const auto &entry:result.entries)if(entry.folder) {
                bool found=false;
                for(int row=0;row<parent->rowCount();++row)if(parent->child(row)->data(LocationRole).toString()==entry.location){found=true;break;}
                if(found)continue;
                int position=0;
                while(position<parent->rowCount() && order.compare(parent->child(position)->text(),entry.name)<0)++position;
                parent->insertRow(position,node(entry));
            }
            const auto path=parent->data(PhysicalPathRole).toString();
            if(!path.isEmpty() && !m_watch->directories().contains(path))m_watch->addPath(path);
            emit dataChanged(saved,saved);
            const bool refresh=parent->data(RefreshRole).toBool();parent->setData(false,RefreshRole);
            if(refresh && changed)changed(path);
        });
        watcher->setFuture(QtConcurrent::run([location]{return listLocation(location);}));
    }
private:
    QFileSystemWatcher *m_watch;
    QTimer *m_timer;
    QSet<QString> m_pending;
};

QString FolderTree::topLocation() {
#ifdef Q_OS_WIN
    return "km:desktop";
#else
    return "km:places";
#endif
}
QString FolderTree::locationName(const QString &location) {
    if(location=="km:desktop")return QCoreApplication::translate("KomaMimic","デスクトップ");
    if(location=="km:places")return QCoreApplication::translate("KomaMimic","場所");
    if(location=="km:computer") {
#ifdef Q_OS_MACOS
        return QCoreApplication::translate("KomaMimic","コンピュータ");
#else
        return QCoreApplication::translate("KomaMimic","PC");
#endif
    }
    if(location.startsWith("shell:")) {
#ifdef Q_OS_WIN
        ComApartment com; auto item=shellItem(location); if(item)return shellText(item.Get(),SIGDN_NORMALDISPLAY);
#endif
        return QCoreApplication::translate("KomaMimic","フォルダ");
    }
    const QFileInfo file(location);return file.fileName().isEmpty()?QDir::toNativeSeparators(location):file.fileName();
}
FolderTree::FolderTree(QWidget *parent):QTreeView(parent),m_files(new FolderModel(this)) {
    setObjectName("folderTree"); setAccessibleName(QCoreApplication::translate("KomaMimic","フォルダツリー"));setModel(m_files);
    m_files->changed=[this](const QString &path){emit contentsChanged(path);};
    setHeaderHidden(true);setUniformRowHeights(true);setEditTriggers(QAbstractItemView::NoEditTriggers);
    // Columns do not exist yet, and resetRoot() recreates them. The global
    // policy also applies to future columns; a section-0 override is lost.
    header()->setSectionResizeMode(QHeaderView::ResizeToContents); header()->setStretchLastSection(false);
    setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    setExpandsOnDoubleClick(false);
    // activated follows the desktop's single-click policy on some platforms.
    // This browser deliberately requires double-click or Enter in two-pane mode.
    connect(this,&QTreeView::doubleClicked,this,[this]{activateSelection();});
    connect(this,&QTreeView::clicked,this,[this]{if(!m_twoPane)activateSelection();});
    connect(m_files,&QAbstractItemModel::rowsInserted,this,[this] {
        if(!m_pendingSelection.isEmpty())selectPath(m_pendingSelection);
    });
    setTwoPane(false);
}
void FolderTree::setBrowseRoot(const QString &location,const QString &label) {
    m_pendingSelection.clear();
    m_files->resetRoot();
    auto *root=node({location,virtualLocation(location)?QString{}:location,label.isEmpty()?locationName(location):label,{},0,{},true});
    m_files->appendRow(root);setCurrentIndex(root->index());expand(root->index());
}
void FolderTree::setTwoPane(bool enabled) {
    m_twoPane=enabled;
#ifdef Q_OS_WIN
    setBrowseRoot(enabled?topLocation():QString("km:computer"));
#else
    setBrowseRoot(enabled?topLocation():QDir::rootPath());
#endif
}
QString FolderTree::browseRoot() const {return m_files->index(0,0).data(LocationRole).toString();}
QString FolderTree::selectedPath() const {return currentIndex().data(PhysicalPathRole).toString();}
void FolderTree::activateSelection() {
    const auto path=selectedPath();
    if(!path.isEmpty())emit directoryActivated(path);
    else if(currentIndex().isValid())setExpanded(currentIndex(),!isExpanded(currentIndex()));
}
void FolderTree::selectPath(const QString &path) {
    if(m_twoPane || path.isEmpty())return;
    const QString absolute=QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    m_pendingSelection=absolute;
    // Advance only along the selected branch as asynchronous rows arrive.
    const auto descend=[this,absolute] {
        if(m_twoPane)return;
        QModelIndex index=m_files->index(0,0);
        while(index.isValid()) {
            if(QDir::cleanPath(index.data(PhysicalPathRole).toString())==absolute) {
                setCurrentIndex(index);scrollTo(index);m_pendingSelection.clear();return;
            }
            if(m_files->canFetchMore(index))m_files->fetchMore(index);
            expand(index); bool found=false;
            for(int row=0;row<m_files->rowCount(index);++row) {
                const auto child=m_files->index(row,0,index);const auto p=QDir::cleanPath(child.data(PhysicalPathRole).toString());
                const auto prefix=p.endsWith('/')?p:p+'/';
                if(!p.isEmpty() && (absolute==p || absolute.startsWith(prefix,
#ifdef Q_OS_WIN
                    Qt::CaseInsensitive
#else
                    Qt::CaseSensitive
#endif
                    ))) {index=child;found=true;break;}
            }
            if(!found)return;
        }
    };
    descend();
}
void FolderTree::keyPressEvent(QKeyEvent *event) {
    if(event->key()==Qt::Key_Return || event->key()==Qt::Key_Enter) {activateSelection();event->accept();}
    else QTreeView::keyPressEvent(event);
}
void FolderTree::navigate(int key) {QKeyEvent event(QEvent::KeyPress,key,Qt::NoModifier);QTreeView::keyPressEvent(&event);}

FolderBrowser::FolderBrowser(QSettings &settings,QWidget *parent):QWidget(parent),m_settings(settings) {
    setObjectName("folderTreePanel");setMinimumWidth(150);
    auto *layout=new QVBoxLayout(this);layout->setContentsMargins(4,4,4,4);
    m_splitter=new QSplitter(Qt::Vertical,this);m_splitter->setObjectName("folderBrowserSplitter");layout->addWidget(m_splitter);
    m_upperPanel=new QWidget(m_splitter);auto *upperLayout=new QVBoxLayout(m_upperPanel);upperLayout->setContentsMargins(0,0,0,0);
    auto *tools=new QHBoxLayout;upperLayout->addLayout(tools);
    for(const auto &[name,icon] : QList<QPair<QString,QStyle::StandardPixmap>>{{"up",QStyle::SP_ArrowUp},{"top",QStyle::SP_ComputerIcon}}) {
        auto *button=new QToolButton(m_upperPanel);button->setObjectName("folderBrowser_"+name);button->setIcon(style()->standardIcon(icon));
        button->setToolTip(name=="up"?QCoreApplication::translate("KomaMimic","上へ"):QCoreApplication::translate("KomaMimic","最上位へ"));tools->addWidget(button);
        connect(button,&QToolButton::clicked,this,[this,name]{if(name=="up")goParent();else {m_parents.clear();openLocation(FolderTree::topLocation());}});
    }
    m_locationLabel=new QLabel(m_upperPanel);m_locationLabel->setObjectName("folderBrowserLocation");m_locationLabel->setMinimumWidth(1);
    tools->addWidget(m_locationLabel,1);
    m_upper=new BrowserList(m_upperPanel);m_upper->setObjectName("folderBrowserList");m_upper->setRootIsDecorated(false);
    m_upper->setHeaderLabels({QCoreApplication::translate("KomaMimic","名前"),QCoreApplication::translate("KomaMimic","サイズ"),QCoreApplication::translate("KomaMimic","種類"),QCoreApplication::translate("KomaMimic","更新日時")});m_upper->setUniformRowHeights(true);
    m_upper->setEditTriggers(QAbstractItemView::NoEditTriggers);m_upper->header()->setStretchLastSection(false);
    m_upper->header()->resizeSection(0,210);upperLayout->addWidget(m_upper);
    m_tree=new FolderTree(m_splitter);m_splitter->addWidget(m_upperPanel);m_splitter->addWidget(m_tree);m_splitter->setSizes({300,300});
    static_cast<BrowserList *>(m_upper)->accept=[this]{activateUpper(m_upper->currentItem());};
    connect(m_upper,&QTreeWidget::itemDoubleClicked,this,[this](QTreeWidgetItem *item){activateUpper(item);});
    connect(m_tree,&FolderTree::directoryActivated,this,&FolderBrowser::directoryActivated);
    connect(m_tree,&FolderTree::contentsChanged,this,[this](const QString &path) {
        if(m_twoPane && path==m_location)openLocation(m_location,m_locationLabel->text(),true);
    });
    connect(m_splitter,&QSplitter::splitterMoved,this,[this]{if(!m_restoring)saveSettings();});
    restoreSettings();
}
void FolderBrowser::restoreSettings() {
    m_restoring=true;
    m_location=m_settings.value("folders/location",FolderTree::topLocation()).toString();
    if(!virtualLocation(m_location) && (!QFileInfo(m_location).isDir() || !QFileInfo(m_location).isReadable()))m_location=FolderTree::topLocation();
    setTwoPane(m_settings.value("folders/twoPane",true).toBool());
    if(!m_twoPane)m_tree->selectPath(m_settings.value("folders/selection",virtualLocation(m_location)?QString{}:m_location).toString());
    const auto state=m_settings.value("folders/splitter").toByteArray();if(!state.isEmpty())m_splitter->restoreState(state);
    if(m_twoPane)openLocation(m_location);
    m_restoring=false;
}
void FolderBrowser::saveSettings() {
    m_settings.setValue("folders/twoPane",m_twoPane);m_settings.setValue("folders/splitter",m_splitter->saveState());
    if(!m_location.isEmpty())m_settings.setValue("folders/location",m_location);
    m_settings.setValue("folders/selection",m_tree->selectedPath());
}
void FolderBrowser::setTwoPane(bool enabled) {
    const bool changed=enabled!=m_twoPane;
    const auto selection=m_tree->selectedPath();
    if(changed)++m_generation;
    m_twoPane=enabled;m_upperPanel->setVisible(enabled);m_tree->setTwoPane(enabled);
    if(enabled)openLocation(changed && !selection.isEmpty()?selection:m_location.isEmpty()?FolderTree::topLocation():m_location);
    else if(!selection.isEmpty())m_tree->selectPath(selection);
    else if(!virtualLocation(m_location))m_tree->selectPath(m_location);
}
void FolderBrowser::openLocation(const QString &location,const QString &label,bool refresh) {
    if(location.isEmpty() || (refresh && m_requestedLocation!=location))return;
    if(!refresh)m_requestedLocation=location;
    const auto selected=m_upper->currentItem()?m_upper->currentItem()->data(0,LocationRole).toString():QString{};
    const int scroll=m_upper->verticalScrollBar()->value();
    const quint64 generation=++m_generation;
    const bool persist=!m_restoring && !refresh;
    auto *watcher=new QFutureWatcher<Listing>(this);
    connect(watcher,&QFutureWatcher<Listing>::finished,this,[this,watcher,location,label,generation,persist,refresh,selected,scroll] {
        const auto result=watcher->result();watcher->deleteLater();if(generation!=m_generation)return;
        if(!result.error.isEmpty()) {
            emit errorOccurred(result.error);
            if(m_location.isEmpty() && location!=FolderTree::topLocation())openLocation(FolderTree::topLocation());
            return;
        }
        m_location=location;m_locationLabel->setText(label.isEmpty()?FolderTree::locationName(location):label);
        m_locationLabel->setToolTip(virtualLocation(location)?m_locationLabel->text():QDir::toNativeSeparators(location));
        m_upper->clear();
        if(location!=FolderTree::topLocation()) {
            auto *up=new QTreeWidgetItem(m_upper,{".."});up->setData(0,LocationRole,"km:parent");
            up->setIcon(0,style()->standardIcon(QStyle::SP_ArrowUp));
        }
        for(const auto &entry:result.entries) {
            auto *item=new QTreeWidgetItem(m_upper,{entry.name,entry.folder?QString{}:QString::number(entry.size),entry.folder?QCoreApplication::translate("KomaMimic","フォルダ"):entry.type,entry.modified.toString(Qt::ISODate)});
            item->setData(0,LocationRole,entry.location);item->setData(0,PhysicalPathRole,entry.path);item->setData(0,IsFolderRole,entry.folder);
            item->setIcon(0,style()->standardIcon(entry.folder?QStyle::SP_DirIcon:QStyle::SP_FileIcon));item->setToolTip(0,entry.path.isEmpty()?entry.name:entry.path);
            if(refresh && entry.location==selected)m_upper->setCurrentItem(item);
        }
        if(refresh)m_upper->verticalScrollBar()->setValue(scroll);
        if(m_twoPane && !refresh)m_tree->setBrowseRoot(location,label);
        if(persist)saveSettings();
    });
    watcher->setFuture(QtConcurrent::run([location]{return listLocation(location);}));
}
void FolderBrowser::activateUpper(QTreeWidgetItem *item) {
    if(!item)return;
    const auto location=item->data(0,LocationRole).toString();
    if(location=="km:parent") {goParent();return;}
    if(item->data(0,IsFolderRole).toBool()) {m_parents.append(m_location);openLocation(location,item->text(0));}
    else {const auto path=item->data(0,PhysicalPathRole).toString();if(!path.isEmpty())emit fileActivated(path);}
}
void FolderBrowser::goParent() {
    if(m_location==FolderTree::topLocation())return;
    if(virtualLocation(m_location)) {openLocation(m_parents.isEmpty()?FolderTree::topLocation():m_parents.takeLast());return;}
    QDir directory(m_location);
    if(directory.isRoot()) {
#ifdef Q_OS_WIN
        openLocation("km:computer");
#endif
        return;
    }
    directory.cdUp();openLocation(directory.absolutePath());
}
void FolderBrowser::navigate(int key) {
    if(m_upper->hasFocus() && m_twoPane) {QKeyEvent event(QEvent::KeyPress,key,Qt::NoModifier);QApplication::sendEvent(m_upper,&event);}
    else m_tree->navigate(key);
}
void FolderBrowser::activateSelection() {if(m_upper->hasFocus() && m_twoPane)activateUpper(m_upper->currentItem());else m_tree->activateSelection();}
void FolderBrowser::focusTree() {m_tree->setFocus();}
