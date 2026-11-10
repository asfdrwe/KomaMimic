// SPDX-License-Identifier: GPL-3.0-only
#include "test_support.h"
#include "thumbnailcache.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>

// Keep filesystem count and capacity work in its own bounded CTest process.
class CacheCapacityTests : public QObject {
    Q_OBJECT
    QElapsedTimer m_caseClock;
private slots:
    void init() {m_caseClock.start();qInfo()<<"cache-case-start"<<QTest::currentTestFunction();}
    void cleanup() {qInfo()<<"cache-case-ms"<<QTest::currentTestFunction()<<m_caseClock.elapsed();}
    void countDoesNotLimitDiskCache() {
        QTemporaryDir d;
        const auto pixels=TestData::png(TestData::solid(Qt::blue,{1,1}));
        // The old 20,000-file limit would delete otherwise valid thumbnails.
        for(int i=0;i<20001;++i) {
            const auto key=QByteArray::number(i);
            const auto name=QString::fromLatin1(QCryptographicHash::hash(key,QCryptographicHash::Sha256).toHex())+".png";
            QVERIFY(TestData::write(d.filePath(name),pixels));
        }
        ThumbnailDiskCache cache(d.path());cache.configure(true,16);
        QVERIFY(cache.stats().bytes<16LL*1024*1024);
        QVERIFY(cache.maintain().isEmpty());
        QCOMPARE(cache.stats().entries,20001);
        int calls=0;
        const auto warm=cache.image("0",[&]{++calls;return ImageResult{TestData::solid(Qt::red),{}};});
        QCOMPARE(warm.image.pixelColor(0,0),QColor(Qt::blue));QCOMPARE(calls,0);
        cache.image("new",[]{return ImageResult{TestData::solid(Qt::green,{1,1}),{}};});
        QCOMPARE(cache.stats().entries,20002);
        cache.image("0",[]{return ImageResult{TestData::solid(Qt::green,{1,1}),{}};},{},true);
        QCOMPARE(cache.stats().entries,20002);
    }
    void diskEvictionPreservesPendingReplacementPixels() {
        QTemporaryDir d;ThumbnailDiskCache cache(d.path());cache.configure(true,16);
        QImage noise(768,768,QImage::Format_RGB32);quint32 state=104729;
        for(int y=0;y<noise.height();++y){auto *row=reinterpret_cast<QRgb *>(noise.scanLine(y));for(int x=0;x<noise.width();++x){state^=state<<13;state^=state>>17;state^=state<<5;row[x]=state|0xff000000;}}
        QVERIFY(!cache.image("0",[&]{return ImageResult{noise,{}};}).image.isNull());
        const auto path=d.filePath(QString::fromLatin1(QCryptographicHash::hash("0",QCryptographicHash::Sha256).toHex())+".png");QVERIFY(QFileInfo::exists(path));
        const auto bytes=QFileInfo(path).size();QVERIFY(bytes>0);const int count=int((16LL*1024*1024)/bytes);QVERIFY(count>1 && count<32);
        for(int i=1;i<count;++i)QVERIFY(!cache.image(QByteArray::number(i),[&]{return ImageResult{noise,{}};}).image.isNull());
        QFile oldest(path);QVERIFY(oldest.open(QIODevice::ReadWrite));QVERIFY(oldest.setFileTime(QDateTime::currentDateTime().addSecs(-120),QFileDevice::FileModificationTime));oldest.close();
        QCOMPARE(cache.stats().entries,count);
        cache.setDeferredWrites(true);cache.image("new",[&]{return ImageResult{noise,{}};});
        cache.image("0",[]{return ImageResult{TestData::solid(Qt::green,{768,768}),{}};},{},true);
        QVERIFY(cache.flushPending());QVERIFY(!QFileInfo::exists(path));QVERIFY(cache.hasPendingWrites());
        QCOMPARE(cache.lookup("0").image.pixelColor(0,0),QColor(Qt::green));QVERIFY(cache.flushPending());
        QCOMPARE(ThumbnailDiskCache(d.path()).lookup("0").image.pixelColor(0,0),QColor(Qt::green));
    }
    void capacityUsesCreationOrder() {
        QTemporaryDir d;ThumbnailDiskCache cache(d.path());cache.configure(true,16);
        QImage noise(768,768,QImage::Format_RGB32);quint32 state=104729;
        for(int y=0;y<noise.height();++y) {
            auto *row=reinterpret_cast<QRgb *>(noise.scanLine(y));
            for(int x=0;x<noise.width();++x) {state^=state<<13;state^=state>>17;state^=state<<5;row[x]=state|0xff000000;}
        }
        const auto generate=[&]{return ImageResult{noise,{}};};
        for(int i=0;i<8;++i) cache.image(QByteArray::number(i),generate);
        cache.image("0",generate);
        for(int i=8;i<12;++i) cache.image(QByteArray::number(i),generate);
        const auto usage=cache.stats();QVERIFY(usage.bytes<=16*1024*1024);QVERIFY(usage.entries<12);QVERIFY(usage.entries>1);
        // Reading the oldest entry above must not make it newer.
        QVERIFY(cache.lookup("0").image.isNull());QVERIFY(cache.lookup("1").image.isNull());
        cache.configure(true,32);
        for(int i=12;i<18;++i) cache.image(QByteArray::number(i),generate);
        QVERIFY(cache.stats().bytes>16*1024*1024);
        cache.configure(true,16);QVERIFY(cache.maintain().isEmpty());QVERIFY(cache.stats().bytes<=16*1024*1024);
    }
};
ISOLATED_TEST_MAIN(CacheCapacityTests)
#include "cache_capacity_tests.moc"
