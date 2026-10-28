// SPDX-License-Identifier: GPL-3.0-only
#include "test_support.h"
#include "archivenames.h"
#include "apng.h"
#include <QImageReader>
#include <algorithm>
#include <clocale>

class FormatTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        // QApplication can upgrade C to C.UTF-8. Restore C after startup so
        // the C-locale suite really exercises the archive locale adapter.
        if(qgetenv("LC_ALL")=="C") QVERIFY(std::setlocale(LC_ALL,"C")!=nullptr);
    }
    void filenamePolicies() {
        QString decoded;
        QVERIFY(decodeStrict(QByteArray::fromHex("93fa967b"), "CP932", decoded)); QCOMPARE(decoded, QString("日本"));
        QVERIFY(!decodeStrict(QByteArray::fromHex("ff"), "UTF-8", decoded));
        QVERIFY(!decodeStrict(QByteArray("a\0b",3), "UTF-8", decoded));
        ArchiveName n; n.rawName = QByteArray::fromHex("955c") + ".png";
        QCOMPARE(decodeName(n,{}).text, QString("表.png")); // 0x5c is a CP932 trail byte.
        n.hasDeclaredName = true; n.declaredName = "Unicode.png";
        QCOMPARE(decodeName(n,{}).text, QString("Unicode.png"));
        NamePolicy force; force.force = true; QCOMPARE(decodeName(n,force).text, QString("表.png"));
        n.hasDeclaredName = false; n.invalidUnicode = true;
        QCOMPARE(decodeName(n,{}).encoding, QString("raw"));
        QVERIFY(!decodeName(n,{}).text.isEmpty());
    }
    void naturalSort() {
        QStringList names{"10.png","02.png","2.png","1.png","99999999999999999999.png","日本2.png","日本10.png"};
        std::sort(names.begin(),names.end(),naturalLess);
        QCOMPARE(names, (QStringList{"1.png","2.png","02.png","10.png","99999999999999999999.png","日本2.png","日本10.png"}));
    }
    void rawZipNamesAndReinterpretation() {
        const QByteArray localeBefore(std::setlocale(LC_CTYPE,nullptr));
        QTemporaryDir d; QVERIFY(d.isValid()); const auto pixels=TestData::png(TestData::solid());
        const auto raw=QByteArray::fromHex("955c")+"2.png";
        const auto bytes=TestData::zip({{raw,pixels},{QString("日本10.png").toUtf8(),pixels,0x800}});
        const auto path=d.filePath("日本語の書庫.zip"); QVERIFY(TestData::write(path,bytes));
        const auto names=readZipNames(path); QVERIFY2(names.error.isEmpty(),qPrintable(names.error));
        QCOMPARE(names.entries.size(),2); QCOMPARE(names.entries[0].rawName,raw);
        auto opened=Book::open(path); QVERIFY2(opened.book,qPrintable(opened.error));
        QCOMPARE(opened.book->pages.size(),2);
        QStringList labels; for(const auto &p:opened.book->pages) labels << p.name;
        QVERIFY(labels.contains("表2.png")); QVERIFY(labels.contains("日本10.png"));
        for(int i=0;i<2;++i) QCOMPARE(opened.book->image(i).image.size(),QSize(32,48));
        NamePolicy p; p.legacyEncoding="CP437"; auto changed=opened.book->reinterpretNames(p);
        QSet<QString> before,after;
        for(const auto &x:opened.book->pages) before.insert(x.identity);
        for(const auto &x:changed->pages) after.insert(x.identity);
        QCOMPARE(before,after);
        QVERIFY(std::any_of(changed->pages.begin(),changed->pages.end(),[](const auto &x){return x.name=="日本10.png";}));
        QCOMPARE(QByteArray(std::setlocale(LC_CTYPE,nullptr)),localeBefore);
    }
    void foldersAndInitialPage() {
        QTemporaryDir d; QVERIFY(TestData::imageFolder(d.filePath("pages"),3));
        QVERIFY(TestData::imageFolder(d.filePath("pages/sub"),1));
        const auto direct=Book::open(d.filePath("pages/2.png")); QVERIFY(direct.book);
        QCOMPARE(direct.book->pages.size(),3); QCOMPARE(direct.book->initialPage,1);
        OpenOptions o; o.recursive=true; auto recursive=Book::open(d.filePath("pages"),{},o);
        QVERIFY(recursive.book); QCOMPARE(recursive.book->pages.size(),4);
        o.cancel=[] {return true;}; QVERIFY(!Book::open(d.filePath("pages"),{},o).error.isEmpty());
    }
    void nestedArchive() {
        QTemporaryDir d; const auto inner=TestData::zip({{"1.png",TestData::png(TestData::solid(Qt::blue))}});
        const auto path=d.filePath("nested.zip"); QVERIFY(TestData::write(path,TestData::zip({{"inner.zip",inner}})));
        QVERIFY(!Book::open(path).book);
        OpenOptions o; o.nestedArchives=true; auto b=Book::open(path,{},o);
        QVERIFY2(b.book,qPrintable(b.error)); QCOMPARE(b.book->pages.size(),1);
        QCOMPARE(b.book->image(0).image.pixelColor(0,0),QColor(Qt::blue));
    }
    void multipageTiff() {
        if(!imageExtensions().contains("tiff")) QSKIP("Qt TIFF plugin is not installed");
        QTemporaryDir d; const auto path=d.filePath("two.tiff"); QVERIFY(TestData::write(path,TestData::tiff()));
        auto b=Book::open(path); QVERIFY2(b.book,qPrintable(b.error)); QCOMPARE(b.book->pages.size(),2);
        QCOMPARE(b.book->image(0).image.pixelColor(0,0),QColor(Qt::black));
        QCOMPARE(b.book->image(1).image.pixelColor(0,0),QColor(Qt::white));
    }
    void apngFramesAndCrc() {
        auto bytes=TestData::apng(); bool recognized=false; auto media=decodeApng(bytes,recognized);
        QVERIFY(recognized); QVERIFY2(media.error.isEmpty(),qPrintable(media.error));
        QCOMPARE(media.frames.size(),2); QCOMPARE(media.delays,(QVector<int>{100,100})); QCOMPARE(media.loopCount,-1);
        QCOMPARE(media.frames[0].pixelColor(0,0),QColor(Qt::red));
        QCOMPARE(media.frames[1].pixelColor(0,0),QColor(Qt::blue));
        bytes[bytes.size()-1]=char(bytes.back() ^ 1);
        auto invalid=decodeApng(bytes,recognized); QVERIFY(recognized); QVERIFY(!invalid.error.isEmpty()); QVERIFY(invalid.frames.isEmpty());
    }
    void changedFileAndBadInput() {
        QTemporaryDir d; QVERIFY(TestData::imageFolder(d.path(),1));
        auto b=Book::open(d.filePath("1.png")); QVERIFY(b.book);
        QVERIFY(TestData::write(d.filePath("1.png"),"changed"));
        QVERIFY(!b.book->encoded(0).error.isEmpty());
        QVERIFY(!b.book->image(-1).error.isEmpty()); QVERIFY(!b.book->image(99).error.isEmpty());
        QVERIFY(!decodeImage("not an image","broken.png").error.isEmpty());
        QVERIFY(TestData::write(d.filePath("broken.zip"),"PK")); QVERIFY(!Book::open(d.filePath("broken.zip")).error.isEmpty());
        NamePolicy invalid; invalid.legacyEncoding="invalid"; QVERIFY(!Book::open(d.path(),invalid).error.isEmpty());
    }
};
ISOLATED_TEST_MAIN(FormatTests)
#include "format_tests.moc"
