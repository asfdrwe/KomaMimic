// SPDX-License-Identifier: GPL-3.0-only
#include "test_support.h"
#include "thumbnailsource.h"
#include "archivenames.h"
#include <QCryptographicHash>
#include <QtEndian>
#include <zlib.h>
#include <algorithm>
#include <clocale>
#include <QScopeGuard>
#include "scriptreader.h"
#include <QtConcurrent>

namespace {
void le16(QByteArray &b,quint16 n){char v[2];qToLittleEndian(n,v);b.append(v,2);}
void le32(QByteArray &b,quint32 n){char v[4];qToLittleEndian(n,v);b.append(v,4);}
void le64(QByteArray &b,quint64 n){char v[8];qToLittleEndian(n,v);b.append(v,8);}
struct Entry {
    QByteArray name,bytes,localExtra,centralExtra;
    Entry(QByteArray label,QByteArray data):name(std::move(label)),bytes(std::move(data)){}
    quint16 flags=0x800,method=0;
    bool descriptor=false,wide=false;
    QByteArray password;
};
QByteArray unicodeExtra(const QByteArray &raw,const QString &name) {
    QByteArray data;data.append(char(1));le32(data,crc32(0,reinterpret_cast<const Bytef*>(raw.constData()),uInt(raw.size())));data+=name.toUtf8();
    QByteArray extra;le16(extra,0x7075);le16(extra,quint16(data.size()));extra+=data;return extra;
}
QByteArray rawDeflate(const QByteArray &bytes) {
    z_stream z{};if(deflateInit2(&z,1,Z_DEFLATED,-MAX_WBITS,8,Z_DEFAULT_STRATEGY)!=Z_OK)return {};
    QByteArray result(qsizetype(compressBound(uLong(bytes.size()))),char(0));
    z.next_in=reinterpret_cast<Bytef*>(const_cast<char*>(bytes.constData()));z.avail_in=uInt(bytes.size());
    z.next_out=reinterpret_cast<Bytef*>(result.data());z.avail_out=uInt(result.size());
    const auto status=deflate(&z,Z_FINISH);result.resize(qsizetype(z.total_out));deflateEnd(&z);return status==Z_STREAM_END?result:QByteArray{};
}
QByteArray encrypt(QByteArray bytes,const QByteArray &password,quint32 crc) {
    quint32 a=0x12345678,b=0x23456789,c=0x34567890;
    const auto step=[&](uchar plain){a=quint32(get_crc_table()[(a^plain)&255])^(a>>8);b=(b+(a&255))*134775813u+1;c=quint32(get_crc_table()[(c^(b>>24))&255])^(c>>8);};
    for(uchar ch:password)step(ch);
    QByteArray result(11,char(0));result.append(char(crc>>24));result+=bytes;
    for(char &ch:result){const uchar plain=uchar(ch);const auto temp=c|2u;ch=char(plain^((temp*(temp^1u))>>8));step(plain);}return result;
}
QByteArray zip(const QVector<Entry> &entries,bool zip64=false) {
    QByteArray bytes,central;
    for(const auto &e:entries) {
        const auto offset=bytes.size();const quint32 crc=crc32(0,reinterpret_cast<const Bytef*>(e.bytes.constData()),uInt(e.bytes.size()));
        auto payload=e.method==8?rawDeflate(e.bytes):e.bytes;
        if(!e.password.isEmpty())payload=encrypt(payload,e.password,crc);
        const quint16 flags=e.flags|(e.descriptor?8:0)|(!e.password.isEmpty()?1:0);
        auto local=e.localExtra,extra=e.centralExtra;
        if(e.wide){QByteArray z;le16(z,1);le16(z,16);le64(z,e.bytes.size());le64(z,payload.size());local.prepend(z);z.clear();le16(z,1);le16(z,24);le64(z,e.bytes.size());le64(z,payload.size());le64(z,offset);extra.prepend(z);}
        le32(bytes,0x04034b50);le16(bytes,e.wide?45:20);le16(bytes,flags);le16(bytes,e.method);le16(bytes,0);le16(bytes,0x21);
        le32(bytes,e.descriptor?0:crc);le32(bytes,e.wide?0xffffffff:e.descriptor?0:quint32(payload.size()));le32(bytes,e.wide?0xffffffff:e.descriptor?0:quint32(e.bytes.size()));
        le16(bytes,quint16(e.name.size()));le16(bytes,quint16(local.size()));bytes+=e.name;bytes+=local;bytes+=payload;
        if(e.descriptor){le32(bytes,0x08074b50);le32(bytes,crc);if(e.wide){le64(bytes,payload.size());le64(bytes,e.bytes.size());}else{le32(bytes,quint32(payload.size()));le32(bytes,quint32(e.bytes.size()));}}
        le32(central,0x02014b50);le16(central,20);le16(central,e.wide?45:20);le16(central,flags);le16(central,e.method);le16(central,0);le16(central,0x21);
        le32(central,crc);le32(central,e.wide?0xffffffff:quint32(payload.size()));le32(central,e.wide?0xffffffff:quint32(e.bytes.size()));
        le16(central,quint16(e.name.size()));le16(central,quint16(extra.size()));le16(central,0);le16(central,0);le16(central,0);le32(central,0);le32(central,e.wide?0xffffffff:quint32(offset));central+=e.name;central+=extra;
    }
    const auto start=bytes.size();bytes+=central;
    if(zip64){const auto end=bytes.size();le32(bytes,0x06064b50);le64(bytes,44);le16(bytes,45);le16(bytes,45);le32(bytes,0);le32(bytes,0);le64(bytes,entries.size());le64(bytes,entries.size());le64(bytes,central.size());le64(bytes,start);le32(bytes,0x07064b50);le32(bytes,0);le64(bytes,end);le32(bytes,1);}
    le32(bytes,0x06054b50);le16(bytes,0);le16(bytes,0);le16(bytes,zip64?65535:quint16(entries.size()));le16(bytes,zip64?65535:quint16(entries.size()));le32(bytes,zip64?0xffffffff:quint32(central.size()));le32(bytes,zip64?0xffffffff:quint32(start));le16(bytes,0);return bytes;
}
QByteArray reverseDirectory(QByteArray bytes) {
    const auto end=bytes.size()-22;const auto offset=qFromLittleEndian<quint32>(bytes.constData()+end+16);
    const auto size=qFromLittleEndian<quint32>(bytes.constData()+end+12);QVector<QByteArray> records;
    for(qsizetype at=offset;at<offset+size;) {
        const auto length=46+qFromLittleEndian<quint16>(bytes.constData()+at+28)+qFromLittleEndian<quint16>(bytes.constData()+at+30)+qFromLittleEndian<quint16>(bytes.constData()+at+32);
        records.append(bytes.mid(at,length));at+=length;
    }
    QByteArray reversed;for(auto i=records.crbegin();i!=records.crend();++i)reversed+=*i;
    bytes.replace(offset,size,reversed);return bytes;
}
}
class ZipCoverTests : public QObject {
    Q_OBJECT
private slots:
    void reusedDecoderKeepsStrictValidationAndReset() {
        struct Sample {QString encoding,key;QByteArray bytes;};
        const QList<Sample> samples{
            {"CP932","jp",QByteArray::fromHex("955c")+".png"},{"CP932","jp",QByteArray::fromHex("81")},
            {"windows-932","jp","plain.png"},{"UTF-8","utf8",QString::fromUtf8("日本語.png").toUtf8()},
            {"UTF-8","utf8",QByteArray::fromHex("e381")},{"UTF-8","utf8","next.png"},
            {"UTF-8","utf8",QByteArray("a\0b",3)},{"UTF-8","utf8",{}},
            {"UTF-16LE","utf16",QByteArray::fromHex("4100")},{"UTF-16LE","utf16",QByteArray::fromHex("41")},
            {"UTF-16LE","utf16",QByteArray::fromHex("70006c00610069006e00")},
            {"ISO-2022-JP","stateful",QByteArray::fromHex("1b2442467c4b5c1b2842")},
            {"ISO-2022-JP","stateful",QByteArray::fromHex("1b244246")},{"ISO-2022-JP","stateful","plain.png"},
            {"not-a-codec","invalid","x"},{"not-a-codec","invalid","y"},
            {"GBK","gbk",QByteArray::fromHex("d6d0cec4")},{"CP949","kr",QByteArray::fromHex("c7d1b1db")},
            {"Big5","big5",QByteArray::fromHex("a4a4a4e5")},{"CP437","cp437",QByteArray::fromHex("82")}
        };
        NameDecoder decoder;QSet<QString> opened;
        for(int repeat=0;repeat<3;++repeat)for(const auto &sample:samples) {
            QString expected="unchanged",actual=expected;
            const bool valid=decodeStrict(sample.bytes,sample.encoding,expected);
            const auto before=NameDecodeActivity::converterOpens.load();
            QCOMPARE(decodeStrict(sample.bytes,sample.encoding,actual,&decoder),valid);QCOMPARE(actual,expected);
            QCOMPARE(NameDecodeActivity::converterOpens.load()-before,opened.contains(sample.key)?quint64(0):quint64(1));opened.insert(sample.key);
        }
    }
    void independentDecodersCanRunOnSeparateWorkers() {
        const auto work=[](const QString &name){NameDecoder decoder;for(int i=0;i<500;++i){QString value;if(decodeStrict(QByteArray::fromHex("e381"),"UTF-8",value,&decoder))return false;if(!decodeStrict(name.toUtf8(),"UTF-8",value,&decoder)||value!=name)return false;}return true;};
        auto first=QtConcurrent::run(work,QString::fromUtf8("日本語の名前.png"));
        auto second=QtConcurrent::run(work,QString::fromUtf8("別の名前と長さ.bmp"));QVERIFY(first.result());QVERIFY(second.result());
    }
    void streamingSelectionRetainsOneNameAndChecksAllOffsets_data() {
        QTest::addColumn<bool>("legacy");QTest::newRow("utf8")<<false;QTest::newRow("legacy")<<true;
    }
    void streamingSelectionRetainsOneNameAndChecksAllOffsets() {
        QFETCH(bool,legacy);QTemporaryDir d;const auto path=d.filePath("sample.zip");QVector<Entry> entries;
        for(int i=999;i>=0;--i){Entry entry{QString("%1.png").arg(i).toUtf8(),TestData::png(TestData::solid(Qt::red))};if(legacy)entry.flags=0;entries.append(entry);}
        QVERIFY(TestData::write(path,zip(entries)));const auto ext=imageExtensions();QSet<QString> suffixes(ext.begin(),ext.end());
        NameDecoder decoder;const auto before=NameDecodeActivity::converterOpens.load();
        const auto selected=selectZipCover(path,{},suffixes,decoder);
        QVERIFY2(selected.archive.error.isEmpty(),qPrintable(selected.archive.error));QCOMPARE(selected.archive.entries.size(),1);QCOMPARE(selected.label,QString("0.png"));
        QCOMPARE(NameDecodeActivity::converterOpens.load()-before,quint64(1));
        QCOMPARE(selected.archive.entries.first().endOffset,selected.archive.directoryOffset);
        const auto pixels=readZipEntry(path,selected.archive,0);QVERIFY2(pixels.error.isEmpty(),qPrintable(pixels.error));
        auto invalid=zip(entries);const auto end=invalid.size()-22;const auto central=qFromLittleEndian<quint32>(invalid.constData()+end+16);
        const auto second=central+46+entries.first().name.size();qToLittleEndian<quint32>(0,invalid.data()+second+42);
        QVERIFY(TestData::write(path,invalid));QVERIFY(!selectZipCover(path,{},suffixes,decoder).archive.error.isEmpty());
    }
    void reorderedDirectoryKeepsPhysicalTieAndBoundary() {
        QTemporaryDir d;const auto path=d.filePath("sample.zip");
        const auto red=TestData::png(TestData::solid(Qt::red)),blue=TestData::png(TestData::solid(Qt::blue));
        QVERIFY(TestData::write(path,reverseDirectory(zip({{"same.png",red},{"same.png",blue}}))));
        const auto ext=imageExtensions();const QSet<QString> suffixes(ext.begin(),ext.end());NameDecoder decoder;
        const auto selected=selectZipCover(path,{},suffixes,decoder);QVERIFY(selected.archive.error.isEmpty());QCOMPARE(selected.archive.entries.size(),1);
        QCOMPARE(selected.archive.entries.first().offset,qint64(0));QCOMPARE(selected.archive.entries.first().endOffset,qint64(30+8+red.size()));
        const auto preview=Book::coverPreview(path);QVERIFY2(!preview.image.isNull(),qPrintable(preview.error));QCOMPARE(preview.image.pixelColor(0,0),QColor(Qt::red));
        auto broken=reverseDirectory(zip({{"1.png",red},{"ignored.bin",blue}}));const auto end=broken.size()-22;
        const auto central=qFromLittleEndian<quint32>(broken.constData()+end+16);const auto second=central+46+11;
        qToLittleEndian<quint32>(quint32(red.size()+1),broken.data()+second+20);QVERIFY(TestData::write(path,broken));
        const auto bad=selectZipCover(path,{},suffixes,decoder);QVERIFY(bad.archive.error.isEmpty());const auto reads=ArchiveReadActivity::previewBytes.load();
        QVERIFY(!readZipEntry(path,bad.archive,0).error.isEmpty());QCOMPARE(ArchiveReadActivity::previewBytes.load(),reads);
    }
    void extensionSnapshotIsRefreshedForNextCover() {
        const auto cleanup=qScopeGuard([]{configureScriptReaders({});});QVERIFY(configureScriptReaders({}).isEmpty());
        QTemporaryDir d;const auto path=d.filePath("sample.zip"),scripts=d.filePath("reader");QVERIFY(QDir().mkpath(scripts));
        QVERIFY(TestData::write(path,zip({{"1.cvrtst",TestData::png(TestData::solid(Qt::red))},{"2.png",TestData::png(TestData::solid(Qt::blue))}})));
        QCOMPARE(Book::coverPreview(path).image.pixelColor(0,0),QColor(Qt::blue));
        QVERIFY(TestData::write(scripts+"/manifest.json",R"({"apiVersion":1,"id":"cover-extension-test","extensions":["cvrtst"],"magic":"53414d504c45","script":"decode.js"})"));
        QVERIFY(TestData::write(scripts+"/decode.js","function decode(){return {width:1,height:1,pixels:new Uint8Array([255,0,0,255]).buffer};}"));
        QVERIFY(configureScriptReaders({scripts}).isEmpty());QCOMPARE(Book::coverPreview(path).image.pixelColor(0,0),QColor(Qt::red));
        QVERIFY(configureScriptReaders({}).isEmpty());QCOMPARE(Book::coverPreview(path).image.pixelColor(0,0),QColor(Qt::blue));
    }
    void longEntryNamesAreNotTruncated_data() {
        QTest::addColumn<bool>("unicode");QTest::newRow("ascii")<<false;QTest::newRow("utf8")<<true;
    }
    void longEntryNamesAreNotTruncated() {
        QFETCH(bool,unicode);QTemporaryDir d;const auto path=d.filePath("sample.zip");
        const QString label="1"+(unicode?QString::fromUtf8("あ").repeated(600):QString(1200,'x'))+".png";
        QVERIFY(label.toUtf8().size()>1024);
        QVERIFY(TestData::write(path,zip({{label.toUtf8(),TestData::png(TestData::solid(Qt::red))},{"2.png",TestData::png(TestData::solid(Qt::blue))}})));
        const auto ext=imageExtensions();NameDecoder decoder;const auto selected=selectZipCover(path,{},QSet<QString>(ext.begin(),ext.end()),decoder);
        QVERIFY2(selected.archive.error.isEmpty(),qPrintable(selected.archive.error));QCOMPARE(selected.label,label);QCOMPARE(selected.archive.entries.size(),1);
        QCOMPARE(selected.archive.entries.first().rawName,label.toUtf8());
        const auto result=Book::coverPreview(path);QVERIFY2(!result.image.isNull(),qPrintable(result.error));QCOMPARE(result.image.pixelColor(0,0),QColor(Qt::red));
    }
    void selectedPayloadOnly_data() {
        QTest::addColumn<bool>("last");QTest::addColumn<bool>("deflate");QTest::addColumn<bool>("descriptor");QTest::addColumn<bool>("wide");
        QTest::newRow("stored-first")<<false<<false<<false<<false;
        QTest::newRow("stored-last")<<true<<false<<false<<false;
        QTest::newRow("deflate-last")<<true<<true<<false<<false;
        QTest::newRow("deflate-descriptor")<<true<<true<<true<<false;
        QTest::newRow("stored-descriptor")<<true<<false<<true<<false;
        QTest::newRow("zip64-descriptor")<<true<<true<<true<<true;
        QTest::newRow("zip64-stored-descriptor")<<true<<false<<true<<true;
        QTest::newRow("zip64-no-descriptor")<<true<<true<<false<<true;
    }
    void selectedPayloadOnly() {
        QFETCH(bool,last);QFETCH(bool,deflate);QFETCH(bool,descriptor);QFETCH(bool,wide);
        QTemporaryDir d;const auto path=d.filePath("sample.zip");
        const auto red=TestData::png(TestData::solid(Qt::red)),blue=TestData::png(TestData::solid(Qt::blue));
        Entry chosen{"2.png",red};chosen.method=deflate?8:0;chosen.descriptor=descriptor;chosen.wide=wide;
        QVector<Entry> entries{{"10.png",blue},{"unrelated.bin",QByteArray(2*1024*1024,'x')}};
        if(last)entries.append(chosen);else entries.prepend(chosen);
        QVERIFY(TestData::write(path,zip(entries,wide)));
        const auto opens=BookActivity::opens.load(),reads=ArchiveReadActivity::previewEntries.load();
        const auto metadata=ArchiveReadActivity::metadataBytes.load(),payload=ArchiveReadActivity::previewBytes.load();
        auto cache=std::make_shared<ThumbnailDiskCache>(d.filePath("cache"));const auto result=cachedCover(path,{},cache);
        QVERIFY2(!result.image.isNull(),qPrintable(result.error));QCOMPARE(result.image.pixelColor(0,0),QColor(Qt::red));
        QCOMPARE(BookActivity::opens.load(),opens);QCOMPARE(ArchiveReadActivity::previewEntries.load(),reads+1);
        QVERIFY(ArchiveReadActivity::metadataBytes.load()-metadata+ArchiveReadActivity::previewBytes.load()-payload<100*1024);
        {const auto reference=Book::open(path);QVERIFY2(reference.book,qPrintable(reference.error));QCOMPARE(reference.book->preview().image,result.image);}
        const auto after=ArchiveReadActivity::metadataBytes.load(),sources=ArchiveReadActivity::previewBytes.load();
        QVERIFY(QFile::remove(path));auto restarted=std::make_shared<ThumbnailDiskCache>(cache->directory());QVERIFY(!cachedCover(path,{},restarted).image.isNull());
        QCOMPARE(ArchiveReadActivity::metadataBytes.load(),after);QCOMPARE(ArchiveReadActivity::previewBytes.load(),sources);
    }
    void namesMatchReader_data() {
        QTest::addColumn<int>("kind");
        for(int i=0;i<7;++i)QTest::newRow(qPrintable(QString("names-%1").arg(i)))<<i;
    }
    void namesMatchReader() {
        QFETCH(int,kind);QTemporaryDir d;const auto path=d.filePath("sample.zip");
        Entry a{"2.png",TestData::png(TestData::solid(Qt::red))},b{"10.png",TestData::png(TestData::solid(Qt::blue))};NamePolicy policy;
        if(kind==0){a.name=QByteArray::fromHex("955c")+"2.png";a.flags=0;b.name=QString("日本10.png").toUtf8();}
        if(kind==1){a.name="30.png";a.flags=0;a.localExtra=unicodeExtra(a.name,"1.png");}
        if(kind==2){a.name="30.png";a.flags=0;a.localExtra=unicodeExtra(a.name,"1.png");a.centralExtra=unicodeExtra(a.name,"40.png");}
        if(kind==3){a.name="30.png";a.flags=0;a.localExtra=unicodeExtra(a.name,"1.png");policy.force=true;}
        if(kind==4){a.name=QByteArray::fromHex("ff")+".png";}
        if(kind==5){b.name=a.name;}
        if(kind==6){a.flags=0;b.flags=0;policy.legacyEncoding="CP437";}
        QVERIFY(TestData::write(path,zip({a,b})));
        const auto reference=Book::open(path,policy);QVERIFY2(reference.book,qPrintable(reference.error));
        const auto expected=reference.book->preview();QVERIFY2(!expected.image.isNull(),qPrintable(expected.error));
        const auto result=Book::coverPreview(path,policy);QVERIFY2(!result.image.isNull(),qPrintable(result.error));QCOMPARE(result.image,expected.image);
    }
    void cLocaleAndUnicodePath() {
        QTemporaryDir d;const auto path=d.filePath(QString::fromUtf8("日本語.zip"));
        Entry entry{QByteArray::fromHex("955c")+".png",TestData::png(TestData::solid(Qt::red))};entry.flags=0;
        QVERIFY(TestData::write(path,zip({entry})));
        const QByteArray saved(std::setlocale(LC_CTYPE,nullptr));const auto restore=qScopeGuard([saved]{std::setlocale(LC_CTYPE,saved.constData());});
        QVERIFY(std::setlocale(LC_CTYPE,"C")!=nullptr);
        const auto result=Book::coverPreview(path);QVERIFY2(!result.image.isNull(),qPrintable(result.error));QCOMPARE(result.image.pixelColor(0,0),QColor(Qt::red));QCOMPARE(QByteArray(std::setlocale(LC_CTYPE,nullptr)),QByteArray("C"));
    }
    void unrelatedTiffIsNotRead() {
        QTemporaryDir d;const auto path=d.filePath("sample.zip");
        QVector<Entry> entries{{"1.png",TestData::png(TestData::solid(Qt::red))}};
        for(int i=0;i<64;++i)entries.append({QString("p/%1.tiff").arg(i).toUtf8(),TestData::tiff()});
        QVERIFY(TestData::write(path,zip(entries)));
        const auto opens=BookActivity::opens.load(),reads=BookActivity::encodedReads.load(),entriesRead=ArchiveReadActivity::previewEntries.load();
        const auto result=Book::coverPreview(path);QVERIFY2(!result.image.isNull(),qPrintable(result.error));
        QCOMPARE(BookActivity::opens.load(),opens);QCOMPARE(BookActivity::encodedReads.load(),reads+1);QCOMPARE(ArchiveReadActivity::previewEntries.load(),entriesRead+1);
    }
    void selectedTiffUsesFirstPage() {
        if(!imageExtensions().contains("tiff"))QSKIP("Qt TIFF plugin is not installed");
        QTemporaryDir d;const auto path=d.filePath("sample.zip");QVERIFY(TestData::write(path,zip({{"1.tiff",TestData::tiff()}})));
        const auto result=Book::coverPreview(path);QVERIFY2(!result.image.isNull(),qPrintable(result.error));QCOMPARE(result.image.pixelColor(0,0),QColor(Qt::black));
    }
    void archiveCommentAndSfxKeepSelection_data() {
        QTest::addColumn<bool>("sfx");QTest::newRow("comment")<<false;QTest::newRow("sfx")<<true;
    }
    void archiveCommentAndSfxKeepSelection() {
        QFETCH(bool,sfx);QTemporaryDir d;const auto path=d.filePath("sample.zip");
        auto bytes=zip({{"2.png",TestData::png(TestData::solid(Qt::red))}});
        if(sfx) {
            const auto central=bytes.indexOf("PK\1\2"),end=bytes.size()-22;
            qToLittleEndian<quint32>(256,bytes.data()+central+42);
            qToLittleEndian<quint32>(quint32(central+256),bytes.data()+end+16);
            bytes.prepend(QByteArray("MZ")+QByteArray(254,char(0)));
        } else {const QByteArray comment="Sample comment PK\5\6";qToLittleEndian<quint16>(quint16(comment.size()),bytes.data()+bytes.size()-2);bytes+=comment;}
        QVERIFY(TestData::write(path,bytes));const auto opens=BookActivity::opens.load();
        const auto result=Book::coverPreview(path);QVERIFY2(!result.image.isNull(),qPrintable(result.error));QCOMPARE(result.image.pixelColor(0,0),QColor(Qt::red));QCOMPARE(BookActivity::opens.load(),opens);
    }
    void nonZipCoverSkipsOtherTiffPayloads() {
        QTemporaryDir d;const auto path=d.filePath("sample.tar");QByteArray tar;
        const auto add=[&](const QByteArray &name,const QByteArray &data) {
            QByteArray header(512,char(0));std::copy(name.begin(),name.end(),header.begin());
            const auto number=[&](int offset,int width,quint64 value){auto digits=QByteArray::number(value,8).rightJustified(width-1,'0');std::copy(digits.begin(),digits.end(),header.begin()+offset);};
            number(100,8,0644);number(108,8,0);number(116,8,0);number(124,12,data.size());number(136,12,0);
            std::fill(header.begin()+148,header.begin()+156,' ');header[156]='0';
            quint64 sum=0;for(uchar c:header)sum+=c;number(148,7,sum);header[155]=' ';
            tar+=header;tar+=data;tar+=QByteArray((512-data.size()%512)%512,char(0));
        };
        add("1.png",TestData::png(TestData::solid(Qt::red)));add("2.tiff",TestData::tiff());tar+=QByteArray(1024,char(0));
        QVERIFY(TestData::write(path,tar));const auto reads=BookActivity::encodedReads.load();
        const auto result=Book::coverPreview(path);QVERIFY2(!result.image.isNull(),qPrintable(result.error));QCOMPARE(result.image.pixelColor(0,0),QColor(Qt::red));QCOMPARE(BookActivity::encodedReads.load(),reads+1);
    }
    void manyEntriesHaveBoundedReads() {
        QTemporaryDir d;const auto path=d.filePath("sample.zip");
        QVector<Entry> entries{{"1.png",TestData::png(TestData::solid(Qt::red))}};
        for(int i=0;i<1000;++i)entries.append({QString("p/%1.bin").arg(i).toUtf8(),QByteArray(4096,'x')});
        QVERIFY(TestData::write(path,zip(entries)));
        const auto metadata=ArchiveReadActivity::metadataBytes.load(),payload=ArchiveReadActivity::previewBytes.load();
        const auto result=Book::coverPreview(path);QVERIFY2(!result.image.isNull(),qPrintable(result.error));
        QVERIFY(ArchiveReadActivity::metadataBytes.load()-metadata<200*1024);QVERIFY(ArchiveReadActivity::previewBytes.load()-payload<1024);
    }
    void badSelectedEntryNeverFallsThrough_data() {
        QTest::addColumn<bool>("crc");QTest::newRow("bad-crc")<<true;QTest::newRow("bad-local")<<false;
    }
    void badSelectedEntryNeverFallsThrough() {
        QFETCH(bool,crc);QTemporaryDir d;const auto path=d.filePath("sample.zip");
        auto bytes=zip({{"1.png",TestData::png(TestData::solid(Qt::red))},{"2.png",TestData::png(TestData::solid(Qt::blue))}});
        if(crc){const auto central=bytes.indexOf("PK\1\2");QVERIFY(central>=0);bytes[central+16]^=1;}else bytes[0]='X';
        QVERIFY(TestData::write(path,bytes));const auto result=Book::coverPreview(path);QVERIFY(result.image.isNull());QVERIFY(!result.error.isEmpty());
    }
    void encryptedEntryKeepsPasswordAndCrcChecks() {
        QTemporaryDir d;const auto path=d.filePath("sample.zip");Entry entry{"1.png",TestData::png(TestData::solid(Qt::red))};entry.password="sample";
        QVERIFY(TestData::write(path,zip({entry})));
        const auto names=readZipNames(path,{},false,ZipNamesMode::Cover);QVERIFY(names.error.isEmpty());
        QVERIFY(!readZipEntry(path,names,0).error.isEmpty());QVERIFY(!readZipEntry(path,names,0,{},"wrong").error.isEmpty());
        const auto result=readZipEntry(path,names,0,{},"sample");QVERIFY2(result.error.isEmpty(),qPrintable(result.error));QCOMPARE(result.bytes,entry.bytes);
    }
    void truncatedSourceIsRejectedAfterSizeSnapshot_data() {
        QTest::addColumn<bool>("metadata");QTest::newRow("metadata")<<true;QTest::newRow("payload")<<false;
    }
    void truncatedSourceIsRejectedAfterSizeSnapshot() {
        QFETCH(bool,metadata);QTemporaryDir d;const auto path=d.filePath("sample.zip");
        Entry entry{"1.png",TestData::png(TestData::solid(Qt::red))};entry.flags=0;
        QVERIFY(TestData::write(path,zip({entry})));int calls=0;bool resized=false;
        const auto truncate=[&] {if(++calls==2){QFile file(path);resized=file.open(QIODevice::WriteOnly)&&file.resize(0);}return false;};
        if(metadata) {
            const auto names=readZipNames(path,truncate,true,ZipNamesMode::Cover);
            QVERIFY(resized);QVERIFY(!names.error.isEmpty());
        } else {
            const auto names=readZipNames(path,{},true,ZipNamesMode::Cover);QVERIFY(names.error.isEmpty());
            const auto result=readZipEntry(path,names,0,truncate);
            QVERIFY(resized);QVERIFY(!result.error.isEmpty());QVERIFY(result.bytes.isEmpty());
        }
    }
    void cancellationStopsMetadataAndPayload() {
        QTemporaryDir d;const auto path=d.filePath("sample.zip");QByteArray payload(2*1024*1024,char(0));quint32 random=139;
        for(char &c:payload){random^=random<<13;random^=random>>17;random^=random<<5;c=char(random);}
        QVERIFY(TestData::write(path,zip({{"1.png",payload}})));
        int calls=0;QVERIFY(!readZipNames(path,[&]{return ++calls>1;},false,ZipNamesMode::Cover).error.isEmpty());
        const auto names=readZipNames(path,{},false,ZipNamesMode::Cover);QVERIFY(names.error.isEmpty());const auto before=ArchiveReadActivity::previewBytes.load();
        const auto result=readZipEntry(path,names,0,[&]{return ArchiveReadActivity::previewBytes.load()>before;});
        QVERIFY(!result.error.isEmpty());QVERIFY(result.bytes.isEmpty());QVERIFY(ArchiveReadActivity::previewBytes.load()-before<quint64(payload.size()/2));
    }
};
ISOLATED_TEST_MAIN(ZipCoverTests)
#include "zip_cover_tests.moc"
