// SPDX-License-Identifier: GPL-3.0-only
#include "test_support.h"
#include "thumbnailcache.h"
#include "displayjobs.h"
#include <latch>
#include "thumbnailsource.h"
#include "scriptreader.h"
#include "librarystore.h"
#include <QAction>
#include <QDateTime>
#include <QElapsedTimer>
#include <QProcess>
#include <QLockFile>
#include <QScrollBar>
#include <algorithm>

// Keep the filesystem-heavy scroll fixtures outside cache_tests' timeout.
class ThumbnailTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        // Native Windows/macOS cache paths ignore XDG_CACHE_HOME. A distinct
        // app name prevents cache_tests' explicit clears from racing this suite.
        QCoreApplication::setApplicationName("ThumbnailRegression");
    }
    void cachedChildDirectoryBypassesParentWork() {
        QTemporaryDir d;const auto parent=d.filePath("parent"),child=parent+"/child";
        QVERIFY(QDir().mkpath(child));
        for(int i=0;i<300;++i)QVERIFY(QDir().mkpath(parent+QString("/folder-%1").arg(i,3,10,QChar('0'))));
        auto cache=std::make_shared<ThumbnailDiskCache>();
        for(int i=0;i<4;++i) {
            const auto folder=child+QString("/book-%1").arg(i);QVERIFY(QDir().mkpath(folder));
            QVERIFY(TestData::write(folder+"/cover.png",TestData::png(TestData::solid(Qt::blue))));
            QVERIFY(!cachedCover(folder,{},cache).image.isNull());
        }
        MainWindow window(d.filePath("settings.ini"));window.show();window.openDirectory(parent);
        QTRY_VERIFY(window.directoryReady());
        auto *grid=window.findChild<ThumbnailGrid *>("zipLibrary");QVERIFY(grid);QCOMPARE(grid->count(),301);
        // Hold all source/persistence workers, including cooperative cancellation
        // completion. Navigation and cached pixels must not need any of them.
        grid->hide();window.requestThumbnails(grid);
        window.m_displayJobs->retain("covers");window.m_displayJobs->retain("directory-siblings");
        std::latch release(1);std::atomic<int> entered=0;
        auto cleanup=qScopeGuard([&]{release.count_down();});
        for(auto lane:{DisplayJobs::Lane::Background,DisplayJobs::Lane::Update,DisplayJobs::Lane::Store,DisplayJobs::Lane::Display}) {
            const auto id=QString("held/%1").arg(int(lane));
            const auto expected=entered.load()+1;
            window.m_displayJobs->submit<int>(lane,"held",id,[&](const auto &){++entered;release.wait();return 0;},[](int,bool){});
            QTRY_COMPARE_WITH_TIMEOUT(entered.load(),expected,3000);
        }
        QTRY_COMPARE_WITH_TIMEOUT(entered.load(),4,3000);
        grid->show();window.openDirectory(child);
        QTRY_VERIFY_WITH_TIMEOUT(window.directoryReady() && grid->count()==4,1000);
        for(int row=0;row<4;++row) {
            QTRY_VERIFY_WITH_TIMEOUT(grid->item(row)->data(ThumbnailRoleReady).toBool(),1000);
            const auto image=grid->item(row)->icon().pixmap(64,64).toImage();
            QCOMPARE(image.pixelColor(image.rect().center()),QColor(Qt::blue));
        }
        release.count_down();cleanup.dismiss();window.close();
    }
    void readerCacheBypassesOldThumbnailGeneration() {
        QTemporaryDir d;const auto path=d.filePath("page.png");
        QVERIFY(TestData::write(path,TestData::png(TestData::solid(Qt::blue))));
        MainWindow window(d.filePath("settings.ini"));window.show();window.openBook(path);
        QTRY_VERIFY(window.renderReady());
        std::latch release(1);std::atomic<bool> entered=false;auto cleanup=qScopeGuard([&]{release.count_down();});
        window.m_displayJobs->submit<int>(DisplayJobs::Lane::Display,"old-thumbnail","held",[&](const auto &){entered=true;release.wait();return 0;},[](int,bool){});
        QTRY_VERIFY(entered.load());window.render();
        QTRY_VERIFY_WITH_TIMEOUT(window.renderReady(),1000);
        release.count_down();cleanup.dismiss();window.close();
    }
    void cachedPixelsBypassExistingValidation() {
        QTemporaryDir d;const auto books=d.filePath("books"),path=books+"/cover.png";
        QVERIFY(QDir().mkpath(books));QVERIFY(TestData::write(path,TestData::png(TestData::solid(Qt::blue))));
        auto cache=std::make_shared<ThumbnailDiskCache>();QVERIFY(!cachedCover(path,{},cache).image.isNull());
        MainWindow window(d.filePath("settings.ini"));window.show();window.openDirectory(books);
        auto *grid=window.findChild<ThumbnailGrid *>("zipLibrary");QVERIFY(grid);
        QTRY_VERIFY(window.directoryReady() && grid->count()==1 && grid->item(0)->data(ThumbnailRoleReady).toBool());
        window.m_displayJobs->retain("covers");
        std::latch release(1);std::atomic<bool> entered=false;auto cleanup=qScopeGuard([&]{release.count_down();});
        const auto id=QString("covers/%1/0/validate").arg(window.m_libraryGeneration);
        window.m_displayJobs->submit<int>(DisplayJobs::Lane::Update,"covers",id,[&](const auto &){entered=true;release.wait();return 0;},[](int,bool){});
        QTRY_VERIFY(entered.load());
        // Equivalent to an evicted row becoming visible while its old validation
        // is still running. Keep the real request id to exercise deduplication.
        grid->item(0)->setData(ThumbnailRoleReady,false);grid->item(0)->setData(ThumbnailNextCheckRole,0);
        grid->item(0)->setIcon(grid->placeholderIcon());window.requestThumbnails(grid);
        QTRY_VERIFY_WITH_TIMEOUT(grid->item(0)->data(ThumbnailRoleReady).toBool(),1000);
        const auto image=grid->item(0)->icon().pixmap(64,64).toImage();QCOMPARE(image.pixelColor(image.rect().center()),QColor(Qt::blue));
        release.count_down();cleanup.dismiss();window.close();
    }
    void emptyFolderInvalidatesPersistentPixels_data() {
        QTest::addColumn<bool>("deferred");QTest::newRow("committed")<<false;QTest::newRow("journal")<<true;
    }
    void emptyFolderInvalidatesPersistentPixels() {
        QFETCH(bool,deferred);
        QTemporaryDir d;const auto folder=d.filePath("folder"),source=folder+"/a.png";
        QVERIFY(QDir().mkpath(folder));QVERIFY(TestData::write(source,TestData::png(TestData::solid(Qt::red))));
        auto cache=std::make_shared<ThumbnailDiskCache>(d.filePath("cache"));cache->setDeferredWrites(deferred);
        QVERIFY(!cachedCover(folder,{},cache).image.isNull());if(deferred)QVERIFY(cache->checkpoint());
        QVERIFY(QFile::remove(source));QVERIFY(cachedCover(folder,{},cache).image.isNull());
        QVERIFY(cachedCover(folder,{},cache,{},false,0,true).image.isNull());
        auto restart=std::make_shared<ThumbnailDiskCache>(d.filePath("cache"));restart->recoverPending();
        while(restart->hasPendingWrites())QVERIFY(restart->flushPending());
        QVERIFY(cachedCover(folder,{},restart,{},false,0,true).image.isNull());
    }
    void visibleNestedChangesAndErrorsRecoverWithoutRequests() {
        QTemporaryDir d;const auto books=d.filePath("books"),folder=books+"/folder",source=folder+"/a.png";
        QVERIFY(QDir().mkpath(folder));QVERIFY(TestData::write(source,TestData::png(TestData::solid(Qt::red))));
        MainWindow window(d.filePath("settings.ini"));window.show();window.openDirectory(books);
        auto *grid=window.findChild<ThumbnailGrid *>("zipLibrary");QVERIFY(grid);
        QTRY_VERIFY(window.directoryReady() && grid->count()==1 && grid->item(0)->data(ThumbnailCoverValidatedRole).toBool());
        const auto color=[&]{auto image=grid->item(0)->icon().pixmap(64,64).toImage();return image.pixelColor(image.rect().center());};
        QCOMPARE(color(),QColor(Qt::red));
        QVERIFY(TestData::write(source,TestData::png(TestData::solid(Qt::green,{39,61}))));
        QTRY_COMPARE_WITH_TIMEOUT(color(),QColor(Qt::green),6000);
        QVERIFY(TestData::write(source,"temporarily broken image"));QTest::qWait(2200);
        QCOMPARE(color(),QColor(Qt::green));
        QVERIFY(TestData::write(source,TestData::png(TestData::solid(Qt::blue,{43,67}))));
        QTRY_COMPARE_WITH_TIMEOUT(color(),QColor(Qt::blue),6000);
        QVERIFY(QFile::remove(source));
        QTRY_COMPARE_WITH_TIMEOUT(grid->item(0)->icon().pixmap(64,64).toImage(),grid->placeholderIcon(true).pixmap(64,64).toImage(),6000);
        window.close();
        auto cache=std::make_shared<ThumbnailDiskCache>();QVERIFY(cachedCover(folder,{},cache,{},false,0,true).image.isNull());
    }
    void pageLookupUsesOpenedSnapshotThenRefreshes() {
        QTemporaryDir d;const auto path=d.filePath("book.zip");
        QVERIFY(TestData::write(path,TestData::zip({{"a.png",TestData::png(TestData::solid(Qt::red))}})));
        const auto book=Book::open(path).book;QVERIFY(book);
        auto cache=std::make_shared<ThumbnailDiskCache>(d.filePath("cache"));
        QCOMPARE(cachedPageThumbnail(book,0,cache).image.pixelColor(0,0),QColor(Qt::red));
        QVERIFY(QFile::remove(path));
        // Cache-only is intentionally speculative and must not consult the source.
        QCOMPARE(cachedPageThumbnail(book,0,cache,{},false,true).image.pixelColor(0,0),QColor(Qt::red));
        QVERIFY(TestData::write(path,TestData::zip({{"a.png",TestData::png(TestData::solid(Qt::blue,{39,61}))}})));
        QCOMPARE(cachedPageThumbnail(book,0,cache).image.pixelColor(0,0),QColor(Qt::blue));
    }
    void closingPreservesGeneratedPixelsWhileWriterLocked() {
        QTemporaryDir d;const auto books=d.filePath("books"),source=books+"/a.png";
        QVERIFY(QDir().mkpath(books));QVERIFY(TestData::write(source,TestData::png(TestData::solid(Qt::blue))));
        const auto root=ThumbnailDiskCache::defaultDirectory();QVERIFY(QDir().mkpath(root));
        QLockFile lock(root+"/.lock");QVERIFY(lock.tryLock(0));
        MainWindow window(d.filePath("settings.ini"));window.show();window.openDirectory(books);
        auto *grid=window.findChild<ThumbnailGrid *>("zipLibrary");QVERIFY(grid);
        QTRY_VERIFY(window.directoryReady() && grid->count()==1 && grid->item(0)->data(ThumbnailRoleReady).toBool());
        window.close();lock.unlock();
        QProcess process;process.start(QCoreApplication::applicationFilePath(),{"--reuse-probe","cover",source,root});
        QVERIFY(process.waitForFinished(15000));QCOMPARE(process.exitCode(),0);
        ThumbnailDiskCache restarted(root);restarted.recoverPending();
        while(restarted.hasPendingWrites())QVERIFY(restarted.flushPending());
        QVERIFY(QDir(root+"/pending").entryList({"*.png"},QDir::Files).isEmpty());
    }
    void pageGridPaintsOldCacheThenUpdatesWithoutInput() {
        QTemporaryDir d;const auto path=d.filePath("book.zip");
        QVERIFY(TestData::write(path,TestData::zip({{"a.png",TestData::png(TestData::solid(Qt::red))}})));
        auto cache=std::make_shared<ThumbnailDiskCache>();const auto old=Book::open(path).book;QVERIFY(old);
        QVERIFY(!cachedPageThumbnail(old,0,cache).image.isNull());
        QVERIFY(TestData::write(path,TestData::zip({{"a.png",TestData::png(TestData::solid(Qt::blue,{39,61}))}})));
        MainWindow window(d.filePath("settings.ini"));window.show();
        window.openBook(path);QTRY_VERIFY(window.renderReady());window.showPageThumbnails();
        auto *grid=window.findChild<ThumbnailGrid *>("pageThumbnails");QVERIFY(grid);
        bool sawOld=false;
        const auto color=[&]{if(!grid->count())return QColor{};auto image=grid->item(0)->icon().pixmap(64,64).toImage();return image.pixelColor(image.rect().center());};
        // Observe model delivery directly, so a fast Update worker cannot hide
        // the fact that the initial cache result really was painted first.
        connect(grid->model(),&QAbstractItemModel::dataChanged,&window,[&]{if(color()==QColor(Qt::red))sawOld=true;});
        QTRY_COMPARE_WITH_TIMEOUT(color(),QColor(Qt::blue),6000);QVERIFY(sawOld);
        QVERIFY(TestData::write(path,TestData::zip({{"a.png",TestData::png(TestData::solid(Qt::green,{43,67}))}})));
        QTRY_COMPARE_WITH_TIMEOUT(color(),QColor(Qt::green),6000);
        window.close();
        const auto fresh=Book::open(path).book;QVERIFY(fresh);
        auto restarted=std::make_shared<ThumbnailDiskCache>();
        QCOMPARE(cachedPageThumbnail(fresh,0,restarted,{},false,true).image.pixelColor(0,0),QColor(Qt::green));
    }
    void collectionWarmHitBypassesSlowMiss() {
        QTemporaryDir d;const auto scripts=d.filePath("scripts");QVERIFY(QDir().mkpath(scripts));
        QVERIFY(TestData::write(scripts+"/manifest.json",R"({"apiVersion":1,"kind":"reader","id":"collection-priority","extensions":["lag"],"magic":"4c414747","script":"decode.js"})"));
        QVERIFY(TestData::write(scripts+"/decode.js","function decode(){while(true){}}"));
        QVERIFY(configureScriptReaders({scripts}).isEmpty());const auto restore=qScopeGuard([]{configureScriptReaders({});});
        const auto slow=d.filePath("slow.lag"),warm=d.filePath("warm.png");
        QVERIFY(TestData::write(slow,"LAGG source"));QVERIFY(TestData::write(warm,TestData::png(TestData::solid(Qt::blue))));
        auto cache=std::make_shared<ThumbnailDiskCache>();QVERIFY(!cachedCover(warm,{},cache).image.isNull());
        QString id;
        {LibraryStore library(d.filePath("library.sqlite3"));QVERIFY(!library.add("playlist",slow).isEmpty());id=library.add("playlist",warm);QVERIFY(!id.isEmpty());}
        MainWindow window(d.filePath("settings.ini"));window.show();window.command("bookLists")->trigger();
        auto *list=window.findChild<QListWidget *>("collection_playlist");QVERIFY(list);QTRY_VERIFY(list->isVisible());
        const auto blue=[&]{for(int i=0;i<list->count();++i)if(list->item(i)->data(Qt::UserRole).toString()==id){auto image=list->item(i)->icon().pixmap(64,64).toImage();return !image.isNull() && image.pixelColor(image.rect().center())==QColor(Qt::blue);}return false;};
        QTRY_VERIFY_WITH_TIMEOUT(blue(),2000);window.close();
    }
    void unchangedSourcesSurviveRestart_data() {
        QTest::addColumn<QString>("kind");QTest::addColumn<bool>("deferred");
        for(const auto &kind:{"folder-image","folder-zip","nested-image","nested-zip","zip-cover","zip-page"})
            for(bool deferred:{false,true})
                QTest::newRow(qPrintable(QString(kind)+(deferred?"-deferred":"-sync")))<<QString(kind)<<deferred;
    }
    void unchangedSourcesSurviveRestart() {
        QFETCH(QString,kind);QFETCH(bool,deferred);
        QTemporaryDir d;QVERIFY(d.isValid());const auto folder=d.filePath("books");
        const auto content=kind.startsWith("nested")?folder+"/child/grandchild":folder;
        QVERIFY(QDir().mkpath(content));
        const auto png=TestData::png(TestData::solid(Qt::red));
        const bool zip=!kind.endsWith("image");const auto file=content+(zip?"/book.zip":"/page.png");
        QByteArray sourceBytes=png;if(zip)sourceBytes=TestData::zip({{"1.png",png}});
        QVERIFY(TestData::write(file,sourceBytes));
        const auto path=kind.startsWith("zip-")?file:folder;
        const bool page=kind=="zip-page";
        const auto cachePath=d.filePath("cache");
        auto cache=std::make_shared<ThumbnailDiskCache>(cachePath);cache->setDeferredWrites(deferred);
        const auto book=page?Book::open(path).book:std::shared_ptr<Book>{};
        if(page)QVERIFY(book);
        const auto warm=page?cachedPageThumbnail(book,0,cache):cachedCover(path,{},cache);
        QCOMPARE(warm.image.pixelColor(0,0),QColor(Qt::red));
        while(cache->hasPendingWrites())QVERIFY(cache->flushPending());
        // Marker pixels distinguish actual disk reuse from a successful decode
        // of the unchanged red source. Preserve the private freshness stamp.
        QMap<QString,QByteArray> saved;
        for(const auto &name:QDir(cachePath).entryList({"*.png"},QDir::Files)) {
            const auto destination=cachePath+'/'+name;const QImage old(destination);
            auto marker=TestData::solid(Qt::blue);
            marker.setText("komamimic-cover-stamp",old.text("komamimic-cover-stamp"));
            const auto bytes=TestData::png(marker);QVERIFY(TestData::write(destination,bytes));saved[name]=bytes;
        }
        QVERIFY(!saved.isEmpty());cache.reset();
        QProcess process;process.start(QCoreApplication::applicationFilePath(),{"--reuse-probe",page?"page":"cover",path,cachePath});
        QVERIFY2(process.waitForFinished(30000),qPrintable(process.errorString()));
        QCOMPARE(process.exitStatus(),QProcess::NormalExit);
        QVERIFY2(process.exitCode()==0,qPrintable(QString("probe exit=%1: %2").arg(process.exitCode()).arg(QString::fromUtf8(process.readAllStandardError()))));
        // Validation must not replace/re-encode an unchanged cached cover.
        QCOMPARE(QDir(cachePath).entryList({"*.png"},QDir::Files).size(),saved.size());
        for(auto it=saved.cbegin();it!=saved.cend();++it) {
            QFile disk(cachePath+'/'+it.key());QVERIFY(disk.open(QIODevice::ReadOnly));QCOMPARE(disk.readAll(),it.value());
        }
    }
    void folderCacheSurvivesLeafEviction() {
        QTemporaryDir d;QVERIFY(d.isValid());const auto folder=d.filePath("books");
        QVERIFY(QDir().mkpath(folder));
        const auto source=folder+"/book.zip";
        QVERIFY(TestData::write(source,TestData::zip({{"1.png",TestData::png(TestData::solid(Qt::red))}})));
        const auto root=d.filePath("cache");auto cache=std::make_shared<ThumbnailDiskCache>(root);
        QVERIFY(!cachedCover(source,{},cache).image.isNull());
        const auto leaves=QDir(root).entryList({"*.png"},QDir::Files);
        QVERIFY(!cachedCover(folder,{},cache).image.isNull());QString folderPng;
        for(const auto &name:QDir(root).entryList({"*.png"},QDir::Files))if(!leaves.contains(name))folderPng=root+'/'+name;
        QVERIFY(!folderPng.isEmpty());
        auto blue=TestData::solid(Qt::blue);blue.setText("komamimic-cover-stamp",QImage(folderPng).text("komamimic-cover-stamp"));
        const auto bytes=TestData::png(blue);QVERIFY(TestData::write(folderPng,bytes));
        for(const auto &name:leaves)QVERIFY(QFile::remove(root+'/'+name));
        cache=std::make_shared<ThumbnailDiskCache>(root);
        QCOMPARE(cachedCover(folder,{},cache).image.pixelColor(0,0),QColor(Qt::blue));
        QCOMPARE(cache->stats().entries,1); // Do not decode/re-save the missing leaf.
        QFile saved(folderPng);QVERIFY(saved.open(QIODevice::ReadOnly));QCOMPARE(saved.readAll(),bytes);saved.close();
        QVERIFY(QDir().rename(folder,folder+"-offline"));
        QCOMPARE(cachedCover(folder,{},cache,{},false,0,true).image.pixelColor(0,0),QColor(Qt::blue));
        QVERIFY(QDir().rename(folder+"-offline",folder));
        // Explicit refresh must still replace both marker and leaf from source.
        QCOMPARE(cachedCover(folder,{},cache,{},true).image.pixelColor(0,0),QColor(Qt::red));
        QCOMPARE(cache->stats().entries,2);
        QVERIFY(TestData::write(folderPng,"incomplete PNG"));
        QVERIFY(cachedCover(folder,{},cache,{},false,0,true).image.isNull());
        QCOMPARE(cachedCover(folder,{},cache).image.pixelColor(0,0),QColor(Qt::red));
        QCOMPARE(cachedCover(folder,{},cache,{},false,0,true).image.pixelColor(0,0),QColor(Qt::red));
    }
    void folderCacheValidatesNestedChanges_data() {
        QTest::addColumn<bool>("zip");QTest::addColumn<bool>("deferred");
        QTest::newRow("image-sync")<<false<<false;QTest::newRow("zip-sync")<<true<<false;
        QTest::newRow("image-deferred")<<false<<true;QTest::newRow("zip-deferred")<<true<<true;
    }
    void folderCacheValidatesNestedChanges() {
        QFETCH(bool,zip);QFETCH(bool,deferred);
        QTemporaryDir d;QVERIFY(d.isValid());const auto folder=d.filePath("books"),nested=folder+"/a/b";
        QVERIFY(QDir().mkpath(nested));
        const auto suffix=zip?QString(".zip"):QString(".png");const auto path=nested+"/20"+suffix;
        const auto encode=[&](QColor color,QSize size) {
            const auto png=TestData::png(TestData::solid(color,size));
            if(zip)return TestData::zip({{"1.png",png}});
            return png;
        };
        QVERIFY(TestData::write(path,encode(Qt::red,{32,48})));
        auto cache=std::make_shared<ThumbnailDiskCache>(d.filePath("cache"));cache->setDeferredWrites(deferred);
        QCOMPARE(cachedCover(folder,{},cache).image.pixelColor(0,0),QColor(Qt::red));
        while(cache->hasPendingWrites())QVERIFY(cache->flushPending());
        const auto modified=QFileInfo(folder).lastModified();
        QVERIFY(TestData::write(path,encode(Qt::green,{39,61})));
        QCOMPARE(QFileInfo(folder).lastModified(),modified);
        QCOMPARE(cachedCover(folder,{},cache,{},false,0,true).image.pixelColor(0,0),QColor(Qt::red));
        QCOMPARE(cachedCover(folder,{},cache).image.pixelColor(0,0),QColor(Qt::green));
        // Earlier natural-order candidates and deletion/fallback must be seen
        // even when the top folder's own timestamp has not changed.
        const auto earlier=nested+"/2"+suffix;
        QVERIFY(TestData::write(earlier,encode(Qt::blue,{32,48})));
        QCOMPARE(cachedCover(folder,{},cache).image.pixelColor(0,0),QColor(Qt::blue));
        QVERIFY(QFile::remove(earlier));
        QCOMPARE(cachedCover(folder,{},cache).image.pixelColor(0,0),QColor(Qt::green));
        QCOMPARE(QFileInfo(folder).lastModified(),modified);
        while(cache->hasPendingWrites())QVERIFY(cache->flushPending());
        auto restarted=std::make_shared<ThumbnailDiskCache>(cache->directory());
        QCOMPARE(cachedCover(folder,{},restarted,{},false,0,true).image.pixelColor(0,0),QColor(Qt::green));
    }
    void changedFolderSourceIsNotStoredLater() {
        QTemporaryDir d;QVERIFY(d.isValid());const auto folder=d.filePath("books");QVERIFY(QDir().mkpath(folder));
        const auto path=folder+"/page.png";QVERIFY(TestData::write(path,TestData::png(TestData::solid(Qt::red))));
        auto cache=std::make_shared<ThumbnailDiskCache>(d.filePath("cache"));cache->setDeferredWrites(true);
        QVERIFY(!cachedCover(folder,{},cache).image.isNull());QVERIFY(cache->hasPendingWrites());
        QVERIFY(TestData::write(path,TestData::png(TestData::solid(Qt::green,{39,61}))));
        while(cache->hasPendingWrites())QVERIFY(cache->flushPending());
        QCOMPARE(cache->stats().entries,0);
        QVERIFY(!cachedCover(folder,{},cache).image.isNull());QVERIFY(cache->clear().isEmpty());
        QVERIFY(!cache->hasPendingWrites());QVERIFY(cachedCover(folder,{},cache,{},false,0,true).image.isNull());
    }
    void clearDuringFolderSearchDoesNotRepopulateCache() {
        QTemporaryDir d;QVERIFY(d.isValid());const auto folder=d.filePath("books");QVERIFY(QDir().mkpath(folder+"/child"));
        QVERIFY(TestData::write(folder+"/child/page.png",TestData::png(TestData::solid(Qt::red))));
        int exercised=0;
        for(int checkpoint=1;checkpoint<=64;++checkpoint) {
            auto cache=std::make_shared<ThumbnailDiskCache>(d.filePath("cache"));cache->setDeferredWrites(true);
            int checks=0;bool cleared=false;
            cachedCover(folder,{},cache,[&]{if(++checks==checkpoint){cache->clear();cleared=true;}return false;});
            if(cleared){++exercised;QVERIFY(!cache->hasPendingWrites());QCOMPARE(cache->stats().entries,0);}
        }
        QVERIFY(exercised>10);
    }
    void textFolderPreviewsStayUncached() {
        QTemporaryDir d;QVERIFY(d.isValid());const auto folder=d.filePath("books");QVERIFY(QDir().mkpath(folder));
        auto cache=std::make_shared<ThumbnailDiskCache>(d.filePath("cache"));cache->setDeferredWrites(true);
        for(const auto &name:{"page.txt","page.md","book.zip"}) {
            const auto path=folder+'/'+name;
            QByteArray bytes="# Preview\nText body\n";
            if(QString(name).endsWith(".zip"))bytes=TestData::zip({{"page.md",bytes}});
            QVERIFY(TestData::write(path,bytes));
            QVERIFY(!cachedCover(folder,{},cache).image.isNull());
            QVERIFY(!cache->hasPendingWrites());QCOMPARE(cache->stats().entries,0);
            QVERIFY(cachedCover(folder,{},cache,{},false,0,true).image.isNull());
            QVERIFY(QFile::remove(path));
        }
    }
    void folderCacheUpdatesAfterBackgroundValidation() {
        QTemporaryDir d;QVERIFY(d.isValid());const auto books=d.filePath("books"),folder=books+"/folder",source=folder+"/book.zip";
        QVERIFY(QDir().mkpath(folder));
        QVERIFY(TestData::write(source,TestData::zip({{"1.png",TestData::png(TestData::solid(Qt::red))}})));
        auto cache=std::make_shared<ThumbnailDiskCache>();
        QVERIFY(!cachedCover(folder,{},cache).image.isNull());
        QVERIFY(TestData::write(source,TestData::zip({{"1.png",TestData::png(TestData::solid(Qt::green,{39,61}))}})));
        MainWindow window(d.filePath("settings.ini"));window.show();window.openDirectory(books);
        QTRY_VERIFY(window.directoryReady());auto *grid=window.findChild<ThumbnailGrid *>("zipLibrary");QVERIFY(grid);QCOMPARE(grid->count(),1);
        const auto color=[&]{const auto image=grid->item(0)->icon().pixmap(64,64).toImage();return image.pixelColor(image.rect().center());};
        QTRY_COMPARE(color(),QColor(Qt::green));QVERIFY(grid->item(0)->data(ThumbnailCoverValidatedRole).toBool());
        window.command("thumbRefresh")->trigger();QTRY_VERIFY(window.directoryReady());
        QTRY_VERIFY(grid->item(0)->data(ThumbnailRoleReady).toBool());QCOMPARE(color(),QColor(Qt::green));
        QVERIFY(QFile::remove(source));window.openDirectory(books);QTRY_VERIFY(window.directoryReady());
        QTRY_VERIFY(grid->item(0)->data(ThumbnailCoverValidatedRole).toBool());
        QCOMPARE(grid->item(0)->icon().pixmap(64,64).toImage(),grid->placeholderIcon(true).pixmap(64,64).toImage());
        window.close();
    }
    void folderCacheDisplaysWhileGenerationIsBusy() {
        QTemporaryDir d;QVERIFY(d.isValid());const auto books=d.filePath("books"),scripts=d.filePath("scripts");
        QVERIFY(QDir().mkpath(books+"/00-slow"));QVERIFY(QDir().mkpath(scripts));
        QVERIFY(TestData::write(scripts+"/manifest.json",R"({"apiVersion":1,"kind":"reader","id":"slow-folder-cover","extensions":["lag"],"magic":"4c414747","script":"decode.js"})"));
        QVERIFY(TestData::write(scripts+"/decode.js","function decode(){while(true){}}"));
        QVERIFY(configureScriptReaders({scripts}).isEmpty());
        const auto restore=qScopeGuard([]{configureScriptReaders({});});
        QVERIFY(TestData::write(books+"/00-slow/cover.lag","LAGG first"));
        auto cache=std::make_shared<ThumbnailDiskCache>();
        for(int i=1;i<=20;++i) {
            const auto folder=books+QString("/%1").arg(i,2,10,QChar('0'));QVERIFY(QDir().mkpath(folder+"/child"));
            QVERIFY(TestData::write(folder+"/child/book.zip",TestData::zip({{"1.png",TestData::png(TestData::solid(Qt::blue))}})));
            QVERIFY(!cachedCover(folder,{},cache).image.isNull());
        }
        MainWindow window(d.filePath("settings.ini"));window.resize(1000,700);window.show();window.openDirectory(books);
        QTRY_VERIFY(window.directoryReady());auto *grid=window.findChild<ThumbnailGrid *>("zipLibrary");QVERIFY(grid);
        QCOMPARE(grid->count(),21);grid->doItemsLayout();const auto rows=grid->visibleRows();QVERIFY(rows.size()>5);QVERIFY(rows.contains(0));
        const auto ready=[&]{for(int row:rows)if(row && !grid->item(row)->data(ThumbnailRoleReady).toBool())return false;return true;};
        QTRY_VERIFY_WITH_TIMEOUT(ready(),2000);
        QVERIFY(!grid->item(0)->data(ThumbnailRoleReady).toBool());
        for(int row:rows)if(row) {
            const auto pixels=grid->item(row)->icon().pixmap(64,64).toImage();QCOMPARE(pixels.pixelColor(pixels.rect().center()),QColor(Qt::blue));
        }
        window.close();QTRY_VERIFY_WITH_TIMEOUT(!window.backgroundWorkActive(),1500);
    }
    void thumbnailEvictionPreservesVisibleImages_data() {
        QTest::addColumn<int>("layout");QTest::addColumn<bool>("rightOrigin");QTest::addColumn<bool>("dense");
        QTest::newRow("custom") << int(ThumbnailGrid::Layout::Custom) << false << false;
        QTest::newRow("right-origin") << int(ThumbnailGrid::Layout::Custom) << true << false;
        QTest::newRow("eight-columns") << int(ThumbnailGrid::Layout::EightColumns) << false << false;
        QTest::newRow("more-than-160-visible") << int(ThumbnailGrid::Layout::Custom) << false << true;
    }
    void thumbnailEvictionPreservesVisibleImages() {
        QFETCH(int,layout);QFETCH(bool,rightOrigin);QFETCH(bool,dense);
        ThumbnailGrid grid;grid.resize(400,280);
        auto settings=grid.customSettings();settings.rightOrigin=rightOrigin;
        if(dense){settings.width=16;settings.height=16;settings.filenameLines=0;}
        grid.setCustomSettings(settings);grid.setLayoutMode(ThumbnailGrid::Layout(layout));
        for(int row=0;row<600;++row)grid.addItem(new QListWidgetItem(grid.placeholderIcon(),QString::number(row)));
        grid.show();grid.doItemsLayout();QTest::qWait(50);
        const auto visible=grid.visibleRows();QVERIFY(visible.size()>1);
        if(dense)QVERIFY(visible.size()>160);
        const int keep=std::max(160,int(visible.size())+20);QVERIFY(keep<grid.count());
        const int newcomer=visible[1];const auto image=TestData::solid(Qt::blue);
        int inserted=0;
        for(int row:visible)if(row!=newcomer){grid.setThumbnail(row,image,{});++inserted;}
        for(int row=0;row<grid.count() && inserted<keep;++row)
            if(!visible.contains(row)){grid.setThumbnail(row,image,{});++inserted;}
        QCOMPARE(inserted,keep);
        // Completing this missing visible image must evict an offscreen image,
        // even though the oldest displayed images are all still on screen.
        grid.setThumbnail(newcomer,image,{});
        const auto readyCount=[&] {
            int ready=0;for(int row=0;row<grid.count();++row)ready+=grid.item(row)->data(ThumbnailRoleReady).toBool();return ready;
        };
        for(int row:visible)QVERIFY(grid.item(row)->data(ThumbnailRoleReady).toBool());
        QCOMPARE(readyCount(),keep);
        // Late offscreen completions must keep both the bound and visible pixels.
        for(int row=0;row<grid.count();++row)if(!visible.contains(row)) {
            grid.setThumbnail(row,image,{});QVERIFY(readyCount()<=keep);
        }
        QCOMPARE(readyCount(),keep);
        for(int row:visible) {
            QVERIFY(grid.item(row)->data(ThumbnailRoleReady).toBool());
            const auto pixels=grid.item(row)->icon().pixmap(64,64).toImage();
            QCOMPARE(pixels.pixelColor(pixels.rect().center()),QColor(Qt::blue));
        }
    }
    void cachedThumbnailsSurviveScrollReturn_data() {
        QTest::addColumn<bool>("pages");
        QTest::newRow("folder-covers") << false;
        QTest::newRow("zip-pages") << true;
    }
    void cachedThumbnailsSurviveScrollReturn() {
        QFETCH(bool,pages);
        QElapsedTimer elapsed;elapsed.start();
        const auto checkpoint=[&](const char *stage) {
            qInfo()<<"thumbnail-scroll"<<(pages?"zip-pages":"folder-covers")<<stage<<elapsed.elapsed()<<"ms";
        };
        checkpoint("start");
        QTemporaryDir d;QVERIFY(d.isValid());const auto books=d.filePath("books");QVERIFY(QDir().mkpath(books));
        const auto pixels=TestData::png(TestData::solid(Qt::blue));QString zipPath;
        std::shared_ptr<const Book> book;
        if(pages) {
            QVector<TestData::ZipEntry> entries;
            for(int row=0;row<220;++row)entries.append({QString("%1.png").arg(row,4,10,QChar('0')).toUtf8(),pixels});
            zipPath=books+"/pages.zip";QVERIFY(TestData::write(zipPath,TestData::zip(entries)));
            book=Book::open(zipPath).book;QVERIFY(book);
        } else for(int row=0;row<220;++row) {
            const auto folder=books+QString("/%1").arg(row,4,10,QChar('0'));QVERIFY(QDir().mkpath(folder));
            QVERIFY(TestData::write(folder+"/page.png",pixels));
        }
        checkpoint("fixtures-ready");
        MainWindow window(d.filePath("settings.ini"));window.resize(680,400);window.show();window.openDirectory(books);
        QTRY_VERIFY(window.directoryReady());
        if(pages){window.showPageThumbnails();QTRY_VERIFY(!window.isLoading() && window.pageCount()==220);}
        auto *grid=window.findChild<ThumbnailGrid *>(pages?"pageThumbnails":"zipLibrary");QVERIFY(grid);QCOMPARE(grid->count(),220);
        grid->doItemsLayout();checkpoint("book-ready");
        const auto allVisibleReady=[&] {
            const auto rows=grid->visibleRows();if(rows.isEmpty())return false;
            for(int row:rows)
                if(!grid->item(row)->data(ThumbnailRoleReady).toBool())return false;
            return true;
        };
        QTRY_VERIFY(allVisibleReady());
        for(int row=0;row<=185;row+=5) {
            grid->scrollToItem(grid->item(row),QAbstractItemView::PositionAtTop);grid->requestVisible();
            QTRY_VERIFY(allVisibleReady());
        }
        checkpoint("outward-scroll-ready");
        QTest::qWait(200);
        int frontier=0;while(frontier<grid->count() && !grid->item(frontier)->data(ThumbnailRoleReady).toBool())++frontier;
        QVERIFY(frontier>=3 && frontier<grid->count());
        // Every previously displayed cover must be persisted by the application.
        auto disk=std::make_shared<ThumbnailDiskCache>();
        for(int row=frontier;row<std::min(grid->count(),frontier+int(grid->visibleRows().size()));++row) {
            const auto path=pages?QString{}:grid->item(row)->data(PathRole).toString();
            const auto persisted=[&] {
                const auto hit=pages?cachedPageThumbnail(book,row,disk,{},false,true):cachedCover(path,{},disk,{},false,0,true);
                return !hit.image.isNull();
            };
            QTRY_VERIFY_WITH_TIMEOUT(persisted(),10000);
        }
        checkpoint("cache-prepared");
        // The normal scroll event must suffice: no extra request or F6 after
        // the returning images complete and trigger bounded icon eviction.
        grid->scrollToItem(grid->item(frontier-3),QAbstractItemView::PositionAtTop);
        QTRY_VERIFY_WITH_TIMEOUT(allVisibleReady(),5000);QTest::qWait(200);QVERIFY(allVisibleReady());
        for(int row:grid->visibleRows()) {
            const auto image=grid->item(row)->icon().pixmap(64,64).toImage();
            QCOMPARE(image.pixelColor(image.rect().center()),QColor(Qt::blue));
        }
        checkpoint("return-verified");
        window.close();checkpoint("closed");
    }
};
int main(int argc,char **argv) {
    if(argc==5 && QByteArray(argv[1])=="--reuse-probe") {
        QCoreApplication app(argc,argv);const auto args=app.arguments();
        auto cache=std::make_shared<ThumbnailDiskCache>(args[4]);
        const bool page=args[2]=="page";const auto book=page?Book::open(args[3]).book:std::shared_ptr<Book>{};
        if(page && !book)return 2;
        const auto hit=page?cachedPageThumbnail(book,0,cache,{},false,true):cachedCover(args[3],{},cache,{},false,0,true);
        if(hit.image.isNull() || hit.image.pixelColor(0,0)!=QColor(Qt::blue))return 3;
        const auto validated=page?cachedPageThumbnail(book,0,cache):cachedCover(args[3],{},cache);
        return !validated.image.isNull() && validated.image.pixelColor(0,0)==QColor(Qt::blue)?0:4;
    }
    return isolatedTestMain<ThumbnailTests>(argc,argv);
}
#include "thumbnail_tests.moc"
