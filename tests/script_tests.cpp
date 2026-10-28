// SPDX-License-Identifier: GPL-3.0-only
#include "test_support.h"
#include "scriptreader.h"
#include "bookexport.h"
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QtEndian>
#include <atomic>
#include <barrier>
#include <thread>
#include <vector>

namespace {
void le16(QByteArray &b, int p, quint16 n) { qToLittleEndian(n, b.data()+p); }
void le32(QByteArray &b, int p, quint32 n) { qToLittleEndian(n, b.data()+p); }
void be16(QByteArray &b, int p, quint16 n) { qToBigEndian(n, b.data()+p); }
void be32(QByteArray &b, int p, quint32 n) { qToBigEndian(n, b.data()+p); }
// Self-authored fixtures. No historical artwork or proprietary plugin binaries.
QByteArray mag(bool eight = false, bool aspect = false, int left = 0, int right = 7) {
    QByteArray prefix("MAKI02  PC98 regression"); prefix.append(char(0x1a));
    QByteArray header(32, char(0)); header[3] = char(eight ? 128 : aspect ? 1 : 0);
    le16(header,4,quint16(left)); le16(header,8,quint16(right)); le16(header,10,2);
    QByteArray palette;
    for (int i=0; i<(eight ? 256 : 16); ++i) {
        const char component = char(eight ? i : i*16);
        palette.append(component); palette.append(component); palette.append(component);
    }
    const int a = 32+palette.size();
    // Four packed bytes per row. Actions: literal row, copy up, copy up.
    le32(header,12,a); le32(header,16,a+1); le32(header,20,1); le32(header,24,a+2); le32(header,28,4);
    return prefix+header+palette+QByteArray::fromHex("404412345678");
}
QByteArray picBlock(const QByteArray &id, const QByteArray &data, int w, int h,
                  int x=0, int y=0, bool transparent=false, quint32 key=0) {
    QByteArray block(26,char(0)); block.replace(0,4,id);
    be32(block,4,quint32(26+data.size())); be16(block,8,transparent ? 1 : 0);
    be16(block,10,quint16(w)); be16(block,12,quint16(h)); be16(block,14,quint16(x)); be16(block,16,quint16(y));
    be32(block,18,key); return block+data;
}
QByteArray pic(const QByteArray &blocks, int w, int h, int depth=24) {
    QByteArray header(125,char(0)); header.replace(0,4,"P2DT");
    for (int p : {30,62,94}) be16(header,p,0x0d0a);
    be16(header,96,0x1a00); be32(header,106,125); be16(header,110,quint16(depth));
    be16(header,112,1); be16(header,114,1); be16(header,116,quint16(w)); be16(header,118,quint16(h));
    return header+blocks+QByteArray(8,char(0));
}
struct Bits {
    QByteArray data; int count=0;
    void add(quint32 value, int width) {
        for (int i=width-1;i>=0;--i,++count) {
            if (!(count&7)) data.append(char(0));
            if ((value>>i)&1) data[count/8] = char(quint8(data[count/8]) | (128>>(count&7)));
        }
    }
};
QByteArray fastData(bool cache, bool chain, int depth) {
    Bits bits; const int n=depth/3, m=(1<<n)-1;
    bits.add(0,1); // Initial length: immediate change.
    bits.add(0,1); bits.add(quint32(m<<n),depth); // Literal red, stored GRB.
    bits.add(chain ? 3 : 0,chain ? 2 : 1); // Vertical chain or none.
    if (!cache) bits.add(4,3); // Length 1: repeat once.
    else {
        bits.add(0,1); // Length 0.
        bits.add(0,1); bits.add(quint32(m<<n),depth); // Populate the red predecessor's cache too.
        bits.add(0,1); bits.add(0,1);
        bits.add(1,1); bits.add(0,6); // Cache hit, first entry (red).
        bits.add(0,1); bits.add(0,1); // No chain; length 0.
        return bits.data;
    }
    if (chain) bits.add(0,1); // Chained pixel on row 2 has no successor.
    bits.add(0,1); bits.add(quint32(m),depth); // Literal blue.
    bits.add(0,1); bits.add(0,1); // No chain; final length 0.
    return bits.data;
}
QByteArray arithmeticData(int depth, bool hit=false) {
    QByteArray data(256,char(0));
    for (int i=0;i<128;++i) be16(data,i*2,32768);
    // Independently encoded decision streams using an unbounded low/range
    // interval encoder, midpoint termination. 2x2: red/red, red/blue, with a
    // downward chain. 2x1: two red pixels, second obtained from the cache.
    return data+QByteArray::fromHex(depth==15 ? (hit ? "e00466c000" : "e003bd9044182000")
                                                          : (hit ? "df9b3600" : "df95eb80204000"));
}
QByteArray solidScript() {
    return "function decode(input,ctx){if(!(input instanceof ArrayBuffer)||ctx.apiVersion!==1)throw 'API';"
           "return {width:1,height:1,pixels:new Uint8Array([10,20,30,255]).buffer};}";
}
}
class ScriptTests : public QObject {
    Q_OBJECT
private slots:
    void registryPreviewAndApiOneValidation() {
        QTemporaryDir d;const auto manifest=d.filePath("manifest.json");
        const auto original=scriptReaderFingerprint();
        const auto cleanup=qScopeGuard([]{configureScriptReaders({});});
        const auto package=[](int api,const QJsonObject &schema=QJsonObject{}) {
            return QJsonDocument(QJsonObject{{"id","test-api-one"},{"apiVersion",api},
                {"multipage",true},{"extensions",QJsonArray{"apione"}}, {"magic","415049"},
                {"script","decode.js"},{"settings",schema}}).toJson();
        };
        QVERIFY(TestData::write(manifest,package(1,{{"red",QJsonObject{{"type","integer"},{"minimum",0},{"maximum",255},{"default",20}}}})));
        QVERIFY(TestData::write(d.filePath("decode.js"),
            "function inspect(b,c){if(c.apiVersion!==1)throw 'API';return {pages:2};}"
            "function decode(b,c){if(c.apiVersion!==1)throw 'API';return {width:1,height:1,pixels:new Uint8Array([c.settings.red,c.pageIndex,0,255]).buffer};}"));
        QVariantList preview;QVERIFY(inspectScriptReaders({d.path()},preview).isEmpty());
        QCOMPARE(scriptReaderFingerprint(),original);QVERIFY(!scriptImageExtensions().contains("apione"));
        QVERIFY(configureScriptReaders({d.path()}).isEmpty());const auto active=scriptReaderFingerprint();
        QString error;QCOMPARE(scriptPageCount("API","test.apione",error),2);QVERIFY(error.isEmpty());
        bool recognized=false;const auto page=decodeScriptImage("API","test.apione",{},recognized,true,1,
            {{"test-api-one",QVariantMap{{"red",80}}}});
        QVERIFY(recognized);QVERIFY2(page.error.isEmpty(),qPrintable(page.error));QCOMPARE(page.image.pixelColor(0,0),QColor(80,1,0));
        QVERIFY(TestData::write(manifest,package(2)));
        QVERIFY(!configureScriptReaders({d.path()}).isEmpty());QCOMPARE(scriptReaderFingerprint(),active);
        QVERIFY(!inspectScriptReaders({d.path()},preview).isEmpty());QCOMPARE(scriptReaderFingerprint(),active);
        QVERIFY(TestData::write(manifest,package(1,{{"enabled",QJsonObject{{"type","boolean"},{"default","yes"}}}})));
        QVERIFY(!registerScriptReaders(d.path()).isEmpty());QCOMPARE(scriptReaderFingerprint(),active);
        QVERIFY(TestData::write(manifest,package(1,{{"count",QJsonObject{{"type","integer"},{"minimum",0},{"maximum",10},{"default",1.5}}}})));
        QVERIFY(!registerScriptReaders(d.path()).isEmpty());QCOMPARE(scriptReaderFingerprint(),active);
        QVERIFY(configureScriptReaders({}).isEmpty());QCOMPARE(scriptReaderFingerprint(),original);
    }
    void concurrentRegistryReads() {
        // Startup cover jobs enumerate extensions while image decoding and
        // settings queries hold implicitly shared copies of the same registry.
        const auto extensions=scriptImageExtensions();
        const auto schemas=scriptSettingsSchemas();
        const auto filters=scriptFilterIds();
        const auto multipage=scriptMultipageExtensions();
        std::atomic<bool> valid{true};
        std::barrier start(8);
        std::vector<std::thread> workers;
        for(int worker=0;worker<8;++worker) workers.emplace_back([&,worker] {
            start.arrive_and_wait();
            for(int i=0;i<4000;++i) {
                if((worker+i)%2==0) {
                    if(scriptImageExtensions()!=extensions) valid=false;
                } else {
                    bool recognized=true;
                    const auto result=decodeScriptImage(QByteArray("not an image"),"unknown.bin",{},recognized,false);
                    if(recognized || !result.error.isEmpty() || scriptSettingsSchemas()!=schemas ||
                       scriptFilterIds()!=filters || scriptMultipageExtensions()!=multipage) valid=false;
                }
            }
        });
        for(auto &worker:workers) worker.join();
        QVERIFY(valid.load());
    }
    void formatsAndMag() {
        for (const auto *extension : {"mag","max","p2","pic2"}) QVERIFY(imageExtensions().contains(extension));
        auto decoded=decodeImage(mag(),"日本.mag"); QVERIFY2(decoded.error.isEmpty(),qPrintable(decoded.error));
        QCOMPARE(decoded.image.size(),QSize(8,3));
        for (int y=0;y<3;++y) for(int x=0;x<8;++x) QCOMPARE(decoded.image.pixelColor(x,y),QColor((x+1)*17,(x+1)*17,(x+1)*17));
        decoded=decodeImage(mag(false,true,3,6),"crop.mag"); QVERIFY2(decoded.error.isEmpty(),qPrintable(decoded.error));
        QCOMPARE(decoded.image.size(),QSize(4,6)); QCOMPARE(decoded.image.pixelColor(0,5),QColor(68,68,68));
        decoded=decodeImage(mag(true,false,0,3),"256.mag"); QVERIFY2(decoded.error.isEmpty(),qPrintable(decoded.error));
        QCOMPARE(decoded.image.pixelColor(0,2),QColor(0x12,0x12,0x12));
        QCOMPARE(decoded.image.pixelColor(3,2),QColor(0x78,0x78,0x78));
        decoded=decodeImage(mag(),"renamed.bin",QSize(4,4)); QVERIFY(decoded.error.isEmpty()); QCOMPARE(decoded.image.size(),QSize(4,1));
        decoded=decodeImage(TestData::png(TestData::solid(Qt::blue)),"renamed.mag");
        QVERIFY(decoded.error.isEmpty()); QCOMPARE(decoded.image.pixelColor(0,0),QColor(Qt::blue));
        for (const auto &[machine, expected] : QVector<QPair<int,int>>{{0x68,82},{0x99,80},{3,73}}) {
            auto bytes=mag(); const int start=bytes.indexOf(char(0x1a))+1; bytes[start+1]=char(machine);
            decoded=decodeImage(bytes,"palette.mag"); QVERIFY2(decoded.error.isEmpty(),qPrintable(decoded.error));
            QCOMPARE(decoded.image.pixelColor(4,0),QColor(expected,expected,expected));
        }
        // MSXUTILS.MAG uses machine 03 / flags 04 / mode 01. Reconstruct
        // that header combination without distributing the original artwork.
        auto msx=mag(false,true); const int start=msx.indexOf(char(0x1a))+1;
        msx[start+1]=char(3); msx[start+2]=char(4);
        decoded=decodeImage(msx,"msx-200line.mag");
        QVERIFY2(decoded.error.isEmpty(),qPrintable(decoded.error)); QCOMPARE(decoded.image.size(),QSize(8,6));
        for (int y=0;y<6;++y) QCOMPARE(decoded.image.pixelColor(4,y),QColor(73,73,73));
    }
    void picRaw_data() {
        QTest::addColumn<QByteArray>("id"); QTest::addColumn<QByteArray>("data"); QTest::addColumn<int>("depth");
        QTest::newRow("RGB24-BM")<<QByteArray("P2BM")<<QByteArray::fromHex("ff000000ff000000ff")<<24;
        QTest::newRow("RGB24-BI")<<QByteArray("P2BI")<<QByteArray::fromHex("ff000000ff000000ff")<<24;
        QTest::newRow("GRB15-BM")<<QByteArray("P2BM")<<QByteArray::fromHex("07c0f800003e")<<15;
        QTest::newRow("GRB15-BI")<<QByteArray("P2BI")<<QByteArray::fromHex("c00700f83e00")<<15;
    }
    void picRaw() {
        QFETCH(QByteArray,id); QFETCH(QByteArray,data); QFETCH(int,depth);
        const auto decoded=decodeImage(pic(picBlock(id,data,3,1),3,1,depth),"sample.p2");
        QVERIFY2(decoded.error.isEmpty(),qPrintable(decoded.error));
        QCOMPARE(decoded.image.size(),QSize(3,1));
        QCOMPARE(decoded.image.pixelColor(0,0),QColor(Qt::red)); QCOMPARE(decoded.image.pixelColor(1,0),QColor(Qt::green));
        QCOMPARE(decoded.image.pixelColor(2,0),QColor(Qt::blue));
    }
    void picCompressed_data() {
        QTest::addColumn<QByteArray>("id"); QTest::addColumn<int>("depth"); QTest::addColumn<bool>("cache"); QTest::addColumn<bool>("chain");
        for (int depth : {15,24}) for (bool cache : {false,true}) {
            const auto suffix=QByteArray::number(depth)+(cache ? "-cache" : "-chain");
            QTest::newRow(("arithmetic-"+suffix).constData())<<QByteArray("P2SS")<<depth<<cache<<!cache;
            QTest::newRow(("fast-"+suffix).constData())<<QByteArray("P2SF")<<depth<<cache<<!cache;
        }
        QTest::newRow("fast-run")<<QByteArray("P2SF")<<24<<false<<false;
    }
    void picCompressed() {
        QFETCH(QByteArray,id); QFETCH(int,depth); QFETCH(bool,cache); QFETCH(bool,chain);
        const auto data=id=="P2SS" ? arithmeticData(depth,cache) : fastData(cache,chain,depth);
        const int w=chain || (cache && id=="P2SS") ? 2 : 3, h=chain ? 2 : 1;
        const auto decoded=decodeImage(pic(picBlock(id,data,w,h),w,h,depth),"sample.pic2");
        QVERIFY2(decoded.error.isEmpty(),qPrintable(decoded.error));
        QCOMPARE(decoded.image.size(),QSize(w,h));
        for(int y=0;y<h;++y) for(int x=0;x<w;++x)
            QCOMPARE(decoded.image.pixelColor(x,y),QColor(!cache && x==w-1 && y==h-1 ? Qt::blue : Qt::red));
    }
    void picComposition() {
        const auto background=picBlock("P2BM",QByteArray::fromHex("0000ff0000ff"),2,1);
        const auto overlay=picBlock("P2BI",QByteArray::fromHex("000000ff0000"),2,1,0,0,true,0);
        const auto decoded=decodeImage(pic(background+overlay,3,2),"composite.p2");
        QVERIFY2(decoded.error.isEmpty(),qPrintable(decoded.error));
        QCOMPARE(decoded.image.pixelColor(0,0),QColor(Qt::blue)); QCOMPARE(decoded.image.pixelColor(1,0),QColor(Qt::red));
        QCOMPARE(decoded.image.pixelColor(2,1).alpha(),0);
    }
    void arithmeticPredictionAndContexts() {
        for (int depth : {15,24}) {
            QByteArray data(256,char(0));
            for (int i=0;i<128;++i) be16(data,i*2,quint16(12000+(i*733)%44000));
            // Independent decision stream: a vertical red / white / blue strip.
            // Nonuniform probability contexts, G-driven R/B predictor clamping.
            data += QByteArray::fromHex(depth==15 ? "cdd2451d805b0b9380" : "c766029a823ab800");
            const auto decoded=decodeImage(pic(picBlock("P2SS",data,1,3),1,3,depth),"prediction.p2");
            QVERIFY2(decoded.error.isEmpty(),qPrintable(decoded.error));
            QCOMPARE(decoded.image.pixelColor(0,0),QColor(Qt::red));
            QCOMPARE(decoded.image.pixelColor(0,1),QColor(Qt::white));
            QCOMPARE(decoded.image.pixelColor(0,2),QColor(Qt::blue));
        }
    }
    void fastLongRun() {
        Bits bits; bits.add(1023,10); bits.add(0,1); bits.add(2,10); // Length 1025 -> 1024 black pixels.
        bits.add(0,1); bits.add(0x00ff00,24); bits.add(0,1); bits.add(0,1);
        const auto decoded=decodeImage(pic(picBlock("P2SF",bits.data,1025,1),1025,1),"long.p2");
        QVERIFY2(decoded.error.isEmpty(),qPrintable(decoded.error));
        QCOMPARE(decoded.image.pixelColor(1023,0),QColor(Qt::black)); QCOMPARE(decoded.image.pixelColor(1024,0),QColor(Qt::red));
    }
    void corruptImages() {
        QVERIFY(!decodeImage("MAKI02  ","bad.mag").error.isEmpty());
        auto bad=mag(); bad.chop(1); QVERIFY(!decodeImage(bad,"bad.mag").error.isEmpty());
        bad=mag(); const int header=bad.indexOf(char(0x1a))+1;
        le32(bad,header+16,0xffffffff); QVERIFY(!decodeImage(bad,"bad.mag").error.isEmpty());
        bad=mag(); bad[bad.size()-6]=char(0xc0); bad[bad.size()-5]=char(0x10);
        QVERIFY(decodeImage(bad,"bad.mag").error.contains("back-reference"));
        bad=pic(picBlock("P2BM",QByteArray::fromHex("ff0000"),1,1),1,1); be16(bad,110,8);
        QVERIFY(decodeImage(bad,"bad.p2").error.contains("15-bit"));
        bad=pic(picBlock("P2SS",arithmeticData(24).left(258),2,2),2,2);
        QVERIFY(!decodeImage(bad,"bad.p2").error.isEmpty());
        bad=pic(picBlock("P2SF",QByteArray::fromHex("ffffff"),2,2),2,2);
        QVERIFY(!decodeImage(bad,"bad.p2").error.isEmpty());
        bad=pic(picBlock("P2BM","",65535,65535),65535,65535); QVERIFY(!decodeImage(bad,"bad.p2").error.isEmpty());
        bad=pic(picBlock("P2BM",QByteArray::fromHex("ff0000"),1,1,1),1,1); QVERIFY(!decodeImage(bad,"bad.p2").error.isEmpty());
    }
    void scriptContractAndIsolation() {
        auto decoded=runImageScript(solidScript(),"contract.js","input");
        QVERIFY2(decoded.error.isEmpty(),qPrintable(decoded.error)); QCOMPARE(decoded.image.pixelColor(0,0),QColor(10,20,30));
        QVERIFY(runImageScript(solidScript(),"large-input.js",QByteArray(1024*1024,'x')).error.isEmpty());
        decoded=runImageScript("function decode(){var p=new Uint8Array(512*512*4);p[3]=255;p[p.length-1]=255;"
            "return {width:512,height:512,pixels:p.buffer};}","large-output.js",{});
        QVERIFY2(decoded.error.isEmpty(),qPrintable(decoded.error)); QCOMPARE(decoded.image.size(),QSize(512,512));
        QCOMPARE(decoded.image.pixelColor(511,511),QColor(Qt::black));
        for (const auto &source : {QByteArray("function decode(){throw 123;}"), QByteArray("function decode(){throw new Error('failure');}"),
             QByteArray("function decode(){return {width:1,height:1,pixels:new Uint8Array(3).buffer};}"),
             QByteArray("function decode(){return {width:1.5,height:1,pixels:new ArrayBuffer(4)};}"),
             QByteArray("function decode(){return {get width(){throw 'getter';}};}"),
             QByteArray("function decode(){return {width:999999999,height:1,pixels:new ArrayBuffer(0)};}"),
             QByteArray("syntax error"), QByteArray("function other(){}")})
            QVERIFY2(!runImageScript(source,"bad.js",{}).error.isEmpty(),source.constData());
        const auto denied=runImageScript("function decode(){if(typeof Qt!=='undefined'||typeof require!=='undefined'||typeof fetch!=='undefined')throw 'capability';"
            "return {width:1,height:1,pixels:new Uint8Array([0,0,0,255]).buffer};}","capabilities.js",{});
        QVERIFY2(denied.error.isEmpty(),qPrintable(denied.error));
        QElapsedTimer elapsed; elapsed.start();
        QVERIFY(!runImageScript("function decode(){while(true){}}","loop.js",{}, {},400).error.isEmpty());
        QVERIFY(elapsed.elapsed()<5000);
        QVERIFY(!runImageScript("while(true){} function decode(){}","init-loop.js",{}, {},400).error.isEmpty());
        QVERIFY(!runImageScript(solidScript(),"cancel.js",{}, {},10000,[]{return true;}).error.isEmpty());
        QVERIFY(runImageScript(solidScript(),"after-timeout.js",{}).error.isEmpty());
    }
    void foldersAndArchives() {
        QTemporaryDir d;
        const auto image=mag();
        QVERIFY(TestData::write(d.filePath("日本語.mag"),image));
        auto book=Book::open(d.path()); QVERIFY2(book.book,qPrintable(book.error));
        QCOMPARE(book.book->pages.size(),1); QCOMPARE(book.book->image(0).image.size(),QSize(8,3));
        const auto exported=d.filePath("export.png");
        QVERIFY(exportPages(book.book,{0},exported,ExportKind::Image).isEmpty());
        QCOMPARE(QImage(exported).pixelColor(0,0),QColor(17,17,17));
        MainWindow window(d.filePath("settings.ini")); window.show();
        window.openBook(d.filePath("日本語.mag"));
        QTRY_VERIFY_WITH_TIMEOUT(!window.isLoading() && window.renderReady(),10000);
        QCOMPARE(window.currentPageName(),QString("日本語.mag")); window.close();
        const auto path=d.filePath("書庫.zip");
        QVERIFY(TestData::write(path,TestData::zip({{QString("日本語.mag").toUtf8(),image,0x800},
            {"2.p2",pic(picBlock("P2SS",arithmeticData(24),2,2),2,2)}})));
        book=Book::open(path); QVERIFY2(book.book,qPrintable(book.error)); QCOMPARE(book.book->pages.size(),2);
        for (int i=0;i<2;++i) { const auto decoded=book.book->image(i,QSize(4,4)); QVERIFY2(decoded.error.isEmpty(),qPrintable(decoded.error)); }
    }
    void customPackages() {
        QTemporaryDir d;
        const QJsonObject manifest{{"apiVersion",1},{"id","test.toy"},{"extensions",QJsonArray{"toy"}},
            {"magic","544f59"},{"script","decode.js"}};
        QVERIFY(TestData::write(d.filePath("manifest.json"),QJsonDocument(manifest).toJson()));
        QVERIFY(TestData::write(d.filePath("decode.js"),solidScript()));
        QVERIFY2(registerScriptReaders(d.path()).isEmpty(),"custom reader registration");
        QCOMPARE(decodeImage("TOY data","test.toy").image.pixelColor(0,0),QColor(10,20,30));
        auto invalid=manifest; invalid["script"]="../outside.js";
        QVERIFY(TestData::write(d.filePath("manifest.json"),QJsonDocument(invalid).toJson()));
        QVERIFY(!registerScriptReaders(d.path()).isEmpty());
        QCOMPARE(decodeImage("TOY data","test.toy").image.pixelColor(0,0),QColor(10,20,30)); // Atomic failure.
    }
};
ISOLATED_TEST_MAIN(ScriptTests)
#include "script_tests.moc"
