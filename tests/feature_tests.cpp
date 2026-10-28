// SPDX-License-Identifier: GPL-3.0-only
#include "test_support.h"
#include "bookexport.h"
#include "librarystore.h"
#include "readerfeatures.h"
#include "zipmetadata.h"
#include <QAction>

class FeatureTests : public QObject {
    Q_OBJECT
private slots:
    void exportRoundTrip_data() {
        QTest::addColumn<int>("kind"); QTest::addColumn<QString>("extension");
        QTest::newRow("zip") << int(ExportKind::Zip) << "zip";
        QTest::newRow("pdf") << int(ExportKind::Pdf) << "pdf";
        QTest::newRow("gif") << int(ExportKind::Gif) << "gif";
    }
    void exportRoundTrip() {
        QFETCH(int,kind); QFETCH(QString,extension);
        QTemporaryDir d; QVERIFY(TestData::imageFolder(d.filePath("pages"),3));
        const auto b=Book::open(d.filePath("pages")); QVERIFY(b.book);
        const auto path=d.filePath("書き出し."+extension);
        const auto error=exportPages(b.book,{0,1,2},path,ExportKind(kind)); QVERIFY2(error.isEmpty(),qPrintable(error));
        const auto reopened=Book::open(path); QVERIFY2(reopened.book,qPrintable(reopened.error));
        if(ExportKind(kind)==ExportKind::Gif) {
            const auto media=reopened.book->image(0); QVERIFY2(media.error.isEmpty(),qPrintable(media.error)); QCOMPARE(media.frames.size(),3);
            for(int i=0;i<3;++i) {
                const auto expected=b.book->image(i).image.pixelColor(0,0),actual=media.frames[i].pixelColor(0,0);
                QVERIFY(std::abs(actual.red()-expected.red())<=36);
                QVERIFY(std::abs(actual.green()-expected.green())<=36);
                QVERIFY(std::abs(actual.blue()-expected.blue())<=85);
            }
        } else {
            QCOMPARE(reopened.book->pages.size(),3);
            for(int i=0;i<3;++i) {
                auto image=reopened.book->image(i); QVERIFY(image.error.isEmpty()); QVERIFY(!image.image.isNull());
                const auto actual=image.image.pixelColor(image.image.width()/2,image.image.height()/2);
                const auto expected=b.book->image(i).image.pixelColor(0,0);
                QVERIFY(std::abs(actual.red()-expected.red())<5);
                QVERIFY(std::abs(actual.green()-expected.green())<5);
                QVERIFY(std::abs(actual.blue()-expected.blue())<5);
            }
        }
    }
    void originalSpreadAndCancellation() {
        QTemporaryDir d; QVERIFY(TestData::imageFolder(d.filePath("pages"),3));
        const auto b=Book::open(d.filePath("pages")); QVERIFY(b.book);
        const auto original=d.filePath("copy.png"); QVERIFY(exportPages(b.book,{0},original,ExportKind::Original).isEmpty());
        QFile file(original); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(),b.book->encoded(0).bytes); file.close();
        const auto combined=composeSpread(TestData::solid(Qt::red),TestData::solid(Qt::blue),true);
        QCOMPARE(combined.size(),QSize(64,48)); QCOMPARE(combined.pixelColor(0,0),QColor(Qt::blue));
        QCOMPARE(combined.pixelColor(63,0),QColor(Qt::red));
        const auto spread=d.filePath("spread.zip");
        QVERIFY(exportPages(b.book,{0,1,2},spread,ExportKind::Zip,{},90,1000,{},true).isEmpty());
        const auto reopened=Book::open(spread); QVERIFY(reopened.book); QCOMPARE(reopened.book->pages.size(),2);
        QCOMPARE(reopened.book->image(0).image.size(),QSize(64,48));
        for(const auto kind:{ExportKind::Zip,ExportKind::Pdf,ExportKind::Gif}) {
            const auto output=d.filePath(QString("cancel-%1").arg(int(kind))); QVERIFY(TestData::write(output,"keep original"));
            int calls=0;
            const auto error=exportPages(b.book,{0,1,2},output,kind,{},90,1000,[&calls](int,int){return ++calls<2;});
            QVERIFY(!error.isEmpty()); QCOMPARE(calls,2);
            QFile saved(output); QVERIFY(saved.open(QIODevice::ReadOnly)); QCOMPARE(saved.readAll(),QByteArray("keep original"));
        }
        QVERIFY(!exportPages(b.book,{99},d.filePath("invalid.zip"),ExportKind::Zip).isEmpty());
        QVERIFY(!QFile::exists(d.filePath("invalid.zip")));
    }
    void zipMetadataPreservesComment() {
        QTemporaryDir d; const auto path=d.filePath("meta.zip"); const QByteArray comment("unknown legacy comment\0tail",27);
        QVERIFY(TestData::write(path,TestData::zip({{"1.png",TestData::png(TestData::solid())}},comment)));
        const QJsonObject data{{"title","日本語"},{"page",2}};
        QVERIFY(writeZipMetadata(path,path,data).isEmpty());
        auto result=readZipMetadata(path); QVERIFY(result.error.isEmpty()); QCOMPARE(result.data,data); QCOMPARE(result.originalComment,comment);
        QVERIFY(writeZipMetadata(path,path,QJsonObject{{"page",3}}).isEmpty());
        result=readZipMetadata(path); QCOMPARE(result.data.value("page").toInt(),3); QCOMPARE(result.originalComment,comment);
        const auto b=Book::open(path); QVERIFY(b.book); QVERIFY(!b.book->image(0).image.isNull());
    }
    void independentCollectionsPersist() {
        QTemporaryDir d; QString favorite,history;
        {
            LibraryStore db(d.filePath("library.sqlite3")); QVERIFY2(db.error().isEmpty(),qPrintable(db.error()));
            favorite=db.add("favorite","/book.zip"); history=db.add("history","/book.zip");
            QVERIFY(!favorite.isEmpty()); QVERIFY(!history.isEmpty()); QVERIFY(favorite!=history);
            QCOMPARE(db.add("favorite","/book.zip"),favorite);
            QVERIFY(db.update(favorite,{{"title","Favorite"}},{{"anchor",1}}));
            QVERIFY(db.update(history,{},{{"anchor",4}}));
        }
        LibraryStore db(d.filePath("library.sqlite3")); QVERIFY(db.error().isEmpty());
        QCOMPARE(db.item(favorite).state.value("anchor").toInt(),1); QCOMPARE(db.item(history).state.value("anchor").toInt(),4);
        QCOMPARE(db.item(favorite).title,QString("Favorite"));
        QVERIFY(db.movePath("/book.zip","/renamed.zip")); QCOMPARE(db.item(history).path,QString("/renamed.zip"));
        QVERIFY(db.clear("history")); QVERIFY(db.items("history").isEmpty()); QCOMPARE(db.items("favorite").size(),1);
    }
    void editUndoRedoAndResume() {
        QTemporaryDir d; const auto images=d.filePath("pages"); QVERIFY(TestData::imageFolder(images,4));
        const auto settings=d.filePath("settings.ini"),book=images+"/1.png";
        {
            MainWindow w(settings); w.show(); w.openBook(book); QTRY_VERIFY_WITH_TIMEOUT(!w.isLoading() && w.renderReady(),10000);
            TestData::singlePage(w); QTRY_VERIFY(w.renderReady());
            w.command("editDelete")->trigger(); QTRY_VERIFY(w.renderReady()); QCOMPARE(w.pageCount(),3); QCOMPARE(w.currentPageName(),QString("2.png"));
            w.command("editUndo")->trigger(); QTRY_VERIFY(w.renderReady()); QCOMPARE(w.pageCount(),4); QCOMPARE(w.currentPageName(),QString("1.png"));
            w.command("editRedo")->trigger(); QTRY_VERIFY(w.renderReady()); QCOMPARE(w.pageCount(),3);
            w.command("insertBlank")->trigger(); QTRY_VERIFY(w.renderReady()); QCOMPARE(w.pageCount(),4); QCOMPARE(w.currentPageName(),QString("白紙.png"));
            w.nextPage(); QTRY_VERIFY(w.renderReady()); QCOMPARE(w.currentPageName(),QString("2.png"));
            w.close();
        }
        MainWindow restored(settings); restored.show(); restored.runRequest({{"path",book},{"entry",0}});
        QTRY_VERIFY_WITH_TIMEOUT(!restored.isLoading() && restored.renderReady(),10000);
        QCOMPARE(restored.pageCount(),4); QCOMPARE(restored.currentPageName(),QString("2.png"));
        restored.previousPage(); QTRY_VERIFY(restored.renderReady()); QCOMPARE(restored.currentPageName(),QString("白紙.png")); restored.close();
        QCOMPARE(Book::open(book).book->pages.size(),4); // Logical edits did not touch source images.
    }
    void legacyCommandsResolve() {
        QSet<QString> ids; for(const auto &spec:shortcutSpecs()) ids.insert(spec.id);
        for(int n=1;n<=175;++n) {
            const auto id=legacyCommandId(n); QVERIFY2(!id.isEmpty(),qPrintable(QString::number(n)));
            QVERIFY2(ids.contains(id),qPrintable(QString("%1 -> %2").arg(n).arg(id)));
        }
    }
};
ISOLATED_TEST_MAIN(FeatureTests)
#include "feature_tests.moc"
