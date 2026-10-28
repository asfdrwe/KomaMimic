// SPDX-License-Identifier: GPL-3.0-only
#include "documentsettings.h"
#include "publication.h"
#include "shortcuts.h"
#include "pageview.h"
#include "thumbnailgrid.h"
#include "test_support.h"
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontComboBox>
#include <QInputDialog>
#include <QListWidget>
#include <QMessageBox>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#ifdef Q_OS_UNIX
#include <sys/socket.h>
#include <unistd.h>
#endif
#include "documentview.h"
#include <QJSEngine>
#include <QWebEnginePage>
#include <QWebEngineScript>
#include <QWebEngineView>
namespace {
QByteArray imageBook(int coverKind = 0, bool invalidBody = false, const QByteArray &ignored = {}) {
  QByteArray metadata = coverKind == 1 ? "<meta name=\"cover\" content=\"front\"/>" : "";
  QByteArray properties = coverKind == 0 ? " properties=\"cover-image\"" : "";
  QByteArray guide = coverKind == 2 ? "<guide><reference type=\"cover\" href=\"cover.xhtml\"/></guide>" : "";
  QByteArray package = "<package><metadata>" + metadata + "</metadata><manifest>"
      "<item id=\"front\" href=\"assets/front.png\" media-type=\"image/png\"" + properties + "/>"
      "<item id=\"c\" href=\"cover.xhtml\" media-type=\"application/xhtml+xml\"/>"
      "<item id=\"w\" href=\"wide.xhtml\" media-type=\"application/xhtml+xml\"/>"
      "<item id=\"t\" href=\"text.xhtml\" media-type=\"application/xhtml+xml\"/>"
      "</manifest><spine page-progression-direction=\"rtl\"><itemref idref=\"c\" properties=\"rendition:layout-pre-paginated\"/>"
      "<itemref idref=\"w\"/><itemref idref=\"t\"/></spine>" + guide + "</package>";
  QByteArray text = "<html><head></head><body>";
  for (int i = 0; i < 600; ++i) text += "<p>Reading text.</p>";
  text += "<img src=\"assets/ornament.png\"/><img src=\"assets/wide.png\"/></body></html>";
  return TestData::zip({
    {"mimetype", "application/epub+zip"},
    {"META-INF/container.xml", "<container><rootfiles><rootfile full-path=\"OPS/book.opf\"/></rootfiles></container>"},
    {"OPS/book.opf", package},
    {"OPS/cover.xhtml", invalidBody ? QByteArray("not XML") : QByteArray("<html><head><meta name=\"viewport\" content=\"width=600,height=800\"/></head><body><img src=\"assets/front.png\"/></body></html>")},
    {"OPS/wide.xhtml", "<html><head></head><body><div><img src=\"assets/wide.png\"/></div></body></html>"},
    {"OPS/text.xhtml", text},
    {"OPS/assets/front.png", TestData::png(TestData::solid(Qt::green,{600,800}))},
    {"OPS/assets/wide.png", TestData::png(TestData::solid(Qt::blue,{1536,1097}))},
    {"OPS/assets/ornament.png", TestData::png(TestData::solid(Qt::red,{128,102}))},
    {"OPS/assets/unrelated.bin", ignored}
  });
}
bool webEngineIpcAvailable() {
#ifdef Q_OS_UNIX
  int fd[2];
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fd) != 0)
    return false;
  const bool ok = ::shutdown(fd[0], SHUT_RD) == 0;
  ::close(fd[0]);
  ::close(fd[1]);
  return ok;
#else
  return true;
#endif
}
QByteArray epub(const QByteArray &extra = {}, bool vertical = true,
                bool rightToLeft = true) {
  auto package = QByteArray(
      R"(<package xmlns="http://www.idpf.org/2007/opf" version="3.0"><metadata><title>試験書籍</title></metadata><manifest>
  <item id="a" href="a.xhtml" media-type="application/xhtml+xml"/><item id="z" href="z.xhtml" media-type="application/xhtml+xml"/>
  <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
  </manifest><spine page-progression-direction="rtl"><itemref idref="z"/><itemref idref="a" properties="rendition:layout-pre-paginated"/></spine></package>)");
  QByteArray chapter = "<html "
                       "xmlns=\"http://www.w3.org/1999/"
                       "xhtml\"><head><title>日本語😀</title>"
                       "<script type=\"text/javascript\" src=\"kobo.js\"/>"
                       "<script>window.headScript=true;</script>"
                       "<style>body{writing-mode:vertical-rl}body > p{color:black}"
                       "p{font-family:serif!important}</"
                       "style></head><body><h1 id=\"start\">開始</h1>";
  chapter += "<span id=\"empty\"/><div id=\"empty-block\"/>";
  chapter += "<script>window.untrustedRan=true;</script>";
  for (int i = 0; i < 600; ++i)
    chapter += QString("<p>本文%1、<ruby>漢字<rt>かんじ</rt></"
                       "ruby>の表示を確認します。</p>")
                   .arg(i)
                   .toUtf8();
  chapter += "<p id=\"end\">終わり</p><a "
             "href=\"a.xhtml#picture\">画像へ</a></body></html>";
  if (!vertical) chapter.replace("vertical-rl", "horizontal-tb");
  if (!rightToLeft) package.replace("direction=\"rtl\"", "direction=\"ltr\"");
  QVector<TestData::ZipEntry> entries{
      {"mimetype", "application/epub+zip"},
      {"META-INF/container.xml",
       R"(<container><rootfiles><rootfile full-path="OPS/book.opf"/></rootfiles></container>)"},
      {"OPS/book.opf", package},
      {"OPS/a.xhtml",
       R"(<html xmlns="http://www.w3.org/1999/xhtml"><head><meta name="viewport" content="width=600,height=800"/></head><body><h1 id="picture">固定ページ</h1><img src="page.png"/></body></html>)"},
      {"OPS/z.xhtml", chapter},
      {"OPS/page.png", TestData::png(TestData::solid(Qt::green, {600, 800}))},
      {"OPS/nav.xhtml",
       R"(<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><body><nav epub:type="toc"><ol><li><a href="z.xhtml#start">開始</a></li><li><a href="z.xhtml#end">終わり</a></li><li><a href="a.xhtml#picture">画像</a></li></ol></nav></body></html>)"}};
  if (!extra.isEmpty())
    entries.append({"../outside.txt", extra});
  return TestData::zip(entries);
}
} // namespace
class DocumentTests : public QObject {
  Q_OBJECT
private slots:
  void epubCoverIndependentOfRenderer() {
    QTemporaryDir d;
    const auto path = d.filePath("表紙.EPUB");
    for (int kind = 0; kind < 4; ++kind) {
      QVERIFY(TestData::write(path, imageBook(kind)));
      const auto cover = epubCover(path);
      QVERIFY2(!cover.image.isNull(), qPrintable(cover.error));
      QVERIFY(cover.image.width() <= 240 && cover.image.height() <= 320);
      QCOMPARE(cover.image.pixelColor(cover.image.rect().center()), QColor(Qt::green));
    }
    QVERIFY(TestData::write(path, imageBook(0, true)));
    QVERIFY(!readPublication(path, {})->error.isEmpty());
    QVERIFY(!epubCover(path).image.isNull()); // Does not parse any book chapters.
    QVERIFY(TestData::write(path, imageBook(0, false, QByteArray(32*1024*1024+1, 'x'))));
    QVERIFY(!epubCover(path).image.isNull()); // Unrelated oversized data is skipped.
    QVERIFY(epubCover(path, {240,320}, []{return true;}).image.isNull());
    QVERIFY(TestData::write(path, "not an EPUB"));
    QVERIFY(epubCover(path).image.isNull());
  }
  void epubCoverInFolderGrid() {
    QTemporaryDir d;
    QVERIFY(TestData::write(d.filePath("cover.epub"), imageBook()));
    MainWindow w(d.filePath("settings.ini"));
    w.show(); w.openDirectory(d.path());
    auto *grid = w.findChild<ThumbnailGrid *>("zipLibrary");
    QVERIFY(grid); QCOMPARE(grid->count(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(grid->item(0)->data(ThumbnailRoleReady).toBool(), 10000);
    const auto image = grid->item(0)->icon().pixmap(80, 120).toImage();
    QVERIFY(!image.isNull());
    QCOMPARE(image.pixelColor(image.rect().center()), QColor(Qt::green));
    QCOMPARE(w.viewMode(), MainWindow::ViewMode::Covers);
    QVERIFY(w.bookPath().isEmpty());
    w.close();
  }
  void wideImageClassificationAndPaging() {
    QTemporaryDir d;
    const auto path = d.filePath("wide.epub");
    QVERIFY(TestData::write(path, imageBook()));
    const auto p = readPublication(path, {});
    QVERIFY2(p->error.isEmpty(), qPrintable(p->error));
    QCOMPARE(p->sections.size(), 3);
    QVERIFY(!p->sections[0].wide);
    QVERIFY(p->sections[1].wide);
    QCOMPARE(p->sections[1].imageSize, QSize(1536,1097));
    QVERIFY(!p->sections[2].wide);
    for (int count = 1; count <= 8; ++count)
      for (int mask = 0; mask < (1 << count); ++mask) {
        QSet<int> wide;
        for (int i = 0; i < count; ++i) if (mask & (1<<i)) wide.insert(i);
        QVector<int> visited;
        int anchor = 0;
        while (true) {
          const auto visible = documentVisiblePages(count, anchor, 1, wide);
          for (int i : visible) visited.append(i);
          const int next = advanceDocumentPage(count, anchor, 1, 1, false, wide);
          if (next == anchor) break;
          QCOMPARE(advanceDocumentPage(count, next, -1, 1, false, wide), anchor);
          anchor = next;
        }
        QCOMPARE(visited.size(), count);
        for (int i = 0; i < count; ++i) QCOMPARE(visited[i], i);
      }
  }
  void documentKeyOverridesAndTextCommands() {
    if (!webEngineIpcAvailable()) QSKIP("Chromium IPC is denied; run document key bindings on a desktop or CI.");
    QTemporaryDir d;
    const auto path = d.filePath("text.txt"), config = d.filePath("settings.ini");
    QByteArray text;
    for (int i=0;i<1000;++i) text += "A paragraph for text reading.\n";
    QVERIFY(TestData::write(path,text));
    { QSettings saved(config,QSettings::IniFormat);saved.setValue("text/renderer",0);saved.setValue("documentKeys/futureCommand","Ctrl+Alt+Q"); }
    MainWindow w(config); w.show(); TestData::singlePage(w);
    auto base = w.bindings();
    QHash<QString,QKeySequence> doc{{"next",QKeySequence("Ctrl+Alt+N")},{"previous",{}}};
    QVERIFY(w.applyBindings(base,doc));
    auto conflict = doc; conflict["next"]=QKeySequence("Ctrl+O");
    QVERIFY(!w.applyBindings(base,conflict));
    w.openBook(path); QTRY_VERIFY_WITH_TIMEOUT(w.renderReady(),10000);
    QCOMPARE(w.command("next")->shortcut(),doc.value("next"));
    QVERIFY(w.command("previous")->shortcut().isEmpty());
    QCOMPARE(w.bindings().value("next"),base.value("next"));
    auto *view=w.findChild<DocumentView *>(); QVERIFY(view); view->setFocus();
    QTest::keyClick(view,Qt::Key_Right); QCOMPARE(w.pageIndex(),0);
    QTest::keyClick(view,Qt::Key_N,Qt::ControlModifier|Qt::AltModifier);
    QTRY_COMPARE(w.pageIndex(),1);
    QTRY_VERIFY(view->location().offset>0);
    QTRY_VERIFY(w.command("textSizeIncrease")->isEnabled());
    w.command("textSizeIncrease")->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!w.isLoading()&&w.renderReady(),10000);
    QSettings s(config,QSettings::IniFormat);
    QCOMPARE(s.value("documentKeys/futureCommand").toString(),QString("Ctrl+Alt+Q"));
    QCOMPARE(s.value(documentBookKey(path)+"size").toInt(),24);
    w.command("textSizeReset")->trigger();
    QTRY_VERIFY(!w.isLoading()&&w.renderReady()); s.sync();
    QVERIFY(!s.contains(documentBookKey(path)+"size"));
    auto *field=new QLineEdit(&w);field->setGeometry(20,40,240,30);field->show();
    const bool spread=w.command("spread")->isChecked();
    const int page=w.pageIndex();
    field->setFocus(); field->clear(); QTest::keyClick(field,Qt::Key_Space);
    QCOMPARE(field->text(),QString(" "));
    QCOMPARE(w.command("spread")->isChecked(),spread); QCOMPARE(w.pageIndex(),page);
    w.showCovers(); QVERIFY(w.command("next")->shortcut().isEmpty());
    QCOMPARE(w.bindings().value("next"),base.value("next"));
    w.close();
    MainWindow restored(config);
    QCOMPARE(restored.documentBindings(),doc);
    QCOMPARE(restored.bindings().value("next"),base.value("next"));
    ShortcutDialog dialog(base,nullptr,doc);
    auto *mode=dialog.findChild<QComboBox *>("documentKeyMode_next"); QVERIFY(mode);
    QCOMPARE(mode->currentIndex(),1); mode->setCurrentIndex(0);
    QVERIFY(!dialog.documentBindings().contains("next"));
    mode->setCurrentIndex(2);
    QVERIFY(dialog.documentBindings().contains("next"));
    QVERIFY(dialog.documentBindings().value("next").isEmpty());
    restored.close();
  }
  void markdownAndPlainAreDifferent() {
    QTemporaryDir d;
    const auto path = d.filePath("sample.md");
    QVERIFY(TestData::write(
        path, "# Heading\n\n**bold** and `code`\n\n|a|b|\n|-|-|\n|1|2|\n"));
    OpenOptions native;
    auto b = Book::open(path, {}, native);
    QVERIFY2(b.book, qPrintable(b.error));
    QVERIFY(isDocumentPath(path));
    QVERIFY(openFileFilter().contains("*.markdown"));
    const auto p = readPublication(path, {});
    QCOMPARE(p->toc.size(), 1);
    QCOMPARE(p->toc.first().title, QString("Heading"));
    QVERIFY(!p->sections.first().plain.contains("**bold**"));
    const auto image = b.book->preview(0);
    QVERIFY2(image.error.isEmpty(), qPrintable(image.error));
    OpenOptions o;
    o.text.contentMode = 1;
    const auto plain = Book::open(path, {}, o);
    QVERIFY(plain.book);
    QVERIFY(image.image != plain.book->preview(0).image);
  }
  void markdownHeadingTypographyAndPositions() {
    QTemporaryDir d;const auto path=d.filePath("headings.md");
    const QByteArray source="# **One**\n\n## Two\n\n### Three\n\n#### Four\n\n##### Five\n\n###### Six\n\nBody `code`\n\n```css\nfont-size:xx-large;\n```\n";
    QVERIFY(TestData::write(path,source));TextOptions options;options.pointSize=40;
    const auto publication=readPublication(path,options);QVERIFY(publication->error.isEmpty());QVERIFY(publication->markdown);
    QCOMPARE(publication->toc.size(),6);
    const auto &section=publication->sections.first();
    QVERIFY(section.html.contains("id=\"koma-markdown-headings\""));
    for(int level=1;level<=6;++level) {
      const auto entry=publication->toc[level-1];
      QVERIFY(section.html.contains(QString("<h%1 ").arg(level)));
      QVERIFY(section.html.contains(QString("data-koma-offset=\"%1\"").arg(entry.offset)));
      QVERIFY(section.plain.mid(entry.offset).startsWith(entry.title));
    }
    QVERIFY(section.plain.contains("font-size:xx-large;")); // Literal code must stay literal.
    options.pointSize=80;
    QCOMPARE(readPublication(path,options)->sections.first().html,section.html); // Relative typography, reusable at any reader size.
    options.contentMode=1;QVERIFY(!readPublication(path,options)->markdown);
    QVERIFY(!readPublication(path,options)->sections.first().html.contains("koma-markdown-headings"));
  }
  void markdownHeadingComputedSizes() {
    if(!webEngineIpcAvailable()) QSKIP("Chromium IPC is denied; run Markdown heading sizes on a desktop or CI.");
    QTemporaryDir d;const auto path=d.filePath("headings.md");
    QVERIFY(TestData::write(path,"# One\n\n## Two\n\n### Three\n\n#### Four\n\n##### Five\n\n###### Six\n\nBody\n"));
    const auto publication=readPublication(path,{});DocumentView view;view.resize(1200,900);view.show();
    for(int size:{40,80}) for(bool styles:{false,true}) {
      TextOptions options;options.pointSize=size;options.publisherStyles=styles;view.open(publication,options);
      QTRY_VERIFY_WITH_TIMEOUT(view.ready(),30000);
      auto *web=view.findChild<QWebEngineView *>();QVERIFY(web);bool done=false;QVariant result;
      web->page()->runJavaScript("(()=>{const d=document.querySelector('iframe').contentDocument;return {body:parseFloat(getComputedStyle(d.body).fontSize),heads:[...d.querySelectorAll('h1,h2,h3,h4,h5,h6')].map(h=>{const s=getComputedStyle(h.querySelector('span')||h);return {size:parseFloat(s.fontSize),line:parseFloat(s.lineHeight)};})};})()",
          QWebEngineScript::ApplicationWorld,[&](const QVariant &value){result=value;done=true;});
      QTRY_VERIFY(done);const auto data=result.toMap();QCOMPARE(data.value("body").toDouble(),double(size));
      const auto headings=data.value("heads").toList();QCOMPARE(headings.size(),6);double previous=3*size;
      for(const auto &heading:headings) {
        const auto values=heading.toMap();const double font=values.value("size").toDouble();
        QVERIFY(font>=size);QVERIFY(font<=previous);QVERIFY(values.value("line").toDouble()>=font);previous=font;
      }
      QCOMPARE(headings[0].toMap().value("size").toDouble(),2.0*size);
      QCOMPARE(headings[1].toMap().value("size").toDouble(),1.5*size);
    }
  }
  void explicitMarkdownAndRuby() {
    QTemporaryDir d;
    const auto path = d.filePath("text.txt");
    QVERIFY(TestData::write(path, "# Title\n\n｜漢字《かんじ》\n"));
    TextOptions o;
    o.contentMode = 2;
    auto p = readPublication(path, o);
    QCOMPARE(p->toc.size(), 1);
    o.contentMode = 1;
    p = readPublication(path, o);
    QVERIFY(
        p->sections.first().html.contains("<ruby>漢字<rt>かんじ</rt></ruby>"));
    QCOMPARE(p->sections.first().plain, QString("# Title\n\n漢字\n"));
    o.ruby = false;
    const auto a = renderDocumentPreview(path, {}, o);
    o.ruby = true;
    const auto b = renderDocumentPreview(path, {}, o);
    QVERIFY(a.error.isEmpty());
    QVERIFY(b.error.isEmpty());
  }
  void settingsScopesAndRetention() {
    QTemporaryDir d;
    QSettings s(d.filePath("settings.ini"), QSettings::IniFormat);
    const auto a = d.filePath("a.md"), b = d.filePath("b.md"),
               txt = d.filePath("a.txt");
    s.setValue("text/size", 25);
    s.setValue(documentFormatKey(a) + "size", 30);
    s.setValue(documentBookKey(a) + "size", 40);
    s.setValue(documentBookKey(a) + "publisherStyles", false);
    s.setValue(documentFormatKey(a) + "pageLayout", 1);
    QCOMPARE(documentOptions(s, a).value("size").toInt(), 40);
    QCOMPARE(documentOptions(s, b).value("size").toInt(), 30);
    QCOMPARE(documentOptions(s, txt).value("size").toInt(), 25);
    QCOMPARE(textOptions(documentOptions(s, a)).pageLayout, 1);
    QCOMPARE(textOptions(documentOptions(s, txt)).pageLayout, 0);
    auto values = documentOptions(s, a);
    saveDocumentOptions(s, documentBookKey(a), values,
                        documentOptions(s, a, false));
    QVERIFY(!s.value(documentBookKey(a) + "publisherStyles").toBool());
    QCOMPARE(textOptions(documentOptions(s, a)).pageLayout, 1);
    QVERIFY(!documentOptions(s, a).contains("renderer"));
    values["size"] = 30;
    saveDocumentOptions(s, documentBookKey(a), values,
                        documentOptions(s, a, false));
    QVERIFY(!s.contains(documentBookKey(a) + "size"));
    values["size"] = -100;
    values["margin"] = 9999;
    values["background"] = "bad color";
    values["pageLayout"] = 500;
    const auto clamped = textOptions(values);
    QCOMPARE(clamped.pointSize, 8);
    QCOMPARE(clamped.margin, 300);
    QVERIFY(clamped.background.isValid());
    QCOMPARE(clamped.pageLayout, 1);
  }
  void documentSpreadNavigation() {
    QVERIFY(documentVisiblePages(0, 0, 1).isEmpty());
    QVERIFY(documentVisiblePages(3, -1, 1).isEmpty());
    QVERIFY(documentVisiblePages(3, 3, 1).isEmpty());
    QCOMPARE(advanceDocumentPage(0, 0, 1, 1), 0);
    for (int count = 1; count < 10; ++count) {
      for (int layout = 0; layout < 2; ++layout) {
        const int step = layout + 1;
        for (int anchor = 0; anchor < count; ++anchor) {
          const auto visible = documentVisiblePages(count, anchor, layout);
          QCOMPARE(visible.size(), std::min(step, count - anchor));
          for (int i = 0; i < visible.size(); ++i)
            QCOMPARE(visible[i], anchor + i);
          const int next = advanceDocumentPage(count, anchor, 1, layout);
          // A normal turn starts after the last visible page, or stays at EOF.
          QCOMPARE(next, visible.last() + 1 < count ? visible.last() + 1 : anchor);
          QCOMPARE(advanceDocumentPage(count, anchor, -1, layout), std::max(0, anchor - step));
          QCOMPARE(advanceDocumentPage(count, anchor, 1, layout, true), std::min(count - 1, anchor + 1));
        }
      }
    }
  }
  void pathsAndResources() {
    QCOMPARE(publicationPath("OPS/book.opf", "../images/a.png"),
             QString("images/a.png"));
    for (const auto &s :
         QStringList{"../../secret", "/etc/passwd", "file:///tmp/a",
                     "https://example.com/a", "%2e%2e/a", "a\\b", "a%00b"})
      QVERIFY2(publicationPath({}, s).isEmpty(), qPrintable(s));
    QTemporaryDir d;
    QVERIFY(
        TestData::write(d.filePath("a.png"), TestData::png(TestData::solid())));
    QVERIFY(
        !documentResource(d.path(), QUrl("a.png")).value<QImage>().isNull());
    QVERIFY(!documentResource(d.path(), QUrl("https://example.com/a.png"))
                 .isValid());
    QVERIFY(!documentResource(d.path(), QUrl("../a.png")).isValid());
    QTemporaryDir outside;
    QVERIFY(TestData::write(outside.filePath("a.png"),
                            TestData::png(TestData::solid())));
    if (QFile::link(outside.filePath("a.png"), d.filePath("escape.png")))
      QVERIFY(!documentResource(d.path(), QUrl("escape.png")).isValid());
  }
  void epubSpineAndToc() {
    QTemporaryDir d;
    const auto path = d.filePath("book.epub");
    QVERIFY(TestData::write(path, epub()));
    const auto p = readPublication(path, {});
    QVERIFY2(p->error.isEmpty(), qPrintable(p->error));
    QCOMPARE(p->sections.size(), 3);
    QVERIFY(p->sections[2].auxiliary);
    QCOMPARE(p->sections[0].path, QString("OPS/z.xhtml"));
    QVERIFY(p->rightToLeft);
    QVERIFY(!p->sections[0].fixed);
    QVERIFY(p->sections[1].fixed);
    QCOMPARE(p->sections[1].viewport, QSize(600, 800));
    QCOMPARE(p->toc.size(), 3);
    QCOMPARE(p->toc[1].fragment, QString("end"));
    const auto standard = Book::open(path);
    QVERIFY2(standard.book, qPrintable(standard.error));
    QVERIFY(standard.book->publication);
    QCOMPARE(standard.book->pages.size(),2);
  }
  void malformedEpub() {
    QTemporaryDir d;
    const auto path = d.filePath("bad.epub");
    QVERIFY(TestData::write(path, epub("forbidden")));
    QVERIFY(!readPublication(path, {})->error.isEmpty());
    QVERIFY(TestData::write(path, TestData::zip({{"mimetype", "wrong"}})));
    QVERIFY(!readPublication(path, {})->error.isEmpty());
    QVERIFY(TestData::write(path, "not zip"));
    QVERIFY(!readPublication(path, {})->error.isEmpty());
  }
  void epubXhtmlScriptsAndEmptyElements() {
    QTemporaryDir d;
    const auto path = d.filePath("book.epub");
    QVERIFY(TestData::write(path, epub()));
    const auto p = readPublication(path, {});
    QVERIFY2(p->error.isEmpty(), qPrintable(p->error));
    const auto html = p->sections.first().html;
    QVERIFY(!html.contains("<script"));
    QVERIFY(!html.contains("headScript"));
    QVERIFY(!html.contains("untrustedRan"));
    QVERIFY(html.contains("<title>日本語😀</title>"));
    QVERIFY(html.contains("<span id=\"empty\"></span>"));
    QVERIFY(html.contains("<div id=\"empty-block\"></div>"));
    QVERIFY(html.contains("body > p{color:black}"));
    QVERIFY(html.contains("<p id=\"end\">終わり</p>"));
    QTextDocument parsed;
    parsed.setHtml(html);
    QVERIFY(parsed.toPlainText().contains("本文599"));
    QVERIFY(parsed.toPlainText().contains("終わり"));
  }
  void documentDefaultAndLegacySettings() {
    QTemporaryDir d;
    QSettings settings(d.filePath("settings.ini"), QSettings::IniFormat);
    const auto txt = d.filePath("book.txt"), md = d.filePath("book.md"), ep = d.filePath("book.epub");
    QVERIFY(!documentOptionDefaults().contains("renderer"));
    settings.setValue("text/renderer", 0);
    settings.setValue(documentFormatKey(md) + "renderer", 1);
    settings.setValue(documentBookKey(md) + "renderer", 0);
    settings.setValue(documentBookKey(md) + "location", QVariantMap{{"section", "text.html"}, {"offset", 123}});
    settings.setValue(documentBookKey(md) + "font", "Serif");
    for (const auto &path : {txt, md, ep}) {
      QVERIFY(!documentOptions(settings, path).contains("renderer"));
      auto values = documentOptions(settings, path); values["renderer"] = 0;
      saveDocumentOptions(settings, documentBookKey(path), values, documentOptions(settings, path, false));
    }
    QCOMPARE(settings.value("text/renderer").toInt(), 0); // Inert legacy data is preserved.
    QCOMPARE(settings.value(documentBookKey(md)+"renderer").toInt(), 0);
    QCOMPARE(settings.value(documentBookKey(md)+"location").toMap().value("offset").toInt(), 123);
    QCOMPARE(documentOptions(settings, md).value("font").toString(), QString("Serif"));
  }

  void legacyRendererCannotSelectAnotherReader() {
    QTemporaryDir d; const auto config = d.filePath("settings.ini");
    QSettings settings(config, QSettings::IniFormat);
    settings.setValue("text/renderer",0);
    settings.setValue("documents/autoOpenEpubExtended",false);
    const auto txt=d.filePath("book.txt"), md=d.filePath("book.md"), ep=d.filePath("book.epub");
    QVERIFY(TestData::write(txt,"Readable text"));
    QVERIFY(TestData::write(md,"# Heading\n\nBody"));
    QVERIFY(TestData::write(ep,epub()));
    for (const auto &path : {txt,md,ep}) {
      settings.setValue(documentFormatKey(path)+"renderer",0);
      settings.setValue(documentBookKey(path)+"renderer",0);
      OpenOptions options; options.text=textOptions(documentOptions(settings,path));
      const auto opened=Book::open(path,{},options);
      QVERIFY2(opened.book,qPrintable(opened.error)); QVERIFY(opened.book->publication);
      QVERIFY(!opened.book->image(0).error.isEmpty());
      if (path != ep) QVERIFY(!opened.book->preview(0).image.isNull());
    }
    settings.sync();
    MainWindow w(config); w.show();
    for(const auto &id:{"documentInherited","documentExtended","documentStandard"}) QVERIFY(!w.command(id));
    bool checked=false;
    QTimer::singleShot(0,&w,[&] {
      auto *dialog=w.findChild<QDialog *>("documentSettingsDialog"); QVERIFY(dialog);
      QVERIFY(!dialog->findChild<QComboBox *>("document_renderer"));
      QVERIFY(!dialog->findChild<QCheckBox *>("epubAutoExtended"));
      auto *layout=dialog->findChild<QComboBox *>("document_pageLayout");
      QVERIFY(layout && layout->isVisible());
      QVERIFY(dialog->findChild<QComboBox *>("document_contentMode"));
      checked=true; dialog->reject();
    });
    w.command("documentSettings")->trigger(); QVERIFY(checked);
    const auto broken=d.filePath("broken.epub"); QVERIFY(TestData::write(broken,"not a zip"));
    w.openBook(broken); QTRY_VERIFY(!w.isLoading());
    QCOMPARE(w.lastError(),readPublication(broken,{})->error);
    QCOMPARE(settings.value("text/renderer").toInt(),0);
    w.close();
  }

  void documentWindowAndSettings() {
    if (!webEngineIpcAvailable()) QSKIP("Chromium IPC is denied; run document settings and markers on a desktop or CI.");
    QTemporaryDir d;
    const auto path = d.filePath("sample.md");
    QByteArray text = "# A heading\n\n";
    for (int i = 0; i < 500; ++i)
      text += "A paragraph for reading and reflow.\n\n";
    QVERIFY(TestData::write(path, text));
    const auto config = d.filePath("settings.ini");
    { QSettings settings(config, QSettings::IniFormat); settings.setValue("text/renderer", 0); }
    MainWindow w(config);
    w.show();
    w.openBook(path);
    QTRY_VERIFY_WITH_TIMEOUT(w.renderReady(), 10000);
    QVERIFY(w.pageCount() > 2);
    w.nextPage();
    QTRY_VERIFY(w.renderReady());
    QVERIFY(w.pageIndex() > 0);
    auto *initialView = w.findChild<DocumentView *>(); QVERIFY(initialView);
    QTRY_VERIFY(initialView->location().offset > 0);
    auto *toc = w.findChild<QListWidget *>("tocList");
    QVERIFY(toc);
    QCOMPARE(toc->count(), 1);
    w.command("marker")->trigger();
    QTimer::singleShot(0, &w, [&] {
      auto *dialog = w.findChild<QInputDialog *>();
      QVERIFY(dialog);
      dialog->setTextValue("User bookmark");
      dialog->accept();
    });
    w.command("addToc")->trigger();
    QCOMPARE(toc->count(), 2);
    QCOMPARE(toc->item(0)->text(), QString("User bookmark"));
    QString selectedFamily;
    QTimer::singleShot(0, &w, [&] {
      auto *dialog = w.findChild<QDialog *>("documentSettingsDialog");
      QVERIFY(dialog);
      auto *size = dialog->findChild<QSpinBox *>("document_size");
      QVERIFY(size);
      size->setValue(35);
      auto *font = dialog->findChild<QFontComboBox *>("document_font");
      QVERIFY(font);
      QVERIFY(font->currentText().isEmpty());
      QVERIFY(font->count() > 0);
      font->setCurrentIndex(0);
      selectedFamily = font->currentText();
      QVERIFY(!selectedFamily.isEmpty());
      auto *pages = dialog->findChild<QComboBox *>("document_pageLayout");
      QVERIFY(pages);
      if (pages) pages->setCurrentIndex(1);
      auto *buttons = dialog->findChild<QDialogButtonBox *>();
      buttons->button(QDialogButtonBox::Ok)->click();
    });
    w.command("documentSettings")->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!w.isLoading() && w.renderReady(), 10000);
    auto *reflowedView = w.findChild<DocumentView *>(); QVERIFY(reflowedView);
    QTRY_VERIFY(!reflowedView->location().section.isEmpty());
    const int reflowOffset = reflowedView->location().offset;
    const int reflowPage = w.pageIndex();
    w.command("first")->trigger();
    QTRY_COMPARE(reflowedView->location().offset, 0);
    w.command("gotoMarker")->trigger();
    QTRY_COMPARE(w.pageIndex(), reflowPage);
    QTRY_COMPARE(reflowedView->location().offset, reflowOffset);
    QCOMPARE(toc->count(), 2);
    QCOMPARE(toc->item(0)->text(), QString("User bookmark"));

    QSettings s(config, QSettings::IniFormat);
    QCOMPARE(s.value(documentBookKey(path) + "size").toInt(), 35);
    QCOMPARE(s.value(documentBookKey(path) + "font").toString(), selectedFamily);
    QCOMPARE(s.value(documentBookKey(path) + "pageLayout", 0).toInt(),
             1);
    QVERIFY(s.value(documentBookKey(path) + "location")
                .toMap()
                .contains("section"));
    QTimer::singleShot(0, &w, [&] {
      auto *dialog = w.findChild<QDialog *>("documentSettingsDialog");
      QVERIFY(dialog);
      auto *font = dialog->findChild<QFontComboBox *>("document_font");
      QVERIFY(font);
      QCOMPARE(font->currentText(), selectedFamily);
      auto *scope = dialog->findChild<QComboBox *>("documentSettingsScope");
      QVERIFY(scope);
      scope->setCurrentIndex(1);
      QVERIFY(font->currentText().isEmpty());
      scope->setCurrentIndex(0);
      QCOMPARE(font->currentText(), selectedFamily);
      dialog->reject();
    });
    w.command("documentSettings")->trigger();
    w.close();
  }
  void documentPanelGeometry() {
    QFile script(":/documents/panels.js");
    QVERIFY(script.open(QIODevice::ReadOnly));
    QJSEngine engine;
    engine.globalObject().setProperty("window", engine.globalObject());
    const auto result = engine.evaluate(QString::fromUtf8(script.readAll()));
    QVERIFY2(!result.isError(), qPrintable(result.toString()));
    auto panels = engine.globalObject().property("komaPanels");
    auto plan = panels.property("plan");
    auto geometry = panels.property("geometry");
    QVERIFY(plan.isCallable());
    QVERIFY(geometry.isCallable());
    for (int mask=0;mask<64;++mask) {
      QSet<int> wide;
      auto flags=engine.newArray(6);
      for(int i=0;i<6;++i) {flags.setProperty(i,bool(mask&(1<<i)));if(mask&(1<<i))wide.insert(i);}
      for(int anchor=0;anchor<6;++anchor)
        for(bool rtl:{false,true}) {
          const auto expected=documentVisiblePages(6,anchor,1,wide);
          const auto actual=plan.call({anchor,6,1,rtl,flags});
          QCOMPARE(actual.property("length").toInt(),expected.size());
          for(int n=0;n<expected.size();++n) QCOMPARE(actual.property(n).property("page").toInt(),expected[n]);
          if(wide.contains(anchor)) QVERIFY(actual.property(0).property("wide").toBool());
        }
    }
    const auto full=geometry.call({1600,900,1,0,true});
    const auto half=geometry.call({1600,900,1,0,false});
    QVERIFY(full.property("width").toNumber()>half.property("width").toNumber()*2);
    for (int count = 1; count < 10; ++count)
      for (int anchor = 0; anchor < count; ++anchor)
        for (int layout = 0; layout < 2; ++layout)
          for (bool rtl : {false, true}) {
            const auto visible = documentVisiblePages(count, anchor, layout);
            const auto planned = plan.call({anchor, count, layout, rtl});
            QCOMPARE(planned.property("length").toInt(), visible.size());
            for (int n = 0; n < visible.size(); ++n) {
              QCOMPARE(planned.property(n).property("page").toInt(), visible[n]);
              QCOMPARE(planned.property(n).property("slot").toInt(), rtl ? layout - n : n);
            }
          }
    for (const auto &size : {QSize(1920, 1080), QSize(800, 1200), QSize(2400, 900)})
      for (int layout = 0; layout < 2; ++layout) {
        double previousRight = -1;
        for (int slot = 0; slot <= layout; ++slot) {
          const auto g = geometry.call({size.width(), size.height(), layout, slot});
          const double left = g.property("left").toNumber();
          const double right = left + g.property("width").toNumber();
          const double bottom = g.property("top").toNumber() + g.property("height").toNumber();
          QVERIFY(left > previousRight);
          QVERIFY(right <= size.width() + .001);
          QVERIFY(bottom <= size.height() + .001);
          previousRight = right;
        }
      }
  }
  void fixedViewportGeometry() {
    QFile script(":/documents/panels.js");
    QVERIFY(script.open(QIODevice::ReadOnly));
    QJSEngine engine;
    engine.globalObject().setProperty("window", engine.globalObject());
    QVERIFY(!engine.evaluate(QString::fromUtf8(script.readAll())).isError());
    auto panels = engine.globalObject().property("komaPanels");
    auto geometry = panels.property("geometry"), content = panels.property("contentGeometry");
    QVERIFY(content.isCallable());
    for (const auto &window : {QSize(1600,900), QSize(800,1200)})
      for (int layout : {0,1})
        for (int slot = 0; slot <= layout; ++slot)
          for (const auto &viewport : {QSize(480,720), QSize(1200,800), QSize(1200,1600)}) {
            const auto panel = geometry.call({window.width(),window.height(),layout,slot});
            const auto fitted = content.call({panel,viewport.width(),viewport.height()});
            const auto x = fitted.property("left").toNumber(), y = fitted.property("top").toNumber();
            const auto w = fitted.property("width").toNumber(), h = fitted.property("height").toNumber();
            const auto px = panel.property("left").toNumber(), py = panel.property("top").toNumber();
            const auto pw = panel.property("width").toNumber(), ph = panel.property("height").toNumber();
            QVERIFY(x >= px - .001); QVERIFY(y >= py - .001);
            QVERIFY(x + w <= px + pw + .001); QVERIFY(y + h <= py + ph + .001);
            QVERIFY(std::abs(w / h - double(viewport.width()) / viewport.height()) < .0001);
            QVERIFY(std::abs((x + w/2) - (px + pw/2)) < .001);
            QVERIFY(std::abs((y + h/2) - (py + ph/2)) < .001);
          }
  }
  void fixedViewportRendering_data() {
    QTest::addColumn<bool>("vertical");
    QTest::newRow("vertical-rl") << true;
    QTest::newRow("horizontal-tb") << false;
  }
  void fixedViewportLimit() {
    if (!webEngineIpcAvailable()) QSKIP("Chromium IPC is denied; run fixed viewport bounds on a desktop or CI.");
    for (const auto &size : {QSize(16385,720),QSize(1200,16385),QSize(8000,8000)}) {
      auto publication=std::make_shared<Publication>(); publication->epub=true;
      PublicationSection section; section.path="page.xhtml";section.fixed=true;section.viewport=size;
      section.html="<html><head></head><body>Oversized fixed viewport</body></html>";
      publication->sections.append(section);
      DocumentView view; QString error;
      connect(&view,&DocumentView::failed,this,[&](const QString &value){error=value;});
      view.open(publication,{});
      QTRY_VERIFY_WITH_TIMEOUT(!error.isEmpty(),30000);
      QVERIFY(!view.ready());
      QCOMPARE(error,DocumentView::readerMessages().value("rangeLimit").toString());
    }
  }
  void fixedViewportRendering() {
    if (!webEngineIpcAvailable()) QSKIP("Chromium IPC is denied; run fixed viewport rendering on a desktop or CI.");
    QFETCH(bool, vertical);
    auto publication = std::make_shared<Publication>();
    publication->epub = true;
    publication->rightToLeft = true;
    publication->resources["cover.png"] = TestData::png(TestData::solid(Qt::green, {480,720}));
    // Same viewport, vertical flow, paragraph and inline image as C16. A larger
    // iframe with a scaled body clips this content before the outer page is fit.
    for (int n = 1; n <= 3; ++n) {
      PublicationSection section;
      section.path = QString("c%1.xhtml").arg(n);
      section.fixed = true;
      section.viewport = {480,720};
      section.plain = QString("固定ページ %1").arg(n);
      section.html = QString("<html><head><meta name=\"viewport\" content=\"width=480,height=720\"/>"
        "<style>body{writing-mode:%1}img{max-width:100%}</style></head><body>"
        "<p>固定ページ %2</p><img src=\"cover.png\" width=\"400\" height=\"600\"/></body></html>")
        .arg(vertical ? "vertical-rl" : "horizontal-tb").arg(n);
      publication->sections.append(section);
    }
    DocumentView view; view.resize(1200,900); view.show();
    int pageCount = 0;
    connect(&view,&DocumentView::laidOut,this,[&](const QVector<DocumentLocation> &locations){pageCount=locations.size();});
    QVariantList original;
    for (int pass = 0; pass < 3; ++pass) {
      TextOptions options; options.pointSize = pass ? 80 : 20;
      options.pageLayout = pass == 2 ? 1 : 0;
      options.family = pass ? "monospace" : "serif";
      options.writingMode = pass ? 1 : 2; // Fixed pages retain the author's mode.
      view.open(publication,options);
      QTRY_VERIFY_WITH_TIMEOUT(view.ready(),30000);
      QCOMPARE(pageCount,3);
      auto *web = view.findChild<QWebEngineView *>(); QVERIFY(web);
      bool done = false; QVariant result;
      web->page()->runJavaScript(R"JS((()=>{
        const rect=r=>({x:r.x,y:r.y,width:r.width,height:r.height});
        return [...document.querySelectorAll('iframe')].map(f=>{
          const d=f.contentDocument, range=d.createRange();range.selectNodeContents(d.querySelector('p'));
          return {width:f.contentWindow.innerWidth,height:f.contentWindow.innerHeight,
            mode:getComputedStyle(d.body).writingMode,font:getComputedStyle(d.body).fontSize,
            image:rect(d.querySelector('img').getBoundingClientRect()),text:rect(range.getBoundingClientRect())};
        });})())JS", QWebEngineScript::ApplicationWorld,[&](const QVariant &v){result=v;done=true;});
      QTRY_VERIFY(done);
      const auto frames=result.toList(); QCOMPARE(frames.size(),3);
      for (const auto &frame : frames) {
        const auto data=frame.toMap();
        QCOMPARE(data.value("width").toInt(),480); QCOMPARE(data.value("height").toInt(),720);
        QCOMPARE(data.value("mode").toString(),vertical ? QString("vertical-rl") : QString("horizontal-tb"));
        for (const auto &key : {"image","text"}) {
          const auto rect=data.value(key).toMap();
          QVERIFY2(rect.value("width").toDouble()>0,key); QVERIFY(rect.value("height").toDouble()>0);
          QVERIFY2(rect.value("x").toDouble()>=-.5,key); QVERIFY(rect.value("y").toDouble()>=-.5);
          QVERIFY2(rect.value("x").toDouble()+rect.value("width").toDouble()<=480.5,key);
          QVERIFY(rect.value("y").toDouble()+rect.value("height").toDouble()<=720.5);
        }
      }
      if (!pass) original=frames; else QCOMPARE(frames,original);
      for (int page : {2,1,0}) {
        view.jump(page);
        QTRY_COMPARE(view.location().section,QString("c%1.xhtml").arg(page+1));
      }
    }
  }
  void extendedSpreadRendering_data() {
    QTest::addColumn<bool>("vertical");
    QTest::addColumn<bool>("rightToLeft");
    QTest::newRow("vertical-rtl") << true << true;
    QTest::newRow("horizontal-ltr") << false << false;
  }
  void extendedWideImagesAndSharedSpace() {
    if (!webEngineIpcAvailable()) QSKIP("Chromium IPC is denied; run wide image rendering on a desktop or CI.");
    QTemporaryDir d;
    const auto path=d.filePath("image.epub"), config=d.filePath("settings.ini");
    QVERIFY(TestData::write(path,imageBook()));
    { QSettings s(config,QSettings::IniFormat);s.setValue(documentBookKey(path)+"renderer",1);s.setValue(documentBookKey(path)+"pageLayout",1);s.setValue("reading/spread",false); }
    MainWindow w(config);w.resize(1600,900);w.show();w.openBook(path);
    QTRY_VERIFY_WITH_TIMEOUT(w.renderReady()||!w.lastError().isEmpty(),40000);
    QVERIFY2(w.renderReady(),qPrintable(w.lastError()));
    QCOMPARE(w.pageIndex(),0);
    w.nextPage();QTRY_COMPARE(w.pageIndex(),1);
    auto *view=w.findChild<DocumentView *>();QVERIFY(view);
    auto *web=view->findChild<QWebEngineView *>();QVERIFY(web);
    bool done=false;QVariant state;
    web->page()->runJavaScript(
      "(()=>{const panes=[...document.querySelectorAll('iframe')].filter(e=>e.style.visibility==='visible');return {count:panes.length,width:panes[0].offsetWidth,image:panes[0].contentDocument.querySelector('img').naturalWidth};})()",
      QWebEngineScript::ApplicationWorld,[&](const QVariant &v){state=v;done=true;});
    QTRY_VERIFY(done);
    QCOMPARE(state.toMap().value("count").toInt(),1);
    QCOMPARE(state.toMap().value("width").toInt(),2424);
    QCOMPARE(state.toMap().value("image").toInt(),1536);
    w.nextPage();QTRY_COMPARE(w.pageIndex(),2);
    w.previousPage();QTRY_COMPARE(w.pageIndex(),1);
    QTRY_VERIFY(w.command("spread")->isEnabled());
    // The key press recreates the view; do not send a release to its old widget.
    web->setFocus();QTest::keyPress(web,Qt::Key_Space);
    QTRY_VERIFY_WITH_TIMEOUT(!w.isLoading()&&w.renderReady(),40000);
    QVERIFY(!w.command("spread")->isChecked());
    view=w.findChild<DocumentView *>();QVERIFY(view);
    web=view->findChild<QWebEngineView *>();QVERIFY(web);
    QTest::keyRelease(web,Qt::Key_Space);
    QCOMPARE(w.pageIndex(),1);
    QSettings s(config,QSettings::IniFormat);
    QCOMPARE(s.value(documentBookKey(path)+"pageLayout").toInt(),0);
    QVERIFY(!s.value("reading/spread").toBool());
    QTimer::singleShot(0,&w,[&]{
      auto *dialog=w.findChild<QDialog *>("documentSettingsDialog");QVERIFY(dialog);
      auto *wide=dialog->findChild<QCheckBox *>("document_wideImages");QVERIFY(wide);wide->setChecked(false);
      auto *css=dialog->findChild<QCheckBox *>("document_publisherStyles");QVERIFY(css);css->setChecked(false);
      dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    w.command("documentSettings")->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!w.isLoading()&&w.renderReady(),40000);
    QCOMPARE(w.pageIndex(),1);
    view=w.findChild<DocumentView *>();QVERIFY(view);web=view->findChild<QWebEngineView *>();QVERIFY(web);
    done=false;
    web->page()->runJavaScript("(()=>{const f=[...document.querySelectorAll('iframe')].find(e=>e.style.visibility==='visible');return {width:f.offsetWidth,fit:getComputedStyle(f.contentDocument.querySelector('img')).objectFit};})()",
      QWebEngineScript::ApplicationWorld,[&](const QVariant &v){state=v;done=true;});
    QTRY_VERIFY(done);QCOMPARE(state.toMap().value("width").toInt(),1200);QCOMPARE(state.toMap().value("fit").toString(),QString("contain"));
    w.previousPage();QTRY_COMPARE(w.pageIndex(),0);
    w.close();
  }
  void extendedSpreadRendering() {
    if (!webEngineIpcAvailable())
      QSKIP("Chromium IPC is denied; run spread rendering on a desktop or CI.");
    QFETCH(bool, vertical);
    QFETCH(bool, rightToLeft);
    QTemporaryDir d;
    const auto path = d.filePath("book.epub"), config = d.filePath("settings.ini");
    QVERIFY(TestData::write(path, epub({}, vertical, rightToLeft)));
    {
      QSettings s(config, QSettings::IniFormat);
      s.setValue(documentBookKey(path) + "renderer", 1);
      s.setValue(documentBookKey(path) + "pageLayout", 1);
    }
    MainWindow w(config);
    w.resize(1500, 900);
    w.show();
    w.openBook(path);
    QTRY_VERIFY_WITH_TIMEOUT(w.renderReady() || !w.lastError().isEmpty(), 40000);
    QVERIFY2(w.renderReady(), qPrintable(w.lastError()));
    auto *view = w.findChild<DocumentView *>();
    QVERIFY(view);
    auto *web = view->findChild<QWebEngineView *>();
    QVERIFY(web);
    QVariant state;
    bool done = false;
    const auto inspect = [&] {
      done = false;
      web->page()->runJavaScript(
          "(()=>{const visible=[...document.querySelectorAll('iframe')]"
          ".filter(e=>e.style.visibility==='visible');"
          "return {current:window.komaDocument.state(false).current,"
          "panes:visible.map(e=>({left:e.getBoundingClientRect().left,"
          "width:e.getBoundingClientRect().width,path:e.src,"
          "scroll:e.contentWindow.scrollX+e.contentWindow.scrollY}))};})()",
          QWebEngineScript::ApplicationWorld, [&](const QVariant &value) {
            state = value;
            done = true;
          });
    };
    inspect();
    QTRY_VERIFY(done);
    auto panes = state.toMap().value("panes").toList();
    QCOMPARE(panes.size(), 2);
    QCOMPARE(panes[0].toMap().value("path"), panes[1].toMap().value("path"));
    const auto first = panes[0].toMap(), second = panes[1].toMap();
    QVERIFY(first.value("scroll").toDouble() < second.value("scroll").toDouble());
    QCOMPARE(first.value("left").toDouble() > second.value("left").toDouble(), rightToLeft);
    const double separation = std::abs(first.value("left").toDouble() - second.value("left").toDouble());
    QVERIFY(separation > first.value("width").toDouble());
    const int count = w.pageCount();
    w.nextPage();
    QTRY_COMPARE(w.pageIndex(), 2);
    inspect();
    QTRY_VERIFY(done);
    QCOMPARE(state.toMap().value("current").toInt(), 2);

    w.command("last")->trigger();
    QTRY_COMPARE(w.pageIndex(), count - 1);
    inspect();
    QTRY_VERIFY(done);
    QCOMPARE(state.toMap().value("panes").toList().size(), 1);
    view->jump(count - 2); // A spread spanning a reflow/fixed chapter boundary.
    QTRY_COMPARE(w.pageIndex(), count - 2);
    inspect();
    QTRY_VERIFY(done);
    panes = state.toMap().value("panes").toList();
    QCOMPARE(panes.size(), 2);
    QVERIFY(panes[0].toMap().value("path") != panes[1].toMap().value("path"));

    QTimer::singleShot(0, &w, [&] {
      auto *dialog = w.findChild<QDialog *>("documentSettingsDialog");
      QVERIFY(dialog);
      auto *layout = dialog->findChild<QComboBox *>("document_pageLayout");
      QVERIFY(layout);
      QCOMPARE(layout->currentIndex(), 1);
      layout->setCurrentIndex(0);
      dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    w.command("documentSettings")->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!w.isLoading() && w.renderReady(), 40000);
    QCOMPARE(w.pageCount(), count);
    QCOMPARE(w.pageIndex(), count - 2);
    view = w.findChild<DocumentView *>();
    QVERIFY(view);
    web = view->findChild<QWebEngineView *>();
    inspect();
    QTRY_VERIFY(done);
    QCOMPARE(state.toMap().value("panes").toList().size(), 1);
    w.close();
  }
  void extendedEpubRenderingAndLinks() {
    if (!webEngineIpcAvailable())
      QSKIP("Chromium IPC socket shutdown is denied by the execution sandbox; "
            "run this case on a normal desktop or CI.");
    QTemporaryDir d;
    const auto path = d.filePath("book.epub"),
               config = d.filePath("settings.ini");
    QVERIFY(TestData::write(path, epub()));
    {
      QSettings s(config, QSettings::IniFormat);
      s.setValue(documentBookKey(path) + "renderer", 1);
      s.setValue(documentBookKey(path) + "font", "monospace");
    }
    MainWindow w(config);
    w.show();
    w.openBook(path);
    QTRY_VERIFY_WITH_TIMEOUT(w.renderReady() || !w.lastError().isEmpty(),
                             40000);
    QVERIFY2(w.renderReady(), qPrintable(w.lastError()));
    QVERIFY(w.pageCount() > 3);
    auto *view = w.findChild<DocumentView *>();
    QVERIFY(view);
    QVERIFY(view->ready());
    auto *web = view->findChild<QWebEngineView *>();
    QVERIFY(web);
    QVariant result;
    bool done = false;
    web->page()->runJavaScript(
        "({scripts:!!document.querySelector('iframe').contentWindow."
        "untrustedRan,ruby:document.querySelector('iframe').contentDocument."
        "querySelectorAll('ruby').length,"
        "family:getComputedStyle(document.querySelector('iframe').contentDocument."
        "querySelector('p')).fontFamily})",
        QWebEngineScript::ApplicationWorld, [&](const QVariant &v) {
          result = v;
          done = true;
        });
    QTRY_VERIFY(done);
    QVERIFY(!result.toMap().value("scripts").toBool());
    QCOMPARE(result.toMap().value("ruby").toInt(), 600);
    QVERIFY(result.toMap().value("family").toString().contains("monospace"));
    auto *toc = w.findChild<QListWidget *>("tocList");
    QVERIFY(toc);
    QCOMPARE(toc->count(), 3);
    view->navigate("OPS/z.xhtml", "end", 0);
    QTRY_VERIFY(w.pageIndex() > 0);
    view->navigate("OPS/a.xhtml", "picture", 0);
    QTRY_COMPARE(w.pageIndex(), w.pageCount() - 1);
    w.previousPage();
    QTRY_VERIFY(w.pageIndex() < w.pageCount() - 1);
    w.close();
  }
  void defaultStandardDocuments_data() {
    QTest::addColumn<QString>("extension");
    QTest::newRow("txt") << "txt";
    QTest::newRow("markdown") << "md";
    QTest::newRow("epub") << "epub";
  }
  void defaultStandardDocuments() {
    if (!webEngineIpcAvailable())
      QSKIP("Chromium IPC is denied; verify default standard rendering on a desktop or CI.");
    QFETCH(QString, extension);
    QTemporaryDir d;
    const auto path = d.filePath("default." + extension);
    QVERIFY(TestData::write(path, extension == "epub" ? epub() : QByteArray("# Heading\n\nA default document.\n")));
    MainWindow w(d.filePath("settings.ini")); w.show();
    w.openBook(path);
    QTRY_VERIFY_WITH_TIMEOUT(w.renderReady() || !w.lastError().isEmpty(), 40000);
    QVERIFY2(w.renderReady(), qPrintable(w.lastError()));
    auto *view = w.findChild<DocumentView *>(); QVERIFY(view); QVERIFY(view->ready());
    w.close();
  }
  void documentReflowAndResume() {
    if (!webEngineIpcAvailable()) QSKIP("Chromium IPC is denied; run document reflow/resume on a desktop or CI.");
    QTemporaryDir d; const auto path=d.filePath("book.txt"), config=d.filePath("settings.ini");
    QByteArray text; for(int i=0;i<800;++i) text+=QString("Line %1: reading text and restoring position.\n").arg(i).toUtf8();
    QVERIFY(TestData::write(path,text));
    { QSettings old(config,QSettings::IniFormat);old.setValue("text/renderer",0); }
    DocumentLocation reopenedLocation;
    {
      MainWindow w(config);w.show();w.openBook(path);
      QTRY_VERIFY_WITH_TIMEOUT(w.renderReady(),40000);
      auto *view=w.findChild<DocumentView *>();QVERIFY(view);
      w.nextPage();QTRY_VERIFY(view->location().offset>0);
      const auto before=view->location();
      w.command("textSizeIncrease")->trigger();
      QTRY_VERIFY_WITH_TIMEOUT(!w.isLoading() && w.renderReady(),40000);
      auto *reflowed=w.findChild<DocumentView *>();QVERIFY(reflowed);
      QTRY_COMPARE(reflowed->location().section,before.section);
      auto *web=reflowed->findChild<QWebEngineView *>();QVERIFY(web);
      bool done=false;QVariantMap state;
      web->page()->runJavaScript("window.komaDocument.state(true)",QWebEngineScript::ApplicationWorld,
          [&](const QVariant &value){state=value.toMap();done=true;});
      QTRY_VERIFY(done);const auto pages=state.value("pages").toList();
      const int current=state.contains("current")?state.value("current").toInt():-1;
      QVERIFY(current>=0 && current<pages.size());QCOMPARE(w.pageIndex(),current);
      const int start=pages[current].toMap().value("offset").toInt();
      const int end=current+1<pages.size()?pages[current+1].toMap().value("offset").toInt():int(text.size());
      QVERIFY2(start<=before.offset && before.offset<end,qPrintable(QString("Expected %1 in [%2,%3)").arg(before.offset).arg(start).arg(end)));
      reopenedLocation=reflowed->location();w.close();
    }
    MainWindow restored(config);restored.show();
    restored.runRequest(QJsonObject{{"path",path},{"entry",0}}); // Resume through the history/--last route.
    QTRY_VERIFY_WITH_TIMEOUT(restored.renderReady(),40000);
    auto *view=restored.findChild<DocumentView *>();QVERIFY(view);
    QTRY_COMPARE(view->location().section,reopenedLocation.section);
    QTRY_COMPARE(view->location().offset,reopenedLocation.offset);
    restored.close();
  }

};
int main(int argc, char **argv) {
  Q_INIT_RESOURCE(documents);
  DocumentView::registerScheme();
  return isolatedTestMain<DocumentTests>(argc, argv);
}
#include "document_tests.moc"
