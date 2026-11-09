// SPDX-License-Identifier: GPL-3.0-only
#include "test_support.h"
#include "documentview.h"
#include "documentsettings.h"
#include "publication.h"
#include "displayjobs.h"
#include "readerfeatures.h"
#include "thumbnailsource.h"
#include <QTimer>
#include <QAction>
#include <QJSEngine>
#include <QJsonArray>
#include <QWebEngineView>
#include <QWebEnginePage>
#include <QWebEngineScript>

namespace {
QByteArray epub(bool rtl=false,const QByteArray &coverProperties="",const QByteArray &extra={}) {
    const QByteArray opf="<package><metadata><meta name=\"cover\" content=\"image\"/></metadata><manifest>"
      "<item id=\"image\" href=\"image.png\" media-type=\"image/png\"/>"
      "<item id=\"front\" href=\"front.xhtml\" media-type=\"application/xhtml+xml\"/>"
      "<item id=\"body\" href=\"body.xhtml\" media-type=\"application/xhtml+xml\"/>"
      "</manifest><spine page-progression-direction=\""+(rtl?QByteArray("rtl"):QByteArray("ltr"))+"\"><itemref idref=\"front\" "+coverProperties+"/><itemref idref=\"body\"/></spine></package>";
    QByteArray text="<html><head><style>body{writing-mode:horizontal-tb}</style></head><body>";
    for(int n=0;n<300;++n)text+="<p>Generic sample paragraph.</p>";
    text+="</body></html>";
    return TestData::zip({{"mimetype","application/epub+zip"},{"META-INF/container.xml","<container><rootfiles><rootfile full-path=\"book.opf\"/></rootfiles></container>"},
      {"book.opf",opf},{"front.xhtml","<html><head></head><body><img src=\"image.png\"/></body></html>"},{"image.png",TestData::png(TestData::solid(Qt::green,{600,800}))},
      {"body.xhtml",text},{"unused.bin",extra}});
}
}
class DocumentBehaviorTests : public QObject {
    Q_OBJECT
private slots:
    void directionAndWideRules() {
        QFile script(":/documents/panels.js");QVERIFY(script.open(QIODevice::ReadOnly));
        QJSEngine engine;engine.evaluate("var window={};");QVERIFY(!engine.evaluate(script.readAll()).isError());
        const auto rules=engine.globalObject().property("window").property("komaPanels");
        auto direction=rules.property("direction"),wide=rules.property("isWide");QVERIFY(direction.isCallable());
        for(bool published:{false,true}) {
            QVERIFY(direction.call({true,true,published,2,false,false}).toBool());
            QVERIFY(!direction.call({true,true,published,1,false,true}).toBool());
            QCOMPARE(direction.call({true,true,published,0,false,!published}).toBool(),published);
            QCOMPARE(direction.call({true,true,published,2,true,false}).toBool(),published);
        }
        QVERIFY(direction.call({true,false,false,0,false,true}).toBool());
        QVERIFY(!direction.call({true,false,true,0,false,false}).toBool());
        QVERIFY(direction.call({false,false,false,2,false,false}).toBool());
        QVERIFY(!direction.call({false,false,true,1,false,true}).toBool());
        QVERIFY(wide.call({1536,768}).toBool());QVERIFY(!wide.call({600,800}).toBool());QVERIFY(!wide.call({300,100}).toBool());
    }
    void lazyEpubSkipsUnrequestedPayloads() {
        QTemporaryDir d;const auto path=d.filePath("sample.epub");QVERIFY(TestData::write(path,epub(false,{},QByteArray(8*1024*1024,'x'))));
        OpenOptions options;options.deferDocuments=true;
        const auto bytes=PublicationActivity::payloadBytes.load(),loads=PublicationActivity::sectionLoads.load();
        const auto opened=Book::open(path,{},options);QVERIFY2(opened.book,qPrintable(opened.error));
        const auto p=opened.book->publication;QVERIFY(p->archive);QCOMPARE(p->sections.size(),2);
        QVERIFY(p->sections.first().html.isEmpty());QVERIFY(!p->resources.contains("unused.bin"));
        QVERIFY(PublicationActivity::payloadBytes.load()-bytes<10000);QCOMPARE(PublicationActivity::sectionLoads.load(),loads);
        const auto section=loadPublicationSection(*p,0);QVERIFY2(section.error.isEmpty(),qPrintable(section.error));
        QVERIFY(section.cover);QCOMPARE(section.imagePath,QString("image.png"));QVERIFY(!section.wide);
        QVERIFY(p->sections[1].html.isEmpty());QVERIFY(PublicationActivity::payloadBytes.load()-bytes<100000);
        const auto before=PublicationActivity::payloadReads.load();
        QVERIFY(!publicationResource(*p,"image.png").isEmpty());QCOMPARE(PublicationActivity::payloadReads.load(),before+1);
        QVERIFY(!publicationResource(*p,"image.png").isEmpty());QCOMPARE(PublicationActivity::payloadReads.load(),before+1);
    }
    void cancellationReachesDocumentReaders() {
        QTemporaryDir d;const auto path=d.filePath("sample.epub");QVERIFY(TestData::write(path,epub(false,{},QByteArray(2*1024*1024,'x'))));
        OpenOptions options;options.deferDocuments=true;int checks=0;options.cancel=[&]{++checks;return true;};
        QVERIFY(!Book::open(path,{},options).book);QVERIFY(checks>0);
        options.cancel={};const auto opened=Book::open(path,{},options);QVERIFY(opened.book);
        QString error;checks=0;
        QVERIFY(publicationResource(*opened.book->publication,"unused.bin",[&]{return ++checks>3;},&error).isEmpty());QVERIFY(!error.isEmpty());
        const auto loaded=loadPublicationSection(*opened.book->publication,1,[]{return true;});QVERIFY(!loaded.error.isEmpty());
    }
    void legacySeparatorsRemainBounded() {
        QTemporaryDir d;const auto path=d.filePath("sample.zip");const auto png=TestData::png(TestData::solid(Qt::blue,{640,900}));
        const auto text=QStringLiteral("<img src=\"images\\\\sample.png\">\n<img src=\"C:\\\\outside.png\">\n<img src=\"\\\\\\\\host\\\\outside.png\">\n<img src=\"..\\\\outside.png\">");
        QVERIFY(TestData::write(path,TestData::zip({{"text.txt",text.toUtf8()},{"images/sample.png",png}})));
        const auto listing=Book::open(path);QVERIFY(listing.book);int index=-1;for(int n=0;n<listing.book->pages.size();++n)if(listing.book->pages[n].name.endsWith(".txt"))index=n;QVERIFY(index>=0);
        const auto opened=Book::open(listing.book->entryPath(index));QVERIFY(opened.book);
        QCOMPARE(opened.book->publication->resources.size(),1);
        QVERIFY(!QImage::fromData(opened.book->publication->resources.constBegin().value()).isNull());
        const auto html=opened.book->publication->sections.first().html;
        QCOMPARE(html.count("<img class=\"illustration\""),1);QCOMPARE(html.count("class=\"koma-missing-image\""),3);
    }
    void illustrationsLoadOnDemandAndKeepOffsets() {
        const auto image=TestData::solid(Qt::blue,{1536,768});int calls=0;
        const auto text=QStringLiteral("前の本文\n［＃改ページ］\n［＃挿絵（sample.png、横1536×縦768）入る］\n［＃改ページ］\n後の本文");
        TextOptions options;options.pageLayout=1;options.vertical=true;
        const auto p=readTextPublication(text,"sample.txt",options,[&](const QUrl &)->QVariant{++calls;return image;},{},{},true);
        QVERIFY2(p->error.isEmpty(),qPrintable(p->error));QCOMPARE(calls,0);QCOMPARE(p->imageSources.size(),1);
        QCOMPARE(p->sections.size(),3);const auto picture=p->sections[1];QVERIFY(picture.wide);QVERIFY(picture.fixed);QVERIFY(!picture.cover);
        QCOMPARE(picture.locationPath,QString("text.html"));QVERIFY(picture.baseOffset>0);QVERIFY(p->sections[2].baseOffset>picture.baseOffset);
        const auto raw=publicationResource(*p,picture.imagePath);QVERIFY(!QImage::fromData(raw).isNull());QCOMPARE(calls,1);
        QVERIFY(!publicationResource(*p,picture.imagePath).isEmpty());QCOMPARE(calls,1);
        QVERIFY(p->sections[0].plain.contains(QStringLiteral("前の本文")));QVERIFY(p->sections[2].plain.contains(QStringLiteral("後の本文")));
    }
    void illustrationCaptionsStayWithTheirPicture() {
        const auto text=QStringLiteral("前\n［＃挿絵（sample.png）入る］\n［＃キャプション］\n図の説明\n［＃キャプション終わり］\n後");
        const auto p=readTextPublication(text,"sample.txt",{},[](const QUrl &)->QVariant{return TestData::solid();},{},{},true);
        QCOMPARE(p->sections.size(),3);QVERIFY(!p->sections[1].caption.isEmpty());
        QVERIFY(p->sections[1].caption.contains(QStringLiteral("図の説明")));
        QVERIFY(p->sections[2].plain.contains(QStringLiteral("後")));
    }
    void bookNavigationDuringOpenDoesNotNeedCoverItems() {
        QTemporaryDir d;QStringList paths;
        for(int n=0;n<3;++n){const auto path=d.filePath(QString("%1.epub").arg(n));QVERIFY(TestData::write(path,epub()));paths.append(path);}
        MainWindow w(d.filePath("settings.ini"));w.m_currentPath=paths[1];w.m_directory=d.path();w.m_neighborDirectory=d.path();w.m_neighborPaths=paths;
        w.m_book=Book::open(paths[1]).book;w.m_mode=MainWindow::ViewMode::Reader;w.m_renderReady=true;w.refreshState();
        QCOMPARE(w.m_library->count(),0);QVERIFY(w.command("previousBook")->isEnabled());
        w.moveBook(1);QVERIFY(w.isLoading());QVERIFY(w.command("previousBook")->isEnabled());w.moveBook(-1);QCOMPARE(w.bookPath(),paths[1]);
        // Prevent creating a native WebEngine view in this scheduling-only case.
        w.m_displayJobs->stop();w.m_displayJobs->waitForDone();
    }
    void leavingReaderDoesNotCreateHiddenDocumentView() {
        QTemporaryDir d;const auto path=d.filePath("sample.epub");QVERIFY(TestData::write(path,epub()));
        MainWindow w(d.filePath("settings.ini"));w.openBook(path);w.setMode(MainWindow::ViewMode::Covers);
        QTRY_VERIFY(!w.isLoading());QVERIFY(w.m_book && w.m_book->publication);QVERIFY(!w.m_documentView);
        QCOMPARE(w.viewMode(),MainWindow::ViewMode::Covers);
    }
    void nativeEpubOpensWithoutPreviewPaint() {
        if(!TestData::webEngineIpcAvailable())QSKIP("WebEngine IPC is unavailable in this execution environment.");
        QTemporaryDir d;const auto path=d.filePath("sample.epub");QVERIFY(TestData::write(path,epub()));
        // Opening a hidden window cannot paint a temporary PageView cover.
        // It must still create the document view directly after metadata opens.
        MainWindow w(d.filePath("settings.ini"));w.openBook(path);
        QTRY_VERIFY_WITH_TIMEOUT(w.m_documentView,10000);
        QVERIFY(!w.findChild<QWidget *>("documentCoverPreview"));
        w.resize(800,1200);w.show();QTRY_VERIFY_WITH_TIMEOUT(w.renderReady(),40000);
        QCOMPARE(w.m_documentView->visiblePages().size(),1);
    }
    void nativeThumbnailsAfterDocumentExit_data() {
        QTest::addColumn<bool>("epubDocument");QTest::newRow("txt")<<false;QTest::newRow("epub")<<true;
    }
    void nativeThumbnailsAfterDocumentExit() {
        if(!TestData::webEngineIpcAvailable())QSKIP("WebEngine IPC is unavailable in this execution environment.");
        QFETCH(bool,epubDocument);QTemporaryDir d;const auto images=d.filePath("images");QVERIFY(QDir().mkpath(images));
        const auto imageZip=images+"/sample.zip",document=d.filePath(epubDocument?"sample.epub":"sample.txt");
        const auto png=TestData::png(TestData::solid(Qt::blue,{240,320}));
        QVERIFY(TestData::write(imageZip,TestData::zip({{"1.png",png},{"2.png",png}})));
        QVERIFY(TestData::write(document,epubDocument?epub():QByteArray("Generic text\n")));
        auto cache=std::make_shared<ThumbnailDiskCache>(d.filePath("cache"));QVERIFY(!cachedCover(imageZip,{},cache).image.isNull());
        const auto book=Book::open(imageZip).book;QVERIFY(book);for(int i=0;i<2;++i)QVERIFY(!cachedPageThumbnail(book,i,cache).image.isNull());
        MainWindow w(d.filePath("settings.ini"));w.m_thumbnailDiskCache=cache;w.resize(900,600);w.show();w.openDirectory(images);
        const auto ready=[](ThumbnailGrid *grid){const auto rows=grid->visibleRows();if(rows.isEmpty())return false;for(int row:rows)if(!grid->item(row)->data(ThumbnailRoleReady).toBool())return false;return true;};
        QTRY_VERIFY(w.directoryReady() && ready(w.m_library));const auto before=cache->lookupActivity().pngDecodes;
        w.openBook(document);QTRY_VERIFY_WITH_TIMEOUT(w.renderReady(),40000);auto *view=w.m_documentView;QVERIFY(view);
        w.openDirectory(images);QTRY_VERIFY(w.directoryReady() && ready(w.m_library));QCOMPARE(cache->lookupActivity().pngDecodes,before);
        for(auto *timer:view->findChildren<QTimer *>(QString(),Qt::FindDirectChildrenOnly))QVERIFY(!timer->isActive());
        w.loadBook(imageZip,MainWindow::ViewMode::Pages);QTRY_VERIFY(!w.isLoading() && ready(w.m_pages));
        QCOMPARE(w.viewMode(),MainWindow::ViewMode::Pages);QVERIFY(!w.m_documentView);QCOMPARE(cache->lookupActivity().pngDecodes,before);
        w.setMode(MainWindow::ViewMode::Covers);QTRY_VERIFY(ready(w.m_library));w.setMode(MainWindow::ViewMode::Pages);QTRY_VERIFY(ready(w.m_pages));
        QCOMPARE(cache->lookupActivity().pngDecodes,before);
    }
    void nativeEvictedSpreadAndRapidNavigation_data() {
        QTest::addColumn<bool>("rtl");QTest::newRow("ltr")<<false;QTest::newRow("rtl")<<true;
    }
    void nativeEvictedSpreadAndRapidNavigation() {
        if(!TestData::webEngineIpcAvailable())QSKIP("WebEngine IPC is unavailable in this execution environment.");
        QFETCH(bool,rtl);
        auto p=std::make_shared<Publication>();p->epub=true;p->directionSpecified=true;p->rightToLeft=rtl;
        for(int i=0;i<6;++i) {
            PublicationSection s;s.path=QString("%1.xhtml").arg(i);s.fixed=true;s.viewport={1200,1600};
            s.html="<html><head></head><body><p>Generic sample page.</p></body></html>";p->sections.append(s);
        }
        DocumentView view;view.resize(1000,800);view.show();QSignalSpy failures(&view,&DocumentView::failed);
        TextOptions options;options.pageLayout=1;view.open(p,options);
        QTRY_VERIFY_WITH_TIMEOUT(view.ready(),40000);QCOMPARE(view.visiblePages().size(),2);
        for(int target:{2,4,0}) {
            view.jump(target);QTRY_COMPARE_WITH_TIMEOUT(view.currentIndex(),target,10000);
            QCOMPARE(view.visiblePages(),(QVector<int>{target,target+1}));QCOMPARE(failures.size(),0);
        }
        for(int i=0;i<4;++i) {
            view.jump(4);view.step(-1,false);view.step(1,false);view.jump(2);
            QTRY_COMPARE_WITH_TIMEOUT(view.currentIndex(),2,10000);QCOMPARE(failures.size(),0);
            view.step(1,false);QTRY_COMPARE_WITH_TIMEOUT(view.currentIndex(),4,10000);
            QCOMPARE(view.visiblePages(),(QVector<int>{4,5}));QCOMPARE(failures.size(),0);
        }
    }
    void nativeAozoraWidePageAndResume() {
        if(!TestData::webEngineIpcAvailable())QSKIP("WebEngine IPC is unavailable in this execution environment.");
        QTemporaryDir d;const auto path=d.filePath("sample.txt"),ini=d.filePath("settings.ini");
        QVERIFY(TestData::write(d.filePath("sample.png"),TestData::png(TestData::solid(Qt::blue,{1536,768}))));
        QVERIFY(TestData::write(path,QStringLiteral("前の本文\n［＃改ページ］\n［＃挿絵（sample.png）入る］\n［＃改ページ］\n後の本文").toUtf8()));
        {QSettings settings(ini,QSettings::IniFormat);settings.setValue(documentBookKey(path)+"pageLayout",1);settings.setValue(documentBookKey(path)+"writingMode",2);}
        MainWindow w(ini);w.resize(800,1200);w.show();w.openBook(path);QTRY_VERIFY_WITH_TIMEOUT(w.renderReady(),40000);
        w.nextPage();QTRY_VERIFY(w.m_documentWidePages.contains(w.pageIndex()));
        auto *view=w.m_documentView;QVERIFY(view->rightToLeft());QCOMPARE(view->visiblePages().size(),1);
        const auto location=view->location();QVERIFY(location.offset>0);QCOMPARE(location.section,QString("text.html"));
        w.setDocumentOption("wideImages",false);QTRY_VERIFY_WITH_TIMEOUT(w.renderReady(),40000);
        QCOMPARE(w.m_documentView->location().offset,location.offset);QCOMPARE(w.m_documentView->location().section,location.section);
        QVERIFY(!w.m_documentWidePages.contains(w.pageIndex()));QCOMPARE(w.m_documentView->visiblePages().size(),2);
    }
    void nativeDirectionCoverAndQuickReturn() {
        if(!TestData::webEngineIpcAvailable())QSKIP("WebEngine IPC is unavailable in this execution environment.");
        QTemporaryDir d;const auto a=d.filePath("a.epub"),b=d.filePath("b.epub"),ini=d.filePath("settings.ini");
        QVERIFY(TestData::write(a,epub()));QVERIFY(TestData::write(b,epub(true)));
        {QSettings s(ini,QSettings::IniFormat);s.setValue(documentBookKey(a)+"pageLayout",1);s.setValue(documentBookKey(a)+"writingMode",2);}
        MainWindow w(ini);w.resize(800,1200);w.show();w.openBook(a);QTRY_VERIFY_WITH_TIMEOUT(w.renderReady(),40000);
        auto *view=w.m_documentView;QVERIFY(view);QVERIFY(view->rightToLeft());QCOMPARE(view->visiblePages().size(),1);
        bool done=false;QVariant shape;auto *web=view->findChild<QWebEngineView *>();
        web->page()->runJavaScript("(()=>{const f=[...document.querySelectorAll('iframe')].find(f=>f.style.visibility==='visible');const r=f.getBoundingClientRect();return {x:r.x,w:r.width};})()",QWebEngineScript::ApplicationWorld,[&](const QVariant &v){shape=v;done=true;});
        QTRY_VERIFY(done);QVERIFY(shape.toMap().value("w").toDouble()>700);
        const auto opens=BookActivity::opens.load();w.openBook(b);w.openBook(a);
        QCOMPARE(w.m_documentView,view);QVERIFY(w.renderReady());QVERIFY(BookActivity::opens.load()<=opens+1);
    }
};
ISOLATED_TEST_MAIN(DocumentBehaviorTests)
#include "document_behavior_tests.moc"
