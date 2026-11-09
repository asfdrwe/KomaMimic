// SPDX-License-Identifier: GPL-3.0-only
#include "test_support.h"
#include "thumbnailcache.h"
#include "thumbnailsource.h"
#include "displayjobs.h"
#include <QAction>
#include <QCheckBox>
#include <QDialog>
#include <QSpinBox>
#include <QTimer>
#include <QThread>
#include <QElapsedTimer>
#include <algorithm>

// Keep optional whole-list loading separate from existing persistence budgets.
class ThumbnailPreloadTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {QCoreApplication::setApplicationName("ThumbnailPreloadRegression");}
    void thumbnailPreloadPreferencesSaveCancelAndRestart() {
        QTemporaryDir d;const auto ini=d.filePath("settings.ini");
        {
            MainWindow w(ini);
            for(bool save:{false,true}) {
                bool inspected=false;
                QTimer::singleShot(0,&w,[&] {
                    auto *dialog=w.findChild<QDialog *>("preferencesDialog");QVERIFY(dialog);
                    QTimer::singleShot(2000,dialog,&QDialog::reject);
                    auto *enabled=dialog->findChild<QCheckBox *>("thumbnails/preloadAll");QVERIFY(enabled);
                    auto *limit=dialog->findChild<QSpinBox *>("thumbnails/memoryMegabytes");QVERIFY(limit);
                    QVERIFY(!enabled->isChecked());QVERIFY(!limit->isEnabled());QCOMPARE(limit->value(),128);
                    enabled->setChecked(true);QVERIFY(limit->isEnabled());limit->setValue(64);inspected=true;
                    if(save)dialog->accept();else dialog->reject();
                });
                w.command("preferences")->trigger();QVERIFY(inspected);
                QCOMPARE(w.m_settings->value("thumbnails/preloadAll",false).toBool(),save);
            }
        }
        MainWindow restored(ini);QVERIFY(restored.m_settings->value("thumbnails/preloadAll").toBool());
        QCOMPARE(restored.m_settings->value("thumbnails/memoryMegabytes").toInt(),64);
    }
    void thumbnailPreloadUsesCacheAndRetainsScrollReturn_data() {
        QTest::addColumn<bool>("pages");QTest::addColumn<bool>("warm");QTest::addColumn<bool>("enabled");QTest::addColumn<bool>("disk");QTest::addColumn<int>("budget");
        for(bool pages:{false,true})for(bool warm:{false,true})for(bool enabled:{false,true})
            QTest::newRow(qPrintable(QString("%1-%2-%3").arg(pages?"pages":"covers",warm?"warm":"cold",enabled?"on":"off")))<<pages<<warm<<enabled<<true<<64;
        QTest::newRow("covers-without-disk")<<false<<false<<true<<false<<64;
        QTest::newRow("pages-without-disk")<<true<<false<<true<<false<<64;
        QTest::newRow("covers-bounded")<<false<<false<<true<<true<<16;
        QTest::newRow("pages-bounded")<<true<<false<<true<<true<<16;
    }
    void thumbnailPreloadUsesCacheAndRetainsScrollReturn() {
        QFETCH(bool,pages);QFETCH(bool,warm);QFETCH(bool,enabled);QFETCH(bool,disk);QFETCH(int,budget);
        QTemporaryDir d;const auto books=d.filePath("books"),ini=d.filePath("settings.ini"),zip=books+"/sample.zip";
        QVERIFY(QDir().mkpath(books));const auto pixels=TestData::png(TestData::solid(Qt::blue,{48,64}));
        QVector<TestData::ZipEntry> entries;QStringList paths;
        for(int i=0;i<40;++i) {
            const auto name=QString("%1.png").arg(i,3,10,QChar('0'));
            if(pages)entries.append({name.toUtf8(),pixels});else {paths.append(books+'/'+name);QVERIFY(TestData::write(paths.last(),pixels));}
        }
        std::shared_ptr<const Book> book;
        if(pages){QVERIFY(TestData::write(zip,TestData::zip(entries)));book=Book::open(zip).book;QVERIFY(book);}
        auto cache=std::make_shared<ThumbnailDiskCache>(d.filePath("cache"));
        cache->configure(disk,512);
        if(warm)for(int i=0;i<40;++i)QVERIFY(!(pages?cachedPageThumbnail(book,i,cache):cachedCover(paths[i],{},cache)).image.isNull());
        {QSettings s(ini,QSettings::IniFormat);if(enabled)s.setValue("thumbnails/preloadAll",true);s.setValue("thumbnails/columns",2);s.setValue("thumbnails/memoryMegabytes",budget);s.setValue("thumbnails/mouseSelection",false);}
        const auto reads=BookActivity::imageRequests.load(),gens=ThumbnailActivity::generations.load();
        MainWindow w(ini);w.m_thumbnailDiskCache=cache;cache->setDeferredWrites(true);w.resize(480,320);w.show();
        if(pages)w.loadBook(zip,MainWindow::ViewMode::Pages);else w.openDirectory(books);
        auto *grid=pages?w.m_pages:w.m_library;
        QTRY_VERIFY(grid->count()==40 && !w.isLoading() && (pages || w.directoryReady()));grid->doItemsLayout();
        const auto visibleReady=[&]{const auto rows=grid->visibleRows();if(rows.isEmpty())return false;for(int row:rows)if(!grid->item(row)->data(ThumbnailRoleReady).toBool())return false;return true;};
        QTRY_VERIFY(visibleReady());QVERIFY(!grid->visibleRows().contains(39));
        const auto allReady=[&]{for(int i=0;i<40;++i)if(!grid->item(i)->data(ThumbnailRoleReady).toBool())return false;return true;};
        if(!enabled) {
            QTest::qWait(600);QVERIFY(!grid->item(39)->data(ThumbnailRoleReady).toBool());
            QVERIFY(!w.m_settings->value("thumbnails/preloadAll",false).toBool());return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(w.m_thumbnailPreload[pages?1:0].next,40,15000);
        QVERIFY(grid->offscreenThumbnailBytes()<=qint64(budget)*1024*1024);
        if(budget==16) {
            QVERIFY(!allReady());const auto completedReads=BookActivity::imageRequests.load(),completedGens=ThumbnailActivity::generations.load();
            QTest::qWait(600);QCOMPARE(BookActivity::imageRequests.load(),completedReads);QCOMPARE(ThumbnailActivity::generations.load(),completedGens);return;
        }
        QVERIFY(allReady());
        if(warm){QCOMPARE(BookActivity::imageRequests.load(),reads);QCOMPARE(ThumbnailActivity::generations.load(),gens);}
        const auto afterReads=BookActivity::imageRequests.load(),afterGens=ThumbnailActivity::generations.load();
        grid->scrollToItem(grid->item(39),QAbstractItemView::PositionAtBottom);QTRY_VERIFY(grid->visibleRows().contains(39));QVERIFY(visibleReady());
        grid->scrollToItem(grid->item(0),QAbstractItemView::PositionAtTop);QTRY_VERIFY(grid->visibleRows().contains(0));QVERIFY(allReady());
        QTest::qWait(250);QCOMPARE(BookActivity::imageRequests.load(),afterReads);QCOMPARE(ThumbnailActivity::generations.load(),afterGens);
        if(!disk){QVERIFY(!cache->hasPendingWrites());QCOMPARE(cache->stats().entries,0);}
    }
    void thumbnailMemoryLimitAndDisablePreserveVisibleImages() {
        ThumbnailGrid grid;grid.resize(400,280);
        for(int i=0;i<300;++i)grid.addItem(new QListWidgetItem(grid.placeholderIcon(),QString::number(i)));
        grid.show();grid.doItemsLayout();QTest::qWait(30);const auto image=TestData::solid(Qt::green,{240,320});
        grid.setMemoryRetention(true,128LL*1024*1024);
        for(int i=0;i<300;++i)grid.setThumbnail(i,image,{});
        for(int i=0;i<300;++i)QVERIFY(grid.item(i)->data(ThumbnailRoleReady).toBool());
        grid.scrollToItem(grid.item(299),QAbstractItemView::PositionAtBottom);QTRY_VERIFY(grid.visibleRows().contains(299));
        for(int i=0;i<300;++i)QVERIFY(grid.item(i)->data(ThumbnailRoleReady).toBool());
        grid.setMemoryRetention(true,2LL*1024*1024);QVERIFY(grid.offscreenThumbnailBytes()<=2LL*1024*1024);
        for(int row:grid.visibleRows())QVERIFY(grid.item(row)->data(ThumbnailRoleReady).toBool());
        grid.setMemoryRetention(true,128LL*1024*1024);for(int i=0;i<300;++i)grid.setThumbnail(i,image,{});
        grid.setMemoryRetention(false,128LL*1024*1024);
        int ready=0;for(int i=0;i<300;++i)ready+=grid.item(i)->data(ThumbnailRoleReady).toBool();
        QCOMPARE(ready,std::max(160,int(grid.visibleRows().size())+20));
        for(int row:grid.visibleRows())QVERIFY(grid.item(row)->data(ThumbnailRoleReady).toBool());
        grid.clearItems();QCOMPARE(grid.offscreenThumbnailBytes(),0);
    }
    void thumbnailPreloadCancellation_data() {
        QTest::addColumn<QString>("action");
        for(const auto *name:{"scroll","mode","folder","book","disable","foreground"})QTest::newRow(name)<<QString(name);
    }
    void thumbnailPreloadCancellation() {
        QFETCH(QString,action);QTemporaryDir d;MainWindow w(d.filePath("settings.ini"));w.resize(480,320);w.show();
        w.m_settings->setValue("thumbnails/preloadAll",true);w.m_settings->setValue("thumbnails/mouseSelection",false);
        auto *grid=w.m_library;for(int i=0;i<100;++i)grid->addItem(new QListWidgetItem(grid->placeholderIcon(),QString::number(i)));
        grid->doItemsLayout();
        for(int i=0;i<100;++i)grid->setThumbnail(i,TestData::solid(),{});
        QTest::qWait(30);
        w.requestThumbnails(grid);w.m_thumbnailPreload[0].next=100;
        const auto started=std::make_shared<std::atomic<bool>>(false),cancelled=std::make_shared<std::atomic<bool>>(false),delivered=std::make_shared<std::atomic<bool>>(false);
        w.m_displayJobs->submit<bool>(DisplayJobs::Lane::Background,"covers-preload","covers-preload",
            [started,cancelled](const auto &cancel){*started=true;QElapsedTimer t;t.start();while(!cancel() && t.elapsed()<3000)QThread::msleep(1);*cancelled=cancel();return true;},
            [delivered](bool,bool obsolete){*delivered=!obsolete;});
        QTRY_VERIFY(started->load());
        if(action=="scroll")grid->scrollToItem(grid->item(99),QAbstractItemView::PositionAtBottom);
        else if(action=="mode"){w.setMode(MainWindow::ViewMode::Reader);QCOMPARE(w.m_thumbnailPreload[0].next,0);}
        else if(action=="folder")w.openDirectory(d.path());
        else if(action=="book") {const auto path=d.filePath("sample.zip");QVERIFY(TestData::write(path,TestData::zip({{"1.png",TestData::png(TestData::solid())}})));w.openBook(path);}
        else if(action=="disable"){w.m_settings->setValue("thumbnails/preloadAll",false);w.restoreThumbnailSettings();}
        else w.m_displayJobs->submit<bool>(DisplayJobs::Lane::Cache,"test","foreground",[](const auto &){return true;},[](bool,bool){});
        QTRY_VERIFY_WITH_TIMEOUT(cancelled->load(),1500);w.cancelThumbnailPreload();w.m_displayJobs->waitForDone();QCoreApplication::processEvents();QVERIFY(!delivered->load());
    }
};
ISOLATED_TEST_MAIN(ThumbnailPreloadTests)
#include "thumbnail_preload_tests.moc"
