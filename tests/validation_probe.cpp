// SPDX-License-Identifier: GPL-3.0-only
// Exercise the generated manual inputs through the real public reader APIs.
#include "book.h"
#include "publication.h"
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cstdio>

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    if (app.arguments().size() != 2) return 2;
    const QString root = app.arguments().at(1) + '/';
    QJsonArray results;
    bool failed = false;
    const auto check = [&](const QString &name, bool ok, const QString &detail = {}) {
        results.append(QJsonObject{{"case", name}, {"pass", ok}, {"detail", detail}});
        failed |= !ok;
    };
    for (const auto &[name, count] : QVector<QPair<QString, int>>{
             {"images", 7}, {"books/01-first.zip", 6}, {"books/02-next.zip", 6},
             {"books/03-cp932.zip", 1}, {"日本語 空白/書庫 & 01.cbz", 6},
             {"books/05-pages.tar", 6}, {"books/06-pages.tar.gz", 6},
             {"animation/tiff/two-pages.tiff", 2}, {"documents/three-pages.pdf", 3}}) {
        const auto opened = Book::open(root + name);
        bool ok = opened.book && opened.book->pages.size() == count;
        QString error = opened.error;
        if (opened.book && opened.book->pages.size() != count)
            error += QString("expected %1 pages, got %2").arg(count).arg(opened.book->pages.size());
        if (ok) for (int i = 0; i < count; ++i) {
            const auto image = opened.book->image(i, {160, 240});
            ok &= image.error.isEmpty() && !image.image.isNull();
            if (!image.error.isEmpty()) error += image.error;
        }
        check(name, ok, error);
        if (opened.book && name.endsWith("03-cp932.zip"))
            check("cp932-name", opened.book->pages.first().name == QString("表紙ソ.png"), opened.book->pages.first().name);
    }
    OpenOptions pdfOptions; pdfOptions.pdfResources = true;
    const auto pdf = Book::open(root + "documents/three-pages.pdf", {}, pdfOptions);
    bool pdfOk = pdf.book && pdf.book->pages.size() == 3;
    if (pdfOk) for (int i = 0; i < 3; ++i) {
        const auto image = pdf.book->image(i);
        pdfOk &= image.error.isEmpty() && image.image.size() == QSize(128, 192);
    }
    check("pdf-internal-images", pdfOk, pdf.error);
    for (const auto &name : {"animation/gif/two-frames.gif", "animation/apng/two-frames.apng"}) {
        QFile input(root + name);
        if (!input.open(QIODevice::ReadOnly)) { check(name, false, input.errorString()); continue; }
        const auto decoded = decodeImage(input.readAll(), name);
        bool ok = decoded.error.isEmpty() && decoded.frames.size() == 2;
        if (ok) ok = decoded.frames[0].pixelColor(30, 30) == QColor(Qt::red)
                     && decoded.frames[1].pixelColor(30, 30) == QColor(Qt::blue)
                     && decoded.delays == QVector<int>{500, 500};
        check(name, ok, decoded.error);
    }
    for (const auto &name : {"aozora-utf8.txt", "aozora-cp932.txt", "sample.md"}) {
        OpenOptions options;
        if (QString(name).contains("cp932")) options.text.encoding = "CP932";
        const auto opened = Book::open(root + "documents/" + name, {}, options);
        bool ok = opened.book && opened.book->publication && !opened.book->pages.isEmpty();
        if (ok) {
            const auto image = opened.book->preview(0, {160, 240});
            ok = image.error.isEmpty() && !image.image.isNull();
            ok &= !opened.book->image(0).error.isEmpty(); // No raster reading/export path.
        }
        check(name, ok, opened.error);
    }
    for (const auto &name : {"reflow.epub", "fixed.epub"}) {
        const auto publication = readPublication(root + "documents/" + name, {});
        const auto readingSections = std::count_if(publication->sections.cbegin(), publication->sections.cend(),
                                                  [](const auto &section) { return !section.auxiliary; });
        bool ok = publication->error.isEmpty() && readingSections == 3 && publication->toc.size() == 3;
        if (ok) for (const auto &section : publication->sections)
            if (!section.auxiliary)
                ok &= section.fixed == QString(name).startsWith("fixed") && section.anchors.contains("p20");
        const auto cover = epubCover(root + "documents/" + name);
        QJsonArray sections;
        for (const auto &section : publication->sections)
            sections.append(QJsonObject{{"path", section.path}, {"fixed", section.fixed}, {"p20", section.anchors.contains("p20")}});
        check(name, ok && cover.error.isEmpty() && !cover.image.isNull(),
              QString::fromUtf8(QJsonDocument(QJsonObject{{"error", publication->error + cover.error},
                  {"sections", sections}, {"toc", publication->toc.size()}, {"cover", !cover.image.isNull()}}).toJson(QJsonDocument::Compact)));
    }
    const auto mixed = Book::open(root + "books/04-mixed-documents.zip");
    bool mixedOk = mixed.book && mixed.book->archiveDirectory && mixed.book->pages.size() == 3;
    if (mixedOk) for (int i = 0; i < mixed.book->pages.size(); ++i) {
        const auto entry = Book::open(mixed.book->entryPath(i));
        mixedOk &= bool(entry.book);
    }
    check("mixed-archive-entry-paths", mixedOk, mixed.error);
    const auto output = QJsonDocument(QJsonObject{{"all_passed", !failed}, {"results", results}}).toJson();
    fwrite(output.constData(), 1, size_t(output.size()), stdout);
    return failed ? 1 : 0;
}
