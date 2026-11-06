// SPDX-License-Identifier: GPL-3.0-only
#include "test_support.h"
#include "displayjobs.h"
#include "archivenames.h"
#include "thumbnailcache.h"
#include "thumbnailsource.h"
#include "appearance.h"
#include "pageview.h"
#include <archive.h>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QColorSpace>
#include <QCryptographicHash>
#include <QDialog>
#include <QDateTime>
#include <QLabel>
#include <QLockFile>
#include <QPushButton>
#include <QProcess>
#include <QSaveFile>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <atomic>
#include <latch>
#include <future>
#include <chrono>
#include <thread>
#include <vector>
#include <cstdio>

class CacheTests:public QObject {
    Q_OBJECT
private slots:
    void cleanup() {setApplicationAppearance(0);}
    void archiveMetadataScanCanYield() {
        QTemporaryDir d;QVector<TestData::ZipEntry> entries;
        for(int i=0;i<20;++i)entries.append({QByteArray::number(i)+".png",TestData::png(TestData::solid())});
        const auto path=d.filePath("many.zip");QVERIFY(TestData::write(path,TestData::zip(entries)));
        int checks=0;const auto cancelled=readZipNames(path,[&]{return ++checks>3;});
        QVERIFY(!cancelled.error.isEmpty());QVERIFY(cancelled.entries.isEmpty());QCOMPARE(checks,4);
        QCOMPARE(readZipNames(path).entries.size(),20);
    }
    void cacheDeliveryPrecedesQueuedSourceAndStoreWork() {
        DisplayJobs jobs;std::latch release(1);std::atomic<bool> entered=false,delivered=false;
        std::atomic<int> early=0,finished=0;auto cleanup=qScopeGuard([&]{release.count_down();});
        jobs.submit<int>(DisplayJobs::Lane::Cache,"cache","cache",[&](const auto &){entered=true;release.wait();return 1;},[&](int,bool){delivered=true;});
        QTRY_VERIFY(entered.load());
        for(auto lane:{DisplayJobs::Lane::Display,DisplayJobs::Lane::Update,DisplayJobs::Lane::Store,DisplayJobs::Lane::Background}) {
            const auto id=QString::number(int(lane));
            jobs.submit<int>(lane,"later",id,[&](const auto &){if(!delivered)++early;return 1;},[&](int,bool){++finished;});
        }
        QTest::qWait(1100);QCOMPARE(early.load(),0);QCOMPARE(finished.load(),0);
        release.count_down();cleanup.dismiss();QTRY_COMPARE(finished.load(),4);
    }
    void cacheLookupHasReservedWorker() {
        QTemporaryDir d;ThumbnailDiskCache cache(d.path());
        cache.image("warm",[]{return ImageResult{TestData::solid(Qt::blue),{}};});
        std::latch entered(1),release(1);bool released=false;DisplayJobs jobs;
        const auto cleanup=qScopeGuard([&]{if(!released)release.count_down();});
        bool generated=false,hit=false;
        jobs.submit<int>(DisplayJobs::Lane::Display,"generation","slow",[&](const DisplayJobs::Cancel &){entered.count_down();release.wait();return 1;},[&](int,bool){generated=true;});
        entered.wait();
        jobs.submit<ImageResult>(DisplayJobs::Lane::Cache,"lookup","warm",[&](const DisplayJobs::Cancel &cancel){return cache.lookup("warm",cancel);},[&](ImageResult image,bool cancelled){hit=!cancelled && image.image.pixelColor(0,0)==QColor(Qt::blue);});
        QTRY_VERIFY_WITH_TIMEOUT(hit,2000);QVERIFY(!generated);
        release.count_down();released=true;QTRY_VERIFY(generated);
    }
    void obsoleteNativeWorkDoesNotOccupyCurrentWorker_data() {
        QTest::addColumn<int>("lane");
        QTest::newRow("cache")<<int(DisplayJobs::Lane::Cache);
        QTest::newRow("generation")<<int(DisplayJobs::Lane::Display);
        QTest::newRow("navigation")<<int(DisplayJobs::Lane::Navigation);
        QTest::newRow("reader")<<int(DisplayJobs::Lane::Render);
    }
    void obsoleteNativeWorkDoesNotOccupyCurrentWorker() {
        QFETCH(int,lane);DisplayJobs jobs;std::latch release(1);std::atomic<bool> entered=false;
        auto cleanup=qScopeGuard([&]{release.count_down();});bool oldDone=false,currentDone=false;
        jobs.submit<int>(DisplayJobs::Lane(lane),"view","old",[&](const auto &){entered=true;release.wait();return 0;},[&](int,bool stopped){oldDone=stopped;});
        QTRY_VERIFY(entered.load());jobs.visible("view");
        jobs.submit<int>(DisplayJobs::Lane(lane),"view","current",[](const auto &){return 1;},[&](int,bool stopped){currentDone=!stopped;});
        QTRY_VERIFY_WITH_TIMEOUT(currentDone,1000);QVERIFY(!oldDone);
        release.count_down();cleanup.dismiss();QTRY_VERIFY(oldDone);
    }
    void obsoleteWaitingJobsAreRemoved() {
        std::latch entered(1),release(1);bool released=false;DisplayJobs jobs;
        const auto cleanup=qScopeGuard([&]{if(!released)release.count_down();});
        bool activeDone=false,cancelled=false;std::atomic<int> calls=0;
        jobs.submit<int>(DisplayJobs::Lane::Display,"active","active",[&](const DisplayJobs::Cancel &){entered.count_down();release.wait();return 1;},[&](int,bool){activeDone=true;});
        entered.wait();
        jobs.submit<int>(DisplayJobs::Lane::Display,"old-view","old",[&](const DisplayJobs::Cancel &){++calls;return 1;},[&](int,bool wasCancelled){cancelled=wasCancelled;});
        jobs.retain("old-view");QVERIFY(cancelled);QVERIFY(!jobs.contains("old"));
        release.count_down();released=true;QTRY_VERIFY(activeDone);QCOMPARE(calls.load(),0);
    }
    void backgroundDoesNotOccupyDisplayWorker() {
        std::latch release(1);std::atomic<bool> entered=false;bool released=false;DisplayJobs jobs;
        const auto cleanup=qScopeGuard([&]{if(!released)release.count_down();});
        bool displayDone=false,backgroundCancelled=false;
        jobs.submit<int>(DisplayJobs::Lane::Background,"prefetch","old-prefetch",[&](const DisplayJobs::Cancel &){entered=true;release.wait();return 1;},[&](int,bool cancelled){backgroundCancelled=cancelled;});
        QTRY_VERIFY(entered.load());
        jobs.submit<int>(DisplayJobs::Lane::Display,"render","new-frame",[](const DisplayJobs::Cancel &){return 1;},[&](int,bool cancelled){displayDone=!cancelled;});
        QTRY_VERIFY_WITH_TIMEOUT(displayDone,2000);QVERIFY(!backgroundCancelled);
        release.count_down();released=true;QTRY_VERIFY(backgroundCancelled);
    }
    void deferredDisplayDoesNotWaitForInventory() {
        QTemporaryDir d;ThumbnailDiskCache cache(d.path());cache.setDeferredWrites(true);
        std::latch entered(1),release(1);int checks=0;
        std::thread maintenance([&]{cache.maintain([&]{if(++checks==2){entered.count_down();release.wait();}return false;});});
        entered.wait();std::promise<ImageResult> answer;auto ready=answer.get_future();
        std::thread display([&]{answer.set_value(cache.image("new",[]{return ImageResult{TestData::solid(Qt::blue),{}};}));});
        const bool returned=ready.wait_for(std::chrono::seconds(2))==std::future_status::ready;
        release.count_down();display.join();maintenance.join();
        QVERIFY(returned);QCOMPARE(ready.get().image.pixelColor(0,0),QColor(Qt::blue));
        QVERIFY(cache.hasPendingWrites());QVERIFY(cache.flushPending());
        QCOMPARE(cache.lookup("new").image.pixelColor(0,0),QColor(Qt::blue));
    }
    void deferredStoresRespectClearAndMemoryBound() {
        QTemporaryDir d;ThumbnailDiskCache cache(d.path());cache.setDeferredWrites(true);
        const auto pixels=[] {return ImageResult{TestData::solid(Qt::blue,{1024,1024}),{}};};
        for(int i=0;i<12;++i)cache.image(QByteArray::number(i),pixels);
        int stored=0;while(cache.hasPendingWrites()){QVERIFY(cache.flushPending());++stored;}
        QCOMPARE(stored,12);
        for(int i=0;i<12;++i)QVERIFY(!ThumbnailDiskCache(d.path()).lookup(QByteArray::number(i)).image.isNull());
        cache.image("old",pixels);QVERIFY(cache.hasPendingWrites());
        QVERIFY(cache.clear().isEmpty());QVERIFY(!cache.hasPendingWrites());QVERIFY(cache.lookup("old").image.isNull());
        cache.image("changed",pixels,{},false,[]{return false;});QVERIFY(!cache.hasPendingWrites());
    }
    void deferredPressurePreservesEveryImage() {
        QTemporaryDir d;ThumbnailDiskCache cache(d.path());cache.setDeferredWrites(true);
        for(int i=0;i<80;++i) {
            const auto result=cache.image(QByteArray::number(i),[]{return ImageResult{TestData::solid(Qt::blue,{320,480}),{}};});
            QVERIFY(result.error.isEmpty());QVERIFY(cache.pendingMemoryBytes()<=32LL*1024*1024);
        }
        int writes=0;while(cache.hasPendingWrites()){QVERIFY(cache.flushPending());QVERIFY(++writes<=80);}
        QCOMPARE(writes,80);ThumbnailDiskCache restart(d.path());
        for(int i=0;i<80;++i)QCOMPARE(restart.lookup(QByteArray::number(i)).image.pixelColor(0,0),QColor(Qt::blue));
    }
    void failedStoreRetainsRetryAndOldPixels() {
        QTemporaryDir d;ThumbnailDiskCache cache(d.path());cache.setDeferredWrites(true);
        const auto target=d.filePath(QString::fromLatin1(QCryptographicHash::hash("key",QCryptographicHash::Sha256).toHex())+".png");
        QVERIFY(QDir().mkpath(target));
        cache.image("key",[]{return ImageResult{TestData::solid(Qt::blue),{}};});
        QVERIFY(!cache.flushPending());QVERIFY(cache.hasPendingWrites());
        QCOMPARE(cache.lookup("key").image.pixelColor(0,0),QColor(Qt::blue));
        QVERIFY(QDir().rmdir(target));QVERIFY(cache.flushPending());
        QCOMPARE(ThumbnailDiskCache(d.path()).lookup("key").image.pixelColor(0,0),QColor(Qt::blue));
    }
    void lateStoreCancellationIsRetried() {
        QTemporaryDir d;ThumbnailDiskCache cache(d.path());cache.setDeferredWrites(true);
        int checks=0;
        cache.image("key",[]{return ImageResult{TestData::solid(Qt::blue),{}};},{},true,[]{return true;});
        QVERIFY(!cache.flushPending([&]{return ++checks>=4;}));QVERIFY(cache.hasPendingWrites());
        QVERIFY(checks>=4);QVERIFY(cache.flushPending());
        QCOMPARE(ThumbnailDiskCache(d.path()).lookup("key").image.pixelColor(0,0),QColor(Qt::blue));
    }
    void journalSurvivesRestartAndConfiguration() {
        QTemporaryDir d;
        {ThumbnailDiskCache cache(d.path());cache.setDeferredWrites(true);
            cache.image("key",[]{return ImageResult{TestData::solid(Qt::green),{}};});
            cache.configure(true,16);QVERIFY(cache.hasPendingWrites());
            cache.configure(false,32);QVERIFY(cache.hasPendingWrites());
            QVERIFY(cache.checkpoint());QCOMPARE(cache.pendingMemoryBytes(),0);
        }
        ThumbnailDiskCache restart(d.path());restart.setDeferredWrites(true);
        QCOMPARE(restart.lookup("key").image.pixelColor(0,0),QColor(Qt::green));
        restart.recoverPending();QVERIFY(restart.hasPendingWrites());QVERIFY(restart.flushPending());
        QVERIFY(QDir(d.filePath("pending")).entryList({"*.png"},QDir::Files).isEmpty());
        QCOMPARE(ThumbnailDiskCache(d.path()).lookup("key").image.pixelColor(0,0),QColor(Qt::green));
        restart.image("clear",[]{return ImageResult{TestData::solid(),{}};});QVERIFY(restart.checkpoint());
        const auto corrupt=d.filePath("pending/"+QString(64,'a')+".png");QVERIFY(TestData::write(corrupt,"broken journal PNG"));
        QVERIFY(restart.clear().isEmpty());QVERIFY(!QFileInfo::exists(corrupt));ThumbnailDiskCache cleared(d.path());cleared.recoverPending();
        QVERIFY(!cleared.hasPendingWrites());QVERIFY(cleared.lookup("clear").image.isNull());
    }
    void visibleWorkPrecedesUpdateStoreAndBackground() {
        DisplayJobs jobs;std::latch release(1);std::atomic<bool> entered=false;
        auto cleanup=qScopeGuard([&]{release.count_down();});
        jobs.submit<int>(DisplayJobs::Lane::Display,"slow","slow",[&](const auto &){entered=true;release.wait();return 1;},[](int,bool){});
        QTRY_VERIFY(entered.load());bool updated=false,stored=false,background=false;
        jobs.submit<int>(DisplayJobs::Lane::Update,"update","update",[](const auto &){return 1;},[&](int,bool cancelled){updated=!cancelled;});
        jobs.submit<int>(DisplayJobs::Lane::Store,"store","store",[](const auto &){return 1;},[&](int,bool cancelled){stored=!cancelled;});
        jobs.submit<int>(DisplayJobs::Lane::Background,"background","background",[](const auto &){return 1;},[&](int,bool cancelled){background=!cancelled;});
        QTest::qWait(1100);QVERIFY(!updated && !stored && !background);
        release.count_down();cleanup.dismiss();
        QTRY_VERIFY_WITH_TIMEOUT(updated && stored && background,3000);
    }
    void invisibleRequestsAreDiscarded() {
        DisplayJobs jobs;std::latch release(1);std::atomic<bool> entered=false;bool called=false,cancelled=false;
        auto cleanup=qScopeGuard([&]{release.count_down();});
        jobs.submit<int>(DisplayJobs::Lane::Display,"held","held",[&](const auto &){entered=true;release.wait();return 0;},[](int,bool){});
        QTRY_VERIFY(entered.load());
        jobs.submit<int>(DisplayJobs::Lane::Display,"covers","old",[&](const auto &){called=true;return 0;},[&](int,bool stopped){cancelled=stopped;});
        jobs.visible("covers");QVERIFY(cancelled);QVERIFY(!jobs.contains("old"));
        release.count_down();cleanup.dismiss();QTest::qWait(200);QVERIFY(!called);
        QVERIFY(!jobs.submit<int>(DisplayJobs::Lane::Display,"covers","already-gone",[](const auto &){return 0;},[](int,bool){},false,true));
    }
    void visibleValidationSurvivesNewCacheRequests() {
        DisplayJobs jobs;std::latch release(1);std::atomic<bool> entered=false;bool completed=false,cancelled=false,hit=false;
        auto cleanup=qScopeGuard([&]{release.count_down();});
        jobs.submit<int>(DisplayJobs::Lane::Update,"covers","validate",[&](const auto &){entered=true;release.wait();return 1;},[&](int,bool stopped){completed=true;cancelled=stopped;});
        QTRY_VERIFY(entered.load());
        jobs.submit<int>(DisplayJobs::Lane::Cache,"covers","hit",[](const auto &){return 1;},[&](int,bool stopped){hit=!stopped;});
        QTRY_VERIFY(hit);QVERIFY(!completed);release.count_down();cleanup.dismiss();
        QTRY_VERIFY(completed);QVERIFY(!cancelled);
    }
    void cancelledRunningRequestNeverRestarts() {
        DisplayJobs jobs;std::atomic<int> calls=0;bool cancelled=false;
        jobs.submit<ImageResult>(DisplayJobs::Lane::Display,"covers","old",[&](const auto &cancel){++calls;while(!cancel())QThread::msleep(2);return ImageResult{};},[&](ImageResult,bool stopped){cancelled=stopped;},true);
        QTRY_COMPARE(calls.load(),1);jobs.visible("covers");QTRY_VERIFY(cancelled);QTest::qWait(1200);QCOMPARE(calls.load(),1);
    }
    void lateOldGenerationCannotReplaceNewPixels() {
        QTemporaryDir d;ThumbnailDiskCache cache(d.path());cache.setDeferredWrites(true);
        std::latch entered(1),release(1);
        std::thread old([&]{cache.image("key",[&]{entered.count_down();release.wait();return ImageResult{TestData::solid(Qt::red),{}};},{},true);});
        entered.wait();cache.image("key",[]{return ImageResult{TestData::solid(Qt::blue),{}};},{},true);
        release.count_down();old.join();
        QCOMPARE(cache.lookup("key").image.pixelColor(0,0),QColor(Qt::blue));
        while(cache.hasPendingWrites())QVERIFY(cache.flushPending());
        QCOMPARE(ThumbnailDiskCache(d.path()).lookup("key").image.pixelColor(0,0),QColor(Qt::blue));
    }
    void freshGenerationReplacesRecoveredJournal() {
        QTemporaryDir d;
        {ThumbnailDiskCache old(d.path());old.setDeferredWrites(true);old.image("key",[]{return ImageResult{TestData::solid(Qt::red),{}};});QVERIFY(old.checkpoint());}
        ThumbnailDiskCache current(d.path());current.setDeferredWrites(true);
        current.image("key",[]{return ImageResult{TestData::solid(Qt::blue),{}};},{},true);
        while(current.hasPendingWrites())QVERIFY(current.flushPending());
        QCOMPARE(current.lookup("key").image.pixelColor(0,0),QColor(Qt::blue));
        QCOMPARE(ThumbnailDiskCache(d.path()).lookup("key").image.pixelColor(0,0),QColor(Qt::blue));
    }
    void failedReplacementPreservesCompletedPendingPixels() {
        QTemporaryDir d;ThumbnailDiskCache cache(d.path());cache.setDeferredWrites(true);
        cache.image("key",[]{return ImageResult{TestData::solid(Qt::red),{}};},{},true);
        QVERIFY(cache.hasPendingWrites());
        cache.image("key",[]{return ImageResult{{},"decode failed"};},{},true);
        QVERIFY(cache.flushPending());
        QCOMPARE(ThumbnailDiskCache(d.path()).lookup("key").image.pixelColor(0,0),QColor(Qt::red));
        // Recovery of an old journal during a new decode must not defeat it.
        {ThumbnailDiskCache old(d.path());old.setDeferredWrites(true);old.image("other",[]{return ImageResult{TestData::solid(Qt::red),{}};});QVERIFY(old.checkpoint());}
        ThumbnailDiskCache current(d.path());current.setDeferredWrites(true);
        current.image("other",[&]{current.recoverPending();return ImageResult{TestData::solid(Qt::blue),{}};},{},true);
        while(current.hasPendingWrites())QVERIFY(current.flushPending());
        QCOMPARE(ThumbnailDiskCache(d.path()).lookup("other").image.pixelColor(0,0),QColor(Qt::blue));
    }
    void persistenceDoesNotRecheckSource() {
        QTemporaryDir d;ThumbnailDiskCache cache(d.path());cache.setDeferredWrites(true);int checks=0;
        cache.image("saved",[]{return ImageResult{TestData::solid(Qt::blue),{}};},{},true,[&]{++checks;return true;});
        const int before=checks;QVERIFY(cache.flushPending());QCOMPARE(checks,before);
        QCOMPARE(ThumbnailDiskCache(d.path()).lookup("saved").image.pixelColor(0,0),QColor(Qt::blue));
    }
    void cacheHitsDoNotDiscardPendingImages() {
        QTemporaryDir d;const auto root=d.filePath("cache");QVERIFY(QDir().mkpath(root));
        const auto png=TestData::png(TestData::solid(Qt::blue));
        // Populate committed entries directly: no generation or inventory is
        // involved in the subsequent stream of LRU timestamp updates.
        for(int i=0;i<300;++i) {
            const auto key=QByteArray::number(i);
            const auto name=QCryptographicHash::hash(key,QCryptographicHash::Sha256).toHex()+".png";
            QVERIFY(TestData::write(root+'/'+QString::fromLatin1(name),png));
        }
        ThumbnailDiskCache cache(root);cache.setDeferredWrites(true);
        cache.image("unwritten",[]{return ImageResult{TestData::solid(Qt::red),{}};});
        for(int i=0;i<300;++i)QVERIFY(!cache.lookup(QByteArray::number(i)).image.isNull());
        int writes=0;while(cache.hasPendingWrites()){QVERIFY(cache.flushPending());QVERIFY(++writes<=256);}
        ThumbnailDiskCache restarted(root);
        const auto saved=restarted.lookup("unwritten");QVERIFY(!saved.image.isNull());
        QCOMPARE(saved.image.pixelColor(0,0),QColor(Qt::red));
        // New images also take priority when the queue was already full of
        // touches, so a short idle window can make useful persistence progress.
        for(int i=0;i<300;++i)QVERIFY(!cache.lookup(QByteArray::number(i)).image.isNull());
        cache.image("next",[]{return ImageResult{TestData::solid(Qt::green),{}};});
        QVERIFY(cache.flushPending());
        const auto next=restarted.lookup("next");QVERIFY(!next.image.isNull());
        QCOMPARE(next.image.pixelColor(0,0),QColor(Qt::green));
    }
    void thumbnailPersistenceAcrossExecutableLocations() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto source=directory.filePath("book.zip"), cachePath=directory.filePath("thumbnails");
        QVERIFY(TestData::write(source,TestData::zip({{"01.png",TestData::png(TestData::solid(Qt::red))}})));
        auto cache=std::make_shared<ThumbnailDiskCache>(cachePath);
        const auto book=Book::open(source).book; QVERIFY(book);
        QCOMPARE(cachedCover(source,{},cache).image.pixelColor(0,0),QColor(Qt::red));
        QCOMPARE(cachedPageThumbnail(book,0,cache).image.pixelColor(0,0),QColor(Qt::red));
        QCOMPARE(cache->stats().entries,2);
        // A marker in each cached image proves a hit: regeneration from the
        // unchanged archive would produce red, not blue.
        for (const auto &name:QDir(cachePath).entryList({"*.png"},QDir::Files)) {
            const auto file=QDir(cachePath).filePath(name);
            const auto old=QImage(file);
            auto blue=TestData::solid(Qt::blue);
            const auto stamp=old.text(QStringLiteral("komamimic-cover-stamp"));
            if(!stamp.isEmpty())blue.setText(QStringLiteral("komamimic-cover-stamp"),stamp);
            QVERIFY(TestData::write(file,TestData::png(blue)));
        }
        const auto original=QCoreApplication::applicationFilePath();
        const auto copyDir=directory.filePath("another-build"); QVERIFY(QDir().mkpath(copyDir));
        const auto relocated=copyDir+'/'+QFileInfo(original).fileName();
        QVERIFY(QFile::copy(original,relocated));
        for (const auto &binary:{original,relocated}) {
            QProcess process; process.start(binary,{"--thumbnail-cache-probe",source,cachePath,ThumbnailDiskCache::defaultDirectory()});
            QVERIFY2(process.waitForFinished(30000),qPrintable(process.errorString()));
            QCOMPARE(process.exitStatus(),QProcess::NormalExit);
            QVERIFY2(process.exitCode()==0,process.readAllStandardError().constData());
        }
        QCOMPARE(cache->stats().entries,2);
    }
    void v2CoverCacheDisplaysBeforeSourceValidation() {
        QVERIFY(ThumbnailDiskCache::defaultDirectory().endsWith("thumbnails-v2"));
        QTemporaryDir d;QVERIFY(d.isValid());
        const auto source=d.filePath("book.zip");
        QVERIFY(TestData::write(source,TestData::zip({{"01.png",TestData::png(TestData::solid(Qt::red))}})));
        auto cache=std::make_shared<ThumbnailDiskCache>(d.filePath("thumbnails-v2"));
        QCOMPARE(cachedCover(source,{},cache).image.pixelColor(0,0),QColor(Qt::red));
        const auto initial=cachedCover(source,{},cache,{},false,0,true);
        QCOMPARE(initial.image.pixelColor(0,0),QColor(Qt::red));
        QVERIFY(!initial.image.text(QStringLiteral("komamimic-cover-stamp")).isEmpty());
        // Cache-only lookup must not require the source even to exist.
        QVERIFY(QFile::remove(source));
        QCOMPARE(cachedCover(source,{},cache,{},false,0,true).image.pixelColor(0,0),QColor(Qt::red));
        QVERIFY(TestData::write(source,TestData::zip({{"01.png",TestData::png(TestData::solid(Qt::blue,{37,59}))}})));
        // Stale-while-revalidate: the old image remains available until full validation.
        QCOMPARE(cachedCover(source,{},cache,{},false,0,true).image.pixelColor(0,0),QColor(Qt::red));
        const auto updated=cachedCover(source,{},cache,{},false,0,false,true);
        QCOMPARE(updated.image.pixelColor(0,0),QColor(Qt::blue));
        QVERIFY(updated.image.text(QStringLiteral("komamimic-cover-stamp"))!=initial.image.text(QStringLiteral("komamimic-cover-stamp")));
        QCOMPARE(cachedCover(source,{},cache,{},false,0,true).image.pixelColor(0,0),QColor(Qt::blue));
        QCOMPARE(cachedCover(source,{},std::make_shared<ThumbnailDiskCache>(d.filePath("thumbnails-v2")),{},false,0,true).image.pixelColor(0,0),QColor(Qt::blue));
    }
    void v2CoverCacheDoesNotWaitForValidation() {
        QTemporaryDir d;QVERIFY(d.isValid());
        const auto source=d.filePath("book.zip");
        QVERIFY(TestData::write(source,TestData::zip({{"01.png",TestData::png(TestData::solid(Qt::red))}})));
        auto cache=std::make_shared<ThumbnailDiskCache>(d.filePath("thumbnails-v2"));
        QVERIFY(!cachedCover(source,{},cache).image.isNull());
        std::latch release(1);std::atomic<bool> started=false;bool released=false;DisplayJobs jobs;
        const auto cleanup=qScopeGuard([&]{if(!released)release.count_down();});
        bool shown=false,validationFinished=false,validationCancelled=false;
        jobs.submit<ImageResult>(DisplayJobs::Lane::Background,"covers","validate",
            [&](const DisplayJobs::Cancel &cancel){started=true;release.wait();return cachedCover(source,{},cache,cancel);},
            [&](ImageResult,bool cancelled){validationFinished=true;validationCancelled=cancelled;});
        QTRY_VERIFY(started.load());
        jobs.submit<ImageResult>(DisplayJobs::Lane::Cache,"covers","paint",
            [&](const DisplayJobs::Cancel &cancel){return cachedCover(source,{},cache,cancel,false,0,true);},
            [&](ImageResult result,bool cancelled){shown=!cancelled && result.image.pixelColor(0,0)==QColor(Qt::red);});
        QTRY_VERIFY_WITH_TIMEOUT(shown,2000);
        QVERIFY(!validationFinished);
        release.count_down();released=true;
        QTRY_VERIFY(validationFinished);
        QVERIFY(validationCancelled); // Visible cache requests may cancel idle validation.

    }

void v2ExplicitRefreshRepairsIncorrectCover() {
        QTemporaryDir directory;QVERIFY(directory.isValid());
        const auto source=directory.filePath("book.zip");
        QVERIFY(TestData::write(source,TestData::zip({{"01.png",TestData::png(TestData::solid(Qt::red))}})));
        auto cache=std::make_shared<ThumbnailDiskCache>(directory.filePath("thumbnails-v2"));
        QCOMPARE(cachedCover(source,{},cache).image.pixelColor(0,0),QColor(Qt::red));
        const auto original=cachedCover(source,{},cache,{},false,0,true);
        const auto stamp=original.image.text(QStringLiteral("komamimic-cover-stamp"));
        QVERIFY(!stamp.isEmpty());
        const auto files=QDir(cache->directory()).entryList({"*.png"},QDir::Files);
        QCOMPARE(files.size(),1);
        auto incorrect=TestData::solid(Qt::blue);
        incorrect.setText(QStringLiteral("komamimic-cover-stamp"),stamp);
        QVERIFY(TestData::write(QDir(cache->directory()).filePath(files.first()),TestData::png(incorrect)));
        QCOMPARE(cachedCover(source,{},cache,{},false,0,true).image.pixelColor(0,0),QColor(Qt::blue));
        // A normal validation sees the same source stamp and leaves the image unchanged.
        QCOMPARE(cachedCover(source,{},cache).image.pixelColor(0,0),QColor(Qt::blue));
        // Explicit refresh must ignore even a plausible, stamped but incorrect PNG.
        QCOMPARE(cachedCover(source,{},cache,{},true).image.pixelColor(0,0),QColor(Qt::red));
        QCOMPARE(cachedCover(source,{},cache,{},false,0,true).image.pixelColor(0,0),QColor(Qt::red));
        QCOMPARE(cache->stats().entries,1);
    }

    void diskPersistenceAndRefresh() {
        QTemporaryDir d;int calls=0;
        const auto generate=[&] {++calls;return ImageResult{TestData::solid(Qt::red),{}};};
        {ThumbnailDiskCache cache(d.path());QCOMPARE(cache.image("one",generate).image.pixelColor(0,0),QColor(Qt::red));}
        ThumbnailDiskCache cache(d.path());
        QCOMPARE(cache.image("one",generate).image.pixelColor(0,0),QColor(Qt::red));QCOMPARE(calls,1);
        QCOMPARE(cache.image("one",[&]{++calls;return ImageResult{TestData::solid(Qt::blue),{}};},{},true).image.pixelColor(0,0),QColor(Qt::blue));
        QCOMPARE(calls,2);QCOMPARE(cache.image("one",generate).image.pixelColor(0,0),QColor(Qt::blue));
        QCOMPARE(calls,2);QCOMPARE(cache.stats().entries,1);
        QImage hidpi=TestData::solid(Qt::green);hidpi.setDevicePixelRatio(2);hidpi.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
        cache.image("hidpi",[&]{return ImageResult{hidpi,{}};});
        const auto restored=cache.image("hidpi",generate).image;
        QCOMPARE(restored.size(),hidpi.size());QCOMPARE(restored.pixelColor(31,47),QColor(Qt::green));
        QCOMPARE(restored.colorSpace(),hidpi.colorSpace());
    }
    void countDoesNotLimitDiskCache() {
        QTemporaryDir d;
        const auto pixels=TestData::png(TestData::solid(Qt::blue,{1,1}));
        // The old 20,000-file limit would delete otherwise valid thumbnails.
        for(int i=0;i<20001;++i) {
            const auto key=QByteArray::number(i);
            const auto name=QString::fromLatin1(QCryptographicHash::hash(key,QCryptographicHash::Sha256).toHex())+".png";
            QVERIFY(TestData::write(d.filePath(name),pixels));
        }
        ThumbnailDiskCache cache(d.path());cache.configure(true,16);
        QVERIFY(cache.stats().bytes<16LL*1024*1024);
        QVERIFY(cache.maintain().isEmpty());
        QCOMPARE(cache.stats().entries,20001);
        int calls=0;
        const auto warm=cache.image("0",[&]{++calls;return ImageResult{TestData::solid(Qt::red),{}};});
        QCOMPARE(warm.image.pixelColor(0,0),QColor(Qt::blue));QCOMPARE(calls,0);
        cache.image("new",[]{return ImageResult{TestData::solid(Qt::green,{1,1}),{}};});
        QCOMPARE(cache.stats().entries,20002);
        cache.image("0",[]{return ImageResult{TestData::solid(Qt::green,{1,1}),{}};},{},true);
        QCOMPARE(cache.stats().entries,20002);
    }
    void deferredFailedReplacementKeepsPreviousPng() {
        QTemporaryDir d;ThumbnailDiskCache cache(d.path());cache.setDeferredWrites(true);
        cache.image("key",[]{return ImageResult{TestData::solid(Qt::blue),{}};});
        QVERIFY(cache.flushPending());
        cache.image("key",[]{return ImageResult{TestData::solid(Qt::green),{}};},{},true);
        int checks=0;
        // A late cancellation must retain both the old committed PNG and
        // the completed new image; persistence never rechecks the source.
        QVERIFY(!cache.flushPending([&]{return ++checks>=4;}));
        QCOMPARE(cache.stats().entries,1);
        QCOMPARE(QImage(d.filePath(QString::fromLatin1(QCryptographicHash::hash("key",QCryptographicHash::Sha256).toHex())+".png")).pixelColor(0,0),QColor(Qt::blue));
        QCOMPARE(cache.lookup("key").image.pixelColor(0,0),QColor(Qt::green));
        QVERIFY(cache.flushPending());
    }
    void pageCacheIgnoresUnrelatedScriptSettings() {
        QTemporaryDir d;
        const auto source=d.filePath("book.zip");
        QVERIFY(TestData::write(source,TestData::zip({{"01.png",TestData::png(TestData::solid(Qt::red))}})));
        auto cache=std::make_shared<ThumbnailDiskCache>(d.filePath("cache"));
        const auto first=Book::open(source).book;
        QVERIFY(first);
        QCOMPARE(cachedPageThumbnail(first,0,cache).image.pixelColor(0,0),QColor(Qt::red));
        const auto pngs=QDir(cache->directory()).entryList({"*.png"},QDir::Files);
        QCOMPARE(pngs.size(),1);
        // A blue committed PNG proves a cache hit: the unchanged source is red.
        const auto file=QDir(cache->directory()).filePath(pngs.first());
        auto marker=TestData::solid(Qt::blue);marker.setText("komamimic-cover-stamp",QImage(file).text("komamimic-cover-stamp"));
        QVERIFY(TestData::write(file,TestData::png(marker)));
        OpenOptions options;
        options.scriptSettings.insert("unrelated-reader-option",42);
        const auto reopened=Book::open(source,{},options).book;
        QVERIFY(reopened);
        QCOMPARE(cachedPageThumbnail(reopened,0,cache).image.pixelColor(0,0),QColor(Qt::blue));
        QCOMPARE(cache->stats().entries,1);
    }
    void maintenanceCancellationPublishesPartialEviction() {
        QTemporaryDir d;
        const auto first=d.filePath(QString(64,'a')+".png");
        const auto second=d.filePath(QString(64,'b')+".png");
        const QByteArray large(9*1024*1024,'x');
        QVERIFY(TestData::write(first,large));
        QVERIFY(TestData::write(second,large));
        ThumbnailDiskCache cache(d.path());cache.configure(true,16);
        // Cancellation becomes true only after the first file is removed.
        const auto cancel=[&]{return QFileInfo::exists(first)!=QFileInfo::exists(second);};
        QVERIFY(cache.maintain(cancel).isEmpty());
        QVERIFY(cancel());
        QVERIFY(QFileInfo::exists(d.filePath(".revision")));
        QCOMPARE(cache.stats().entries,1);
    }
    void deferredRetryNeverReplacesNewerRequest() {
        QTemporaryDir d;ThumbnailDiskCache cache(d.path());cache.setDeferredWrites(true);
        cache.image("key",[]{return ImageResult{TestData::solid(Qt::red),{}};});
        bool injected=false;
        const bool completed=cache.flushPending([&] {
            if(!injected) {
                injected=true;
                cache.image("key",[]{return ImageResult{TestData::solid(Qt::green),{}};},{},true);
            }
            return true; // Force the old in-flight request to retry.
        });
        QVERIFY(injected);
        QVERIFY(!completed);
        QVERIFY(cache.hasPendingWrites());
        QCOMPARE(cache.lookup("key").image.pixelColor(0,0),QColor(Qt::green));
        QVERIFY(cache.flushPending());
        QCOMPARE(cache.lookup("key").image.pixelColor(0,0),QColor(Qt::green));
        QCOMPARE(cache.stats().entries,1);
    }
    void disabledAndClear() {
        QTemporaryDir d;const auto root=d.filePath("cache");ThumbnailDiskCache cache(root);int calls=0;
        const auto generate=[&] {++calls;return ImageResult{TestData::solid(),{}};};
        cache.configure(false,32);cache.image("one",generate);cache.image("one",generate);
        QCOMPARE(calls,2);QVERIFY(!QFileInfo::exists(root));
        cache.configure(true,32);cache.image("one",generate);QCOMPARE(cache.stats().entries,1);
        cache.configure(false,32);cache.image("one",generate);QCOMPARE(calls,4);QCOMPARE(cache.stats().entries,1);
        QVERIFY(TestData::write(root+"/unrelated.txt","keep"));QVERIFY(cache.clear().isEmpty());
        QCOMPARE(cache.stats().entries,0);QVERIFY(QFileInfo::exists(root+"/unrelated.txt"));
    }
    void corruptAndUnavailable() {
        QTemporaryDir d;ThumbnailDiskCache cache(d.path());int calls=0;
        const auto generate=[&] {++calls;return ImageResult{TestData::solid(),{}};};
        cache.image("one",generate);
        const auto path=d.filePath(QString::fromLatin1(QCryptographicHash::hash("one",QCryptographicHash::Sha256).toHex())+".png");
        QVERIFY(TestData::write(path,"broken"));cache.image("one",generate);QCOMPARE(calls,2);
        QCOMPARE(cache.stats().entries,1);
        const auto file=d.filePath("not-a-directory");QVERIFY(TestData::write(file,"keep"));
        ThumbnailDiskCache unavailable(file);QVERIFY(!unavailable.image("one",generate).image.isNull());
        QVERIFY(!unavailable.stats().error.isEmpty());QFile original(file);QVERIFY(original.open(QIODevice::ReadOnly));QCOMPARE(original.readAll(),QByteArray("keep"));
    }
    void diskCacheSurvivesKilledWriter() {
        for (const auto &mode:{QString("pending"),QString("committed")}) {
            QTemporaryDir directory; QVERIFY(directory.isValid());
            const auto root=directory.filePath("cache");
            const auto name=QString::fromLatin1(QCryptographicHash::hash("one",QCryptographicHash::Sha256).toHex())+".png";
            {
                ThumbnailDiskCache cache(root);
                QVERIFY(!cache.image("one",[]{return ImageResult{TestData::solid(Qt::blue),{}};}).image.isNull());
            }
            QProcess writer;
            writer.start(QCoreApplication::applicationFilePath(),{"--cache-interruption-probe",root,name,mode});
            QVERIFY2(writer.waitForReadyRead(10000),writer.readAllStandardError().constData());
            QCOMPARE(writer.readAllStandardOutput().trimmed(),QByteArray("ready"));
            QVERIFY(QFileInfo::exists(root+"/.lock"));
            // A live writer can hold the directory lock while preparing a
            // replacement. Readers must use the old/new committed PNG, never
            // regenerate the source because of that lock.
            ThumbnailDiskCache liveReader(root);int liveCalls=0;
            const auto live=liveReader.image("one",[&]{++liveCalls;return ImageResult{TestData::solid(Qt::red),{}};});
            QCOMPARE(live.image.pixelColor(0,0),QColor(mode=="pending"?Qt::blue:Qt::green));QCOMPARE(liveCalls,0);
            writer.kill(); QVERIFY(writer.waitForFinished(5000));
            QCOMPARE(writer.exitStatus(),QProcess::CrashExit);
            // The dead writer leaves a lock and may leave a pending QSaveFile.
            // The next instance must recover without regenerating intact PNGs,
            // including when a commit happened before .revision was updated.
            ThumbnailDiskCache recovered(root); int calls=0;
            const auto image=recovered.image("one",[&]{++calls;return ImageResult{TestData::solid(Qt::red),{}};});
            QVERIFY(!image.image.isNull());
            QCOMPARE(image.image.pixelColor(0,0),QColor(mode=="pending"?Qt::blue:Qt::green));
            QCOMPARE(calls,0);
            const auto stats=recovered.stats(); QVERIFY2(stats.error.isEmpty(),qPrintable(stats.error));
            QCOMPARE(stats.entries,1);
            QCOMPARE(stats.bytes,QFileInfo(root+'/'+name).size());
            QVERIFY(!QFileInfo::exists(root+"/.lock"));
        }
    }
    void cancelledAndChangedSources() {
        QTemporaryDir d;ThumbnailDiskCache cache(d.path());
        bool cancelled=false;
        cache.image("one",[&]{cancelled=true;return ImageResult{TestData::solid(),{}};},[&]{return cancelled;});
        QCOMPARE(cache.stats().entries,0);
        cache.image("two",[]{return ImageResult{TestData::solid(),{}};},{},false,[]{return false;});
        QCOMPARE(cache.stats().entries,0);
    }
    void cancelWhileFindingFolderCover() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto folder=directory.filePath("books"); QVERIFY(QDir().mkpath(folder));
        const auto archive=TestData::zip({{"01.png",TestData::png(TestData::solid())}});
        for (int i=0;i<256;++i)
            QVERIFY(TestData::write(folder+QString("/%1.zip").arg(i,4,10,QChar('0')),archive));
        auto cache=std::make_shared<ThumbnailDiskCache>(directory.filePath("cache"));
        int checks=0;
        const auto result=cachedCover(folder,{},cache,[&] {return ++checks>=100;});
        QVERIFY(result.image.isNull());
        QCOMPARE(cache->stats().entries,0);
    }
    void zipCancellationDuringForwardSkip() {
        qInfo() << "ZIP cancellation backend:" << archive_version_string();
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto path=directory.filePath("skip.zip");
        QVERIFY(TestData::write(path,TestData::zip({{"00-padding.bin",QByteArray(256*1024,'x')},
            {"01.png",TestData::png(TestData::solid())}})));
        // Probe different cancellation boundaries in separate processes. A
        // backend spin must fail this test without hanging the whole suite.
        for (const auto &mode:{QString("open"),QString("encoded")})
            for (int checkpoint=1;checkpoint<=24;++checkpoint) {
                QProcess process;
                process.start(QCoreApplication::applicationFilePath(),{"--zip-cancel-probe",mode,path,QString::number(checkpoint)});
                const bool finished=process.waitForFinished(2000);
                if (!finished) {process.kill();process.waitForFinished();}
                QVERIFY2(finished,qPrintable(QString("ZIP cancellation stalled: %1 checkpoint %2").arg(mode).arg(checkpoint)));
                QCOMPARE(process.exitStatus(),QProcess::NormalExit);
                QVERIFY2(process.exitCode()==0,process.readAllStandardError().constData());
            }
    }
    void folderCoverOrderAndAttemptLimit() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto folder=directory.filePath("books"); QVERIFY(QDir().mkpath(folder+"/0-folder"));
        QVERIFY(TestData::write(folder+"/0-folder/cover.png",TestData::png(TestData::solid(Qt::blue))));
        auto cache=std::make_shared<ThumbnailDiskCache>(directory.filePath("cache"));
        const auto red=TestData::zip({{"1.png",TestData::png(TestData::solid(Qt::red))}});
        const auto green=TestData::zip({{"1.png",TestData::png(TestData::solid(Qt::green))}});
        QVERIFY(TestData::write(folder+"/10.zip",green));
        QVERIFY(TestData::write(folder+"/2.zip",red));
        QCOMPARE(cachedCover(folder,{},cache,{},false,0,false,true,1).image.pixelColor(0,0),QColor(Qt::red));
        QVERIFY(QFile::remove(folder+"/2.zip"));
        QCOMPARE(cachedCover(folder,{},cache,{},false,0,false,true,1).image.pixelColor(0,0),QColor(Qt::green));
        QVERIFY(QFile::remove(folder+"/10.zip"));
        QCOMPARE(cachedCover(folder,{},cache,{},false,0,false,true,1).image.pixelColor(0,0),QColor(Qt::blue));
        for (int i=1;i<=16;++i)
            QVERIFY(TestData::write(folder+QString("/%1.zip").arg(i,2,10,QChar('0')),"invalid"));
        QVERIFY(TestData::write(folder+"/17.zip",red));
        // Hitting the bounded candidate limit must retain the previous valid image.
        QCOMPARE(cachedCover(folder,{},cache,{},false,0,false,true,1).image.pixelColor(0,0),QColor(Qt::blue));
    }
    void cancelCacheMaintenanceInventory() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto png=TestData::png(TestData::solid());
        for (int i=0;i<128;++i)
            QVERIFY(TestData::write(directory.filePath(QString("%1.png").arg(i,64,16,QChar('0'))),png));
        ThumbnailDiskCache cache(directory.path());
        int checks=0;
        QVERIFY(cache.maintain([&]{return ++checks>=32;}).isEmpty());
        QVERIFY(!QFileInfo::exists(directory.filePath(".revision")));
        QCOMPARE(cache.stats().entries,128);
        QVERIFY(cache.maintain().isEmpty());
        // A successful no-op maintenance does not publish a new revision.
        QVERIFY(!QFileInfo::exists(directory.filePath(".revision")));
        QCOMPARE(cache.stats().entries,128);
    }
    void clearDuringGeneration() {
        QTemporaryDir d;ThumbnailDiskCache cache(d.path());std::latch entered(1),release(1);
        std::thread worker([&]{cache.image("one",[&]{entered.count_down();release.wait();return ImageResult{TestData::solid(),{}};});});
        entered.wait();const auto error=cache.clear();release.count_down();worker.join();
        QVERIFY(error.isEmpty());QCOMPARE(cache.stats().entries,0);
    }
    void concurrentWriters() {
        QTemporaryDir d;ThumbnailDiskCache first(d.path()),second(d.path());std::atomic<bool> good{true};
        std::vector<std::thread> workers;
        for(int i=0;i<4;++i) workers.emplace_back([&,i]{
            auto &cache=i%2?first:second;
            for(int j=0;j<32;++j) {
                const auto result=cache.image(QByteArray::number(j),[]{return ImageResult{TestData::solid(Qt::green),{}};});
                if(result.image.isNull() || result.image.pixelColor(0,0)!=QColor(Qt::green)) good=false;
            }
        });
        for(auto &worker:workers) worker.join();
        QVERIFY(good.load());
        // Persistence is best-effort when another instance holds the directory
        // lock. Contention must not corrupt a stored PNG or lose the image
        // returned to the reader; it does not guarantee 32 immediate writes.
        const auto stored=first.stats();QVERIFY(stored.error.isEmpty());
        QVERIFY(stored.entries<=32);QCOMPARE(second.stats().entries,stored.entries);
        for(const auto &name:QDir(d.path()).entryList({"*.png"},QDir::Files)) {
            const QImage image(d.filePath(name));QVERIFY(!image.isNull());
            QCOMPARE(image.pixelColor(0,0),QColor(Qt::green));
        }
        // Once writers have stopped, missing entries can be saved normally and
        // both independent inventories must converge without clearing anything.
        for(int j=0;j<32;++j) {
            const auto result=first.image(QByteArray::number(j),[]{return ImageResult{TestData::solid(Qt::green),{}};});
            QCOMPARE(result.image.pixelColor(0,0),QColor(Qt::green));
        }
        // Cache hits are read-only. Completed writes resume through the saver,
        // rather than regenerating or writing just because a key was looked up.
        while(first.hasPendingWrites())QVERIFY(first.flushPending());
        while(second.hasPendingWrites())QVERIFY(second.flushPending());
        QCOMPARE(first.stats().entries,32);QCOMPARE(second.stats().entries,32);
    }
    void contendedReadUsesCommittedPixels() {
        QTemporaryDir d;
        {ThumbnailDiskCache seed(d.path());for(int i=0;i<32;++i)seed.image(QByteArray::number(i),[]{return ImageResult{TestData::solid(Qt::blue),{}};});}
        QLockFile writer(d.filePath(".lock"));QVERIFY(writer.tryLock());
        ThumbnailDiskCache reader(d.path());int calls=0;
        for(int i=0;i<32;++i) {
            const auto image=reader.image(QByteArray::number(i),[&]{++calls;return ImageResult{TestData::solid(Qt::red),{}};});
            QCOMPARE(image.image.pixelColor(0,0),QColor(Qt::blue));
        }
        QCOMPARE(calls,0);writer.unlock();QCOMPARE(reader.stats().entries,32);
    }
    void warmReadsDoNotWaitForInventory() {
        QTemporaryDir d;
        {ThumbnailDiskCache seed(d.path());seed.image("warm",[]{return ImageResult{TestData::solid(Qt::blue),{}};});}
        ThumbnailDiskCache cache(d.path());std::latch inventoryEntered(1),release(1);int checks=0;
        std::thread maintenance([&]{cache.maintain([&]{if(++checks==2){inventoryEntered.count_down();release.wait();}return false;});});
        inventoryEntered.wait();std::promise<ImageResult> answer;auto ready=answer.get_future();int calls=0;
        std::thread reader([&]{answer.set_value(cache.image("warm",[&]{++calls;return ImageResult{TestData::solid(Qt::red),{}};}));});
        const auto status=ready.wait_for(std::chrono::seconds(2));
        release.count_down();reader.join();maintenance.join();
        QVERIFY(status==std::future_status::ready);QCOMPARE(ready.get().image.pixelColor(0,0),QColor(Qt::blue));QCOMPARE(calls,0);
    }
    void warmReadsPreserveCreationTime() {
        QTemporaryDir d;const QByteArray key="warm";
        const auto name=QString::fromLatin1(QCryptographicHash::hash(key,QCryptographicHash::Sha256).toHex())+".png";
        // Simulate a populated inventory without generating source images.
        const auto pixels=TestData::png(TestData::solid(Qt::blue));
        for(int i=0;i<2048;++i) {
            const auto other=QString::fromLatin1(QCryptographicHash::hash(QByteArray::number(i),QCryptographicHash::Sha256).toHex())+".png";
            QVERIFY(TestData::write(d.filePath(other),pixels));
        }
        QVERIFY(TestData::write(d.filePath(name),pixels));
        QFile file(d.filePath(name));QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.setFileTime(QDateTime::currentDateTime().addSecs(-120),QFileDevice::FileModificationTime));file.close();
        ThumbnailDiskCache cache(d.path());int calls=0;
        QCOMPARE(cache.image(key,[&]{++calls;return ImageResult{TestData::solid(Qt::red),{}};}).image.pixelColor(0,0),QColor(Qt::blue));
        QCOMPARE(calls,0);QVERIFY(QFileInfo(d.filePath(name)).lastModified().secsTo(QDateTime::currentDateTime())>=120);
        const auto inventory=cache.stats();QVERIFY(inventory.error.isEmpty());QCOMPARE(inventory.entries,2049);
        QCOMPARE(inventory.bytes,qint64(2049)*pixels.size());
        // Cancellation and explicit refresh must still bypass a warm hit.
        QVERIFY(cache.image(key,[&]{++calls;return ImageResult{TestData::solid(Qt::red),{}};},[]{return true;}).image.isNull());QCOMPARE(calls,0);
        QCOMPARE(cache.image(key,[&]{++calls;return ImageResult{TestData::solid(Qt::green),{}};},{},true).image.pixelColor(0,0),QColor(Qt::green));QCOMPARE(calls,1);
    }
    void contendedDiskWriteStillReturnsImage() {
        QTemporaryDir d;ThumbnailDiskCache cache(d.path());
        QLockFile lock(d.filePath(".lock"));QVERIFY(lock.tryLock());
        const auto result=cache.image("busy",[]{return ImageResult{TestData::solid(Qt::green),{}};});
        QVERIFY(result.error.isEmpty());QCOMPARE(result.image.pixelColor(0,0),QColor(Qt::green));
        lock.unlock();QCOMPARE(cache.stats().entries,0);
        const auto saved=cache.image("busy",[]{return ImageResult{TestData::solid(Qt::green),{}};});
        QCOMPARE(saved.image.pixelColor(0,0),QColor(Qt::green));QVERIFY(cache.hasPendingWrites());
        QVERIFY(cache.flushPending());QCOMPARE(cache.stats().entries,1);
    }
    void capacityUsesCreationOrder() {
        QTemporaryDir d;ThumbnailDiskCache cache(d.path());cache.configure(true,16);
        QImage noise(768,768,QImage::Format_RGB32);quint32 state=104729;
        for(int y=0;y<noise.height();++y) {
            auto *row=reinterpret_cast<QRgb *>(noise.scanLine(y));
            for(int x=0;x<noise.width();++x) {state^=state<<13;state^=state>>17;state^=state<<5;row[x]=state|0xff000000;}
        }
        const auto generate=[&]{return ImageResult{noise,{}};};
        for(int i=0;i<8;++i) cache.image(QByteArray::number(i),generate);
        cache.image("0",generate);
        for(int i=8;i<12;++i) cache.image(QByteArray::number(i),generate);
        const auto usage=cache.stats();QVERIFY(usage.bytes<=16*1024*1024);QVERIFY(usage.entries<12);QVERIFY(usage.entries>1);
        // Reading the oldest entry above must not make it newer.
        QVERIFY(cache.lookup("0").image.isNull());QVERIFY(cache.lookup("1").image.isNull());
        cache.configure(true,32);
        for(int i=12;i<18;++i) cache.image(QByteArray::number(i),generate);
        QVERIFY(cache.stats().bytes>16*1024*1024);
        cache.configure(true,16);QVERIFY(cache.maintain().isEmpty());QVERIFY(cache.stats().bytes<=16*1024*1024);
    }
    void coverSourceChangesAndPageIdentity() {
        QTemporaryDir d;const auto folder=d.filePath("images");QVERIFY(QDir().mkpath(folder));
        const auto path=folder+"/01.png";QVERIFY(TestData::write(path,TestData::png(TestData::solid(Qt::red))));
        auto cache=std::make_shared<ThumbnailDiskCache>(d.filePath("cache"));
        QCOMPARE(cachedCover(folder,{},cache).image.pixelColor(0,0),QColor(Qt::red));
        QCOMPARE(cachedCover(folder,{},cache).image.pixelColor(0,0),QColor(Qt::red));QCOMPARE(cache->stats().entries,2);
        QVERIFY(TestData::write(path,TestData::png(TestData::solid(Qt::blue,{35,55}))));
        // v2 keys do not include source metadata: a changed cover replaces the same PNG.
        QCOMPARE(cachedCover(folder,{},cache,{},false,0,false,true).image.pixelColor(0,0),QColor(Qt::blue));
        QCOMPARE(cache->stats().entries,2);
        const auto zip=d.filePath("pages.zip");
        QVERIFY(TestData::write(zip,TestData::zip({{"01.png",TestData::png(TestData::solid(Qt::red))},{"02.png",TestData::png(TestData::solid(Qt::green))}})));
        const auto book=Book::open(zip).book;QVERIFY(book);
        QCOMPARE(cachedPageThumbnail(book,0,cache).image.pixelColor(0,0),QColor(Qt::red));
        QCOMPARE(cachedPageThumbnail(book,1,cache).image.pixelColor(0,0),QColor(Qt::green));
        QCOMPARE(cachedCollectionCover(zip,{},{{"cover",1}},false,cache).image.pixelColor(0,0),QColor(Qt::green));
        QCOMPARE(cachedCollectionCover(zip,{},{{"cover",0}},false,cache).image.pixelColor(0,0),QColor(Qt::red));
        const auto before=cache->stats().entries;
        cachedPageThumbnail(book,1,cache);cachedCollectionCover(zip,{},{{"cover",1}},false,cache);
        QCOMPARE(cache->stats().entries,before);
        QVERIFY(TestData::write(zip,TestData::zip({{"01.png",TestData::png(TestData::solid(Qt::blue,{37,59}))},{"02.png",TestData::png(TestData::solid(Qt::yellow,{37,59}))}})));
        const auto updated=Book::open(zip).book;QVERIFY(updated);
        QCOMPARE(cachedPageThumbnail(updated,0,cache,{},false,false,true).image.pixelColor(0,0),QColor(Qt::blue));
        QCOMPARE(cachedPageThumbnail(book,0,cache).image.pixelColor(0,0),QColor(Qt::blue));
    }
    void preferencesAndAppearance() {
        QTemporaryDir d;const auto settings=d.filePath("settings.ini");
        {
            MainWindow w(settings);w.show();bool edited=false;
            QTimer::singleShot(0,&w,[&] {
                auto *dialog=w.findChild<QDialog *>("preferencesDialog");if(!dialog)return;
                auto *mode=dialog->findChild<QComboBox *>("display/appearance");
                auto *enabled=dialog->findChild<QCheckBox *>("cache/thumbnailsEnabled");
                auto *limit=dialog->findChild<QSpinBox *>("cache/thumbnailMegabytes");
                auto *memory=dialog->findChild<QSpinBox *>("cache/megabytes");
                if(mode && enabled && limit && memory) {mode->setCurrentIndex(2);enabled->setChecked(false);limit->setValue(64);memory->setValue(192);edited=true;}
                dialog->accept();
            });
            w.command("preferences")->trigger();QVERIFY(edited);
            QVERIFY(qApp->palette().color(QPalette::Base).lightness()<80);
            auto *grid=w.findChild<ThumbnailGrid *>("zipLibrary");QVERIFY(grid);
            QTRY_COMPARE(grid->palette().color(QPalette::Text),appearancePalette(true).color(QPalette::Text));
            QSettings stored(settings,QSettings::IniFormat);
            QCOMPARE(stored.value("display/appearance").toInt(),2);
            QCOMPARE(stored.value("cache/thumbnailsEnabled").toBool(),false);
            QCOMPARE(stored.value("cache/thumbnailMegabytes").toInt(),64);
            QCOMPARE(stored.value("cache/megabytes").toInt(),192);
            QTimer::singleShot(0,&w,[&] {
                auto *dialog=w.findChild<QDialog *>("preferencesDialog");if(!dialog)return;
                dialog->findChild<QComboBox *>("display/appearance")->setCurrentIndex(1);dialog->reject();
            });
            w.command("preferences")->trigger();QVERIFY(qApp->palette().color(QPalette::Base).lightness()<80);w.close();
        }
        MainWindow restored(settings);QVERIFY(qApp->palette().color(QPalette::Base).lightness()<80);
        setApplicationAppearance(1);QVERIFY(qApp->palette().color(QPalette::Base).lightness()>220);
        restored.close();
    }
    void appearancePreservesImageAndCanvas() {
        PageView view;view.resize(400,400);view.show();
        for(int mode:{1,2}) {
            setApplicationAppearance(mode);QApplication::processEvents();
            view.setPages({TestData::solid(Qt::red)},false,{});QApplication::processEvents();
            const auto snapshot=view.grab().toImage();
            QCOMPARE(snapshot.pixelColor(200,180),QColor(Qt::red));
            QCOMPARE(snapshot.pixelColor(0,0),qApp->palette().color(QPalette::Base));
        }
    }
    void forceRefreshRole() {
        ThumbnailGrid grid;grid.resize(300,300);grid.addItem(new QListWidgetItem("one"));
        grid.setThumbnail(0,TestData::solid(),{});QVERIFY(!grid.item(0)->data(ThumbnailRefreshRole).toBool());
        grid.invalidateThumbnails();QVERIFY(grid.item(0)->data(ThumbnailRefreshRole).toBool());
        grid.setThumbnail(0,TestData::solid(),{});QVERIFY(!grid.item(0)->data(ThumbnailRefreshRole).toBool());
    }
    void clearThroughPreferences() {
        QTemporaryDir d;ThumbnailDiskCache disk;
        disk.image("preferences-clear",[]{return ImageResult{TestData::solid(),{}};});QVERIFY(disk.stats().entries>0);
        MainWindow w(d.filePath("settings.ini"));w.show();bool cleared=false;
        QTimer::singleShot(0,&w,[&] {
            auto *dialog=w.findChild<QDialog *>("preferencesDialog");if(!dialog)return;
            auto *button=dialog->findChild<QPushButton *>("clearThumbnailDiskCache");
            auto *usage=dialog->findChild<QLabel *>("thumbnailCacheUsage");
            if(!button || !usage) {dialog->reject();return;}
            button->click();
            auto *poll=new QTimer(dialog);poll->setInterval(20);
            connect(poll,&QTimer::timeout,dialog,[&,dialog,button,usage,poll] {
                if(button->isEnabled() && usage->text().contains("/ 0枚")) {cleared=true;poll->stop();dialog->reject();}
            });
            QTimer::singleShot(4000,dialog,&QDialog::reject);poll->start();
        });
        w.command("preferences")->trigger();QVERIFY(cleared);QCOMPARE(disk.stats().entries,0);w.close();
    }
    void encodedCacheAccounting() {
        QTemporaryDir d;const auto path=d.filePath("book.zip");
        const auto first=TestData::png(TestData::solid(Qt::red)),second=TestData::png(TestData::solid(Qt::green));
        QVERIFY(TestData::write(path,TestData::zip({{"01.png",first},{"02.png",second}})));
        OpenOptions options;options.fileCacheMiB=7;const auto book=Book::open(path,{},options).book;QVERIFY(book);
        book->clearEncodedCache();QCOMPARE(book->encodedCacheInfo().entries,0);
        QVERIFY(book->encoded(0).error.isEmpty());QVERIFY(book->encoded(1).error.isEmpty());
        const auto info=book->encodedCacheInfo();QCOMPARE(info.entries,2);QCOMPARE(info.limitBytes,7LL*1024*1024);
        QCOMPARE(info.bytes,((first.size()+1023)/1024+(second.size()+1023)/1024)*1024);
        book->clearEncodedCache();QCOMPARE(book->encodedCacheInfo().bytes,0);
    }
    void cacheInformationNoBook() {
        QTemporaryDir d;MainWindow w(d.filePath("settings.ini"));w.show();
        QString state,used,note;bool complete=false;
        QTimer::singleShot(0,&w,[&] {
            auto *dialog=w.findChild<QDialog *>("cacheInfoDialog");if(!dialog)return;
            auto *table=dialog->findChild<QTableWidget *>("cacheInfoTable");
            auto *refresh=dialog->findChild<QPushButton *>("refreshCacheInfo");
            if(!table || !refresh) {dialog->reject();return;}
            auto *poll=new QTimer(dialog);poll->setInterval(10);
            connect(poll,&QTimer::timeout,dialog,[&,dialog,table,refresh,poll] {
                if(!refresh->isEnabled())return;
                state=table->item(0,4)->text();used=table->item(0,1)->text();
                note=dialog->findChild<QLabel *>("cacheInfoNote")->text();
                complete=table->rowCount()==4;poll->stop();dialog->accept();
            });
            QTimer::singleShot(4000,dialog,&QDialog::reject);poll->start();
        });
        w.command("cacheInfo")->trigger();QVERIFY(complete);QCOMPARE(used,QString("—"));
        QCOMPARE(state,QString("本を開いていません"));QVERIFY(note.contains("アプリ全体の使用量ではなく"));
        // Closing before queued I/O completes must also be safe.
        QTimer::singleShot(0,&w,[&] {if(auto *dialog=w.findChild<QDialog *>("cacheInfoDialog"))dialog->reject();});
        w.command("cacheInfo")->trigger();w.close();
    }
    void cacheInformationWithBookAndRefresh() {
        QTemporaryDir d;const auto path=d.filePath("book.zip"),settings=d.filePath("settings.ini");
        QVERIFY(TestData::write(path,TestData::zip({{"01.png",TestData::png(TestData::solid(Qt::red))}})));
        {QSettings s(settings,QSettings::IniFormat);s.setValue("cache/fileMegabytes",7);s.setValue("cache/megabytes",64);
            s.setValue("cache/thumbnailsEnabled",false);s.setValue("cache/thumbnailMegabytes",64);s.setValue("cache/ahead",0);s.setValue("cache/behind",0);}
        ThumbnailDiskCache disk;QVERIFY(disk.clear().isEmpty());
        const auto generate=[] {return ImageResult{TestData::solid(),{}};};disk.image("cache-info-first",generate);
        MainWindow w(settings);w.show();w.openBook(path);QTRY_VERIFY_WITH_TIMEOUT(w.renderReady(),10000);
        QStringList first;QString refreshedCount;int stage=0;
        QTimer::singleShot(0,&w,[&] {
            auto *dialog=w.findChild<QDialog *>("cacheInfoDialog");if(!dialog)return;
            auto *table=dialog->findChild<QTableWidget *>("cacheInfoTable");
            auto *refresh=dialog->findChild<QPushButton *>("refreshCacheInfo");
            if(!table || !refresh) {dialog->reject();return;}
            auto *poll=new QTimer(dialog);poll->setInterval(10);
            connect(poll,&QTimer::timeout,dialog,[&,dialog,table,refresh,poll] {
                if(!refresh->isEnabled())return;
                if(stage==0) {
                    for(int row=0;row<4;++row)for(int col=1;col<5;++col)first.append(table->item(row,col)->text());
                    disk.image("cache-info-second",generate);stage=1;refresh->click();
                } else {refreshedCount=table->item(3,3)->text();stage=2;poll->stop();dialog->accept();}
            });
            QTimer::singleShot(4000,dialog,&QDialog::reject);poll->start();
        });
        w.command("cacheInfo")->trigger();QCOMPARE(stage,2);QCOMPARE(first.size(),16);
        QCOMPARE(first[1],QString("7.0 MiB"));QCOMPARE(first[2],QString("1件"));QCOMPARE(first[3],QString("有効（現在の本）"));
        QCOMPARE(first[5],QString("64.0 MiB"));QCOMPARE(first[6],QString("1ページ"));QCOMPARE(first[9],QString("32.0 MiB"));
        QCOMPARE(first[13],QString("64.0 MiB"));QCOMPARE(first[14],QString("1枚"));QVERIFY(first[15].contains("無効"));
        QCOMPARE(refreshedCount,QString("2枚"));w.close();
    }
};
int main(int argc,char **argv) {
    if (argc==5 && QByteArray(argv[1])=="--cache-interruption-probe") {
        QCoreApplication app(argc,argv); const auto arguments=app.arguments();
        QLockFile lock(arguments[2]+"/.lock"); if(!lock.tryLock(1000))return 2;
        QSaveFile image(arguments[2]+'/'+arguments[3]);
        QSaveFile revision(arguments[2]+"/.revision");
        if(!image.open(QIODevice::WriteOnly) || !revision.open(QIODevice::WriteOnly))return 3;
        if(arguments[4]=="committed") {
            if(!image.write(TestData::png(TestData::solid(Qt::green))) || !image.commit())return 4;
        } else if(image.write("incomplete PNG")<0)return 5;
        if(revision.write("incomplete revision")<0)return 6;
        std::puts("ready"); std::fflush(stdout);
        QThread::sleep(600); return 7;
    }
    if (argc==5 && QByteArray(argv[1])=="--zip-cancel-probe") {
        QCoreApplication app(argc,argv);const auto arguments=app.arguments();
        const int checkpoint=arguments[4].toInt();int checks=0;
        const auto cancel=[&]{return ++checks>=checkpoint;};
        OpenOptions options;if(arguments[2]=="open") options.cancel=cancel;
        const auto opened=Book::open(arguments[3],{},options);
        if(arguments[2]=="open") return checks>=checkpoint && opened.book ? 2 : 0;
        if(!opened.book) return 3;
        const auto data=opened.book->encoded(0,cancel);
        return checks>=checkpoint && data.error.isEmpty() ? 4 : 0;
    }
    if (argc==5 && QByteArray(argv[1])=="--thumbnail-cache-probe") {
        QCoreApplication app(argc,argv);
        QCoreApplication::setOrganizationName("KomaMimicTests");
        QCoreApplication::setApplicationName("Regression");
        const auto arguments=app.arguments();
        if (ThumbnailDiskCache::defaultDirectory()!=arguments[4]) return 2;
        auto cache=std::make_shared<ThumbnailDiskCache>(arguments[3]);
        const auto book=Book::open(arguments[2]).book;
        if (!book || cachedCover(arguments[2],{},cache).image.pixelColor(0,0)!=QColor(Qt::blue)
            || cachedPageThumbnail(book,0,cache).image.pixelColor(0,0)!=QColor(Qt::blue)) return 3;
        return 0;
    }
    return isolatedTestMain<CacheTests>(argc,argv);
}
#include "cache_tests.moc"
