// SPDX-License-Identifier: GPL-3.0-only
#include "test_support.h"
#include "bookexport.h"
#include "documentsettings.h"
#include "archivepath.h"
#include "publication.h"
#include "documentview.h"
#include <QLabel>
#include <QKeyEvent>
#include "thumbnailgrid.h"
#include "thumbnailsource.h"
#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QDir>
#include <QSpinBox>
#include <QTimer>

namespace {
// Explicit concatenated reading contains images/PDF only.
Book::OpenResult openCombinedArchive(const QString &path, const NamePolicy &policy = {}, OpenOptions options = {}) {
    options.archiveAsPages = true; return Book::open(path, policy, options);
}
QByteArray longText() {
    QString text;
    for (int i = 0; i < 260; ++i) text += QString("Line %1: ｜漢字《かんじ》と本文の確認。\n").arg(i);
    return text.toUtf8();
}
}
class ArchiveTextTests : public QObject {
    Q_OBJECT
private slots:
    void textEntriesHaveIndependentPublicationsAndFirstPagePreviews() {
        QTemporaryDir d;const auto path=d.filePath("text.zip");
        const auto text=longText();
        QVERIFY(TestData::write(path,TestData::zip({{"10.md","# Chapter\n\nBody"},{"2.txt",text},{"1.txt",""}})));
        const auto listing=Book::open(path);QVERIFY(listing.book && listing.book->archiveDirectory);
        QCOMPARE(listing.book->pages.size(),3);QCOMPARE(listing.book->pages[0].name,QString("1.txt"));
        QCOMPARE(listing.book->pages[1].name,QString("2.txt"));
        const auto opened=Book::open(listing.book->entryPath(1));QVERIFY(opened.book && opened.book->publication);
        QCOMPARE(opened.book->pages.size(),1);QVERIFY(opened.book->hasArchivedText());
        QCOMPARE(opened.book->preview().image,renderTextPreview(text,"2.txt").image);
        QVERIFY(!opened.book->image(0).error.isEmpty());QVERIFY(!opened.book->encoded(0).error.isEmpty());
        auto cache=std::make_shared<ThumbnailDiskCache>(d.filePath("cache"));
        QVERIFY(!cachedCover(listing.book->entryPath(1),{},cache).image.isNull());QCOMPARE(cache->stats().entries,0);
        const auto combined=openCombinedArchive(path);QVERIFY(!combined.book); // No raster text reading route.
    }
    void cp932NameAndBodyUseDocumentReader() {
        QTemporaryDir d;const auto path=d.filePath("cp932.zip");
        const auto name=QByteArray::fromHex("955c")+".TXT";
        const auto text=QByteArray::fromHex("93fa967b8cea0a8abf8e9a");
        QVERIFY(TestData::write(path,TestData::zip({{name,text},{"0.png",TestData::png(TestData::solid(Qt::blue))}})));
        const auto listing=Book::open(path);QVERIFY(listing.book);QCOMPARE(listing.book->pages.last().name,QString("表.TXT"));
        const auto doc=Book::open(listing.book->entryPath(1));QVERIFY(doc.book && doc.book->publication);
        QCOMPARE(doc.book->publication->sections.first().plain,QString("日本語\n漢字"));
        QVERIFY(!doc.book->preview().image.isNull());
        const auto images=openCombinedArchive(path);QVERIFY(images.book);QCOMPARE(images.book->pages.size(),1);
        QCOMPARE(images.book->image(0).image,TestData::solid(Qt::blue));
    }
    void previewRetainsOnlyArchiveResourcesAndSourceLifetime() {
        QTemporaryDir d;const auto path=d.filePath("resources.zip");
        const auto green=TestData::png(TestData::solid(Qt::green,{100,160}));
        const QByteArray markdown=QString("# Chapter\n\n![image](../assets/緑%20画像.png)\n\nAfter image").toUtf8();
        QVERIFY(TestData::write(path,TestData::zip({{"chap/readme.md",markdown},{QString("assets/緑 画像.png").toUtf8(),green,0x800}})));
        auto listing=Book::open(path);QVERIFY(listing.book);
        const int index=listing.book->pages[0].name.endsWith(".md")?0:1;
        const auto opened=Book::open(listing.book->entryPath(index));QVERIFY(opened.book && opened.book->publication);
        listing.book.reset();QVERIFY(opened.book->sourceUnchanged(0));
        const auto expected=renderTextPreview(markdown,"chap/readme.md",{320,480},{},[green](const QUrl &)->QVariant{return QImage::fromData(green);});
        QCOMPARE(opened.book->preview().image,expected.image);
        QVERIFY(opened.book->publication->localRoot.isEmpty());
    }
    void previewsCannotReadHostFilesOrExternalUrls() {
        QTemporaryDir d;const auto secret=d.filePath("secret.png"), path=d.filePath("isolated.zip");
        QVERIFY(TestData::write(secret,TestData::png(TestData::solid(Qt::magenta,{500,500}))));
        const auto markdown=QString("# Isolated\n\n![a](%1)\n\n![b](../secret.png)\n\n![c](https://example.invalid/x.png)\n\n![d](secret.png)")
            .arg(QUrl::fromLocalFile(secret).toString()).toUtf8();
        QVERIFY(TestData::write(path,TestData::zip({{"readme.md",markdown}})));
        const auto listing=Book::open(path);QVERIFY(listing.book);
        const auto opened=Book::open(listing.book->entryPath(0));QVERIFY(opened.book);
        QCOMPARE(opened.book->preview().image,renderTextPreview(markdown,"readme.md").image);
        QVERIFY(!QFileInfo(d.filePath("readme.md")).exists());
        const auto disguised=QString("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\"><image href=\"%1\"/></svg>").arg(QUrl::fromLocalFile(secret).toString()).toUtf8();
        const QByteArray reference("![image](z-poison.png)");const auto poisoned=d.filePath("poisoned.zip");
        QVERIFY(TestData::write(poisoned,TestData::zip({{"readme.md",reference},{"z-poison.png",disguised}})));
        const auto entries=Book::open(poisoned);QVERIFY(entries.book);
        const auto safe=Book::open(entries.book->entryPath(0));QVERIFY(safe.book);
        QCOMPARE(safe.book->preview().image,renderTextPreview(reference,"readme.md").image);
    }
    void invalidOversizeCancelledAndChangedSources() {
        QTemporaryDir d;const auto path=d.filePath("invalid.zip");
        QVERIFY(TestData::write(path,TestData::zip({{"bad.txt",QByteArray("a\0b",3)},{"good.md","# Good"}})));
        auto listing=Book::open(path);QVERIFY(listing.book);
        QVERIFY(!Book::open(listing.book->entryPath(0)).book);
        const auto opened=Book::open(listing.book->entryPath(1));QVERIFY(opened.book);
        QVERIFY(!opened.book->preview().image.isNull());
        QVERIFY(!opened.book->preview(0,{320,480},[]{return true;}).error.isEmpty());
        QVERIFY(TestData::write(path,TestData::zip({{"changed.txt","Changed source"}})));
        QVERIFY(!opened.book->sourceUnchanged(0));QVERIFY(!opened.book->preview().error.isEmpty());
        QVERIFY(TestData::write(path,TestData::zip({{"large.txt",QByteArray(16*1024*1024+1,'a')}})));
        listing=Book::open(path);QVERIFY(listing.book);
        QVERIFY(Book::open(listing.book->entryPath(0)).error.contains("16 MiB"));
    }
    void nestedAndFolderReadingNeverRasterizesText() {
        QTemporaryDir d;const auto path=d.filePath("outer.zip");const auto image=TestData::png(TestData::solid(Qt::red));
        const auto child=TestData::zip({{"text.txt","Nested text"},{"image.png",image}});
        QVERIFY(TestData::write(path,TestData::zip({{"inner.zip",child},{"outer.txt","Outer text"}})));
        OpenOptions options;options.nestedArchives=true;
        const auto nested=Book::open(path,{},options);QVERIFY2(nested.book,qPrintable(nested.error));
        QCOMPARE(nested.book->pages.size(),1);QVERIFY(!nested.book->pages[0].document);
        QCOMPARE(nested.book->image(0).image,TestData::solid(Qt::red));
        QVERIFY(TestData::write(d.filePath("standalone.txt"),longText()));
        QVERIFY(TestData::write(d.filePath("standalone.epub"),"not an EPUB; not part of image/PDF reading"));
        options.folderContainers=true;
        const auto folder=Book::open(d.path(),{},options);QVERIFY2(folder.book,qPrintable(folder.error));
        QCOMPARE(folder.book->pages.size(),1);QVERIFY(!folder.book->hasArchivedText());
    }

    void archiveListsFilesBeforeReadingAnyDocument() {
        QTemporaryDir d; const auto path = d.filePath("files.zip");
        QVERIFY(TestData::write(path, TestData::zip({{"2.txt", longText()}, {"1.md", "# Heading"}, {"3.txt", QByteArray("bad\0text", 8)}})));
        const auto opened = Book::open(path); QVERIFY2(opened.book, qPrintable(opened.error));
        QVERIFY(opened.book->archiveDirectory); QCOMPARE(opened.book->pages.size(), 3);
        QCOMPARE(opened.book->pages.first().name, QString("1.md"));
        QCOMPARE(opened.book->encodedCacheInfo().entries, 0); // Listing does not decode/reflow all bodies.
        for (int i = 0; i < 3; ++i) {
            const auto address = parseArchiveEntryPath(opened.book->entryPath(i));
            QCOMPARE(address.archive, QFileInfo(path).absoluteFilePath()); QVERIFY(!address.id.isEmpty());
        }
        QVERIFY(!Book::open(opened.book->entryPath(2)).error.isEmpty());
    }
    void individualDocumentsUsePublicationsAndPreviewOnly() {
        QTemporaryDir d; const auto path = d.filePath("documents.zip");
        QVERIFY(TestData::write(path, TestData::zip({{"1.txt", longText()}, {"2.md", "# Heading\n\n**Body**"}})));
        const auto root = Book::open(path); QVERIFY(root.book);
        const auto text = Book::open(root.book->entryPath(0)); QVERIFY2(text.book, qPrintable(text.error));
        QVERIFY(text.book->publication); QVERIFY(!text.book->archiveDirectory); QVERIFY(text.book->archiveParent);
        QVERIFY(text.book->publication->sections.first().plain.contains("本文"));
        const auto markdown = Book::open(root.book->entryPath(1)); QVERIFY(markdown.book && markdown.book->publication);
        QVERIFY(markdown.book->publication->markdown); QVERIFY(!markdown.book->publication->toc.isEmpty());
        QVERIFY(markdown.book->publication->sections.first().html.contains("<h1"));
        QCOMPARE(text.book->pages.size(),1);
        QVERIFY(!text.book->image(0).error.isEmpty());
        QCOMPARE(text.book->preview().image,renderTextPreview(longText(),"1.txt").image);
        QVERIFY(text.book->sourceUnchanged(0));
        QVERIFY(!text.book->preview(0,{320,480},[]{return true;}).error.isEmpty());
    }
    void standardMarkdownOwnsOnlyBoundedArchiveImages() {
        QTemporaryDir d; const auto path = d.filePath("resources.zip");
        const auto image = TestData::png(TestData::solid(Qt::green, {100,160}));
        const QByteArray text = QString("# Chapter\n\n![image](../assets/緑%20画像.png)\n\n![outside](file:///tmp/not-an-archive-resource.png)").toUtf8();
        QVERIFY(TestData::write(path, TestData::zip({{"chap/text.md", text}, {QString("assets/緑 画像.png").toUtf8(), image, 0x800}})));
        auto root = Book::open(path); QVERIFY(root.book); const int entry = root.book->pages[0].name.endsWith(".md") ? 0 : 1;
        auto child = Book::open(root.book->entryPath(entry)); QVERIFY2(child.book, qPrintable(child.error));
        auto publication = child.book->publication; QVERIFY(publication); QCOMPARE(publication->resources.size(), 1);
        QVERIFY(publication->localRoot.isEmpty());
        QVERIFY(publication->sections.first().html.contains("__archive_images__/0.png"));
        QVERIFY(!publication->sections.first().html.contains("file:///tmp/"));
        QCOMPARE(QImage::fromData(publication->resources.constBegin().value()).pixelColor(0,0), QColor(Qt::green));
        child.book.reset(); root.book.reset(); QVERIFY(!publication->resources.constBegin().value().isEmpty());
    }
    void stableEntryIdentitySurvivesRepackingAndSeparatesDuplicates() {
        QTemporaryDir d; const auto path = d.filePath("stable.zip");
        QVERIFY(TestData::write(path, TestData::zip({{"a.txt", "A"}, {"b.txt", "B"}, {"b.txt", "C"}})));
        auto root = Book::open(path); QVERIFY(root.book);
        const auto a = root.book->entryPath(0), b = root.book->entryPath(1), duplicate = root.book->entryPath(2);
        QVERIFY(documentBookKey(b) != documentBookKey(duplicate));
        root.book.reset();
        QVERIFY(TestData::write(path, TestData::zip({{"b.txt", "B"}, {"a.txt", "A"}, {"b.txt", "C"}})));
        const auto repacked = Book::open(path); QVERIFY(repacked.book);
        QCOMPARE(repacked.book->entryPath(0), a); QCOMPARE(repacked.book->entryPath(1), b);
        const auto child = Book::open(duplicate); QVERIFY(child.book && child.book->publication);
        QCOMPARE(child.book->publication->sections.first().plain, QString("C"));
        QVERIFY(!QFileInfo(a).exists()); QVERIFY(bookPathExists(a));
    }
    void imageOnlyArchiveKeepsReadingAndMixedImagesStayASequence() {
        QTemporaryDir d; const auto plain = d.filePath("images.zip"), mixed = d.filePath("mixed.zip");
        const auto one = TestData::png(TestData::solid(Qt::red)), two = TestData::png(TestData::solid(Qt::blue));
        QVERIFY(TestData::write(plain, TestData::zip({{"1.png", one}, {"3.png", two}})));
        QVERIFY(TestData::write(mixed, TestData::zip({{"1.png", one}, {"2.txt", "Text"}, {"3.png", two}})));
        const auto images = Book::open(plain); QVERIFY(images.book); QVERIFY(!images.book->archiveDirectory); QCOMPARE(images.book->pages.size(),2);
        const auto root = Book::open(mixed); QVERIFY(root.book && root.book->archiveDirectory);
        const auto opened = Book::open(root.book->entryPath(2)); QVERIFY2(opened.book,qPrintable(opened.error));
        QVERIFY(!opened.book->archiveDirectory); QCOMPARE(opened.book->pages.size(),2); QCOMPARE(opened.book->initialPage,1);
        QCOMPARE(opened.book->image(1).image, TestData::solid(Qt::blue)); QVERIFY(opened.book->sourceUnchanged(1));
    }
    void fileListReturnsToSelectedDocumentAndKeepsSeparateSettings() {
        if (!TestData::webEngineIpcAvailable()) QSKIP("Chromium IPC is denied; run archive document navigation on a desktop or CI.");
        QTemporaryDir d; const auto path = d.filePath("book.zip"), config = d.filePath("settings.ini");
        const auto text = longText(); QVERIFY(TestData::write(path,TestData::zip({{"1.txt",text},{"2.md","# Ending"}})));
        { QSettings settings(config,QSettings::IniFormat);settings.setValue("text/renderer",0); }
        MainWindow w(config); w.show(); TestData::singlePage(w);w.openBook(path);
        QTRY_VERIFY_WITH_TIMEOUT(!w.isLoading(),15000); QCOMPARE(w.viewMode(),MainWindow::ViewMode::Covers); QCOMPARE(w.pageCount(),0);
        auto *grid=w.findChild<ThumbnailGrid *>("zipLibrary");QVERIFY(grid);QCOMPARE(grid->count(),2);
        QTRY_VERIFY_WITH_TIMEOUT(grid->item(0)->data(ThumbnailRoleReady).toBool(),15000);
        grid->setCurrentRow(1); const auto second=grid->item(1)->data(PathRole).toString();
        w.command("thumbAccept")->trigger();QTRY_VERIFY_WITH_TIMEOUT(w.renderReady(),15000);QCOMPARE(w.bookPath(),second);
        QVERIFY(isMarkdownDocument(w.bookPath(),{})); QVERIFY(w.command("findDocument")->isEnabled());
        bool rendererAbsent=false;
        QTimer::singleShot(0,&w,[&]{auto *dialog=qobject_cast<QDialog *>(QApplication::activeModalWidget());if(!dialog)return;
            rendererAbsent=!dialog->findChild<QComboBox *>("document_renderer");
            dialog->findChild<QSpinBox *>("document_size")->setValue(40);dialog->accept();});
        w.command("documentSettings")->trigger();QVERIFY(rendererAbsent);QTRY_VERIFY_WITH_TIMEOUT(w.renderReady(),15000);
        w.parentFolder();QCOMPARE(w.viewMode(),MainWindow::ViewMode::Covers);QCOMPARE(grid->count(),2);QCOMPARE(grid->currentRow(),1);
        grid->setCurrentRow(0);w.showCovers();QCOMPARE(grid->currentRow(),0);
        const auto first=grid->item(0)->data(PathRole).toString();QSettings settings(config,QSettings::IniFormat);
        QCOMPARE(documentOptions(settings,second).value("size").toInt(),40);QCOMPARE(documentOptions(settings,first).value("size").toInt(),22);
        grid->setCurrentRow(0);w.command("thumbAccept")->trigger();QTRY_VERIFY_WITH_TIMEOUT(w.renderReady(),15000);
        QVERIFY(w.pageCount()>1);
        w.command("nextOne")->trigger();QTRY_VERIFY(w.renderReady());QCOMPARE(w.pageIndex(),1);
        auto *document=w.findChild<DocumentView *>();QVERIFY(document);
        QTRY_VERIFY(document->location().offset>0);
        w.parentFolder();QCOMPARE(grid->currentRow(),0);
        grid->setCurrentRow(1);w.command("thumbAccept")->trigger();QTRY_VERIFY(w.renderReady());
        w.parentFolder();grid->setCurrentRow(0);w.command("thumbAccept")->trigger();QTRY_VERIFY(w.renderReady());QTRY_COMPARE(w.pageIndex(),1);
        w.parentFolder();w.parentFolder();QVERIFY(w.directory()==d.path());QVERIFY(grid->currentItem());
        QCOMPARE(grid->currentItem()->data(PathRole).toString(),path);w.close();
    }
    void archiveListingRefreshAndLeavingForARealFile() {
        QTemporaryDir d;const auto path=d.filePath("book.zip"),file=d.filePath("image.png");
        QVERIFY(TestData::write(path,TestData::zip({{"1.txt","Text"},{"2.txt","More"}})));
        QVERIFY(TestData::write(file,TestData::png(TestData::solid())));
        MainWindow w(d.filePath("settings.ini"));w.show();w.openBook(path);QTRY_VERIFY(!w.isLoading());
        auto *grid=w.findChild<ThumbnailGrid *>("zipLibrary");QCOMPARE(grid->count(),2);
        grid->setCurrentRow(1);
        QVERIFY(TestData::write(path,TestData::zip({{"1.txt","Text"},{"2.txt","More"},{"3.md","# Third"}})));
        w.command("thumbRefresh")->trigger();QTRY_VERIFY(!w.isLoading());QCOMPARE(grid->count(),3);QCOMPARE(grid->currentRow(),1);
        w.openBook(file);QTRY_VERIFY(w.renderReady());w.showCovers();
        QVERIFY(grid->count()>=2);for(int i=0;i<grid->count();++i)QVERIFY(!parseArchiveEntryPath(grid->item(i)->data(PathRole).toString()).valid());
        w.close();
    }

};
ISOLATED_TEST_MAIN(ArchiveTextTests)
#include "archive_text_tests.moc"
