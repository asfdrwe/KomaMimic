// SPDX-License-Identifier: GPL-3.0-only
#include "test_support.h"
#include "bookexport.h"
#include <QtEndian>

namespace {
struct Bits {
    QByteArray data; int count=0;
    void add(quint32 value, int width=1) {
        for (int i=width-1;i>=0;--i,++count) {
            if (!(count&7)) data.append(char(0));
            if ((value>>i)&1) data[count/8]=char(quint8(data[count/8]) | (128>>(count&7)));
        }
    }
    void gap(unsigned value) {
        int n=1;
        while (value >= (1u<<(n+1))-1) ++n;
        add((1u<<(n-1))-1,n-1); add(0); add(value-((1u<<n)-1),n);
    }
    void literal(unsigned gapLength, quint32 value, int depth, bool cached=true) {
        gap(gapLength); if (cached) add(0); add(value,depth); add(0);
    }
    void hit(unsigned gapLength, int index) { gap(gapLength); add(1); add(unsigned(index),7); add(0); }
};
QByteArray header(int type, int mode, int depth, int width, int height, int paletteBits=0) {
    // Nonempty comment and dummy test both terminators and the reserved byte.
    QByteArray prefix("PIC regression"); prefix.append(char(0x1a)); prefix.append("dummy"); prefix.append(char(0));
    QByteArray h(8,char(0)); h[1]=char((mode<<4)|type);
    qToBigEndian<quint16>(quint16(depth),h.data()+2);
    qToBigEndian<quint16>(quint16(width),h.data()+4);
    qToBigEndian<quint16>(quint16(height),h.data()+6);
    if (type==15 || (type==2 && mode==0)) {
        h += QByteArray(6,char(0)); h[h.size()-2]=char(36); h[h.size()-1]=char(25);
    }
    if(type==15 && depth<=8) h.append(char(paletteBits));
    return prefix+h;
}
QByteArray strip(int type=0, int mode=0, int depth=15) {
    Bits bits; const bool cached=type!=3;
    const quint32 red = type==3 ? 31u<<10 : 31u<<5;
    const quint32 green = type==3 ? 31u<<5 : 31u<<10;
    bits.literal(1,red,depth,cached); bits.literal(2,green,depth,cached); bits.gap(4);
    return header(type,mode,depth,3,2)+bits.data;
}
QColor red5(unsigned value) { const int v=int((value<<3)|(value>>2)); return {v,0,0}; }
}
class PicTests : public QObject {
    Q_OBJECT
private slots:
    void profiles_data() {
        QTest::addColumn<int>("type"); QTest::addColumn<int>("mode");
        QTest::newRow("X68000")<<0<<0;
        QTest::newRow("generic-0")<<15<<0;
        QTest::newRow("generic-1")<<15<<1;
        QTest::newRow("generic-f")<<15<<15;
        QTest::newRow("FM-TOWNS-0")<<2<<0;
        QTest::newRow("FM-TOWNS-5")<<2<<5;
        QTest::newRow("FM-TOWNS-c")<<2<<12;
        QTest::newRow("Mac-RGB-no-cache")<<3<<0;
    }
    void profiles() {
        QFETCH(int,type); QFETCH(int,mode);
        const auto result=decodeImage(strip(type,mode),"日本語.PIC");
        QVERIFY2(result.error.isEmpty(),qPrintable(result.error)); QCOMPARE(result.image.size(),QSize(3,2));
        for(int i=0;i<6;++i) QCOMPARE(result.image.pixelColor(i%3,i/3),QColor(i<2 ? Qt::red : Qt::green));
    }
    void cacheRecencyAndWrap() {
        Bits bits;
        for (unsigned i=0;i<130;++i) bits.literal(1,(i%31+1)<<5,15);
        bits.hit(1,1); // Entry 1 has been overwritten with color 128.
        bits.hit(1,127); // Moving it to the front must retain stable indices.
        bits.literal(1,31u,15); // Oldest entry after those hits is entry 3.
        bits.hit(1,3); bits.hit(1,1);
        const auto result=decodeImage(header(0,0,15,135,1)+bits.data,"cache.pic");
        QVERIFY2(result.error.isEmpty(),qPrintable(result.error));
        for(unsigned i=0;i<130;++i) QCOMPARE(result.image.pixelColor(int(i),0),red5(i%31+1));
        QCOMPARE(result.image.pixelColor(130,0),red5(128%31+1));
        QCOMPARE(result.image.pixelColor(131,0),red5(126%31+1));
        QCOMPARE(result.image.pixelColor(132,0),QColor(Qt::blue)); QCOMPARE(result.image.pixelColor(133,0),QColor(Qt::blue));
        QCOMPARE(result.image.pixelColor(134,0),red5(128%31+1));
    }
    void implicitBlackAndEmptyFinalRun() {
        Bits bits; bits.hit(2,0); bits.literal(2,31u<<5,15); bits.gap(3);
        auto result=decodeImage(header(0,0,15,6,1)+bits.data,"initial-black.pic");
        QVERIFY2(result.error.isEmpty(),qPrintable(result.error));
        for(int i=0;i<6;++i) QCOMPARE(result.image.pixelColor(i,0),QColor(i<3 ? Qt::black : Qt::red));
        bits={}; bits.gap(7);
        result=decodeImage(header(0,0,15,6,1)+bits.data,"all-black.pic");
        QVERIFY2(result.error.isEmpty(),qPrintable(result.error)); QCOMPARE(result.image.pixelColor(5,0),QColor(Qt::black));
    }
    void chainsAndRunFill() {
        Bits bits;
        bits.gap(1); bits.add(0); bits.add(31u<<5,15); bits.add(1); // Red, start chain.
        bits.add(3,2); bits.add(0,3); // +1 on the next row; stop.
        bits.literal(2,31u,15); bits.gap(6); // Blue change point, then final run.
        auto result=decodeImage(header(0,0,15,4,2)+bits.data,"chain.pic");
        QVERIFY2(result.error.isEmpty(),qPrintable(result.error));
        for(int i=0;i<8;++i) QCOMPARE(result.image.pixelColor(i%4,i/4),QColor(i==2||i==3||i==4 ? Qt::blue : Qt::red));
        // A chain may reach the image boundary without an explicit stop code.
        bits={}; bits.gap(1); bits.add(0); bits.add(31u<<5,15); bits.add(1); bits.add(2,2); bits.add(2,2); bits.gap(8);
        result=decodeImage(header(0,0,15,4,2)+bits.data,"boundary.pic");
        QVERIFY2(result.error.isEmpty(),qPrintable(result.error)); QCOMPARE(result.image.pixelColor(3,1),QColor(Qt::red));
    }
    void chainDirections_data() {
        QTest::addColumn<quint32>("code"); QTest::addColumn<int>("length"); QTest::addColumn<int>("dx");
        QTest::newRow("left2")<<quint32(2)<<4<<-2;
        QTest::newRow("left1")<<quint32(1)<<2<<-1;
        QTest::newRow("center")<<quint32(2)<<2<<0;
        QTest::newRow("right1")<<quint32(3)<<2<<1;
        QTest::newRow("right2")<<quint32(3)<<4<<2;
    }
    void chainDirections() {
        QFETCH(quint32,code); QFETCH(int,length); QFETCH(int,dx);
        Bits bits; bits.gap(3); bits.add(0); bits.add(31u<<5,15); bits.add(1);
        bits.add(code,length); bits.add(0,3);
        bits.literal(1,31u,15); bits.gap(7);
        const auto result=decodeImage(header(0,0,15,5,2)+bits.data,"direction.pic");
        QVERIFY2(result.error.isEmpty(),qPrintable(result.error));
        const int target=7+dx;
        QCOMPARE(result.image.pixelColor((target-1)%5,(target-1)/5),QColor(Qt::blue));
        QCOMPARE(result.image.pixelColor(target%5,target/5),QColor(Qt::red));
    }
    void directDepths_data() {
        QTest::addColumn<int>("depth"); QTest::addColumn<quint32>("value"); QTest::addColumn<QColor>("expected");
        QTest::newRow("GRB444")<<12<<quint32(0xf0)<<QColor(Qt::red);
        QTest::newRow("GRB555")<<15<<quint32(31<<5)<<QColor(Qt::red);
        QTest::newRow("GRB555-I0")<<16<<quint32(31<<6)<<QColor(251,0,0);
        QTest::newRow("GRB555-I1")<<16<<quint32((31<<6)|1)<<QColor(255,4,4);
        QTest::newRow("GRB888")<<24<<quint32(0x123456)<<QColor(0x34,0x12,0x56);
    }
    void directDepths() {
        QFETCH(int,depth); QFETCH(quint32,value); QFETCH(QColor,expected);
        Bits bits; bits.literal(1,value,depth);
        const auto result=decodeImage(header(15,0,depth,1,1)+bits.data,"depth.pic");
        QVERIFY2(result.error.isEmpty(),qPrintable(result.error)); QCOMPARE(result.image.pixelColor(0,0),expected);
    }
    void palettes_data() {
        QTest::addColumn<int>("type"); QTest::addColumn<int>("depth"); QTest::addColumn<int>("precision");
        QTest::newRow("X68K-4")<<0<<4<<5;
        QTest::newRow("X68K-8")<<0<<8<<5;
        QTest::newRow("generic-4-RGB1")<<15<<4<<1;
        QTest::newRow("generic-8-RGB3")<<15<<8<<3;
        QTest::newRow("generic-8-RGB8")<<15<<8<<8;
    }
    void palettes() {
        QFETCH(int,type); QFETCH(int,depth); QFETCH(int,precision);
        Bits palette;
        for(int i=0;i<(1<<depth);++i) {
            const int shared=type==0 ? 1 : 0;
            palette.add(i==1 ? quint32(((1<<precision)-1)<<(precision+shared)) : 0,precision*3+shared);
        }
        Bits bits; bits.literal(1,1,depth,false);
        const auto result=decodeImage(header(type,0,depth,1,1,precision)+palette.data+bits.data,"palette.pic");
        QVERIFY2(result.error.isEmpty(),qPrintable(result.error));
        QCOMPARE(result.image.pixelColor(0,0),QColor(type==0 ? 251 : 255,0,0));
    }
    void pc88_data() {
        QTest::addColumn<int>("depth"); QTest::addColumn<quint32>("value");
        QTest::newRow("GRB332")<<8<<quint32(7<<2);
        QTest::newRow("GRB444")<<12<<quint32(15<<4);
        QTest::newRow("GRB655")<<16<<quint32(31<<5);
    }
    void pc88() {
        QFETCH(int,depth); QFETCH(quint32,value);
        Bits bits; bits.literal(1,value,depth,depth>8); bits.gap(64000);
        const auto result=decodeImage(header(1,0,depth,320,200)+bits.data,"PC88.pic");
        QVERIFY2(result.error.isEmpty(),qPrintable(result.error)); QCOMPARE(result.image.size(),QSize(320,200));
        QCOMPARE(result.image.pixelColor(319,199),QColor(Qt::red));
    }
    void corruptAndUnsupported() {
        QVERIFY(!decodeImage("PIC","truncated.pic").error.isEmpty());
        QVERIFY(!decodeImage(header(0,0,15,1,1),"missing-data.pic").error.isEmpty());
        auto truncated=strip(); truncated.chop(3); QVERIFY(!decodeImage(truncated,"cut.pic").error.isEmpty());
        for (const auto &bytes : {header(9,0,15,1,1),header(0,0,15,65535,65535),header(15,0,32,1,1),
                                 header(1,2,16,320,200),header(15,0,8,1,1,0),header(0,0,15,0,1)})
            QVERIFY(!decodeImage(bytes+QByteArray(20,char(0)),"invalid.pic").error.isEmpty());
        QVERIFY(!decodeImage(header(0,0,15,2,2)+QByteArray(8,char(0xff)),"overflow.pic").error.isEmpty());
        Bits cycle; cycle.gap(1); cycle.add(0); cycle.add(31u<<5,15); cycle.add(1);
        for(int i=0;i<20;++i) cycle.add(1,2); // width 1, dx -1: no progress.
        cycle.add(0,3);
        QVERIFY(decodeImage(header(0,0,15,1,2)+cycle.data,"cycle.pic").error.contains("work limit"));
    }
    void folderArchiveGuiAndExport() {
        QVERIFY(isImagePath("image.PIC"));
        QTemporaryDir d; QVERIFY(d.isValid());
        const auto path=d.filePath("日本語.pic"); QVERIFY(TestData::write(path,strip()));
        auto book=Book::open(path); QVERIFY2(book.book,qPrintable(book.error)); QCOMPARE(book.book->pages.size(),1);
        const auto thumb=book.book->image(0,QSize(2,2)); QVERIFY(thumb.error.isEmpty()); QCOMPARE(thumb.image.size(),QSize(2,1));
        const auto png=d.filePath("out.png"); QVERIFY(exportPages(book.book,{0},png,ExportKind::Image).isEmpty());
        QCOMPARE(QImage(png).pixelColor(0,0),QColor(Qt::red));
        MainWindow window(d.filePath("settings.ini")); window.show(); window.openBook(path);
        QTRY_VERIFY_WITH_TIMEOUT(!window.isLoading()&&window.renderReady(),10000);
        QCOMPARE(window.currentPageName(),QString("日本語.pic")); window.close();
        const auto archive=d.filePath("旧形式.zip");
        QVERIFY(TestData::write(archive,TestData::zip({{QString("日本語.pic").toUtf8(),strip(),0x800}})));
        book=Book::open(archive); QVERIFY2(book.book,qPrintable(book.error));
        QCOMPARE(book.book->image(0).image.pixelColor(0,0),QColor(Qt::red));
        QCOMPARE(decodeImage(strip(),"renamed.bin").image.pixelColor(0,0),QColor(Qt::red));
        QCOMPARE(decodeImage(TestData::png(TestData::solid(Qt::blue)),"renamed.pic").image.pixelColor(0,0),QColor(Qt::blue));
    }
};
ISOLATED_TEST_MAIN(PicTests)
#include "pic_tests.moc"
