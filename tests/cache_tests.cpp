// SPDX-License-Identifier: GPL-3.0-only
#include "test_support.h"
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
#include <thread>
#include <vector>
#include <cstdio>

class CacheTests:public QObject {
    Q_OBJECT
private slots:
    void cleanup() {setApplicationAppearance(0);}
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
        for (const auto &name:QDir(cachePath).entryList({"*.png"},QDir::Files))
            QVERIFY(TestData::write(QDir(cachePath).filePath(name),TestData::png(TestData::solid(Qt::blue))));
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
        QCOMPARE(cachedCover(folder,{},cache).image.pixelColor(0,0),QColor(Qt::red));
        QVERIFY(QFile::remove(folder+"/2.zip"));
        QCOMPARE(cachedCover(folder,{},cache).image.pixelColor(0,0),QColor(Qt::green));
        QVERIFY(QFile::remove(folder+"/10.zip"));
        QCOMPARE(cachedCover(folder,{},cache).image.pixelColor(0,0),QColor(Qt::blue));
        for (int i=1;i<=16;++i)
            QVERIFY(TestData::write(folder+QString("/%1.zip").arg(i,2,10,QChar('0')),"invalid"));
        QVERIFY(TestData::write(folder+"/17.zip",red));
        QVERIFY(cachedCover(folder,{},cache).image.isNull());
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
        QVERIFY(QFileInfo::exists(directory.filePath(".revision")));
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
        QVERIFY(good.load());QCOMPARE(first.stats().entries,32);QCOMPARE(second.stats().entries,32);
    }
    void capacityAndRecentUse() {
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
        bool regenerated=false;
        cache.image("0",[&]{regenerated=true;return generate();});QVERIFY(!regenerated);
        cache.image("1",[&]{regenerated=true;return generate();});QVERIFY(regenerated);
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
        QCOMPARE(cachedCover(folder,{},cache).image.pixelColor(0,0),QColor(Qt::red));QCOMPARE(cache->stats().entries,1);
        QVERIFY(TestData::write(path,TestData::png(TestData::solid(Qt::blue,{35,55}))));
        QCOMPARE(cachedCover(folder,{},cache).image.pixelColor(0,0),QColor(Qt::blue));QCOMPARE(cache->stats().entries,2);
        const auto zip=d.filePath("pages.zip");
        QVERIFY(TestData::write(zip,TestData::zip({{"01.png",TestData::png(TestData::solid(Qt::red))},{"02.png",TestData::png(TestData::solid(Qt::green))}})));
        const auto book=Book::open(zip).book;QVERIFY(book);
        QCOMPARE(cachedPageThumbnail(book,0,cache).image.pixelColor(0,0),QColor(Qt::red));
        QCOMPARE(cachedPageThumbnail(book,1,cache).image.pixelColor(0,0),QColor(Qt::green));
        QCOMPARE(cachedCollectionCover(zip,{},{{"cover",1}},false,cache).pixelColor(0,0),QColor(Qt::green));
        QCOMPARE(cachedCollectionCover(zip,{},{{"cover",0}},false,cache).pixelColor(0,0),QColor(Qt::red));
        const auto before=cache->stats().entries;
        cachedPageThumbnail(book,1,cache);cachedCollectionCover(zip,{},{{"cover",1}},false,cache);
        QCOMPARE(cache->stats().entries,before);
        QVERIFY(TestData::write(zip,TestData::zip({{"01.png",TestData::png(TestData::solid(Qt::blue,{37,59}))},{"02.png",TestData::png(TestData::solid(Qt::yellow,{37,59}))}})));
        const auto updated=Book::open(zip).book;QVERIFY(updated);
        QCOMPARE(cachedPageThumbnail(updated,0,cache).image.pixelColor(0,0),QColor(Qt::blue));
        QVERIFY(!cachedPageThumbnail(book,0,cache).error.isEmpty());
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
