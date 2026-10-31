// SPDX-License-Identifier: GPL-3.0-only
#include "test_support.h"
#include "documentsettings.h"
#include "bookexport.h"
#include "scriptreader.h"
#include "shortcuts.h"
#include "librarystore.h"
#include "readerfeatures.h"
#include "documentview.h"
#include <QWebEngineView>
#include <QWebEnginePage>
#include <QWebEngineScript>
#include <QFileDialog>
#include <QInputDialog>
#include <QListWidget>
#include <QColorSpace>
#include <QImageReader>
#include <thread>
#include <chrono>
#include <QMessageBox>
#include <QTimer>
#include <QDialogButtonBox>
#include <QPushButton>
#ifdef Q_OS_UNIX
#include <sys/socket.h>
#include <unistd.h>
#endif
#include "pageview.h"
#include <QAction>
#include <QCryptographicHash>
#include <QDateTime>
#include <QJSEngine>
#include <QJsonDocument>
#include <QJsonArray>
#include <QFileInfo>
#include <archive.h>
#include <archive_entry.h>

namespace {
QByteArray ppm() { return QByteArray("P6\n# page one\n2 1\n255\n")+QByteArray::fromHex("ff000000ff00")+"P6\n1 1\n255\n"+QByteArray::fromHex("0000ff"); }
QByteArray noteBook(QString algorithm="http://www.idpf.org/2008/embedding",QString encrypted="OPS/font.otf") {
  QByteArray font("OTTO");font+=QByteArray(1200,'x');
  const auto key=QCryptographicHash::hash("book-id",QCryptographicHash::Sha1);
  for(int i=0;i<1040;++i)font[i]=char(uchar(font[i])^uchar(key[i%20]));
  return TestData::zip({{"mimetype","application/epub+zip"},
    {"META-INF/container.xml","<container><rootfile full-path=\"OPS/book.opf\"/></container>"},
    {"META-INF/encryption.xml",("<encryption><EncryptedData><EncryptionMethod Algorithm=\""+algorithm+"\"/><CipherData><CipherReference URI=\""+encrypted+"\"/></CipherData></EncryptedData></encryption>").toUtf8()},
    {"OPS/book.opf",R"(<package unique-identifier="uid"><metadata><identifier id="uid"> book-id </identifier></metadata><manifest><item id="a" href="a.xhtml" media-type="application/xhtml+xml"/><item id="b" href="b.xhtml" media-type="application/xhtml+xml"/><item id="n" href="note.xhtml" media-type="application/xhtml+xml"/><item id="f" href="font.otf" media-type="font/otf"/></manifest><spine page-progression-direction="rtl"><itemref idref="a" properties="page-spread-right"/><itemref idref="b" properties="rendition:page-spread-center"/><itemref idref="n" linear="no"/></spine></package>)"},
    {"OPS/a.xhtml",R"(<html><head></head><body><p id="start">Body <a href="note.xhtml#note">Note</a></p></body></html>)"},
    {"OPS/b.xhtml","<html><head></head><body><p>Page two</p></body></html>"},
    {"OPS/note.xhtml",R"(<html><head></head><body><p id="note">Footnote <a href="a.xhtml#start">Return</a></p></body></html>)"},
    {"OPS/font.otf",font}});
}
}
class EvolutionTests: public QObject {
 Q_OBJECT
private slots:
 void renameSettingsMerge() {
  QTemporaryDir d;QSettings s(d.filePath("settings.ini"),QSettings::IniFormat);
  const auto a=d.filePath("old.epub"),b=d.filePath("new.epub");
  s.setValue(documentBookKey(a)+"font","Test Font");s.setValue(documentBookKey(a)+"renderer",0);
  s.setValue(documentBookKey(a)+"location",QVariantMap{{"offset",123}});
  s.setValue(documentBookKey(b)+"font","Other");s.setValue(documentBookKey(b)+"size",30);
  moveDocumentSettings(s,a,b);
  QCOMPARE(s.value(documentBookKey(b)+"font").toString(),"Test Font");
  QCOMPARE(s.value(documentBookKey(b)+"renderer").toInt(),0);
  QCOMPARE(s.value(documentBookKey(b)+"size").toInt(),30);
  QCOMPARE(s.value(documentBookKey(b)+"location").toMap().value("offset").toInt(),123);
  QVERIFY(!s.contains(documentBookKey(a)+"font"));
  moveDocumentSettings(s,b,b);QCOMPARE(s.value(documentBookKey(b)+"font").toString(),"Test Font");
 }
 void renameDocumentThroughUi() {
  if (!TestData::webEngineIpcAvailable()) QSKIP("Chromium IPC is denied; run document rename on a desktop or CI.");
  QTemporaryDir d;const auto path=d.filePath("before.txt"),target=d.filePath("after.txt"),config=d.filePath("settings.ini");
  QString text;for(int i=0;i<1000;++i)text+=QString("Line %1: document text.\n").arg(i);
  QVERIFY(TestData::write(path,text.toUtf8()));
  {QSettings s(config,QSettings::IniFormat);s.setValue(documentBookKey(path)+"renderer",0);s.setValue(documentBookKey(path)+"font","Serif");s.setValue(documentBookKey(path)+"size",30);s.setValue(documentBookKey(target)+"size",28);s.setValue("reading/spread",false);}
  MainWindow w(config);w.show();w.openBook(path);QTRY_VERIFY(w.renderReady());w.nextPage();QTRY_VERIFY(w.renderReady());QVERIFY(w.pageIndex()>0);
  auto *document=w.findChild<DocumentView *>();QVERIFY(document);QTRY_VERIFY(document->location().offset>0);
  const auto savedLocation=document->location();
  QTimer dialog;dialog.setInterval(10);connect(&dialog,&QTimer::timeout,&w,[&]{if(auto *input=qobject_cast<QInputDialog*>(QApplication::activeModalWidget())){input->setTextValue("after.txt");input->accept();}});
  dialog.start();w.command("renameFile")->trigger();dialog.stop();QTRY_COMPARE(w.bookPath(),target);QTRY_VERIFY(w.renderReady());
  QVERIFY(!QFileInfo::exists(path));QVERIFY(w.pageIndex()>0);
  auto *renamed=w.findChild<DocumentView *>();QVERIFY(renamed);
  QTRY_COMPARE(renamed->location().section,savedLocation.section);QTRY_COMPARE(renamed->location().offset,savedLocation.offset);
  QSettings s(config,QSettings::IniFormat);QCOMPARE(s.value(documentBookKey(target)+"font").toString(),"Serif");QCOMPARE(s.value(documentBookKey(target)+"size").toInt(),30);QCOMPARE(s.value(documentBookKey(target)+"renderer").toInt(),0);
  for(const auto &key:s.allKeys())QVERIFY(!key.startsWith(documentBookKey(path)));
  w.close();
 }
 void libraryRenameCollision() {
  QTemporaryDir d;LibraryStore library(d.filePath("books.sqlite"));
  const auto source=library.add("history","old.epub"),target=library.add("history","new.epub");
  QVERIFY(library.update(source,{{"title","Source"}},{{"anchor",12}}));
  QVERIFY(library.update(target,{{"author","Author"}},{{"anchor",2},{"marker",50}}));
  QVERIFY(library.movePath("old.epub","new.epub"));
  QCOMPARE(library.items("history").size(),1);const auto moved=library.item(source);
  QCOMPARE(moved.path,"new.epub");QCOMPARE(moved.state.value("anchor").toInt(),12);
  QCOMPARE(moved.state.value("marker").toInt(),50);QCOMPARE(moved.metadata.value("author").toString(),"Author");
 }
 void legacyIniImportReport() {
  QTemporaryDir d;const auto fixture=d.filePath("legacy.ini"),config=d.filePath("settings.ini");
  QVERIFY(TestData::write(fixture,"[KeyBoardKeyConfig]\nCMD1=39\nCMD16=549\nCMD9=444\nCMD999=65\n[Config]\nPrepageForwardNum=3\nSubFolder=1\nUnknown=42\n"));
  MainWindow w(config);w.show();QTimer dialogs;dialogs.setInterval(10);int steps=0;
  connect(&dialogs,&QTimer::timeout,&w,[&]{
    if(auto *file=qobject_cast<QFileDialog*>(QApplication::activeModalWidget())) {
      file->selectFile(fixture);QMetaObject::invokeMethod(file,"accept",Qt::DirectConnection);++steps;
    } else if(auto *report=qobject_cast<QMessageBox*>(QApplication::activeModalWidget())){report->accept();++steps;}
    else if(auto *preview=qobject_cast<QDialog*>(QApplication::activeModalWidget());preview && preview->objectName()=="mangaMeeyaImportDialog") { preview->findChild<QPushButton*>("applyMangaMeeyaImport")->click();++steps; }
  });
  ReaderFeatures *features=nullptr;
  for(auto *child:w.children())if(auto *candidate=dynamic_cast<ReaderFeatures*>(child))features=candidate;
  QVERIFY(features);dialogs.start();features->settingsFile(true,fixture);dialogs.stop();QVERIFY(steps>=1);
  QSettings s(config,QSettings::IniFormat);QCOMPARE(s.value("cache/ahead").toInt(),3);QCOMPARE(s.value("source/recursive").toBool(),true);
  QCOMPARE(s.value("legacy/Config/Unknown").toInt(),42);QCOMPARE(w.bindings().value("scrollLeft"),QKeySequence("Ctrl+Left"));
  const auto report=s.value("migration/lastIniReport").toStringList().join('\n');QVERIFY(report.contains("CMD999"));QVERIFY(report.contains("CMD9"));w.close();
 }
 void legacyListReadingOrder() {
  QTemporaryDir d;const auto book=d.filePath("book.cbz"),fixture=d.filePath("books.lst"),config=d.filePath("settings.ini");
  QVector<TestData::ZipEntry> entries;
  for(int i=1;i<=3;++i)entries.append({QString("%1.png").arg(i).toUtf8(),TestData::png(TestData::solid(QColor(i*40,0,0)))});
  QVERIFY(TestData::write(book,TestData::zip(entries)));
  QStringList columns;for(int i=0;i<31;++i)columns.append("");
  columns[0]='"'+book+'"';columns[1]="2";columns[2]="3";columns[3]="3";columns[7]="2";columns[8]="2";columns[28]="0";columns[30]="secret-must-not-import";
  QVERIFY(TestData::write(fixture,("30\n"+columns.join(',')+"\n").toUtf8()));
  {QSettings s(config,QSettings::IniFormat);s.setValue("reading/spread",false);}
  MainWindow w(config);w.show();ReaderFeatures *features=nullptr;
  for(auto *child:w.children())if(auto *candidate=dynamic_cast<ReaderFeatures*>(child))features=candidate;
  QVERIFY(features);features->loadList(false,fixture);
  LibraryStore library(d.filePath("library.sqlite3"));const auto items=library.items("playlist");QCOMPARE(items.size(),1);
  QCOMPARE(items[0].state.value("anchor").toInt(),1);QCOMPARE(items[0].state.value("marker").toInt(),2);
  QVERIFY(!QJsonDocument(items[0].metadata).toJson().contains("secret-must-not-import"));
  QListWidget *list=nullptr;
  for(auto *candidate:w.findChildren<QListWidget*>())if(candidate->count() && candidate->item(0)->data(Qt::UserRole).toString()==items[0].id){list=candidate;break;}
  QVERIFY(list);QMetaObject::invokeMethod(list,"itemActivated",Qt::DirectConnection,Q_ARG(QListWidgetItem*,list->item(0)));
  QTRY_VERIFY(w.renderReady());QCOMPARE(w.currentPageName(),"2.png");w.nextPage();QTRY_VERIFY(w.renderReady());QCOMPARE(w.currentPageName(),"1.png");
  // Restoring the collection does not change the shared default. An explicit
  // user sort still overrides this book's restored order and keeps its page.
  {QSettings settings(config,QSettings::IniFormat);QCOMPARE(settings.value("covers/sort",0).toInt(),0);}
  w.command("sortNatural")->trigger();QCOMPARE(w.currentPageName(),"1.png");w.nextPage();QTRY_VERIFY(w.renderReady());QCOMPARE(w.currentPageName(),"2.png");w.close();
 }
 void epubNoteRoundTripRendering() {
#ifdef Q_OS_UNIX
  int fd[2];if(::socketpair(AF_UNIX,SOCK_STREAM,0,fd)!=0)QSKIP("Chromium IPC is denied.");
  const bool allowed=::shutdown(fd[0],SHUT_RD)==0;::close(fd[0]);::close(fd[1]);if(!allowed)QSKIP("Chromium IPC is denied; run EPUB note rendering on a desktop or CI.");
#endif
  QTemporaryDir d;const auto path=d.filePath("notes.epub");QVERIFY(TestData::write(path,noteBook()));
  MainWindow w(d.filePath("settings.ini"));w.show();w.openBook(path);
  QTRY_VERIFY_WITH_TIMEOUT(w.renderReady() || !w.lastError().isEmpty(),40000);QVERIFY2(w.renderReady(),qPrintable(w.lastError()));
  auto *view=w.findChild<DocumentView*>();QVERIFY(view);auto *web=view->findChild<QWebEngineView*>();QVERIFY(web);
  view->navigate("OPS/note.xhtml","note",0);QTRY_VERIFY(view->auxiliaryActive());const int position=w.pageIndex();
  QVariant state;bool done=false;
  web->page()->runJavaScript("window.komaDocument.state(false)",QWebEngineScript::ApplicationWorld,[&](const QVariant &v){state=v;done=true;});QTRY_VERIFY(done);
  QCOMPARE(state.toMap().value("noteSection").toString(),"OPS/note.xhtml");
  view->back();QTRY_VERIFY(!view->auxiliaryActive());QCOMPARE(w.pageIndex(),position);w.close();
 }
 void coversSortAndAdjacent() {
  QTemporaryDir d;
  for(const auto *name:{"2.cbz","10.cbz","3.zip"})QVERIFY(TestData::write(d.filePath(name),TestData::zip({{"p.png",TestData::png(TestData::solid())}})));
  const auto config=d.filePath("settings.ini");
  {QSettings s(config,QSettings::IniFormat);s.setValue("covers/reverse",true);s.setValue("cache/ahead",0);s.setValue("cache/behind",0);}
  MainWindow w(config);w.show();w.openDirectory(d.path());
  auto *grid=w.findChild<ThumbnailGrid*>("zipLibrary");QVERIFY(grid);QCOMPARE(grid->item(0)->text(),"10.cbz");
  grid->setCurrentRow(1);w.openBook(d.filePath("10.cbz"));QTRY_VERIFY(w.renderReady());
  w.moveBook(1);QTRY_COMPARE(w.bookPath(),d.filePath("3.zip"));QTRY_VERIFY(w.renderReady());
  QVERIFY(browserLess(QFileInfo(d.filePath("3.zip")),QFileInfo(d.filePath("10.cbz")),2,true));
  w.close();
 }
 void pdfZoomRerasterizes() {
  QTemporaryDir d;const auto path=d.filePath("zoom.pdf");QVERIFY(TestData::pdf(path));
  const auto config=d.filePath("settings.ini");
  {QSettings s(config,QSettings::IniFormat);s.setValue("cache/ahead",0);s.setValue("cache/behind",0);}
  MainWindow w(config);w.show();w.openBook(path);QTRY_VERIFY_WITH_TIMEOUT(w.renderReady(),10000);
  auto *v=w.findChild<PageView*>();QVERIFY(v);const int initial=v->images().first().height();
  v->setScaleMode(ScaleMode::Actual);QTRY_VERIFY_WITH_TIMEOUT(v->images().first().height()==2560,10000);
  v->zoomBy(2);v->zoomBy(.5);v->zoomBy(1.5);
  QTRY_VERIFY_WITH_TIMEOUT(v->images().first().height()==3840 && w.renderReady(),10000);
  QVERIFY(v->images().first().height()>initial);
  const double logical=v->pageRects().first().height();
  QVERIFY(std::abs(logical-3600/v->devicePixelRatioF())<2);
  v->zoomBy(100);QTRY_VERIFY_WITH_TIMEOUT(v->images().first().height()==6144,10000);
  w.close();
 }
 void pdfAutoSpreadUsesReferencePixels() {
  QTemporaryDir d;const auto path=d.filePath("spread.pdf"),config=d.filePath("settings.ini");QVERIFY(TestData::pdf(path));
  {QSettings s(config,QSettings::IniFormat);s.setValue("reading/cover",false);s.setValue("reading/autoSpread",true);s.setValue("reading/spreadCondition","@width<2000");s.setValue("cache/ahead",0);s.setValue("cache/behind",0);}
  MainWindow w(config);w.show();w.openBook(path);QTRY_VERIFY(w.renderReady());auto *view=w.findChild<PageView*>();QVERIFY(view);
  view->setScaleMode(ScaleMode::Actual);QTRY_VERIFY(view->images().size()==2 && view->images().first().height()==2560);
  view->zoomBy(2);QTRY_VERIFY(view->images().size()==2 && view->images().first().height()==4864);
  // 3243 raster pixels wide, still approximately 1600 reference pixels.
  QVERIFY(view->images().first().width()>2000);w.close();
 }
 void scanRoundTrip() {
  for(bool rtl:{false,true})for(QSize size:{QSize(300,2400),QSize(2400,300),QSize(2400,2400)}) {
    PageView v;v.resize(700,550);v.setPages({TestData::solid(Qt::red,size)},rtl,{});v.setScaleMode(ScaleMode::Actual);
    v.enterScan(1,rtl);v.applyScanEntry();QVector<QPointF> positions{v.pan()};
    while(v.scan(1,rtl)) {positions.append(v.pan());QVERIFY(positions.size()<200);}
    QVERIFY(positions.size()>1);
    for(int i=positions.size()-2;i>=0;--i){QVERIFY(v.scan(-1,rtl));QCOMPARE(v.pan(),positions[i]);}
    QVERIFY(!v.scan(-1,rtl));
  }
 }
 void epubAuxiliaryFontsAndSides() {
  QTemporaryDir d;const auto path=d.filePath("notes.epub");QVERIFY(TestData::write(path,noteBook()));
  auto p=readPublication(path,{});QVERIFY2(p->error.isEmpty(),qPrintable(p->error));
  QCOMPARE(p->sections.size(),3);QVERIFY(p->sections[2].auxiliary);QCOMPARE(p->sections[0].side,1);QVERIFY(p->sections[1].solo);
  QVERIFY(p->resources.value("OPS/font.otf").startsWith("OTTO"));
  const auto b=Book::open(path);QVERIFY(b.book);QCOMPARE(b.book->pages.size(),2);
  QVERIFY(TestData::write(path,noteBook("unknown-font")));p=readPublication(path,{});
  QVERIFY(p->error.isEmpty());QVERIFY(!p->warnings.isEmpty());QVERIFY(!p->resources.contains("OPS/font.otf"));
  QVERIFY(TestData::write(path,noteBook("unknown-body","OPS/a.xhtml")));QVERIFY(!readPublication(path,{})->error.isEmpty());
  QJSEngine engine;engine.evaluate("var window={};");QFile js(":/documents/panels.js");QVERIFY(js.open(QIODevice::ReadOnly));QVERIFY(!engine.evaluate(js.readAll()).isError());
  for(bool rtl:{false,true}) {
    const QHash<int,int> sides{{0,rtl?-1:1},{1,rtl?1:-1},{2,rtl?-1:1}};const QSet<int> solo{3};
    int anchor=0,visited=0;
    while(true) {
      const auto pages=documentVisiblePages(5,anchor,1,{},sides,solo,rtl);visited+=pages.size();
      const auto value=engine.evaluate(QString("window.komaPanels.plan(%1,5,1,%2,[],[%3,%4,%5,0,0],[false,false,false,true,false]).map(p=>p.page).join(',')").arg(anchor).arg(rtl?"true":"false").arg(sides[0]).arg(sides[1]).arg(sides[2]));
      QStringList expected;for(int page:pages)expected.append(QString::number(page));QCOMPARE(value.toString(),expected.join(','));
      const int next=advanceDocumentPage(5,anchor,1,1,false,{},sides,solo,rtl);if(next==anchor)break;
      QCOMPARE(advanceDocumentPage(5,next,-1,1,false,{},sides,solo,rtl),anchor);anchor=next;
    }
    QCOMPARE(visited,5);
  }
 }
 void aozoraAnnotationsAndOffsets() {
  const auto html=aozoraHtml(QStringLiteral("［＃ここから大見出し］\n序章\n［＃ここで大見出し終わり］\n［＃改ページ］［＃２字下げ］｜漢字《かんじ》本文［＃未知注記］\n最後"));
  QVERIFY(html.contains("<h2"));QVERIFY(html.contains("class=\"koma-break\""));QVERIFY(html.contains("padding-inline-start:2em"));
  QVERIFY(html.contains("<ruby>漢字<rt>かんじ</rt></ruby>"));QVERIFY(html.contains(QStringLiteral("［＃未知注記］")));
  QVERIFY(!html.contains(QStringLiteral("［＃改ページ］")));QVERIFY(html.contains("data-koma-offset="));
 }
 void legacyKeyboardCodes() {
  QCOMPARE(legacyKeySequence(39),QKeySequence("Right"));QCOMPARE(legacyKeySequence(1063),QKeySequence("Alt+Right"));
  QCOMPARE(legacyKeySequence(549),QKeySequence("Ctrl+Left"));QCOMPARE(legacyKeySequence(294),QKeySequence("Shift+Up"));
  QVERIFY(legacyKeySequence(444).isEmpty());QVERIFY(legacyKeySequence(0x10000).isEmpty());
 }
 void exportPreflight() {
  QTemporaryDir d;QVERIFY(TestData::imageFolder(d.path(),3));auto b=Book::open(d.path());QVERIFY(b.book);
  ExportNaming n;n.mode=2;n.prefix="out_";n.digits=3;n.start=8;n.format="jpg";
  auto plan=planExport(*b.book,{0,1,2},d.path(),n);QVERIFY(plan.error.isEmpty());QCOMPARE(plan.names.size(),3);QVERIFY(plan.names[0].endsWith("_008.jpg"));
  QVERIFY(TestData::write(d.filePath(plan.names[1]),"keep"));plan=planExport(*b.book,{0,1,2},d.path(),n);QCOMPARE(plan.conflicts.size(),1);
  QVERIFY(!exportPages(b.book,{0,1,2},d.path(),ExportKind::Directory,{},90,1000,{},false,true,n).isEmpty());
  QVERIFY(!QFileInfo::exists(d.filePath(plan.names[0])));
  n.conflict=1;QVERIFY(exportPages(b.book,{0,1,2},d.path(),ExportKind::Directory,{},90,1000,{},false,true,n).isEmpty());
  QFile retained(d.filePath(plan.names[1]));QVERIFY(retained.open(QIODevice::ReadOnly));QCOMPARE(retained.readAll(),"keep");
  n.mode=1;auto pages=b.book->pages;pages[1].name=pages[0].name;auto duplicate=b.book->withPages(pages);QVERIFY(!planExport(*duplicate,{0,1},d.path(),n).error.isEmpty());
 }
 void processedMetadataPolicy() {
  QTemporaryDir d;auto original=TestData::solid();original.setText("Author","fixture author");original.setColorSpace(QColorSpace::SRgb);
  const auto source=d.filePath("source.png");QVERIFY(TestData::write(source,TestData::png(original)));auto b=Book::open(source);QVERIFY(b.book);
  const auto output=d.filePath("result.png");QVERIFY(exportPages(b.book,{0},output,ExportKind::Image).isEmpty());
  QImageReader reader(output);auto decoded=reader.read();QVERIFY(!decoded.isNull());QVERIFY(decoded.text("Author").isEmpty());QVERIFY(decoded.colorSpace().isValid());
  ExportNaming naming;naming.keepColorProfile=false;
  const auto stripped=d.filePath("stripped.png");QVERIFY(exportPages(b.book,{0},stripped,ExportKind::Image,{},90,1000,{},false,true,naming).isEmpty());
  QVERIFY(!QImage(stripped).colorSpace().isValid());
  const auto untouched=d.filePath("untouched.png");QVERIFY(exportPages(b.book,{0},untouched,ExportKind::Original).isEmpty());
  QFile a(source),c(untouched);QVERIFY(a.open(QIODevice::ReadOnly));QVERIFY(c.open(QIODevice::ReadOnly));QCOMPARE(a.readAll(),c.readAll());
 }
 void scriptMultipageAndFilters() {
  QCOMPARE(configuredScriptFilter("reading-filter"),QString("reading-filter"));
  QCOMPARE(configuredScriptFilter("another.package"),QString("another.package"));
  QVERIFY(configuredScriptFilter(QVariant{}).isEmpty());
  QTemporaryDir d;const auto path=d.filePath("two.ppm");QVERIFY(TestData::write(path,ppm()));
  auto b=Book::open(path);QVERIFY2(b.book,qPrintable(b.error));QCOMPARE(b.book->pages.size(),2);
  QCOMPARE(b.book->image(0).image.pixelColor(0,0),QColor(Qt::red));QCOMPARE(b.book->image(1).image.pixelColor(0,0),QColor(Qt::blue));
  const auto archive=d.filePath("two.cbz");QVERIFY(TestData::write(archive,TestData::zip({{"two.ppm",ppm()}})));
  auto packed=Book::open(archive);QVERIFY2(packed.book,qPrintable(packed.error));QCOMPARE(packed.book->pages.size(),2);QCOMPARE(packed.book->image(1).image.pixelColor(0,0),QColor(Qt::blue));
  auto sixteen=QByteArray("P6\n1 1\n65535\n")+QByteArray::fromHex("ffff80000000");
  auto sample=decodeImage(sixteen,"16bit.ppm");QVERIFY2(sample.error.isEmpty(),qPrintable(sample.error));QCOMPARE(sample.image.pixelColor(0,0),QColor(255,128,0));
  sample=decodeImage(QByteArray("P6\n1 1\n15\n")+QByteArray::fromHex("0f0800"),"low.ppm");QVERIFY(sample.error.isEmpty());QCOMPARE(sample.image.pixelColor(0,0),QColor(255,136,0));
  const auto filtered=applyScriptFilter("reading-filter",TestData::solid(Qt::red),{{"reading-filter",QVariantMap{{"grayscale",true},{"gamma",1},{"contrast",0}}}});
  QVERIFY2(filtered.error.isEmpty(),qPrintable(filtered.error));const auto pixel=filtered.image.pixelColor(0,0);QCOMPARE(pixel.red(),pixel.green());
  QVERIFY(!applyScriptFilter("reading-filter",TestData::solid(),{},[]{return true;}).error.isEmpty());
  QVERIFY(!b.book->image(0,{},[]{return true;}).error.isEmpty());
  ImageOptions options;options.scriptFilter="reading-filter";auto animated=ImageResult(TestData::solid(Qt::red));animated.frames={animated.image,TestData::solid(Qt::blue)};animated.delays={100,200};
  const auto media=processMedia(animated,options);QVERIFY(media.error.isEmpty());QCOMPARE(media.frames.size(),2);QCOMPARE(media.delays,animated.delays);
  QCOMPARE(media.frames[1].pixelColor(0,0).red(),media.frames[1].pixelColor(0,0).green());
  QVERIFY(TestData::write(path,ppm().chopped(1)));QVERIFY(!Book::open(path).error.isEmpty());
 }
 void scriptFilterExportCancellation() {
  QTemporaryDir d;
  QVERIFY(TestData::write(d.filePath("manifest.json"),R"({"id":"test-cancel","kind":"filter","apiVersion":1,"script":"filter.js"})"));
  QVERIFY(TestData::write(d.filePath("filter.js"),"function filter(){while(true){}}"));QVERIFY(registerScriptReaders(d.path()).isEmpty());
  const auto source=d.filePath("image.png"),target=d.filePath("result.png");
  QVERIFY(TestData::write(source,TestData::png(TestData::solid())));auto book=Book::open(source);QVERIFY(book.book);
  QVERIFY(TestData::write(target,"keep original"));
  ImageOptions options;options.scriptFilter="test-cancel";std::atomic_bool cancel=false;
  QElapsedTimer elapsed;elapsed.start();std::thread request([&]{std::this_thread::sleep_for(std::chrono::milliseconds(150));cancel.store(true);});
  const auto error=exportPages(book.book,{0},target,ExportKind::Image,options,90,1000,[&](int,int){return !cancel.load();});
  request.join();QVERIFY(!error.isEmpty());QVERIFY(elapsed.elapsed()<5000);
  QFile saved(target);QVERIFY(saved.open(QIODevice::ReadOnly));QCOMPARE(saved.readAll(),QByteArray("keep original"));
 }
 void archiveCursorCancellation() {
  QTemporaryDir d;QVector<TestData::ZipEntry> entries;
  for(int i=0;i<80;++i)entries.append({QString("%1.png").arg(i,3,10,QChar('0')).toUtf8(),TestData::png(TestData::solid(QColor(i,0,0)))});
  const auto path=d.filePath("many.cbz");QVERIFY(TestData::write(path,TestData::zip(entries)));
  OpenOptions options;options.fileCacheMiB=0;auto opened=Book::open(path,{},options);QVERIFY(opened.book);
  for(int index:{0,40,79,0,1,78,2})QCOMPARE(opened.book->image(index).image.pixelColor(0,0).red(),index);
  int checks=0;auto cancelled=opened.book->encoded(79,[&]{return ++checks>10;});QVERIFY(!cancelled.error.isEmpty());
  QCOMPARE(opened.book->image(79).image.pixelColor(0,0).red(),79);
 }
};
int main(int argc,char **argv){Q_INIT_RESOURCE(documents);DocumentView::registerScheme();return isolatedTestMain<EvolutionTests>(argc,argv);}
#include "evolution_tests.moc"
