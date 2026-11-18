// SPDX-License-Identifier: GPL-3.0-only
#include "test_support.h"
#include "thumbnailcache.h"
#include "thumbnailsource.h"
#include "displayjobs.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QLockFile>
#include <QProcess>
#include <QFileSystemWatcher>
#include <QSignalBlocker>
#include <QAction>
#include <latch>
#include <thread>
#include <filesystem>

class CacheDisplayTests : public QObject {
    Q_OBJECT
public:
    static int savedImages(const QString &root,int count,bool create) {
        ThumbnailDiskCache cache(root);cache.configure(true,8192);int generated=0;
        for(int i=0;i<count;++i){const auto result=cache.image(QByteArray::number(i),[&]{++generated;return ImageResult{TestData::solid(create?Qt::blue:Qt::red),{}};});if(result.image.isNull() || result.image.pixelColor(0,0)!=QColor(Qt::blue))return 2;}
        return generated==(create?count:0)?0:3;
    }
    static int defaultCache(const QString &path,bool seed) {
        auto cache=std::make_shared<ThumbnailDiskCache>();cache->configure(true,8192);
        const auto book=Book::open(path).book;if(!book)return 5;
        const auto generated=ThumbnailActivity::generations.load(),reads=BookActivity::imageRequests.load();
        const auto cover=cachedCover(path,{},cache,{},false,0,false,true),page=cachedPageThumbnail(book,0,cache,{},false,false,true);
        if(cover.image.isNull() || page.image.isNull() || cover.image.pixelColor(0,0)!=QColor(Qt::blue) || page.image.pixelColor(0,0)!=QColor(Qt::blue))return 6;
        if(!seed && (ThumbnailActivity::generations.load()!=generated || BookActivity::imageRequests.load()!=reads))return 7;
        QFile output;if(!output.open(stdout,QIODevice::WriteOnly))return 8;output.write(QJsonDocument(QJsonObject{{"directory",cache->directory()}}).toJson(QJsonDocument::Compact));return 0;
    }
private slots:
    void startupPreservesLargeDiskCapacity() {
        QTemporaryDir d;const auto ini=d.filePath("settings.ini");
        {QSettings s(ini,QSettings::IniFormat);s.setValue("cache/thumbnailMegabytes",2147483648LL);}
        MainWindow w(ini);QCOMPARE(w.m_thumbnailDiskCache->limitBytes(),2147483648LL*1024*1024);
    }
    void initTestCase() {QCoreApplication::setApplicationName("CacheDisplayRegression");}
    void writingPixelsRemainReadableAfterLruEviction_data() {
        QTest::addColumn<bool>("replacement");QTest::newRow("new")<<false;QTest::newRow("replacement")<<true;
    }
    void writingPixelsRemainReadableAfterLruEviction() {
        QFETCH(bool,replacement);QTemporaryDir d;const auto size=QSize(320,480);
        const auto disk=TestData::png(TestData::solid(Qt::red,size));
        const auto file=[&](const QByteArray &key){return d.filePath(QString::fromLatin1(QCryptographicHash::hash(key,QCryptographicHash::Sha256).toHex())+".png");};
        for(int i=0;i<64;++i)QVERIFY(TestData::write(file(QByteArray::number(i)),disk));
        if(replacement)QVERIFY(TestData::write(file("generated"),disk));
        ThumbnailDiskCache cache(d.path());cache.configure(true,8192);QLockFile lock(d.filePath(".lock"));QVERIFY(lock.tryLock(0));
        std::thread writer([&]{cache.image("generated",[&]{return ImageResult{TestData::solid(Qt::blue,size),{}};},{},true);});
        const auto cleanup=qScopeGuard([&]{lock.unlock();writer.join();});
        QTRY_VERIFY(!cache.lookupMemory("generated").image.isNull());
        for(int i=0;i<64;++i)QVERIFY(!cache.lookup(QByteArray::number(i)).image.isNull());
        int generated=0;const auto reused=cache.image("generated",[&]{++generated;return ImageResult{TestData::solid(Qt::green,size),{}};});
        QCOMPARE(generated,0);QCOMPARE(reused.image.pixelColor(0,0),QColor(Qt::blue));
    }
    void generatedThumbnailsSurviveRestart_data() {
        QTest::addColumn<int>("count");QTest::newRow("165-images")<<165;QTest::newRow("322-images")<<322;
    }
    void generatedThumbnailsSurviveRestart() {
        QFETCH(int,count);QTemporaryDir d;const auto root=d.filePath("cache");
        for(const auto &mode:{"create","reuse"}){QProcess process;process.start(QCoreApplication::applicationFilePath(),{"--saved-images-probe",root,QString::number(count),mode});QVERIFY(process.waitForFinished(90000));QVERIFY2(process.exitCode()==0,process.readAllStandardError().constData());}
        QCOMPARE(QDir(root).entryList({"*.png"},QDir::Files).size(),count);
        QVERIFY(QDir(root).entryList(QDir::Dirs|QDir::NoDotAndDotDot).isEmpty());
    }
    void onlyNewerListedModificationTimeRegenerates() {
        QTemporaryDir d;const auto path=d.filePath("page.png"),root=d.filePath("cache");
        QVERIFY(TestData::write(path,TestData::png(TestData::solid(Qt::blue))));auto cache=std::make_shared<ThumbnailDiskCache>(root);
        const auto image=cachedCover(path,{},cache);QVERIFY(!image.image.isNull());const auto names=QDir(root).entryList({"*.png"},QDir::Files);QCOMPARE(names.size(),1);
        const auto saved=root+'/'+names.first();const auto timestamp=QFileInfo(saved).lastModified();const auto count=ThumbnailActivity::generations.load();
        QVERIFY(TestData::write(path,"not an image"));
        QJsonObject observed{{"modified",QString::number(image.cacheCreatedMs)},{"bytes","999999"},{"resolved","different"}};
        QCOMPARE(cachedCover(path,{},cache,{},false,0,false,true,0,observed).image.pixelColor(0,0),QColor(Qt::blue));
        QCOMPARE(ThumbnailActivity::generations.load(),count);QCOMPARE(QFileInfo(saved).lastModified(),timestamp);
        QVERIFY(TestData::write(path,TestData::png(TestData::solid(Qt::red))));
        QFile file(saved);QVERIFY(file.open(QIODevice::ReadWrite));QVERIFY(file.setFileTime(timestamp.addSecs(-120),QFileDevice::FileModificationTime));file.close();
        cache=std::make_shared<ThumbnailDiskCache>(root);observed=thumbnailEntry(QFileInfo(path),QFileInfo(path).absoluteDir().canonicalPath());
        QCOMPARE(cachedCover(path,{},cache,{},false,0,false,true,0,observed).image.pixelColor(0,0),QColor(Qt::red));QCOMPARE(ThumbnailActivity::generations.load(),count+1);
        const auto next=ThumbnailActivity::generations.load();QVERIFY(!cachedCover(path,{},cache,{},false,0,false,true,0,observed).image.isNull());QCOMPARE(ThumbnailActivity::generations.load(),next);
    }
    void folderUpdateReusesAnAlreadyUpdatedImageCache() {
        QTemporaryDir d;const auto folder=d.filePath("folder"),source=folder+"/page.png";QVERIFY(QDir().mkpath(folder));
        QVERIFY(TestData::write(source,TestData::png(TestData::solid(Qt::blue))));auto cache=std::make_shared<ThumbnailDiskCache>(d.filePath("cache"));
        const auto original=cachedCover(folder,{},cache);QVERIFY(!original.image.isNull());QTest::qWait(5);
        QVERIFY(TestData::write(source,TestData::png(TestData::solid(Qt::red))));QCOMPARE(cachedCover(source,{},cache,{},true).image.pixelColor(0,0),QColor(Qt::red));
        const auto generated=ThumbnailActivity::generations.load();
        const auto observed=QJsonObject{{"directory",true},{"_sourceModified",QString::number(QFileInfo(source).lastModified().toMSecsSinceEpoch())}};
        QCOMPARE(cachedCover(folder,{},cache,{},false,0,false,true,0,observed).image.pixelColor(0,0),QColor(Qt::red));QCOMPARE(ThumbnailActivity::generations.load(),generated);
    }
    void visibleGenerationRespectsConcurrencyAndCacheStillWins_data() {
        QTest::addColumn<QString>("scope");QTest::addColumn<int>("count");
        for(const auto &scope:{QString("covers"),QString("pages")})for(int count:{0,1,2,3,4})
            QTest::newRow(qPrintable(scope+QString::number(count)))<<scope<<count;
    }
    void visibleGenerationRespectsConcurrencyAndCacheStillWins() {
        QFETCH(QString,scope);QFETCH(int,count);
        QTemporaryDir d;ThumbnailDiskCache cache(d.path());cache.image("saved",[]{return ImageResult{TestData::solid(Qt::blue),{}};});
        DisplayJobs jobs;QCOMPARE(jobs.thumbnailConcurrency(),1);if(count)jobs.setThumbnailConcurrency(count);
        const int limit=count?count:1;
        std::latch release(1);std::atomic<int> entered=0;int finished=0;bool displayed=false;auto cleanup=qScopeGuard([&]{release.count_down();});
        for(int i=0;i<limit+1;++i)jobs.submit<int>(DisplayJobs::Lane::Display,scope,QString::number(i),[&](const auto &){++entered;release.wait();return 1;},[&](int,bool cancelled){QVERIFY(!cancelled);++finished;});
        QTRY_COMPARE(entered.load(),limit);QTest::qWait(50);QCOMPARE(entered.load(),limit);
        jobs.submit<ImageResult>(DisplayJobs::Lane::Cache,scope,"saved",[&](const auto &cancel){return cache.lookup("saved",cancel);},[&](ImageResult image,bool cancelled){displayed=!cancelled && image.image.pixelColor(0,0)==QColor(Qt::blue);});
        QTRY_VERIFY(displayed);QCOMPARE(finished,0);release.count_down();cleanup.dismiss();QTRY_COMPARE(finished,limit+1);
    }
    void concurrencyChangesKeepRunningJobs() {
        DisplayJobs jobs;std::latch release(1);std::atomic<int> entered=0;int finished=0;auto cleanup=qScopeGuard([&]{release.count_down();});
        for(int i=0;i<3;++i)jobs.submit<int>(DisplayJobs::Lane::Display,"covers",QString::number(i),[&](const auto &cancel){++entered;release.wait();return cancel()?0:1;},[&](int value,bool cancelled){QVERIFY(!cancelled);QCOMPARE(value,1);++finished;});
        QTRY_COMPARE(entered.load(),1);jobs.setThumbnailConcurrency(2);QTRY_COMPARE(entered.load(),2);
        jobs.setThumbnailConcurrency(1);QTest::qWait(50);QCOMPARE(entered.load(),2);QCOMPARE(finished,0);
        release.count_down();cleanup.dismiss();QTRY_COMPARE(finished,3);
    }
    void symbolicLinkCacheRemainsReadable_data() {
        QTest::addColumn<bool>("directory");QTest::newRow("directory")<<true;QTest::newRow("file")<<false;
    }
    void symbolicLinkCacheRemainsReadable() {
        QFETCH(bool,directory);QTemporaryDir d;const auto real=d.filePath("real"),alias=d.filePath("alias");QVERIFY(QDir().mkpath(real));
        const auto name=QString::fromLatin1(QCryptographicHash::hash("linked",QCryptographicHash::Sha256).toHex())+".png";
        QVERIFY(TestData::write(real+'/'+name,TestData::png(TestData::solid(Qt::blue))));
        const auto native=[](const QString &path){
#ifdef Q_OS_WIN
            return std::filesystem::path(path.toStdWString());
#else
            return std::filesystem::path(QFile::encodeName(path).constData());
#endif
        };
        std::error_code error;if(directory)std::filesystem::create_directory_symlink(native(real),native(alias),error);
        else {QVERIFY(QDir().mkpath(alias));std::filesystem::create_symlink(native(real+'/'+name),native(alias+'/'+name),error);}
        if(error)QSKIP(qPrintable(QString::fromStdString(error.message())));
        ThumbnailDiskCache cache(alias);int generated=0;const auto image=cache.image("linked",[&]{++generated;return ImageResult{TestData::solid(Qt::red),{}};});QCOMPARE(image.image.pixelColor(0,0),QColor(Qt::blue));QCOMPARE(generated,0);
    }
    void defaultCacheSurvivesVersionExecutableAndWorkingDirectoryChanges() {
        QTemporaryDir d;const auto source=d.filePath("book.zip");
        QVERIFY(TestData::write(source,TestData::zip({{"page.png",TestData::png(TestData::solid(Qt::blue))}})));
        QString root;
        const auto run=[&](const QString &exe,const QString &version,bool seed,const QString &cwd) {
            QProcess process;process.setWorkingDirectory(cwd);
            auto env=QProcessEnvironment::systemEnvironment();env.insert("PATH",QCoreApplication::applicationDirPath()+QDir::listSeparator()+env.value("PATH"));process.setProcessEnvironment(env);
            process.start(exe,{"--default-cache-probe",source,seed?"seed":"reuse",version});
            if(!process.waitForFinished(60000) || process.exitCode()!=0){qWarning()<<process.readAllStandardError()<<process.readAllStandardOutput()<<process.exitCode();return false;}
            const auto directory=QJsonDocument::fromJson(process.readAllStandardOutput()).object().value("directory").toString();
            if(directory.isEmpty())return false;
            if(root.isEmpty())root=directory;
            return directory==root;
        };
        QVERIFY(run(QCoreApplication::applicationFilePath(),"1.6.0",true,d.path()));
        QVERIFY(TestData::write(source,TestData::zip({{"page.png",TestData::png(TestData::solid(Qt::red,{43,67}))}})));
        const auto relocated=d.filePath("other-location");QVERIFY(QDir().mkpath(relocated));
        const auto executable=relocated+"/different-name"+
#ifdef Q_OS_WIN
            ".exe";
#else
            "";
#endif
        QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(),executable));
        QVERIFY(run(executable,"99.0.0",false,relocated));
#ifdef Q_OS_LINUX
        // Exercise the actual packaging launcher, with the same Qt runtime.
        QFile packaging(QFileInfo(QString::fromUtf8(__FILE__)).absoluteDir().filePath("../tools/build_appimage.py"));QVERIFY(packaging.open(QIODevice::ReadOnly));
        const auto script=packaging.readAll();const QByteArray marker="APP_RUN = '''";const auto begin=script.indexOf(marker)+marker.size(),end=script.indexOf("'''",begin);QVERIFY(begin>=marker.size() && end>begin);
        const auto appDir=d.filePath("different-version.AppDir");QVERIFY(QDir().mkpath(appDir+"/usr/bin"));
        QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(),appDir+"/usr/bin/komamimic"));
        const auto launcher=appDir+"/AppRun";QVERIFY(TestData::write(launcher,script.mid(begin,end-begin)));QVERIFY(QFile::setPermissions(launcher,QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner));
        QVERIFY(run(launcher,"100.0.0",false,d.path()));
#endif
    }
    void lateCompletedCacheBeatsQueuedSource_data() {
        QTest::addColumn<bool>("pages");QTest::addColumn<bool>("disk");
        QTest::newRow("cover-memory")<<false<<false;QTest::newRow("page-memory")<<true<<false;
        QTest::newRow("cover-disk")<<false<<true;QTest::newRow("page-disk")<<true<<true;
    }
    void lateCompletedCacheBeatsQueuedSource() {
        QFETCH(bool,pages);QFETCH(bool,disk);QTemporaryDir d;const auto books=d.filePath("books");QVERIFY(QDir().mkpath(books));
        const auto png=TestData::png(TestData::solid(Qt::blue));const auto path=books+(pages?"/book.zip":"/page.png");
        auto data=png;if(pages){const QVector<TestData::ZipEntry> entries{{"page.png",png}};data=TestData::zip(entries);}QVERIFY(TestData::write(path,data));
        MainWindow window(d.filePath("settings.ini"));window.show();if(pages)window.loadBook(path,MainWindow::ViewMode::Pages);else window.openDirectory(books);
        auto *grid=pages?window.m_pages:window.m_library;QTRY_VERIFY(window.requestReady() && grid->count()==1 && grid->item(0)->data(ThumbnailRoleReady).toBool());
        QSignalBlocker layout(grid),notices(window.m_directoryWatcher);window.m_directoryRefreshTimer->stop();
        window.m_displayJobs->retain(pages?"pages":"covers");auto cache=std::make_shared<ThumbnailDiskCache>(d.filePath("empty-cache"));window.m_thumbnailDiskCache=cache;
        std::latch release(1);std::atomic<bool> entered=false;auto cleanup=qScopeGuard([&]{release.count_down();});
        window.m_displayJobs->submit<int>(DisplayJobs::Lane::Display,"test-hold","held",[&](const auto &){entered=true;release.wait();return 0;},[](int,bool){});QTRY_VERIFY(entered.load());
        auto *item=grid->item(0);item->setData(ThumbnailRoleReady,false);item->setData(ThumbnailNextCheckRole,0);item->setData(ThumbnailCacheNextCheckRole,0);
        const auto id=QString("%1/%2/0").arg(pages?"pages":"covers").arg(pages?window.m_pageGeneration:window.m_libraryGeneration);
        window.requestThumbnails(grid);QTRY_VERIFY(window.m_displayJobs->contains(id+"/generate"));
        const auto producer=pages?cachedPageThumbnail(window.m_book,0,cache):cachedCover(path,{},cache);QVERIFY(!producer.image.isNull());
        if(disk) {
            const auto png=TestData::png(TestData::solid(Qt::red,{320,480}));
            for(int i=0;i<64;++i){const auto key="warm/"+QByteArray::number(i);const auto name=QCryptographicHash::hash(key,QCryptographicHash::Sha256).toHex()+".png";QVERIFY(TestData::write(cache->directory()+'/'+QString::fromLatin1(name),png));QVERIFY(!cache->lookup(key).image.isNull());}
        }
        const auto generated=ThumbnailActivity::generations.load(),reads=BookActivity::imageRequests.load();
        window.requestThumbnails(grid);QTRY_VERIFY_WITH_TIMEOUT(item->data(ThumbnailRoleReady).toBool(),2000);
        const auto image=item->icon().pixmap(64,64).toImage();QCOMPARE(image.pixelColor(image.rect().center()),QColor(Qt::blue));
        QCOMPARE(ThumbnailActivity::generations.load(),generated);QCOMPARE(BookActivity::imageRequests.load(),reads);
        window.m_displayJobs->retain(pages?"pages":"covers");release.count_down();cleanup.dismiss();window.close();
    }
};
int main(int argc,char **argv) {
    if(argc==5 && QByteArray(argv[1])=="--saved-images-probe") {QCoreApplication app(argc,argv);return CacheDisplayTests::savedImages(QString::fromLocal8Bit(argv[2]),QByteArray(argv[3]).toInt(),QByteArray(argv[4])=="create");}
    if(argc==5 && QByteArray(argv[1])=="--default-cache-probe") {
        QCoreApplication app(argc,argv);QCoreApplication::setOrganizationName("KomaMimicTests");QCoreApplication::setApplicationName("CacheDisplayRegression");QCoreApplication::setApplicationVersion(QString::fromLocal8Bit(argv[4]));
        return CacheDisplayTests::defaultCache(QString::fromLocal8Bit(argv[2]),QByteArray(argv[3])=="seed");
    }
    return isolatedTestMain<CacheDisplayTests>(argc,argv);
}
#include "cache_display_tests.moc"
