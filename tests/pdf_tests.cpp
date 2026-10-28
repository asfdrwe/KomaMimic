// SPDX-License-Identifier: GPL-3.0-only
#include "test_support.h"
#include "documentsource.h"
#include <future>

class PdfTests : public QObject {
    Q_OBJECT
private slots:
    void pagesAndUnicodePath() {
        QTemporaryDir d; const auto path=d.filePath("日本語 # % の本.pdf"); QVERIFY(TestData::pdf(path));
        const auto info=inspectDocument(path); QVERIFY2(info.error.isEmpty(),qPrintable(info.error)); QCOMPARE(info.pages,2);
        auto b=Book::open(path); QVERIFY2(b.book,qPrintable(b.error)); QCOMPARE(b.book->pages.size(),2);
        auto first=b.book->image(0),second=b.book->image(1);
        QVERIFY2(first.error.isEmpty(),qPrintable(first.error)); QVERIFY(second.error.isEmpty());
        QCOMPARE(first.image.height(),2400);
        QVERIFY(first.image.pixelColor(first.image.width()/2,1200).red()>240);
        QVERIFY(second.image.pixelColor(second.image.width()/2,1200).blue()>240);
        const auto thumbnail=b.book->image(0,{120,160});
        QVERIFY(thumbnail.error.isEmpty()); QVERIFY(thumbnail.image.width()<=120); QVERIFY(thumbnail.image.height()<=160);
    }
    void imageResources() {
        QTemporaryDir d; const auto path=d.filePath("images.pdf"); QVERIFY(TestData::pdf(path));
        auto resources=inspectPdfResources(path); QVERIFY2(resources.error.isEmpty(),qPrintable(resources.error));
        QCOMPARE(resources.images.size(),2);
        OpenOptions o; o.pdfResources=true; const auto b=Book::open(path,{},o);
        QVERIFY2(b.book,qPrintable(b.error)); QCOMPARE(b.book->pages.size(),2);
        QCOMPARE(b.book->image(0).image.size(),QSize(32,48));
        const auto pixel=b.book->image(1).image.pixelColor(0,0); QVERIFY(pixel.blue()>240 && pixel.red()<10);
        QVERIFY(!renderPdfResource(path,0,9999).error.isEmpty());
    }
    void invalidInput() {
        QTemporaryDir d; const auto path=d.filePath("broken.pdf"); QVERIFY(TestData::write(path,"%PDF-1.7\ntruncated"));
        QVERIFY(!inspectDocument(path).error.isEmpty()); QVERIFY(!renderDocument(path,0).error.isEmpty());
        QVERIFY(!inspectDocument(d.filePath("missing.pdf")).error.isEmpty());
        const auto valid=d.filePath("valid.pdf"); QVERIFY(TestData::pdf(valid));
        QVERIFY(!renderDocument(valid,-1).error.isEmpty()); QVERIFY(!renderDocument(valid,2).error.isEmpty());
    }
    void concurrentRenderingWithoutExternalTools() {
        QTemporaryDir d; const auto path=d.filePath("parallel.pdf"); QVERIFY(TestData::pdf(path));
        const auto oldPath=qgetenv("PATH"); qputenv("PATH","");
        auto a=std::async(std::launch::async,[path]{return renderDocument(path,0,{160,240});});
        auto b=std::async(std::launch::async,[path]{return renderDocument(path,1,{160,240});});
        const auto first=a.get(),second=b.get(); qputenv("PATH",oldPath);
        QVERIFY2(first.error.isEmpty(),qPrintable(first.error)); QVERIFY2(second.error.isEmpty(),qPrintable(second.error));
        QVERIFY(first.image.width()>0 && first.image.width()<=160);
        QVERIFY(first.image.height()>0 && first.image.height()<=240);
        QCOMPARE(second.image.size(),first.image.size());
        QVERIFY(first.image.pixelColor(first.image.width()/2,first.image.height()/2).red()>240);
        QVERIFY(second.image.pixelColor(second.image.width()/2,second.image.height()/2).blue()>240);
    }
};
ISOLATED_TEST_MAIN(PdfTests)
#include "pdf_tests.moc"
