// SPDX-License-Identifier: GPL-3.0-only
#include "test_support.h"
#include "thumbnailcache.h"
#include "thumbnailsource.h"
#include <QElapsedTimer>
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
        // Make the returning viewport's old images persistent too, independently
        // of the application's best-effort deferred-write queue.
        auto disk=std::make_shared<ThumbnailDiskCache>();
        for(int row=frontier;row<std::min(grid->count(),frontier+int(grid->visibleRows().size()));++row) {
            const auto path=pages?QString{}:grid->item(row)->data(PathRole).toString()+"/page.png";
            // A successful image result does not promise an immediate write
            // while the application's deferred writer owns the directory lock.
            const auto persisted=[&] {
                const auto warm=pages?cachedPageThumbnail(book,row,disk):cachedCover(path,{},disk);
                if(warm.image.isNull())return false;
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
ISOLATED_TEST_MAIN(ThumbnailTests)
#include "thumbnail_tests.moc"
