// SPDX-License-Identifier: GPL-3.0-only
#include "test_support.h"
#include "expression.h"

class ReaderTests : public QObject {
    Q_OBJECT
private slots:
    void pageNavigation_data() {
        QTest::addColumn<int>("count"); QTest::addColumn<int>("anchor");
        QTest::addColumn<bool>("spread"); QTest::addColumn<bool>("cover"); QTest::addColumn<bool>("blank");
        QTest::addColumn<QVector<int>>("visible"); QTest::addColumn<int>("next"); QTest::addColumn<int>("previous");
        QTest::newRow("empty") << 0 << 0 << true << true << false << QVector<int>{} << 0 << 0;
        QTest::newRow("single-cover") << 5 << 0 << true << true << false << QVector<int>{0} << 1 << 0;
        QTest::newRow("first-spread") << 5 << 1 << true << true << false << QVector<int>{1,2} << 3 << 0;
        QTest::newRow("second-spread") << 5 << 3 << true << true << false << QVector<int>{3,4} << 3 << 1;
        QTest::newRow("shifted-spread") << 6 << 2 << true << true << false << QVector<int>{2,3} << 4 << 1;
        QTest::newRow("trailing-single") << 4 << 3 << true << true << false << QVector<int>{3} << 3 << 1;
        QTest::newRow("no-cover") << 5 << 0 << true << false << false << QVector<int>{0,1} << 2 << 0;
        QTest::newRow("leading-blank") << 5 << 0 << true << false << true << QVector<int>{0} << 1 << 0;
        QTest::newRow("single-mode") << 5 << 2 << false << false << false << QVector<int>{2} << 3 << 1;
        QTest::newRow("one-image") << 1 << 0 << true << false << false << QVector<int>{0} << 0 << 0;
    }
    void pageNavigation() {
        QFETCH(int, count); QFETCH(int, anchor); QFETCH(bool, spread); QFETCH(bool, cover); QFETCH(bool, blank);
        QFETCH(QVector<int>, visible); QFETCH(int, next); QFETCH(int, previous);
        ReadingOptions o; o.spread = spread; o.coverAlone = cover; o.leadingBlank = blank;
        for (bool rtl : {false, true}) {
            o.rightToLeft = rtl;
            QCOMPARE(visiblePages(count, anchor, o), visible);
            QCOMPARE(nextAnchor(count, anchor, o), next);
            QCOMPARE(previousAnchor(count, anchor, o), previous);
        }
    }
    void expression_data() {
        QTest::addColumn<QString>("source"); QTest::addColumn<double>("expected");
        QTest::newRow("precedence") << "2+3*4" << 14.;
        QTest::newRow("parentheses") << "(2+3)*4" << 20.;
        QTest::newRow("spread-condition") << "@height>@width && @width>0" << 1.;
        QTest::newRow("split-position") << "@width/2" << 320.;
        QTest::newRow("conditional") << "@height>@width ? 2 : 1" << 2.;
        QTest::newRow("functions") << "max(2,min(5,3))+abs(-2)" << 5.;
        QTest::newRow("rounding") << "floor(2.9)+ceil(2.1)+round(2.6)" << 8.;
    }
    void expression() {
        QFETCH(QString, source); QFETCH(double, expected);
        auto v = evaluateExpression(source, {{"width",640},{"height",960}});
        QVERIFY2(v.error.isEmpty(), qPrintable(v.error)); QCOMPARE(v.value, expected);
    }
    void invalidExpressions() {
        for (const QString s : {"1/0", "unknown", "min(1)", "(2+3", "1 trailing", ""})
            QVERIFY2(!evaluateExpression(s).error.isEmpty(), qPrintable(s));
        QVERIFY(!evaluateExpression(QString(5000, '1')).error.isEmpty());
        QVERIFY(!evaluateExpression(QString(100, '(') + "1" + QString(100, ')')).error.isEmpty());
    }
    void pixelsAndProcessing() {
        QImage input(2, 3, QImage::Format_ARGB32); input.fill(Qt::red); input.setPixelColor(0,0,Qt::blue);
        ImageOptions o; o.flipH = true;
        QCOMPARE(processImage(input,o).pixelColor(1,0), QColor(Qt::blue));
        o = {}; o.rotation = 1; QCOMPARE(processImage(input,o).size(), QSize(3,2));
        o = {}; o.gray = true; auto gray = processImage(input,o).pixelColor(1,1);
        QCOMPARE(gray.red(), gray.green()); QCOMPARE(gray.green(), gray.blue());
        o = {}; o.invert = true; QCOMPARE(processImage(input,o).pixelColor(1,1), QColor(Qt::cyan));
        o = {}; o.crop = QMargins(1,0,0,1); QCOMPARE(processImage(input,o).size(), QSize(1,2));
        o = {}; o.border = QMargins(1,2,3,4); o.borderColor = Qt::green;
        auto bordered = processImage(input,o); QCOMPARE(bordered.size(), QSize(6,9));
        QCOMPARE(bordered.pixelColor(0,0), QColor(Qt::green));
        QCOMPARE(bordered.pixelColor(1,2), QColor(Qt::blue));
        o.enabled = false; QCOMPARE(processImage(input,o), input);
    }
    void resizeAlgorithms_data() {
        QTest::addColumn<int>("algorithm");
        for (int i=0;i<8;++i) QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void resizeAlgorithms() {
        QFETCH(int, algorithm);
        const auto image = resizeImage(TestData::solid(Qt::red), {8,12}, ResizeAlgorithm(algorithm));
        QCOMPARE(image.size(), QSize(8,12)); QCOMPARE(image.pixelColor(4,6), QColor(Qt::red));
    }
    void processingKeepsAnimation() {
        ImageResult media(TestData::solid(Qt::red));
        media.frames = {media.image, TestData::solid(Qt::blue)}; media.delays = {80,120}; media.loopCount = -1;
        ImageOptions o; o.rotation = 1; const auto result = processMedia(media,o);
        QCOMPARE(result.frames.size(), 2); QCOMPARE(result.frames[0].size(), QSize(48,32));
        QCOMPARE(result.delays, media.delays); QCOMPARE(result.loopCount,-1);
    }
};
ISOLATED_TEST_MAIN(ReaderTests)
#include "reader_tests.moc"
