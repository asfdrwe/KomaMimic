// SPDX-License-Identifier: GPL-3.0-only
#include "test_support.h"
#include "thumbnailcache.h"
#include "displayjobs.h"
#include <latch>
#include "thumbnailsource.h"
#include "scriptreader.h"
#include "librarystore.h"
#include <QAction>
#include <QDialog>
#include <QSpinBox>
#include <QTimer>
#include <QDateTime>
#include <QElapsedTimer>
#include <QProcess>
#include <QLockFile>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QFileSystemWatcher>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <algorithm>
#include <filesystem>

namespace {
QString makeSymbolicLink(const QString &source,const QString &link,bool directory=false) {
    std::error_code error;
#ifdef Q_OS_WIN
    const std::filesystem::path target(source.toStdWString()),alias(link.toStdWString());
#else
    const std::filesystem::path target(QFile::encodeName(source).constData()),alias(QFile::encodeName(link).constData());
#endif
    if(directory)std::filesystem::create_directory_symlink(target,alias,error);
    else std::filesystem::create_symlink(target,alias,error);
    return error?QString::fromStdString(error.message()):QString{};
}
QJsonObject savedThumbnails(const QString &directory) {
    QJsonObject result;
    for(const auto &name:QDir(directory).entryList({"*.png"},QDir::Files)) {
        QFile file(QDir(directory).filePath(name));
        if(!file.open(QIODevice::ReadOnly))return {};
        result[name]=QString::fromLatin1(QCryptographicHash::hash(file.readAll(),QCryptographicHash::Sha256).toHex())+":"+
            QString::number(QFileInfo(file).lastModified().toMSecsSinceEpoch());
    }
    return result;
}
}

// Keep the filesystem-heavy scroll fixtures outside cache_tests' timeout.
class ThumbnailTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        // Native Windows/macOS cache paths ignore XDG_CACHE_HOME. A distinct
        // app name prevents cache_tests' explicit clears from racing this suite.
        QCoreApplication::setApplicationName("ThumbnailRegression");
    }
    void symbolicLinkModeSwitchesKeepSavedCover_data() {
        QTest::addColumn<bool>("legacy");
        QTest::newRow("current-stamp")<<false;
        QTest::newRow("legacy-link-stamp")<<true;
    }
    void symbolicLinkModeSwitchesKeepSavedCover() {
        QFETCH(bool,legacy);
        QTemporaryDir d;const auto real=d.filePath("real"),aliases=d.filePath("aliases");
        QVERIFY(QDir().mkpath(real));QVERIFY(QDir().mkpath(aliases));
        const auto source=real+"/book.zip",link=aliases+"/book.zip";
        const auto png=TestData::png(TestData::solid(Qt::red));
        QVERIFY(TestData::write(source,TestData::zip({{"1.png",png},{"2.png",png}})));
        const auto error=makeSymbolicLink(source,link);if(!error.isEmpty())QSKIP(qPrintable("Cannot create a symbolic link: "+error));
        auto cache=std::make_shared<ThumbnailDiskCache>(d.filePath("cache"));
        QVERIFY(!cachedCover(link,{},cache).image.isNull());
        if(legacy) {
            const auto names=QDir(cache->directory()).entryList({"*.png"},QDir::Files);QCOMPARE(names.size(),1);
            const auto path=QDir(cache->directory()).filePath(names.first());QImage pixels(path);QVERIFY(!pixels.isNull());
            // This fixture covers the old resolved-path spelling with valid
            // target attributes, not Windows' invalid link-owned attributes.
            const QFileInfo info(link),target(source);
            const QJsonObject record{{"path",link},{"resolved",QDir(info.absoluteDir().canonicalPath()).filePath(info.fileName())},
                {"bytes",QString::number(target.size())},{"modified",QString::number(target.lastModified().toMSecsSinceEpoch())}};
            pixels.setText("komamimic-cover-stamp",QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(record).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex()));
            QVERIFY(pixels.save(path,"PNG"));cache=std::make_shared<ThumbnailDiskCache>(cache->directory());
        }
        const auto book=Book::open(link).book;QVERIFY(book);
        for(int i=0;i<book->pages.size();++i)QVERIFY(!cachedPageThumbnail(book,i,cache).image.isNull());
        const auto before=savedThumbnails(cache->directory());QCOMPARE(before.size(),3);
        const auto generations=ThumbnailActivity::generations.load();
        MainWindow window(d.filePath("settings.ini"));window.m_thumbnailDiskCache=cache;cache->setDeferredWrites(true);window.show();
        const auto ready=[](ThumbnailGrid *grid){if(!grid->count())return false;for(int row:grid->visibleRows())if(!grid->item(row)->data(ThumbnailRoleReady).toBool())return false;return true;};
        window.openDirectory(aliases);QTRY_VERIFY(window.directoryReady() && ready(window.m_library));
        window.openBook(link);QTRY_VERIFY(window.renderReady());window.showPageThumbnails();QTRY_VERIFY(ready(window.m_pages));
        for(int i=0;i<2;++i) {
            window.showCovers();QTRY_VERIFY(ready(window.m_library));QTest::qWait(300);
            window.showPageThumbnails();QTRY_VERIFY(ready(window.m_pages));QTest::qWait(300);
        }
        window.showCovers();QTest::qWait(300);window.close();
        QCOMPARE(ThumbnailActivity::generations.load(),generations);
        QCOMPARE(savedThumbnails(cache->directory()),before);
    }
    void symbolicLinkPageVerificationAndChanges_data() {
        QTest::addColumn<bool>("directory");QTest::addColumn<bool>("image");
        QTest::newRow("file-link-zip")<<false<<false;
        QTest::newRow("file-link-image")<<false<<true;
        QTest::newRow("directory-link-zip")<<true<<false;
        QTest::newRow("directory-link-image")<<true<<true;
    }
    void symbolicLinkPageVerificationAndChanges() {
        QFETCH(bool,directory);QFETCH(bool,image);
        QTemporaryDir d;const auto real=d.filePath("real"),aliases=d.filePath("aliases");QVERIFY(QDir().mkpath(real));
        const auto name=image?QString("page.png"):QString("book.zip"),source=real+'/'+name,link=aliases+'/'+name;
        const auto red=TestData::png(TestData::solid(Qt::red));
        auto redInput=red;if(!image)redInput=TestData::zip({{"1.png",red}});
        QVERIFY(TestData::write(source,redInput));
        if(!directory)QVERIFY(QDir().mkpath(aliases));
        const auto error=directory?makeSymbolicLink(real,aliases,true):makeSymbolicLink(source,link);
        if(!error.isEmpty())QSKIP(qPrintable("Cannot create a symbolic link: "+error));
        const auto book=Book::open(link).book;QVERIFY(book);
        auto cache=std::make_shared<ThumbnailDiskCache>(d.filePath("cache"));
        QVERIFY(!cachedPageThumbnail(book,0,cache).image.isNull());
        const auto before=savedThumbnails(cache->directory());QCOMPARE(before.size(),1);
        const auto opens=BookActivity::opens.load(),generations=ThumbnailActivity::generations.load();
        int calls=0;auto cancel=[&]{return ++calls>100;};
        const auto verified=cachedPageThumbnail(book,0,cache,cancel,false,false,true);
        QVERIFY2(!verified.image.isNull(),qPrintable(verified.error));QVERIFY(calls<=100);
        QCOMPARE(BookActivity::opens.load(),opens);QCOMPARE(ThumbnailActivity::generations.load(),generations);
        QCOMPARE(savedThumbnails(cache->directory()),before);
        const auto observed=thumbnailEntry(QFileInfo(link),QFileInfo(link).absoluteDir().canonicalPath());
        QCOMPARE(observed.value("resolved"),QJsonDocument::fromJson(book->thumbnailSource(0)).object().value("resolved"));
        QCOMPARE(QJsonDocument::fromJson(book->thumbnailSource(0)).object().value("bytes").toString().toLongLong(),qint64(redInput.size()));
        QVERIFY(book->sourceUnchanged(0));
        // Old pixels remain available before a concrete change is verified.
        const auto blue=TestData::png(TestData::solid(Qt::blue,{43,67}));
        auto blueInput=blue;if(!image)blueInput=TestData::zip({{"1.png",blue}});
        QVERIFY(redInput.size()!=blueInput.size());QVERIFY(TestData::write(source,blueInput));
        QVERIFY(!book->sourceUnchanged(0));
        QCOMPARE(cachedPageThumbnail(book,0,cache).image.pixelColor(0,0),QColor(Qt::red));
        calls=0;const auto changed=cachedPageThumbnail(book,0,cache,cancel,false,false,true);
        QVERIFY2(!changed.image.isNull(),qPrintable(changed.error));QVERIFY(calls<=100);
        QCOMPARE(changed.image.pixelColor(0,0),QColor(Qt::blue));
        QCOMPARE(BookActivity::opens.load(),opens+1);QCOMPARE(ThumbnailActivity::generations.load(),generations+1);
        auto restarted=std::make_shared<ThumbnailDiskCache>(cache->directory());
        QCOMPARE(cachedPageThumbnail(Book::open(link).book,0,restarted,{},false,true).image.pixelColor(0,0),QColor(Qt::blue));
        // Cover validation must follow the same target attributes as pages.
        QCOMPARE(cachedCover(link,{},cache).image.pixelColor(0,0),QColor(Qt::blue));
        QVERIFY(TestData::write(source,redInput));
        QCOMPARE(cachedCover(link,{},cache).image.pixelColor(0,0),QColor(Qt::blue));
        QCOMPARE(cachedCover(link,{},cache,{},false,0,false,true).image.pixelColor(0,0),QColor(Qt::red));
    }
    void cachedFileLinkEntryUsesCurrentTargetMetadata() {
        QTemporaryDir d;const auto source=d.filePath("target.bin"),link=d.filePath("alias.bin");
        const QByteArray original="old",replacement="updated target contents";QVERIFY(TestData::write(source,original));
        const auto error=makeSymbolicLink(source,link);if(!error.isEmpty())QSKIP(qPrintable("Cannot create a symbolic link: "+error));
        const auto entries=QDir(d.path()).entryInfoList({"alias.bin"},QDir::Files|QDir::Readable);QCOMPARE(entries.size(),1);
        const auto listed=entries.first();QVERIFY(listed.isSymLink());
        const auto oldSize=listed.size();const auto oldModified=listed.lastModified();Q_UNUSED(oldSize);Q_UNUSED(oldModified);
        const auto before=thumbnailEntry(listed,d.path());
        QCOMPARE(before.value("bytes").toString().toLongLong(),qint64(original.size()));
        QVERIFY(TestData::write(source,replacement));
        // A directory entry can retain link metadata while its target changes.
        // The dependency record must contain the current target attributes.
        const auto after=thumbnailEntry(listed,d.path());
        QCOMPARE(after.value("bytes").toString().toLongLong(),qint64(replacement.size()));
        QCOMPARE(after.value("modified").toString().toLongLong(),QFileInfo(source).lastModified().toMSecsSinceEpoch());
        QCOMPARE(after.value("resolved").toString(),QFileInfo(source).canonicalFilePath());
        QVERIFY(before.value("stamp")!=after.value("stamp"));
        const auto next=d.filePath("next.bin");const QByteArray nextContents="new target";
        QVERIFY(TestData::write(next,nextContents));QVERIFY(QFile::remove(link));QVERIFY(makeSymbolicLink(next,link).isEmpty());
        const auto retargeted=thumbnailEntry(listed,d.path());
        QCOMPARE(retargeted.value("bytes").toString().toLongLong(),qint64(nextContents.size()));
        QCOMPARE(retargeted.value("resolved").toString(),QFileInfo(next).canonicalFilePath());
        QVERIFY(retargeted.value("stamp")!=after.value("stamp"));
    }
    void stalePageObservationDoesNotRepeatReopen() {
        QTemporaryDir d;const auto path=d.filePath("book.zip");
        QVERIFY(TestData::write(path,TestData::zip({{"1.png",TestData::png(TestData::solid(Qt::red))}})));
        const auto book=Book::open(path).book;QVERIFY(book);
        auto cache=std::make_shared<ThumbnailDiskCache>(d.filePath("cache"));
        QVERIFY(!cachedPageThumbnail(book,0,cache).image.isNull());
        const auto before=savedThumbnails(cache->directory());QCOMPARE(before.size(),1);
        auto observed=thumbnailEntry(QFileInfo(path),QFileInfo(path).absoluteDir().canonicalPath());
        observed["modified"]=QString::number(observed.value("modified").toString().toLongLong()+1);
        const auto opens=BookActivity::opens.load(),generations=ThumbnailActivity::generations.load();
        int calls=0;const auto result=cachedPageThumbnail(book,0,cache,[&]{return ++calls>100;},false,false,true,0,observed);
        QVERIFY(calls<=100);QVERIFY(result.image.isNull());QVERIFY(!result.error.isEmpty());
        QCOMPARE(BookActivity::opens.load(),opens+1);QCOMPARE(ThumbnailActivity::generations.load(),generations);
        QCOMPARE(savedThumbnails(cache->directory()),before);
        QCOMPARE(cachedPageThumbnail(book,0,cache).image.pixelColor(0,0),QColor(Qt::red));
    }
    void irrelevantFolderAdditionDoesNotRewriteOrRepeat() {
        QTemporaryDir d;const auto parent=d.filePath("A"),folder=parent+"/B",source=folder+"/01.png";QVERIFY(QDir().mkpath(folder));
        QVERIFY(TestData::write(source,TestData::png(TestData::solid(Qt::red))));auto cache=std::make_shared<ThumbnailDiskCache>();
        QVERIFY(!cachedCover(source,{},cache).image.isNull());const auto before=QDir(cache->directory()).entryList({"*.png"},QDir::Files);
        QVERIFY(!cachedCover(folder,{},cache).image.isNull());QString png;
        for(const auto &name:QDir(cache->directory()).entryList({"*.png"},QDir::Files))if(!before.contains(name))png=cache->directory()+'/'+name;
        QVERIFY(!png.isEmpty());QFile file(png);QVERIFY(file.open(QIODevice::ReadOnly));const auto bytes=file.readAll();file.close();const auto created=QFileInfo(png).lastModified();
        QVERIFY(!cachedCover(parent,{},cache,{},false,0,false,false,1).image.isNull());
        MainWindow window(d.filePath("settings.ini"));window.show();window.openDirectory(parent);
        auto *grid=window.findChild<ThumbnailGrid *>("zipLibrary");QVERIFY(grid);QTRY_VERIFY(window.directoryReady() && grid->count()==1 && grid->item(0)->data(ThumbnailRoleReady).toBool());
        const auto generations=ThumbnailActivity::generations.load(),checks=ThumbnailActivity::metadataChecks.load(),scans=ThumbnailActivity::folderScans.load();
        QVERIFY(TestData::write(folder+"/99.png",TestData::png(TestData::solid(Qt::blue))));
        QTRY_VERIFY(ThumbnailActivity::folderScans.load()>scans);QTest::qWait(300);
        const auto quiet=ThumbnailActivity::folderScans.load();QTest::qWait(1500);QCOMPARE(ThumbnailActivity::folderScans.load(),quiet);
        window.openDirectory(d.path());QTRY_VERIFY(window.directoryReady());window.openDirectory(parent);QTRY_VERIFY(window.directoryReady() && grid->count()==1 && grid->item(0)->data(ThumbnailRoleReady).toBool());
        QTest::qWait(400);QCOMPARE(ThumbnailActivity::folderScans.load(),quiet);
        QCOMPARE(ThumbnailActivity::generations.load(),generations);QCOMPARE(ThumbnailActivity::metadataChecks.load(),checks);
        window.close();QCOMPARE(QFileInfo(png).lastModified(),created);QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),bytes);
    }
    void depthPreferencePersistsAndReevaluatesOnlyEmptyRows() {
        QTemporaryDir d;const auto parent=d.filePath("A"),folder=parent+"/B/C";QVERIFY(QDir().mkpath(folder));
        QVERIFY(TestData::write(folder+"/a.png",TestData::png(TestData::solid(Qt::red))));const auto settings=d.filePath("settings.ini");
        MainWindow window(settings);window.show();window.openDirectory(parent);
        auto *grid=window.findChild<ThumbnailGrid *>("zipLibrary");QVERIFY(grid);
        QTRY_VERIFY(window.directoryReady() && grid->count()==1 && grid->item(0)->data(ThumbnailRoleReady).toBool());QVERIFY(!grid->item(0)->data(ThumbnailHasPixelsRole).toBool());
        bool edited=false;QTimer::singleShot(0,&window,[&]{auto *dialog=window.findChild<QDialog *>("preferencesDialog");if(!dialog)return;auto *depth=dialog->findChild<QSpinBox *>("thumbnails/folderDepth");if(!depth){dialog->reject();return;}QCOMPARE(depth->value(),0);depth->setValue(1);edited=true;dialog->accept();});
        window.command("preferences")->trigger();QVERIFY(edited);
        QTRY_VERIFY_WITH_TIMEOUT(grid->item(0)->data(ThumbnailHasPixelsRole).toBool(),5000);window.close();
        {QSettings saved(settings,QSettings::IniFormat);QCOMPARE(saved.value("thumbnails/folderDepth").toInt(),1);saved.setValue("thumbnails/folderDepth",0);}
        const auto scans=ThumbnailActivity::folderScans.load(),gens=ThumbnailActivity::generations.load();
        MainWindow reopened(settings);reopened.show();reopened.openDirectory(parent);auto *again=reopened.findChild<ThumbnailGrid *>("zipLibrary");QVERIFY(again);
        QTRY_VERIFY(reopened.directoryReady() && again->count()==1 && again->item(0)->data(ThumbnailHasPixelsRole).toBool());
        QCOMPARE(ThumbnailActivity::folderScans.load(),scans);QCOMPARE(ThumbnailActivity::generations.load(),gens);reopened.close();
    }
    void laterCandidateDoesNotTouchExistingCache() {
        QTemporaryDir d;const auto folder=d.filePath("books"),source=folder+"/01.png";QVERIFY(QDir().mkpath(folder));
        QVERIFY(TestData::write(source,TestData::png(TestData::solid(Qt::red))));
        auto cache=std::make_shared<ThumbnailDiskCache>(d.filePath("thumbnails-v2"));
        QVERIFY(!cachedCover(source,{},cache).image.isNull());const auto leaves=QDir(cache->directory()).entryList({"*.png"},QDir::Files);
        QVERIFY(!cachedCover(folder,{},cache).image.isNull());QString cover;
        for(const auto &name:QDir(cache->directory()).entryList({"*.png"},QDir::Files))if(!leaves.contains(name))cover=cache->directory()+'/'+name;
        QVERIFY(!cover.isEmpty());QFile saved(cover);QVERIFY(saved.open(QIODevice::ReadOnly));const auto bytes=saved.readAll();saved.close();
        const auto created=QFileInfo(cover).lastModified();
        const auto scans=ThumbnailActivity::folderScans.load(),checks=ThumbnailActivity::metadataChecks.load(),gens=ThumbnailActivity::generations.load();
        const auto opens=BookActivity::opens.load(),reads=BookActivity::imageRequests.load();
        QVERIFY(TestData::write(folder+"/99.png",TestData::png(TestData::solid(Qt::blue))));
        QCOMPARE(cachedCover(folder,{},cache,{},false,0,false,true,0,{{"directory",true}}).image.pixelColor(0,0),QColor(Qt::red));
        QVERIFY(TestData::write(folder+"/notes.ini","unrelated"));
        QCOMPARE(cachedCover(folder,{},cache,{},false,0,false,true,0,{{"directory",true}}).image.pixelColor(0,0),QColor(Qt::red));
        QCOMPARE(ThumbnailActivity::folderScans.load(),scans+2);QCOMPARE(ThumbnailActivity::metadataChecks.load(),checks);
        QCOMPARE(ThumbnailActivity::generations.load(),gens);QCOMPARE(BookActivity::opens.load(),opens);QCOMPARE(BookActivity::imageRequests.load(),reads);
        QCOMPARE(QFileInfo(cover).lastModified(),created);QVERIFY(saved.open(QIODevice::ReadOnly));QCOMPARE(saved.readAll(),bytes);saved.close();
        QCOMPARE(cache->stats().entries,2);QVERIFY(!cache->hasPendingWrites());
        QVERIFY(!QImage(cover).textKeys().contains("komamimic-source-records"));
        QVERIFY(TestData::write(folder+"/00.png",TestData::png(TestData::solid(Qt::blue))));
        QCOMPARE(cachedCover(folder,{},cache,{},false,0,false,true,0,{{"directory",true}}).image.pixelColor(0,0),QColor(Qt::blue));
        QCOMPARE(ThumbnailActivity::generations.load(),gens+1);QCOMPARE(ThumbnailActivity::metadataChecks.load(),checks);
    }
    void depthIsBoundedAndNeverInvalidatesWarmCache() {
        QTemporaryDir d;const auto folder=d.filePath("B"),nested=folder+"/C/D";QVERIFY(QDir().mkpath(nested));
        QVERIFY(TestData::write(nested+"/a.png",TestData::png(TestData::solid(Qt::red))));
        auto cache=std::make_shared<ThumbnailDiskCache>(d.filePath("cache"));
        auto scans=ThumbnailActivity::folderScans.load();
        QVERIFY(cachedCover(folder,{},cache).image.isNull());QCOMPARE(ThumbnailActivity::folderScans.load(),scans+1);
        scans=ThumbnailActivity::folderScans.load();
        QVERIFY(cachedCover(folder,{},cache,{},false,0,false,false,1).image.isNull());QCOMPARE(ThumbnailActivity::folderScans.load(),scans+2);
        scans=ThumbnailActivity::folderScans.load();
        QCOMPARE(cachedCover(folder,{},cache,{},false,0,false,false,2).image.pixelColor(0,0),QColor(Qt::red));
        QCOMPARE(ThumbnailActivity::folderScans.load(),scans+3);const int count=cache->stats().entries;
        scans=ThumbnailActivity::folderScans.load();
        QCOMPARE(cachedCover(folder,{},cache,{},false,0,false,true,0,{{"directory",true}}).image.pixelColor(0,0),QColor(Qt::red));
        QCOMPARE(ThumbnailActivity::folderScans.load(),scans+1);QCOMPARE(cache->stats().entries,count);
    }
    void invalidEarlierCandidateIsNotReadAgainWithoutChange() {
        QTemporaryDir d;const auto folder=d.filePath("B");QVERIFY(QDir().mkpath(folder));
        QVERIFY(TestData::write(folder+"/01.png",TestData::png(TestData::solid(Qt::red))));
        auto cache=std::make_shared<ThumbnailDiskCache>(d.filePath("cache"));QVERIFY(!cachedCover(folder,{},cache).image.isNull());
        QVERIFY(TestData::write(folder+"/00.zip","invalid archive"));
        const auto count=ThumbnailActivity::generations.load();
        QCOMPARE(cachedCover(folder,{},cache,{},false,0,false,true,0,{{"directory",true}}).image.pixelColor(0,0),QColor(Qt::red));
        QCOMPARE(ThumbnailActivity::generations.load(),count+1);
        QVERIFY(TestData::write(folder+"/99.png",TestData::png(TestData::solid(Qt::blue))));
        QCOMPARE(cachedCover(folder,{},cache,{},false,0,false,true,0,{{"directory",true}}).image.pixelColor(0,0),QColor(Qt::red));
        QCOMPARE(ThumbnailActivity::generations.load(),count+1);
    }
    void existingV2MetadataIsOptionalAndCacheRemainsReadOnly() {
        QTemporaryDir d;const auto path=d.filePath("a.png");QVERIFY(TestData::write(path,TestData::png(TestData::solid(Qt::red))));
        auto cache=std::make_shared<ThumbnailDiskCache>(d.filePath("thumbnails-v2"));QVERIFY(!cachedCover(path,{},cache).image.isNull());
        const auto files=QDir(cache->directory()).entryList({"*.png"},QDir::Files);QCOMPARE(files.size(),1);
        const auto saved=cache->directory()+'/'+files.first();
        // An old PNG without any extra dependency metadata is still trusted.
        QVERIFY(TestData::write(saved,TestData::png(TestData::solid(Qt::blue))));
        const auto created=QFileInfo(saved).lastModified();const auto gens=ThumbnailActivity::generations.load(),checks=ThumbnailActivity::metadataChecks.load();
        auto restart=std::make_shared<ThumbnailDiskCache>(cache->directory());
        QCOMPARE(cachedCover(path,{},restart).image.pixelColor(0,0),QColor(Qt::blue));
        QCOMPARE(cachedCover(path,{},restart,{},false,0,false,true).image.pixelColor(0,0),QColor(Qt::blue));
        QCOMPARE(QFileInfo(saved).lastModified(),created);QCOMPARE(ThumbnailActivity::generations.load(),gens);
        // Explicit verification uses directory entries, not a separate source stat.
        QCOMPARE(ThumbnailActivity::metadataChecks.load(),checks);
    }
    void unchangedCachesNeverReopenSources() {
        QTemporaryDir d;const auto books=d.filePath("books"),folder=books+"/folder",zip=books+"/book.zip";
        QVERIFY(QDir().mkpath(folder));QVERIFY(TestData::write(folder+"/page.png",TestData::png(TestData::solid(Qt::blue))));
        QVERIFY(TestData::write(zip,TestData::zip({{"page.png",TestData::png(TestData::solid(Qt::red))}})));
        auto cache=std::make_shared<ThumbnailDiskCache>(d.filePath("private-store"));
        QVERIFY(!cachedCover(folder,{},cache).image.isNull());QVERIFY(!cachedCover(zip,{},cache).image.isNull());
        QVERIFY(!cachedCover(books,{},cache).image.isNull());
        const auto opened=Book::open(zip).book;QVERIFY(opened);QVERIFY(!cachedPageThumbnail(opened,0,cache).image.isNull());
        const auto checks=ThumbnailActivity::metadataChecks.load(),scans=ThumbnailActivity::folderScans.load(),generated=ThumbnailActivity::generations.load();
        const auto opens=BookActivity::opens.load(),reads=BookActivity::imageRequests.load();
        const auto before=cache->stats().entries;
        cache->setDeferredWrites(true);
        MainWindow window(d.filePath("settings.ini"));window.m_thumbnailDiskCache=cache;window.show();window.openDirectory(books);
        auto *grid=window.findChild<ThumbnailGrid *>("zipLibrary");QVERIFY(grid);
        QTRY_VERIFY(window.directoryReady() && grid->count()==2);
        QTRY_VERIFY(grid->item(0)->data(ThumbnailRoleReady).toBool() && grid->item(1)->data(ThumbnailRoleReady).toBool());
        QTest::qWait(2400); // The old two-second periodic validator must not run.
        const auto elsewhere=d.filePath("elsewhere");QVERIFY(QDir().mkpath(elsewhere));
        window.openDirectory(elsewhere);QTRY_VERIFY(window.directoryReady());
        window.openDirectory(books);QTRY_VERIFY(window.directoryReady() && grid->count()==2);
        QTRY_VERIFY(grid->item(0)->data(ThumbnailRoleReady).toBool() && grid->item(1)->data(ThumbnailRoleReady).toBool());
        for(int i=0;i<3;++i)QVERIFY(!cachedPageThumbnail(opened,0,cache).image.isNull());
        QCOMPARE(ThumbnailActivity::metadataChecks.load(),checks);QCOMPARE(ThumbnailActivity::folderScans.load(),scans);
        QCOMPARE(ThumbnailActivity::generations.load(),generated);QCOMPARE(BookActivity::opens.load(),opens);QCOMPARE(BookActivity::imageRequests.load(),reads);
        window.close();QCOMPARE(cache->stats().entries,before);
    }
    void changesAreTargetedAndConsumed() {
        QTemporaryDir d;const auto books=d.filePath("books");QVERIFY(QDir().mkpath(books));
        const auto a=books+"/a.png",b=books+"/b.png";
        QVERIFY(TestData::write(a,TestData::png(TestData::solid(Qt::red))));QVERIFY(TestData::write(b,TestData::png(TestData::solid(Qt::blue))));
        MainWindow window(d.filePath("settings.ini"));window.show();window.openDirectory(books);
        auto *grid=window.findChild<ThumbnailGrid *>("zipLibrary");QVERIFY(grid);
        QTRY_VERIFY(window.directoryReady() && grid->count()==2 && grid->item(0)->data(ThumbnailRoleReady).toBool() && grid->item(1)->data(ThumbnailRoleReady).toBool());
        const auto color=[&](int row){auto image=grid->item(row)->icon().pixmap(64,64).toImage();return image.pixelColor(image.rect().center());};
        const auto generated=ThumbnailActivity::generations.load();
        QVERIFY(TestData::write(a,TestData::png(TestData::solid(Qt::green,{39,61}))));
        QTRY_COMPARE_WITH_TIMEOUT(color(0),QColor(Qt::green),6000);QCOMPARE(color(1),QColor(Qt::blue));
        QCOMPARE(ThumbnailActivity::generations.load(),generated+1);
        const auto settled=ThumbnailActivity::metadataChecks.load();QTest::qWait(2400);
        QCOMPARE(ThumbnailActivity::metadataChecks.load(),settled);
        // A notice without a content/attribute change can check, but must not decode.
        window.m_thumbnailChanges->changed(a);
        QTRY_VERIFY(grid->item(0)->data(ThumbnailCoverValidatedRole).toBool());QTest::qWait(400);
        QCOMPARE(ThumbnailActivity::generations.load(),generated+1);
        const auto quiet=ThumbnailActivity::metadataChecks.load();QTest::qWait(1500);QCOMPARE(ThumbnailActivity::metadataChecks.load(),quiet);
        window.close();
    }
    void readerDoesNoThumbnailWork() {
        QTemporaryDir d;const auto books=d.filePath("books"),path=books+"/book.zip";QVERIFY(QDir().mkpath(books));
        QVERIFY(TestData::write(path,TestData::zip({{"a.png",TestData::png(TestData::solid(Qt::blue))}})));
        MainWindow window(d.filePath("settings.ini"));window.show();window.openDirectory(books);
        auto *grid=window.findChild<ThumbnailGrid *>("zipLibrary");QVERIFY(grid);
        QTRY_VERIFY(window.directoryReady() && grid->count()==1 && grid->item(0)->data(ThumbnailRoleReady).toBool());
        window.openBook(path);QTRY_VERIFY(window.renderReady());
        const auto checks=ThumbnailActivity::metadataChecks.load(),scans=ThumbnailActivity::folderScans.load(),gens=ThumbnailActivity::generations.load();
        window.m_thumbnailChanges->changed(path);QTest::qWait(2400);
        QCOMPARE(ThumbnailActivity::metadataChecks.load(),checks);QCOMPARE(ThumbnailActivity::folderScans.load(),scans);QCOMPARE(ThumbnailActivity::generations.load(),gens);
        QVERIFY(!window.m_displayJobs->contains("cache-maintain"));window.close();
    }
    void failedRefreshPreservesExistingCacheAndPixels() {
        QTemporaryDir d;const auto books=d.filePath("books"),path=books+"/a.png";QVERIFY(QDir().mkpath(books));
        QVERIFY(TestData::write(path,TestData::png(TestData::solid(Qt::blue))));
        auto cache=std::make_shared<ThumbnailDiskCache>();QVERIFY(!cachedCover(path,{},cache).image.isNull());
        MainWindow window(d.filePath("settings.ini"));window.show();window.openDirectory(books);
        auto *grid=window.findChild<ThumbnailGrid *>("zipLibrary");QVERIFY(grid);
        QTRY_VERIFY(window.directoryReady() && grid->count()==1 && grid->item(0)->data(ThumbnailRoleReady).toBool());
        const auto color=[&]{auto image=grid->item(0)->icon().pixmap(64,64).toImage();return image.pixelColor(image.rect().center());};
        QCOMPARE(color(),QColor(Qt::blue));QVERIFY(TestData::write(path,"temporarily invalid source"));
        window.command("thumbRefresh")->trigger();QTRY_VERIFY(window.directoryReady());QTest::qWait(1500);
        QCOMPARE(color(),QColor(Qt::blue));
        QCOMPARE(cachedCover(path,{},cache,{},false,0,true).image.pixelColor(0,0),QColor(Qt::blue));window.close();
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
        for(auto lane:{DisplayJobs::Lane::Store,DisplayJobs::Lane::Background,DisplayJobs::Lane::Update,DisplayJobs::Lane::Display}) {
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
        // This fixture does not change files. Late initial watcher/layout
        // notices must not remove the held validation before the cache probe.
        QSignalBlocker directoryNotices(window.m_directoryWatcher);
        QSignalBlocker initialLayout(grid);
        window.m_directoryRefreshTimer->stop();
        window.m_displayJobs->retain("covers");
        bool finished=false,obsolete=false;
        std::latch release(1);std::atomic<bool> entered=false;auto cleanup=qScopeGuard([&]{release.count_down();});
        const auto generation=window.m_libraryGeneration;
        const auto id=QString("covers/%1/0/verify").arg(generation);
        const bool accepted=window.m_displayJobs->submit<int>(DisplayJobs::Lane::Update,"covers",id,
            [&](const auto &){entered=true;release.wait();return 0;},[&](int,bool stale){finished=true;obsolete=stale;});
        QVERIFY(accepted);
        QTRY_VERIFY2(entered.load(),qPrintable(QString("finished=%1 obsolete=%2 generation=%3 current=%4")
            .arg(finished).arg(obsolete).arg(generation).arg(window.m_libraryGeneration)));
        QVERIFY(!finished);QVERIFY(window.m_displayJobs->contains(id));
        initialLayout.unblock();
        // Equivalent to an evicted row becoming visible while its old validation
        // is still running. Keep the real request id to exercise deduplication.
        grid->item(0)->setData(ThumbnailRoleReady,false);grid->item(0)->setData(ThumbnailNextCheckRole,0);
        grid->item(0)->setIcon(grid->placeholderIcon());window.m_displayJobs->retain("covers");window.requestThumbnails(grid);
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
        QVERIFY(!cachedCover(folder,{},cache,{},false,0,false,true).image.isNull());if(deferred)QVERIFY(cache->checkpoint());
        QVERIFY(QFile::remove(source));QVERIFY(cachedCover(folder,{},cache,{},false,0,false,true).image.isNull());
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
        auto cache=std::make_shared<ThumbnailDiskCache>(d.filePath("cache"));
        {
            const auto producer=Book::open(path).book;QVERIFY(producer);
            QCOMPARE(cachedPageThumbnail(producer,0,cache).image.pixelColor(0,0),QColor(Qt::red));
        } // Release the generation cursor before removing the ZIP on Windows.
        const auto book=Book::open(path).book;QVERIFY(book);
        const auto opens=BookActivity::opens.load(),reads=BookActivity::imageRequests.load();
        const auto checks=ThumbnailActivity::metadataChecks.load(),scans=ThumbnailActivity::folderScans.load();
        // A new Book keeps the opened metadata snapshot, without retaining the
        // producer's source cursor. Even a normal cache hit must not reopen it.
        QCOMPARE(cachedPageThumbnail(book,0,cache).image.pixelColor(0,0),QColor(Qt::red));
        QFile source(path);QVERIFY2(source.remove(),qPrintable(source.errorString()));
        QVERIFY(!QFileInfo::exists(path));
        // Cache-only lookup must still work when the source does not exist.
        QCOMPARE(cachedPageThumbnail(book,0,cache,{},false,true).image.pixelColor(0,0),QColor(Qt::red));
        QVERIFY(TestData::write(path,TestData::zip({{"a.png",TestData::png(TestData::solid(Qt::blue,{39,61}))}})));
        QCOMPARE(cachedPageThumbnail(book,0,cache).image.pixelColor(0,0),QColor(Qt::red));
        QCOMPARE(BookActivity::opens.load(),opens);QCOMPARE(BookActivity::imageRequests.load(),reads);
        QCOMPARE(ThumbnailActivity::metadataChecks.load(),checks);QCOMPARE(ThumbnailActivity::folderScans.load(),scans);
        const auto refreshed=cachedPageThumbnail(book,0,cache,{},false,false,true);
        QVERIFY2(refreshed.error.isEmpty(),qPrintable(refreshed.error));
        QVERIFY(!refreshed.image.isNull());QCOMPARE(refreshed.image.pixelColor(0,0),QColor(Qt::blue));
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
        const auto warm=page?cachedPageThumbnail(book,0,cache):cachedCover(path,{},cache,{},false,0,false,false,2);
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
        cache=std::make_shared<ThumbnailDiskCache>(root);
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
        QCOMPARE(cachedCover(folder,{},cache,{},false,0,false,true,2).image.pixelColor(0,0),QColor(Qt::red));
        while(cache->hasPendingWrites())QVERIFY(cache->flushPending());
        const auto modified=QFileInfo(folder).lastModified();
        QVERIFY(TestData::write(path,encode(Qt::green,{39,61})));
        QCOMPARE(QFileInfo(folder).lastModified(),modified);
        QCOMPARE(cachedCover(folder,{},cache,{},false,0,true).image.pixelColor(0,0),QColor(Qt::red));
        QCOMPARE(cachedCover(folder,{},cache,{},false,0,false,true,2).image.pixelColor(0,0),QColor(Qt::green));
        // Earlier natural-order candidates and deletion/fallback must be seen
        // even when the top folder's own timestamp has not changed.
        const auto earlier=nested+"/2"+suffix;
        QVERIFY(TestData::write(earlier,encode(Qt::blue,{32,48})));
        QCOMPARE(cachedCover(folder,{},cache,{},false,0,false,true,2).image.pixelColor(0,0),QColor(Qt::blue));
        QVERIFY(QFile::remove(earlier));
        QCOMPARE(cachedCover(folder,{},cache,{},false,0,false,true,2).image.pixelColor(0,0),QColor(Qt::green));
        QCOMPARE(QFileInfo(folder).lastModified(),modified);
        while(cache->hasPendingWrites())QVERIFY(cache->flushPending());
        auto restarted=std::make_shared<ThumbnailDiskCache>(cache->directory());
        QCOMPARE(cachedCover(folder,{},restarted,{},false,0,true).image.pixelColor(0,0),QColor(Qt::green));
    }
    void completedFolderPixelsSaveWithoutRechecking() {
        QTemporaryDir d;QVERIFY(d.isValid());const auto folder=d.filePath("books");QVERIFY(QDir().mkpath(folder));
        const auto path=folder+"/page.png";QVERIFY(TestData::write(path,TestData::png(TestData::solid(Qt::red))));
        auto cache=std::make_shared<ThumbnailDiskCache>(d.filePath("cache"));cache->setDeferredWrites(true);
        QVERIFY(!cachedCover(folder,{},cache).image.isNull());QVERIFY(cache->hasPendingWrites());
        QVERIFY(TestData::write(path,TestData::png(TestData::solid(Qt::green,{39,61}))));
        while(cache->hasPendingWrites())QVERIFY(cache->flushPending());
        QCOMPARE(cache->stats().entries,2);
        QCOMPARE(cachedCover(folder,{},cache).image.pixelColor(0,0),QColor(Qt::red));
        QCOMPARE(cachedCover(folder,{},cache,{},false,0,false,true).image.pixelColor(0,0),QColor(Qt::green));
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
            cachedCover(folder,{},cache,[&]{if(++checks==checkpoint){cache->clear();cleared=true;}return false;},false,0,false,false,1);
            if(cleared){++exercised;QVERIFY(!cache->hasPendingWrites());QCOMPARE(cache->stats().entries,0);}
        }
        QVERIFY(exercised>10);
    }
    void textFolderPreviewsRemainCached() {
        QTemporaryDir d;const auto folder=d.filePath("books");QVERIFY(QDir().mkpath(folder));
        auto cache=std::make_shared<ThumbnailDiskCache>(d.filePath("cache"));cache->setDeferredWrites(true);
        for(const auto &name:{"page.txt","page.md","book.zip"}) {
            QVERIFY(cache->clear().isEmpty());
            const auto path=folder+'/'+name;QByteArray bytes="# Preview\nText body\n";
            if(QString(name).endsWith(".zip"))bytes=TestData::zip({{"page.md",bytes}});
            QVERIFY(TestData::write(path,bytes));QVERIFY(!cachedCover(folder,{},cache).image.isNull());
            QVERIFY(cache->hasPendingWrites());while(cache->hasPendingWrites())QVERIFY(cache->flushPending());
            const auto reads=BookActivity::opens.load(),checks=ThumbnailActivity::metadataChecks.load();
            QVERIFY(QFile::remove(path));
            auto restart=std::make_shared<ThumbnailDiskCache>(cache->directory());
            QVERIFY(!cachedCover(folder,{},restart).image.isNull());
            QCOMPARE(BookActivity::opens.load(),reads);QCOMPARE(ThumbnailActivity::metadataChecks.load(),checks);
        }
    }
    void documentFolderRoundTripsReuseDecodedThumbnails_data() {
        QTest::addColumn<bool>("preload");QTest::addColumn<bool>("documentLoaded");
        for(bool preload:{false,true})for(bool document:{false,true})
            QTest::newRow(qPrintable(QString("%1-%2").arg(preload?"preload":"visible",document?"after-document":"without-document")))<<preload<<document;
    }
    void documentFolderRoundTripsReuseDecodedThumbnails() {
        QFETCH(bool,preload);QFETCH(bool,documentLoaded);
        QTemporaryDir d;const auto imageFolder=d.filePath("images"),textFolder=d.filePath("text"),epubFolder=d.filePath("epub"),cachePath=d.filePath("cache");
        for(const auto &folder:{imageFolder,textFolder,epubFolder})QVERIFY(QDir().mkpath(folder));
        const auto png=TestData::png(TestData::solid(Qt::blue,{240,320}));QString imageZip,textZip;
        const auto epub=TestData::zip({{"mimetype","application/epub+zip"},{"META-INF/container.xml","<container><rootfiles><rootfile full-path=\"book.opf\"/></rootfiles></container>"},
            {"book.opf","<package><metadata><meta name=\"cover\" content=\"image\"/></metadata><manifest><item id=\"image\" href=\"cover.png\" media-type=\"image/png\"/><item id=\"body\" href=\"body.xhtml\" media-type=\"application/xhtml+xml\"/></manifest><spine><itemref idref=\"body\"/></spine></package>"},
            {"cover.png",png},{"body.xhtml","<html><head></head><body>Generic text</body></html>"}});
        auto writer=std::make_shared<ThumbnailDiskCache>(cachePath);
        for(int i=0;i<3;++i)for(const auto &folder:{imageFolder,textFolder,epubFolder}) {
            const auto path=folder+QString("/%1.%2").arg(i).arg(folder==epubFolder?"epub":"zip");
            QByteArray bytes=epub;
            if(folder==imageFolder)bytes=TestData::zip({{"1.png",png},{"2.png",png}});
            else if(folder==textFolder)bytes=TestData::zip({{"text.txt","Generic preview text\n"}});
            QVERIFY(TestData::write(path,bytes));QVERIFY(!cachedCover(path,{},writer).image.isNull());
            if(i==0 && folder==imageFolder)imageZip=path;
            if(i==0 && folder==textFolder)textZip=path;
        }
        const auto imageBook=Book::open(imageZip).book;QVERIFY(imageBook);
        for(int i=0;i<2;++i)QVERIFY(!cachedPageThumbnail(imageBook,i,writer).image.isNull());
        writer.reset();auto cache=std::make_shared<ThumbnailDiskCache>(cachePath);
        const auto ini=d.filePath("settings.ini");{QSettings settings(ini,QSettings::IniFormat);settings.setValue("thumbnails/preloadAll",preload);settings.setValue("thumbnails/mouseSelection",false);}
        MainWindow w(ini);w.m_thumbnailDiskCache=cache;cache->setDeferredWrites(true);w.resize(900,600);w.show();
        if(documentLoaded) {
            const auto listing=Book::open(textZip).book;QVERIFY(listing);w.m_book=Book::open(listing->entryPath(0)).book;QVERIFY(w.m_book && w.m_book->publication);
            w.m_currentPath=w.m_book->path;w.m_mode=MainWindow::ViewMode::Reader;
        }
        const auto ready=[&](ThumbnailGrid *grid){const auto rows=grid->visibleRows();if(rows.isEmpty())return false;for(int row:rows)if(!grid->item(row)->data(ThumbnailRoleReady).toBool())return false;return true;};
        w.openDirectory(imageFolder);QTRY_VERIFY(w.directoryReady() && w.m_library->count()==3 && ready(w.m_library));
        const auto gens=ThumbnailActivity::generations.load(),opens=BookActivity::opens.load();
        for(const auto &folder:{textFolder,epubFolder}) {
            w.openDirectory(folder);QTRY_VERIFY(w.directoryReady() && w.m_library->count()==3 && ready(w.m_library));
            const auto before=cache->lookupActivity();
            w.openDirectory(imageFolder);QTRY_VERIFY(w.directoryReady() && w.m_library->count()==3 && ready(w.m_library));
            QCOMPARE(cache->lookupActivity().pngDecodes,before.pngDecodes);
        }
        QCOMPARE(ThumbnailActivity::generations.load(),gens);QCOMPARE(BookActivity::opens.load(),opens);
        w.loadBook(imageZip,MainWindow::ViewMode::Pages);QTRY_VERIFY(!w.isLoading() && w.m_pages->count()==2 && ready(w.m_pages));
        const auto pageReads=cache->lookupActivity().pngDecodes;
        w.setMode(MainWindow::ViewMode::Covers);QTRY_VERIFY(ready(w.m_library));w.setMode(MainWindow::ViewMode::Pages);QTRY_VERIFY(ready(w.m_pages));
        QCOMPARE(cache->lookupActivity().pngDecodes,pageReads);
    }
    void folderCacheUpdatesAfterChangeNotice() {
        QTemporaryDir d;QVERIFY(d.isValid());const auto books=d.filePath("books"),folder=books+"/folder",source=folder+"/book.zip";
        QVERIFY(QDir().mkpath(folder));
        QVERIFY(TestData::write(source,TestData::zip({{"1.png",TestData::png(TestData::solid(Qt::red))}})));
        auto cache=std::make_shared<ThumbnailDiskCache>();
        QVERIFY(!cachedCover(folder,{},cache).image.isNull());
        QVERIFY(TestData::write(source,TestData::zip({{"1.png",TestData::png(TestData::solid(Qt::green,{39,61}))}})));
        MainWindow window(d.filePath("settings.ini"));window.show();window.openDirectory(books);
        QTRY_VERIFY(window.directoryReady());auto *grid=window.findChild<ThumbnailGrid *>("zipLibrary");QVERIFY(grid);QCOMPARE(grid->count(),1);
        const auto color=[&]{const auto image=grid->item(0)->icon().pixmap(64,64).toImage();return image.pixelColor(image.rect().center());};
        QTRY_COMPARE(color(),QColor(Qt::red));
        QVERIFY(TestData::write(source,TestData::zip({{"1.png",TestData::png(TestData::solid(Qt::green,{43,67}))}})));
        // An actual directory-entry change supplies the evidence for this old folder PNG.
        QVERIFY(TestData::write(folder+"/note.ini","changed"));
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
            QVERIFY(!cachedCover(folder,{},cache,{},false,0,false,false,1).image.isNull());
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
