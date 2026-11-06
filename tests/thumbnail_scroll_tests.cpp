// SPDX-License-Identifier: GPL-3.0-only
#include "test_support.h"
#include "thumbnailcache.h"
#include "thumbnailgrid.h"
#include "thumbnailsource.h"
#include <QElapsedTimer>
#include <QScrollBar>
#include <algorithm>

// Isolate real 220-item persistence from the source-access contract tests.
class ThumbnailScrollTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {QCoreApplication::setApplicationName("ThumbnailScrollRegression");}
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
int main(int argc,char **argv) {return isolatedTestMain<ThumbnailScrollTests>(argc,argv);}
#include "thumbnail_scroll_tests.moc"
