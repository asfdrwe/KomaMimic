// SPDX-License-Identifier: GPL-3.0-only
#include "test_support.h"
#include "displayjobs.h"
#include "pageview.h"
#include "readerfeatures.h"
#include "legacysettings.h"
#include "librarystore.h"
#include "apng.h"
#include "thumbnailsource.h"
#include <QCryptographicHash>
#include <QAction>
#include <QJsonDocument>
#include <QTimer>

class ReaderStartupTests : public QObject {
    Q_OBJECT
    static void settings(const QString &path) {
        QSettings s(path,QSettings::IniFormat);
        s.setValue("cache/ahead",0);s.setValue("cache/behind",0);
        s.setValue("cache/fileMegabytes",0);s.setValue("reading/spread",false);
        s.setValue("General/watch",false);s.sync();
    }
    static QByteArray pages(int count=5,bool reverse=false) {
        QVector<TestData::ZipEntry> entries;
        const auto png=TestData::png(TestData::solid());
        for(int n=0;n<count;++n)entries.append({QString("%1.png").arg(reverse?count-n-1:n,5,10,QChar('0')).toUtf8(),png});
        return TestData::zip(entries);
    }
private slots:
    void readerDefersThumbnailPreparation() {
        QTemporaryDir d;const auto path=d.filePath("pages.zip"),ini=d.filePath("settings.ini");
        QVERIFY(TestData::write(path,pages(300)));settings(ini);
        MainWindow w(ini);w.show();
        const auto snapshots=BookActivity::thumbnailSnapshots.load();
        const auto images=BookActivity::imageRequests.load();
        w.openBook(path);QTRY_VERIFY(!w.isLoading() && w.renderReady() && w.m_readerPresented);
        QCOMPARE(w.m_pages->count(),0);QVERIFY(w.m_pagesDirty);
        QCOMPARE(w.m_library->count(),0);QVERIFY(!w.m_directoryLoading);
        QCOMPARE(BookActivity::thumbnailSnapshots.load(),snapshots);
        QCOMPARE(BookActivity::imageRequests.load()-images,quint64(1));
        w.showPageThumbnails();QCOMPARE(w.m_pages->count(),300);QVERIFY(!w.m_pagesDirty);
        QVERIFY(BookActivity::thumbnailSnapshots.load()>snapshots);
        w.m_pages->setCurrentRow(123);
        const auto before=BookActivity::imageRequests.load();
        w.acceptThumbnail();QTRY_VERIFY(w.renderReady() && w.m_readerPresented);
        QCOMPARE(w.pageIndex(),123);
        QCOMPARE(BookActivity::imageRequests.load()-before,quint64(1));
    }
    void transitionCaptureDoesNotPresentPreviousPage() {
        PageView view;view.resize(400,600);view.show();
        QSignalSpy presented(&view,&PageView::imagePresented);
        view.setPages({TestData::solid(Qt::red)},false,{});QTRY_COMPARE(presented.count(),1);
        presented.clear();view.setTransition(1,180);
        view.setPages({TestData::solid(Qt::blue)},false,{});
        // grab() paints the previous page synchronously during the transition.
        QCoreApplication::sendPostedEvents(&view,QEvent::MetaCall);
        QCOMPARE(presented.count(),0);
        view.repaint();QTRY_COMPARE(presented.count(),1);
    }
    void directoryDeliveryStopsOnReaderEntry() {
        QTemporaryDir d;const auto folder=d.filePath("pages"),ini=d.filePath("settings.ini");
        QVERIFY(TestData::imageFolder(folder,450));settings(ini);
        MainWindow w(ini);w.show();bool requested=false;
        connect(w.m_library->model(),&QAbstractItemModel::rowsInserted,&w,[&](const QModelIndex &,int,int last){
            if(last==199 && !requested){requested=true;QTimer::singleShot(0,&w,[&]{w.readFolder();});}
        });
        w.openDirectory(folder);
        QTRY_VERIFY(requested && !w.isLoading() && w.renderReady());
        QCOMPARE(w.viewMode(),MainWindow::ViewMode::Reader);
        QCOMPARE(w.m_library->count(),200);
        QTest::qWait(150);QCOMPARE(w.m_library->count(),200);
        QVERIFY(w.m_directoryDirty);QVERIFY(!w.m_directoryLoading);
        w.showCovers();QTRY_VERIFY(w.directoryReady() && w.m_library->count()==450);
    }
    void readerFindsSiblingFoldersOnlyWhenRequested() {
        QTemporaryDir d;const auto first=d.filePath("01"),second=d.filePath("02"),ini=d.filePath("settings.ini");
        QVERIFY(TestData::imageFolder(first,1));QVERIFY(TestData::imageFolder(second,1));settings(ini);
        MainWindow w(ini);w.show();w.openBook(first+"/1.png");
        QTRY_VERIFY(!w.isLoading() && w.renderReady());QVERIFY(w.m_siblingDirectory.isEmpty());
        QVERIFY(w.command("nextFolder")->isEnabled());w.command("nextFolder")->trigger();
        QTRY_COMPARE(w.directory(),second);QTRY_VERIFY(w.directoryReady());
        QCOMPARE(w.viewMode(),MainWindow::ViewMode::Covers);
    }
    void openingSortRequestsCurrentImageOnce() {
        QTemporaryDir d;const auto path=d.filePath("pages.zip"),ini=d.filePath("settings.ini");
        QVERIFY(TestData::write(path,pages(8,true)));settings(ini);
        {QSettings s(ini,QSettings::IniFormat);s.setValue("covers/sort",4);s.sync();}
        MainWindow w(ini);w.show();const auto before=BookActivity::encodedReads.load();
        w.openBook(path);QTRY_VERIFY(!w.isLoading() && w.renderReady() && w.m_readerPresented);
        QCOMPARE(w.currentPageName(),QString("00000.png"));
        QCOMPARE(BookActivity::encodedReads.load()-before,quint64(1));
        QCOMPARE(w.m_pages->count(),0);
    }
    void bundleIsSavedOptionAndOnlyLoadsInReader() {
        QTemporaryDir d;const auto path=d.filePath("pages.zip"),ini=d.filePath("settings.ini");
        QVERIFY(TestData::write(path,pages()));settings(ini);
        {
            MainWindow w(ini);w.show();auto *action=w.command("preloadAll");
            QVERIFY(action->isEnabled());QVERIFY(action->isCheckable());QVERIFY(!action->isChecked());
            action->trigger();QVERIFY(action->isChecked());
            const auto before=BookActivity::opens.load();QTest::qWait(100);QCOMPARE(BookActivity::opens.load(),before);
            w.runRequest({{"path",path},{"entry",3}});
            QTRY_VERIFY(!w.isLoading() && w.pageCount()==5);QCOMPARE(w.viewMode(),MainWindow::ViewMode::Pages);
            QTest::qWait(150);QCOMPARE(w.m_book->encodedCacheInfo().bytes,qint64(0));
            const auto readerReads=BookActivity::encodedReads.load();
            w.m_pages->setCurrentRow(0);w.acceptThumbnail();
            QTRY_VERIFY(w.renderReady() && w.m_readerPresented);
            QTRY_COMPARE(w.m_book->encodedCacheInfo().entries,5);
            QCOMPARE(BookActivity::encodedReads.load()-readerReads,quint64(5));
            const auto images=BookActivity::imageRequests.load();
            QTest::qWait(150);QCOMPARE(BookActivity::imageRequests.load(),images);
            action->trigger();QCOMPARE(w.m_book->encodedCacheInfo().bytes,qint64(0));
            action->trigger();w.close();
        }
        MainWindow restored(ini);QVERIFY(restored.command("preloadAll")->isChecked());
        QVERIFY(restored.command("preloadAll")->isEnabled());
    }
    void rawBundleIsIncrementalAndBudgeted() {
        QTemporaryDir d;const auto path=d.filePath("pages.zip");QVERIFY(TestData::write(path,pages(3)));
        OpenOptions options;options.fileCacheMiB=0;
        const auto opened=Book::open(path,{},options);QVERIFY(opened.book);
        const auto book=opened.book;const auto bytes=book->pages[0].encodedSize;
        const auto images=BookActivity::imageRequests.load();
        QVERIFY(book->cacheEncoded(0,bytes).error.isEmpty());QCOMPARE(book->encodedCacheInfo().entries,1);
        QVERIFY(book->cacheEncoded(1,bytes).full);QCOMPARE(book->encodedCacheInfo().entries,1);
        QVERIFY(book->cacheEncoded(2,bytes*3,[]{return true;}).error.size()>0);
        QCOMPARE(book->encodedCacheInfo().entries,1);
        const auto reads=BookActivity::encodedReads.load();
        QVERIFY(book->encoded(0).error.isEmpty());QCOMPARE(BookActivity::encodedReads.load(),reads);
        QCOMPARE(BookActivity::imageRequests.load(),images);
        book->clearEncodedCache();QCOMPARE(book->encodedCacheInfo().bytes,qint64(0));
    }
    void disablingBundleRejectsStaleReaderRetention() {
        QTemporaryDir d;const auto path=d.filePath("pages.zip");QVERIFY(TestData::write(path,pages(2)));
        OpenOptions options;options.fileCacheMiB=0;const auto opened=Book::open(path,{},options);QVERIFY(opened.book);
        opened.book->setReaderCacheBudget(0);
        QVERIFY(!opened.book->image(0,{}, {},1024*1024).image.isNull());
        QCOMPARE(opened.book->encodedCacheInfo().entries,0);
        opened.book->setReaderCacheBudget(1024*1024);
        QVERIFY(!opened.book->image(0,{}, {},1024*1024).image.isNull());
        QCOMPARE(opened.book->encodedCacheInfo().entries,1);
        opened.book->clearBundleCache();
        QVERIFY(!opened.book->image(1,{}, {},1024*1024).image.isNull());
        QCOMPARE(opened.book->encodedCacheInfo().entries,0);
    }
    void lazySnapshotsSurviveSourceRemoval() {
        QTemporaryDir d;const auto folder=d.filePath("pages");QVERIFY(TestData::imageFolder(folder,2));
        const auto count=BookActivity::thumbnailSnapshots.load();
        const auto opened=Book::open(folder);QVERIFY(opened.book);
        QCOMPARE(BookActivity::thumbnailSnapshots.load(),count);
        const auto size=QFileInfo(folder+"/1.png").size();
        const auto resolved=QFileInfo(folder+"/1.png").canonicalFilePath();
        QVERIFY(QFile::remove(folder+"/1.png"));
        const auto source=QJsonDocument::fromJson(opened.book->thumbnailSource(0)).object();
        QCOMPARE(source.value("bytes").toString(),QString::number(size));
        QCOMPARE(source.value("resolved").toString(),resolved);
        QVERIFY(!opened.book->image(0).error.isEmpty());
    }
    void nestedAndFolderContainersDoNotReadUnshownImages() {
        QTemporaryDir d;auto inner=pages(2);
        const auto name=inner.indexOf("00001.png");QVERIFY(name>0);
        inner[name+9]=char(inner[name+9]^1); // corrupt payload, retaining valid headers and declared CRC
        const auto nested=d.filePath("nested.zip");
        QVERIFY(TestData::write(nested,TestData::zip({{"inner.zip",inner}})));
        OpenOptions options;options.nestedArchives=true;options.fileCacheMiB=0;
        auto opened=Book::open(nested,{},options);QVERIFY2(opened.book,qPrintable(opened.error));
        QCOMPARE(opened.book->pages.size(),2);QVERIFY(opened.book->pages[1].inlineData.isEmpty());
        QCOMPARE(opened.book->image(0).image,TestData::solid());
        QVERIFY(!opened.book->image(1).error.isEmpty());
        const auto folder=d.filePath("containers");QVERIFY(QDir().mkpath(folder));
        QVERIFY(TestData::write(folder+"/inner.zip",inner));options.folderContainers=true;
        opened=Book::open(folder,{},options);QVERIFY2(opened.book,qPrintable(opened.error));
        QCOMPARE(opened.book->pages.size(),2);QVERIFY(opened.book->pages[0].sourceBook);
        QCOMPARE(opened.book->image(0).image,TestData::solid());
        QVERIFY(!opened.book->image(1).error.isEmpty());
    }
    void containersShareFileCacheBudgetAndCursor() {
        QTemporaryDir d;const auto path=d.filePath("book");QVERIFY(QDir().mkpath(path));
        const QList<QColor> colors{Qt::red,Qt::green,Qt::blue};
        for(int i=0;i<colors.size();++i) {
            auto png=TestData::png(TestData::solid(colors[i]));png+=QByteArray(400*1024-png.size(),'x');
            QVERIFY(TestData::write(path+QString("/%1.zip").arg(i),TestData::zip({{"1.png",png}})));
        }
        OpenOptions options;options.folderContainers=true;options.fileCacheMiB=1;
        const auto opened=Book::open(path,{},options);QVERIFY(opened.book);
        const auto reads=BookActivity::encodedReads.load();
        for(int i=0;i<colors.size();++i)QCOMPARE(opened.book->image(i).image,TestData::solid(colors[i]));
        QCOMPARE(opened.book->encodedCacheInfo().entries,2);
        QVERIFY(opened.book->encodedCacheInfo().bytes<=1024*1024);
        QCOMPARE(opened.book->image(0).image,TestData::solid(Qt::red));
        QCOMPARE(BookActivity::encodedReads.load()-reads,quint64(4));
    }
    void nestedStorageLivesUntilItsCursorCloses() {
        QTemporaryDir d;const auto path=d.filePath("outer.zip");
        QVERIFY(TestData::write(path,TestData::zip({{"inner.zip",pages(2)}})));
        QString storage;
        {
            OpenOptions options;options.nestedArchives=true;
            const auto opened=Book::open(path,{},options);QVERIFY(opened.book);
            storage=opened.book->pages[0].sourceBook->path;QVERIFY(QFileInfo::exists(storage));
            QVERIFY(!opened.book->image(0).image.isNull());
        }
        QVERIFY(!QFileInfo::exists(storage));
    }
    void deepNestedBundleKeepsDistinctFiles() {
        QTemporaryDir d;const auto path=d.filePath("outer.zip");
        const auto red=TestData::zip({{"1.png",TestData::png(TestData::solid(Qt::red))}});
        const auto blue=TestData::zip({{"1.png",TestData::png(TestData::solid(Qt::blue))}});
        const auto middle=TestData::zip({{"a.zip",red},{"b.zip",blue}});
        QVERIFY(TestData::write(path,TestData::zip({{"middle.zip",middle}})));
        OpenOptions options;options.nestedArchives=true;options.fileCacheMiB=0;
        const auto opened=Book::open(path,{},options);QVERIFY2(opened.book,qPrintable(opened.error));
        QCOMPARE(opened.book->pages.size(),2);
        QVERIFY(opened.book->encodedFileId(0)!=opened.book->encodedFileId(1));
        QVERIFY(opened.book->cacheEncoded(0,1024*1024).error.isEmpty());
        QVERIFY(opened.book->cacheEncoded(1,1024*1024).error.isEmpty());
        QCOMPARE(opened.book->image(0).image,TestData::solid(Qt::red));
        QCOMPARE(opened.book->image(1).image,TestData::solid(Qt::blue));
    }
    void containerRefreshKeepsSameNamedEntriesSeparate() {
        QTemporaryDir d;const auto path=d.filePath("book");QVERIFY(QDir().mkpath(path));
        const auto red=TestData::zip({{"1.png",TestData::png(TestData::solid(Qt::red))}});
        QVERIFY(TestData::write(path+"/a.zip",red));QVERIFY(TestData::write(path+"/b.zip",red));
        OpenOptions options;options.folderContainers=true;
        const auto opened=Book::open(path,{},options);QVERIFY(opened.book);
        auto cache=std::make_shared<ThumbnailDiskCache>(d.filePath("cache"));cache->configure(true,16);
        QCOMPARE(cachedPageThumbnail(opened.book,1,cache).image.pixelColor(0,0),QColor(Qt::red));
        const auto blue=TestData::zip({{"1.png",TestData::png(TestData::solid(Qt::blue,{33,49}))}});
        QVERIFY(TestData::write(path+"/b.zip",blue));
        const auto observed=thumbnailEntry(QFileInfo(path+"/b.zip"),QFileInfo(path).canonicalFilePath());
        const auto result=cachedPageThumbnail(opened.book,1,cache,{},false,false,true,0,observed);
        QVERIFY2(result.error.isEmpty(),qPrintable(result.error));
        QCOMPARE(result.image.pixelColor(0,0),QColor(Qt::blue));
    }
    void oldContainerThumbnailsRemainReusable() {
        for(bool folder:{false,true}) {
            QTemporaryDir d;const auto image=TestData::png(TestData::solid());
            const auto child=TestData::zip({{"1.png",image}});OpenOptions options;
            QString path;
            if(folder){path=d.filePath("book");QVERIFY(QDir().mkpath(path));QVERIFY(TestData::write(path+"/inner.zip",child));options.folderContainers=true;}
            else {path=d.filePath("outer.zip");QVERIFY(TestData::write(path,TestData::zip({{"inner.zip",child}})));options.nestedArchives=true;}
            const auto opened=Book::open(path,{},options);QVERIFY(opened.book);const auto book=opened.book;
            const auto &page=book->pages.first();
            const auto hash=[](const QByteArray &data){return QString::fromLatin1(QCryptographicHash::hash(data,QCryptographicHash::Sha256).toHex());};
            QJsonObject key{{"revision",3},{"source",folder?QFileInfo(QString{}).absoluteFilePath():path},
                {"encoding","CP932"},{"force",false},{"width",320},{"height",480},{"kind","page"},
                {"identity",page.archiveEntryId.isEmpty()?page.identity:page.archiveEntryId},{"name",page.name},
                {"page",page.documentPage},{"pdfResource",page.pdfResource},{"portion",QJsonArray{0,0,0,0}},
                {"inline",hash(image)},{"password",hash({})}};
            auto cache=std::make_shared<ThumbnailDiskCache>(d.filePath("cache"));cache->configure(true,16);
            const auto bytes=QJsonDocument(key).toJson(QJsonDocument::Compact);
            QVERIFY(!cache->image(bytes,[]{return ImageResult(TestData::solid(Qt::blue));}).image.isNull());
            const auto before=BookActivity::imageRequests.load(),reads=BookActivity::encodedReads.load();
            QVERIFY(cachedPageThumbnail(book,0,cache,{},false,true).image.isNull());
            QCOMPARE(BookActivity::encodedReads.load(),reads);
            const auto result=cachedPageThumbnail(book,0,cache);
            QCOMPARE(result.image.pixelColor(0,0),QColor(Qt::blue));
            QCOMPARE(BookActivity::imageRequests.load(),before);
            QCOMPARE(cache->stats().entries,1);
        }
    }
    void visibleContainerMissReadsPayloadOnce() {
        for(bool folder:{false,true}) {
            QTemporaryDir d;const auto child=pages(1);OpenOptions options;options.fileCacheMiB=0;
            QString path;
            if(folder){path=d.filePath("book");QVERIFY(QDir().mkpath(path));QVERIFY(TestData::write(path+"/inner.zip",child));options.folderContainers=true;}
            else {path=d.filePath("outer.zip");QVERIFY(TestData::write(path,TestData::zip({{"inner.zip",child}})));options.nestedArchives=true;}
            const auto opened=Book::open(path,{},options);QVERIFY(opened.book);
            auto cache=std::make_shared<ThumbnailDiskCache>(d.filePath("cache"));cache->configure(true,16);
            const auto reads=BookActivity::encodedReads.load();
            const auto result=cachedPageThumbnail(opened.book,0,cache);
            QVERIFY2(result.error.isEmpty(),qPrintable(result.error));QVERIFY(!result.image.isNull());
            QCOMPARE(result.image.pixelColor(0,0),TestData::solid().pixelColor(0,0));
            QCOMPARE(BookActivity::encodedReads.load()-reads,quint64(1));
        }
    }
    void unsupportedOrBrokenNestedArchiveDoesNotBlockOtherImages() {
        QTemporaryDir d;const auto path=d.filePath("outer.zip");
        OpenOptions options;options.nestedArchives=true;
        const auto text=TestData::zip({{"text.txt","Only text"}});
        QVERIFY(TestData::write(path,TestData::zip({{"empty.zip",text},{"last.png",TestData::png(TestData::solid())}})));
        auto opened=Book::open(path,{},options);QVERIFY2(opened.book,qPrintable(opened.error));
        QCOMPARE(opened.book->pages.size(),1);QCOMPARE(opened.book->image(0).image,TestData::solid());
        QVERIFY(!opened.book->warnings.isEmpty());
        QVERIFY(TestData::write(path,TestData::zip({{"0.png",TestData::png(TestData::solid())},{"bad.zip","not an archive"}})));
        opened=Book::open(path,{},options);QVERIFY2(opened.book,qPrintable(opened.error));
        QCOMPARE(opened.book->pages.size(),2);QCOMPARE(opened.book->image(0).image,TestData::solid());
        QVERIFY(!opened.book->image(1).error.isEmpty());
    }
    void zipAttributesKeepBackendFileTypes() {
        QTemporaryDir d;
        struct Case {int host;quint32 attributes;bool image;};
        const QList<Case> cases{{0,0x40000000,true},{0,0x10,false},{3,0x40000000,false},
            {3,0x80000000,true},{19,0x80000000,true},{3,0xa0000000,false}};
        for(int i=0;i<cases.size();++i) {
            auto zip=pages(1);const auto header=zip.indexOf(QByteArray::fromHex("504b0102"));QVERIFY(header>=0);
            zip[header+5]=char(cases[i].host);
            for(int byte=0;byte<4;++byte)zip[header+38+byte]=char(cases[i].attributes>>(byte*8));
            const auto path=d.filePath(QString("%1.zip").arg(i));QVERIFY(TestData::write(path,zip));
            const auto opened=Book::open(path);
            QCOMPARE(bool(opened.book),cases[i].image);
            if(opened.book){QCOMPARE(opened.book->pages.size(),1);QVERIFY(!opened.book->image(0).image.isNull());}
        }
    }
    void unshownLocalHeaderErrorDoesNotBlockFirstPage() {
        QTemporaryDir d;auto bytes=pages(2);const auto second=bytes.indexOf("PK\3\4",4);QVERIFY(second>0);
        bytes[second]=0;const auto path=d.filePath("pages.zip");QVERIFY(TestData::write(path,bytes));
        const auto opened=Book::open(path);QVERIFY2(opened.book,qPrintable(opened.error));
        QCOMPARE(opened.book->image(0).image,TestData::solid());
        QVERIFY(!opened.book->image(1).error.isEmpty());
    }
    void unshownOversizeTiffDoesNotBlockFirstPage() {
        if(!imageExtensions().contains("tiff"))QSKIP("Qt TIFF plugin not installed");
        QTemporaryDir d;QVERIFY(TestData::write(d.filePath("1.png"),TestData::png(TestData::solid())));
        QFile file(d.filePath("2.tiff"));QVERIFY(file.open(QIODevice::WriteOnly));QVERIFY(file.resize(MaxEncodedBytes+1));file.close();
        const auto opened=Book::open(d.path());QVERIFY2(opened.book,qPrintable(opened.error));
        QCOMPARE(opened.book->image(0).image,TestData::solid());QVERIFY(!opened.book->image(1).error.isEmpty());
    }
    void animationDecodeCanBeCancelled() {
        bool recognized=false;int calls=0;
        const auto result=decodeApng(TestData::apng(),recognized,[&]{return ++calls==4;});
        QVERIFY(result.image.isNull());QVERIFY(result.frames.isEmpty());QCOMPARE(calls,4);
    }
    void deferredHistoryKeepsNewestRevisionAndFlushesOnExit() {
        QTemporaryDir d;const auto path=d.filePath("library.sqlite3");
        QString id;
        {
            LibraryStore store(path);QVERIFY(store.error().isEmpty());store.setDeferredWrites(true);
            id=store.add("history","/synthetic/book.zip");QVERIFY(!id.isEmpty());
            QVERIFY(store.update(id,{},{{"anchor",1}}));const auto first=store.pendingWrites();
            QVERIFY(store.update(id,{},{{"anchor",2}}));
            QVERIFY(LibraryStore::writePending(path,first));store.acknowledge(first);
            QCOMPARE(store.item(id).state.value("anchor").toInt(),2);QCOMPARE(store.pendingWrites().size(),1);
            QVERIFY(!LibraryStore::writePending(path,store.pendingWrites(),[]{return true;}));
            QVERIFY(!LibraryStore::writePending(d.filePath("missing/library.sqlite3"),store.pendingWrites()));
            QCOMPARE(store.pendingWrites().size(),1);
        }
        LibraryStore restored(path);QCOMPARE(restored.item(id).state.value("anchor").toInt(),2);
        restored.setDeferredWrites(true);QVERIFY(restored.update(id,{},{{"anchor",3}}));
        const auto epoch=restored.writeEpoch();const auto previous=epoch->load();
        const auto obsolete=restored.pendingWrites();
        QVERIFY(restored.remove(id));QVERIFY(epoch->load()!=previous);
        QVERIFY(!LibraryStore::writePending(path,obsolete,[epoch,previous]{return epoch->load()!=previous;}));
        QVERIFY(restored.items("history").isEmpty());
    }
    void filesystemReadCancellationStopsBetweenChunks() {
        QTemporaryDir d;const auto path=d.filePath("large.png");
        QVERIFY(TestData::write(path,QByteArray(1024*1024,'x')));
        const auto opened=Book::open(path);QVERIFY(opened.book);
        int checks=0;const auto data=opened.book->encoded(0,[&]{return ++checks>3;});
        QVERIFY(!data.error.isEmpty());QVERIFY(data.bytes.isEmpty());
        QVERIFY(checks>=4);
    }
    void importsBundleLoadSetting() {
        const auto plan=planLegacySettings("[Config]\nBundleLoad=1\n",{},{});
        QVERIFY(plan.error.isEmpty());bool found=false;
        for(const auto &change:plan.changes)if(change.values.contains("cache/bundleLoad")) {
            found=true;QVERIFY(change.values.value("cache/bundleLoad").toBool());
        }
        QVERIFY(found);
    }
};
ISOLATED_TEST_MAIN(ReaderStartupTests)
#include "reader_startup_tests.moc"
