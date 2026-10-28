// SPDX-License-Identifier: GPL-3.0-only
#include "publication.h"
#include <QCoreApplication>
#include <QDir>
#include <QCryptographicHash>
#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>
#include <QTextBlock>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QTextCursor>
#include <QXmlStreamReader>
#include <archive.h>
#include <archive_entry.h>

QJsonObject DocumentLocation::json() const {
  return {{"version", 1},
          {"section", section},
          {"offset", offset},
          {"quote", quote}};
}
DocumentLocation DocumentLocation::fromJson(const QJsonObject &o) {
  return {o.value("section").toString(), std::max(0, o.value("offset").toInt()),
          o.value("quote").toString(), o.value("wide").toBool(),
          o.value("side").toInt(), o.value("solo").toBool()};
}
QString publicationPath(const QString &base, const QString &reference) {
  const QUrl ref(reference);
  if (!ref.isRelative() || reference.startsWith('/') ||
      reference.contains('\\') || !ref.query().isEmpty())
    return {};
  if (ref.path().isEmpty() && !base.isEmpty() && !ref.fragment().isEmpty())
    return base;
  const auto decoded = ref.path(QUrl::FullyDecoded);
  if (decoded.contains(QChar(0)) || decoded.contains('\\'))
    return {};
  const auto path = QDir::cleanPath(
      (base.isEmpty() ? QString{} : QFileInfo(base).path() + '/') + decoded);
  if (path == ".." || path.startsWith("../") || path.startsWith('/') ||
      path.contains(':'))
    return {};
  return path;
}
QTextDocument::MarkdownFeatures documentMarkdownFeatures(const TextOptions &o) {
  auto flags = QTextDocument::MarkdownFeatures(
      o.markdownDialect ? QTextDocument::MarkdownDialectGitHub
                        : QTextDocument::MarkdownDialectCommonMark);
  flags.setFlag(QTextDocument::MarkdownNoHTML);
  return flags;
}
QVariant documentResource(const QString &root, const QUrl &url) {
  if (!url.isRelative() && !url.isLocalFile())
    return {};
  const auto base = QFileInfo(root).canonicalFilePath();
  const auto path =
      QFileInfo(url.isLocalFile() ? url.toLocalFile()
                                  : QDir(root).filePath(url.path()))
          .canonicalFilePath();
  if (base.isEmpty() || !path.startsWith(base + '/') ||
      QFileInfo(path).size() > 16 * 1024 * 1024)
    return {};
  QImageReader reader(path);
  const auto size = reader.size();
  if (!size.isValid() || qint64(size.width()) * size.height() > 16000000)
    return {};
  return reader.read();
}
QVariant LocalTextDocument::loadResource(int type, const QUrl &url) {
  if (type != QTextDocument::ImageResource || ++m_count > 256 ||
      m_bytes > 64 * 1024 * 1024)
    return QImage{};
  const auto image = (m_resources ? m_resources(url) : documentResource(m_root, url)).value<QImage>();
  if (m_bytes + image.sizeInBytes() > 64 * 1024 * 1024)
    return QImage{};
  m_bytes += image.sizeInBytes();
  return image;
}
QString aozoraHtml(const QString &text) {
  const QRegularExpression ruby(QStringLiteral("(?:｜([^《\n]+)|([\\p{Han}々]+))《([^》\n]+)》"));
  // Offsets use the canonical plain text shared with cover previews: ruby
  // readings are excluded; annotation characters retain their source positions.
  const auto render=[&](const QString &part, int &offset) {
    QString result; int previous=0;
    auto matches=ruby.globalMatch(part);
    while(matches.hasNext()) {
      const auto m=matches.next(); const auto before=part.mid(previous,m.capturedStart()-previous);
      result+=before.toHtmlEscaped(); offset+=before.size();
      const auto base=m.captured(1).isEmpty() ? m.captured(2) : m.captured(1);
      result+="<ruby>"+base.toHtmlEscaped()+"<rt>"+m.captured(3).toHtmlEscaped()+"</rt></ruby>";
      offset+=base.size(); previous=m.capturedEnd();
    }
    const auto rest=part.mid(previous); result+=rest.toHtmlEscaped();offset+=rest.size();return result;
  };
  const QRegularExpression tokens(QStringLiteral("［＃[^］\n]{1,120}］"));
  const QRegularExpression indent(QStringLiteral("^［＃(?:ここから)?([0-9０-９一二三四五六七八九十]+)字下げ］$"));
  const auto number=[](QString value) {
    const QString kanji=QStringLiteral("〇一二三四五六七八九十");
    if(value.size()==1 && kanji.contains(value)) return kanji.indexOf(value);
    for(auto &c:value) if(c.unicode()>=0xff10 && c.unicode()<=0xff19)c=QChar(c.unicode()-0xff10+'0');
    return qsizetype(value.toInt());
  };
  QString html; int offset=0, blockIndent=0, heading=0; bool horizontal=false;
  for(const auto &line:text.split('\n')) {
    const int lineStart=offset; int lineHeading=heading;
    QString content; int previous=0, lineIndent=blockIndent;
    auto matches=tokens.globalMatch(line);
    while(matches.hasNext()) {
      const auto m=matches.next();
      const int begin=offset; const auto part=render(line.mid(previous,m.capturedStart()-previous),offset);
      content+="<span data-koma-offset=\""+QString::number(begin)+"\">"+part+"</span>";
      const auto token=m.captured(); bool known=true;
      const auto ind=indent.match(token);
      if(token==QStringLiteral("［＃改ページ］") || token==QStringLiteral("［＃改丁］")) content+="<span class=\"koma-break\"></span>";
      else if(ind.hasMatch()) {lineIndent=std::clamp(int(number(ind.captured(1))),0,20);if(token.contains(QStringLiteral("ここから")))blockIndent=lineIndent;}
      else if(token==QStringLiteral("［＃ここで字下げ終わり］")) blockIndent=lineIndent=0;
      else if(token==QStringLiteral("［＃ここから大見出し］"))heading=lineHeading=2;
      else if(token==QStringLiteral("［＃ここから中見出し］"))heading=lineHeading=3;
      else if(token==QStringLiteral("［＃ここから小見出し］"))heading=lineHeading=4;
      else if(token==QStringLiteral("［＃ここで大見出し終わり］") || token==QStringLiteral("［＃ここで中見出し終わり］") || token==QStringLiteral("［＃ここで小見出し終わり］")) heading=lineHeading=0;
      else if(token==QStringLiteral("［＃ここから横組み］")) horizontal=true;
      else if(token==QStringLiteral("［＃ここで横組み終わり］")) horizontal=false;
      else {
        const auto annotation=QRegularExpression(QStringLiteral("^［＃「([^」]+)」は(大|中|小)見出し］$")).match(token);
        if(annotation.hasMatch() && line.left(m.capturedStart()).trimmed()==annotation.captured(1))
          lineHeading=annotation.captured(2)==QStringLiteral("大") ? 2 : annotation.captured(2)==QStringLiteral("中") ? 3 : 4;
        else known=false;
      }
      if(known)offset+=token.size();else content+=render(token,offset);
      previous=m.capturedEnd();
    }
    const int begin=offset; const auto part=render(line.mid(previous),offset);
    content+="<span data-koma-offset=\""+QString::number(begin)+"\">"+part+"</span>";
    const auto tag=lineHeading ? "h"+QString::number(lineHeading) : QString("div");
    html+='<'+tag+" data-koma-offset=\""+QString::number(lineStart)+"\" style=\""+(horizontal ? "writing-mode:horizontal-tb;" : "")+"margin-block:0;padding-inline-start:"+QString::number(lineIndent)+"em\">"+content+"<br/></"+tag+'>';
    ++offset;
  }
  return "<!doctype html><html lang=\"ja\"><head><meta charset=\"utf-8\"/><style>"
         "body{line-break:strict;word-break:normal;overflow-wrap:normal;text-orientation:mixed}ruby{ruby-align:space-around;ruby-position:over}"
         "rt{font-size:50%;line-break:strict;white-space:normal}.koma-break{display:block;break-before:column}"
         "</style></head><body>"+html+"</body></html>";
}

namespace {
// The view serves HTML, whereas EPUB content is XML. In HTML a self-closing
// <script src="..."/> consumes the rest of the document as script text. Strip
// scripts using XML tokens (not a regex), and expand empty non-void XHTML tags.
// Keep publisher CSS, SVG namespaces and all other source text unchanged.
QString epubHtml(QString html, QString &error) {
  struct Edit {
    qsizetype start, end;
    QString replacement;
  };
  QVector<Edit> edits;
  const QStringList voidElements{
      "area", "base", "br", "col", "embed", "hr", "img", "input",
      "link", "meta", "param", "source", "track", "wbr"};
  QXmlStreamReader xml(html);
  while (!xml.atEnd()) {
    xml.readNext();
    if (!xml.isStartElement())
      continue;
    const auto end = qsizetype(xml.characterOffset());
    const auto start = html.lastIndexOf('<', end - 1);
    if (xml.name() == u"script") {
      xml.skipCurrentElement();
      edits.append({start, qsizetype(xml.characterOffset()), {}});
    } else if ((xml.namespaceUri().isEmpty() ||
                xml.namespaceUri() == u"http://www.w3.org/1999/xhtml") &&
               !voidElements.contains(xml.name().toString()) && end >= 2 &&
               html.mid(end - 2, 2) == "/>") {
      edits.append({end - 2, end, "></" + xml.qualifiedName().toString() + '>'});
    }
  }
  if (xml.hasError()) {
    error = QCoreApplication::translate("KomaMimic", "EPUB本文のXHTMLが不正です（%1行）：%2")
                .arg(xml.lineNumber())
                .arg(xml.errorString());
    return {};
  }
  for (auto i = edits.crbegin(); i != edits.crend(); ++i)
    html.replace(i->start, i->end - i->start, i->replacement);
  return html;
}
QString xmlText(const QByteArray &bytes, QHash<QString, int> *anchors) {
  QXmlStreamReader xml(bytes);
  QString text;
  bool pre = false;
  while (!xml.atEnd()) {
    xml.readNext();
    if (xml.isStartElement()) {
      const auto id = xml.attributes().value("id").toString();
      if (anchors && !id.isEmpty())
        anchors->insert(id, text.size());
      if (xml.name() == u"rt" || xml.name() == u"rp" ||
          xml.name() == u"script" || xml.name() == u"style" ||
          xml.name() == u"head")
        xml.skipCurrentElement();
      else if (xml.name() == u"br")
        text += '\n';
      else if (xml.name() == u"pre")
        pre = true;
    } else if (xml.isCharacters()) {
      const auto part = xml.text().toString();
      if (pre || !part.trimmed().isEmpty() || !part.contains('\n'))
        text += part;
    } else if (xml.isEndElement() && xml.name() == u"pre")
      pre = false;
  }
  return text;
}
bool unpack(const QString &path, Publication &p,
            const std::function<bool()> &cancel = {}) {
  auto *a = archive_read_new();
  archive_read_support_format_zip(a);
  const auto name = QFile::encodeName(path);
  if (archive_read_open_filename(a, name.constData(), 65536) != ARCHIVE_OK) {
    p.error = QCoreApplication::translate("KomaMimic", "EPUBのZIPを開けません。");
    archive_read_free(a);
    return false;
  }
  qint64 total = 0;
  int count = 0, status = ARCHIVE_OK;
  archive_entry *entry = nullptr;
  while ((status = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
    if (cancel && cancel()) { p.error = QCoreApplication::translate("KomaMimic", "EPUBの読み込みを中止しました。"); break; }
    if (archive_entry_filetype(entry) == AE_IFDIR) {
      archive_read_data_skip(a);
      continue;
    }
    const char *raw = archive_entry_pathname_utf8(entry);
    const auto key = raw ? QString::fromUtf8(raw) : QString{};
    if (++count > 10000 || archive_entry_filetype(entry) != AE_IFREG ||
        key.isEmpty() || publicationPath({}, key) != key ||
        archive_entry_size(entry) < 0 ||
        archive_entry_size(entry) > 32 * 1024 * 1024 ||
        p.resources.contains(key) || archive_entry_is_encrypted(entry) > 0) {
      p.error = QCoreApplication::translate("KomaMimic", "EPUBに未対応・不正な項目、重複パス、または大きすぎる項目があり"
                "ます。");
      break;
    }
    QByteArray bytes;
    char buffer[65536];
    la_ssize_t read;
    while ((read = archive_read_data(a, buffer, sizeof buffer)) > 0) {
      if (cancel && cancel()) { p.error = QCoreApplication::translate("KomaMimic", "EPUBの読み込みを中止しました。"); break; }
      total += read;
      if (total > 128 * 1024 * 1024 || bytes.size() + read > 32 * 1024 * 1024)
        break;
      bytes.append(buffer, read);
    }
    if (!p.error.isEmpty()) break;
    if (read < 0 || total > 128 * 1024 * 1024 ||
        bytes.size() > 32 * 1024 * 1024 || read > 0) {
      p.error = QCoreApplication::translate("KomaMimic", "EPUBの展開上限（項目32 MiB／合計128 "
                "MiB）を超えたか、ZIPが壊れています。");
      break;
    }
    p.resources.insert(key, bytes);
  }
  if (p.error.isEmpty() && status != ARCHIVE_EOF)
    p.error = QCoreApplication::translate("KomaMimic", "EPUBのZIPを読み終えられません。");
  archive_read_free(a);
  if (p.error.isEmpty() &&
      p.resources.value("mimetype").trimmed() != "application/epub+zip")
    p.error = QCoreApplication::translate("KomaMimic", "EPUBのmimetypeがありません。");

  return p.error.isEmpty();
}
QString packagePath(const Publication &p) {
  QXmlStreamReader xml(p.resources.value("META-INF/container.xml"));
  while (!xml.atEnd()) {
    xml.readNext();
    if (xml.isStartElement() && xml.name() == u"rootfile")
      return publicationPath({}, xml.attributes().value("full-path").toString());
  }
  return {};
}
// Cover lookup reads only requested entries; unrelated chapters/images are
// skipped without decompression, including large comic EPUBs.
bool coverEntries(const QString &path, Publication &p, const QStringList &wanted,
                  const std::function<bool()> &cancel) {
  auto *a = archive_read_new();
  archive_read_support_format_zip(a);
  const auto name = QFile::encodeName(path);
  if (archive_read_open_filename(a, name.constData(), 65536) != ARCHIVE_OK) {
    p.error = QCoreApplication::translate("KomaMimic", "EPUBのZIPを開けません。"); archive_read_free(a); return false;
  }
  QSet<QString> seen;
  archive_entry *entry = nullptr;
  int status = ARCHIVE_OK;
  while ((status = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
    if (cancel && cancel()) { p.error = QCoreApplication::translate("KomaMimic", "EPUBの読み込みを中止しました。"); break; }
    if (archive_entry_filetype(entry) == AE_IFDIR) { archive_read_data_skip(a); continue; }
    const char *raw = archive_entry_pathname_utf8(entry);
    const auto key = raw ? QString::fromUtf8(raw) : QString{};
    if (seen.size() >= 10000 || archive_entry_filetype(entry) != AE_IFREG || key.isEmpty() ||
        publicationPath({}, key) != key || seen.contains(key)) {
      p.error = QCoreApplication::translate("KomaMimic", "EPUBに不正な項目・重複パスがあります。"); break;
    }
    seen.insert(key);
    if (!wanted.contains(key)) { archive_read_data_skip(a); continue; }
    if (archive_entry_size(entry) < 0 || archive_entry_size(entry) > 32*1024*1024 || archive_entry_is_encrypted(entry) > 0) {
      p.error = QCoreApplication::translate("KomaMimic", "EPUB表紙の項目が大きすぎるか、暗号化されています。"); break;
    }
    QByteArray bytes;
    char buffer[65536];
    la_ssize_t read;
    while ((read = archive_read_data(a, buffer, sizeof buffer)) > 0) {
      if (cancel && cancel()) { p.error = QCoreApplication::translate("KomaMimic", "EPUBの読み込みを中止しました。"); break; }
      if (bytes.size() + read > 32*1024*1024) { p.error = QCoreApplication::translate("KomaMimic", "EPUB表紙の項目上限を超えています。"); break; }
      bytes.append(buffer, read);
    }
    if (!p.error.isEmpty()) break;
    if (read < 0) { p.error = QCoreApplication::translate("KomaMimic", "EPUB表紙の項目を展開できません。"); break; }
    p.resources.insert(key, bytes);
  }
  if (p.error.isEmpty() && status != ARCHIVE_EOF) p.error = QCoreApplication::translate("KomaMimic", "EPUBのZIPを読み終えられません。");
  archive_read_free(a);
  return p.error.isEmpty();
}
// Only image-only XHTML/SVG documents qualify. Inline ornaments in text do not.
QString singleImage(const QByteArray &bytes, const QString &base) {
  QXmlStreamReader xml(bytes);
  QString result;
  bool content = false;
  while (!xml.atEnd()) {
    xml.readNext();
    if (xml.isStartElement()) {
      const auto name = xml.name();
      if (name == u"head" || name == u"script" || name == u"style") {
        xml.skipCurrentElement(); continue;
      }
      if (name == u"body" || name == u"svg") content = true;
      if (!content) continue;
      if (name == u"img" || name == u"image") {
        if (!result.isEmpty()) return {};
        const auto a = xml.attributes();
        QString href = a.value("src").toString();
        if (href.isEmpty()) href = a.value("href").toString();
        if (href.isEmpty()) href = a.value("http://www.w3.org/1999/xlink", "href").toString();
        result = publicationPath(base, href);
        if (result.isEmpty()) return {};
      } else if (name == u"path" || name == u"rect" || name == u"circle" ||
                 name == u"object" || name == u"video" || name == u"use") return {};
    } else if (content && xml.isCharacters() && !xml.text().toString().trimmed().isEmpty()) return {};
  }
  return xml.hasError() ? QString{} : result;
}
QSize resourceImageSize(const Publication &p, const QString &key) {
  QBuffer buffer;
  buffer.setData(p.resources.value(key));
  buffer.open(QIODevice::ReadOnly);
  const auto format = QImageReader::imageFormat(&buffer);
  if (format.isEmpty() || format == "svg" || format == "svgz") return {};
  buffer.seek(0);
  QImageReader reader(&buffer, format);
  reader.setAutoDetectImageFormat(false);
  const auto size = reader.size();
  return size.isValid() && qint64(size.width()) * size.height() <= MaxPixels ? size : QSize{};
}
QString coverResource(Publication &p, const std::function<bool(const QString &)> &fetch) {
  const auto package = packagePath(p);
  if (package.isEmpty()) return {};
  QHash<QString, QString> manifest;
  QStringList modern, documents;
  QString legacy, nav, first;
  QXmlStreamReader xml(p.resources.value(package));
  while (!xml.atEnd()) {
    xml.readNext();
    if (!xml.isStartElement()) continue;
    const auto a = xml.attributes();
    if (xml.name() == u"item") {
      const auto path = publicationPath(package, a.value("href").toString());
      manifest[a.value("id").toString()] = path;
      const auto properties = a.value("properties").toString().split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
      if (properties.contains("cover-image")) modern.append(path);
      if (properties.contains("nav")) nav = path;
    } else if (xml.name() == u"meta" && a.value("name") == u"cover") legacy = a.value("content").toString();
    else if (xml.name() == u"reference" && a.value("type") == u"cover") documents.append(publicationPath(package, a.value("href").toString()));
    else if (xml.name() == u"itemref" && first.isEmpty()) first = a.value("idref").toString();
  }
  if (xml.hasError()) return {};
  modern.append(manifest.value(legacy));
  int candidates = 0;
  // Try declared images before looking at the fallback cover documents.
  const auto usable = [&](const QString &key) -> QString {
    if (key.isEmpty() || ++candidates > 32 || !fetch(key)) return {};
    if (key.endsWith(".svg", Qt::CaseInsensitive)) {
      const auto image = singleImage(p.resources.value(key), key);
      if (!image.isEmpty() && !image.endsWith(".svg", Qt::CaseInsensitive) && fetch(image) && resourceImageSize(p, image).isValid()) return image;
    } else if (resourceImageSize(p, key).isValid()) return key;
    return {};
  };
  for (const auto &key : modern) {
    const auto result = usable(key);
    if (!result.isEmpty()) return result;
  }
  QXmlStreamReader navigation(p.resources.value(nav));
  // Navigation/guide fallback is bounded to a small number of candidates.
  if (!nav.isEmpty()) { fetch(nav); navigation.clear(); navigation.addData(p.resources.value(nav)); }
  while (!navigation.atEnd()) {
    navigation.readNext();
    if (navigation.isStartElement() && navigation.name() == u"a" &&
        navigation.attributes().value("http://www.idpf.org/2007/ops", "type").toString().split(' ').contains("cover"))
      documents.append(publicationPath(nav, navigation.attributes().value("href").toString()));
  }
  documents.append(manifest.value(first));
  for (const auto &document : documents)
    if (!document.isEmpty() && candidates < 32 && fetch(document)) {
      const auto result = usable(singleImage(p.resources.value(document), document));
      if (!result.isEmpty()) return result;
    }
  return {};
}
void epub(Publication &p, const QString &path) {
  if (!unpack(path, p))
    return;
  QXmlStreamReader container(p.resources.value("META-INF/container.xml"));
  QString package;
  while (!container.atEnd()) {
    container.readNext();
    if (container.isStartElement() && container.name() == u"rootfile") {
      package = publicationPath(
          {}, container.attributes().value("full-path").toString());
      break;
    }
  }
  if (package.isEmpty() || !p.resources.contains(package)) {
    p.error = QCoreApplication::translate("KomaMimic", "EPUBのpackageが見つかりません。");
    return;
  }
  struct Item {
    QString path, type, properties;
  };
  QHash<QString, Item> manifest;
  struct Spine {
    QString id, properties;
  };
  QVector<Spine> spine;
  QString nav, ncx;
  bool fixed = false;
  QXmlStreamReader xml(p.resources.value(package));
  while (!xml.atEnd()) {
    xml.readNext();
    if (!xml.isStartElement())
      continue;
    const auto a = xml.attributes();
    if (xml.name() == u"title")
      p.title = xml.readElementText();
    else if (xml.name() == u"meta" &&
             a.value("property") == u"rendition:layout")
      fixed = xml.readElementText().trimmed() == "pre-paginated";
    else if (xml.name() == u"item") {
      Item item{publicationPath(package, a.value("href").toString()),
                a.value("media-type").toString(),
                a.value("properties").toString()};
      manifest.insert(a.value("id").toString(), item);
      if (item.properties.split(' ').contains("nav"))
        nav = item.path;
    } else if (xml.name() == u"spine") {
      p.rightToLeft = a.value("page-progression-direction") == u"rtl";
      ncx = a.value("toc").toString();
    } else if (xml.name() == u"itemref" && a.value("linear") != u"no")
      spine.append(
          {a.value("idref").toString(), a.value("properties").toString()});
  }
  if (xml.hasError() || spine.isEmpty() || spine.size() > 1000) {
    p.error = QCoreApplication::translate("KomaMimic", "EPUBのpackage／spineが不正です。");
    return;
  }
  // Font obfuscation is not DRM. Only IDPF's defined font transform is decoded.
  // Unknown font transforms are omitted; encrypted non-font resources fail closed.
  if(p.resources.contains("META-INF/encryption.xml")) {
    QString identifier, uniqueId;
    QXmlStreamReader packageXml(p.resources.value(package));
    while(!packageXml.atEnd()) {
      packageXml.readNext();
      if(!packageXml.isStartElement()) continue;
      if(packageXml.name()==u"package") uniqueId=packageXml.attributes().value("unique-identifier").toString();
      if(packageXml.name()==u"identifier" && packageXml.attributes().value("id")==uniqueId)
        identifier=packageXml.readElementText();
    }
    identifier.remove(QRegularExpression("[ \\t\\r\\n]"));
    const auto key=QCryptographicHash::hash(identifier.toUtf8(),QCryptographicHash::Sha1);
    QXmlStreamReader encryption(p.resources.value("META-INF/encryption.xml"));
    int count=0; QSet<QString> encryptedResources;
    while(!encryption.atEnd()) {
      encryption.readNext();
      if(!encryption.isStartElement() || encryption.name()!=u"EncryptedData") continue;
      if(++count>10000) {p.error=QCoreApplication::translate("KomaMimic", "暗号化リソースの件数上限を超えています。");return;}
      QString algorithm, uri;
      while(!encryption.atEnd()) {
        encryption.readNext();
        if(encryption.isEndElement() && encryption.name()==u"EncryptedData") break;
        if(!encryption.isStartElement()) continue;
        if(encryption.name()==u"EncryptionMethod") algorithm=encryption.attributes().value("Algorithm").toString();
        if(encryption.name()==u"CipherReference") uri=encryption.attributes().value("URI").toString();
      }
      const auto resource=publicationPath({},uri);
      if(encryptedResources.contains(resource)){p.error=QCoreApplication::translate("KomaMimic", "EPUBに重複した暗号化リソース指定があります。");return;}
      encryptedResources.insert(resource);
      bool font=false;
      for(const auto &item:manifest) if(item.path==resource &&
          (item.type.startsWith("font/") || item.type=="application/vnd.ms-opentype" || item.type=="application/font-sfnt" || item.type=="application/font-woff")) font=true;
      if(resource.isEmpty() || !font) {p.error=QCoreApplication::translate("KomaMimic", "暗号化された本文・画像など、未対応の保護リソースを含むEPUBです。");return;}
      if(algorithm=="http://www.idpf.org/2008/embedding" && !identifier.isEmpty() && p.resources.contains(resource)) {
        auto &bytes=p.resources[resource];
        for(int i=0;i<std::min<qsizetype>(1040,bytes.size());++i) bytes[i]=char(uchar(bytes[i]) ^ uchar(key[i%key.size()]));
      } else {
        p.resources.remove(resource); p.warnings.append(QCoreApplication::translate("KomaMimic", "難読化フォントを代替フォントで表示：")+resource);
      }
    }
    if(encryption.hasError()) {p.error=QCoreApplication::translate("KomaMimic", "EPUBのencryption.xmlが不正です。");return;}
  }
  // Auxiliary XHTML remains addressable without joining the reading sequence.
  QSet<QString> linearPaths;
  for(const auto &ref:spine) linearPaths.insert(manifest.value(ref.id).path);
  QStringList ids=manifest.keys(); std::sort(ids.begin(),ids.end());
  for(const auto &id:ids) if(manifest[id].type=="application/xhtml+xml" && !linearPaths.contains(manifest[id].path))
    spine.append({id,"koma:auxiliary"});
  if(spine.size()>2000) {p.error=QCoreApplication::translate("KomaMimic", "EPUB本文・注釈の件数上限を超えています。");return;}
  for (const auto &ref : spine) {
    const auto item = manifest.value(ref.id);
    if (item.path.isEmpty() || !p.resources.contains(item.path) ||
        (item.type != "application/xhtml+xml" &&
         item.type != "image/svg+xml")) {
      p.error = QCoreApplication::translate("KomaMimic", "EPUBのspineに未対応または存在しない本文があります。");
      return;
    }
    PublicationSection s;
    s.path = item.path;
    const auto properties=ref.properties.split(QRegularExpression("\\s+"),Qt::SkipEmptyParts);
    s.auxiliary=properties.contains("koma:auxiliary");
    s.side=properties.contains("page-spread-left") || properties.contains("rendition:page-spread-left") ? -1 :
           properties.contains("page-spread-right") || properties.contains("rendition:page-spread-right") ? 1 : 0;
    s.solo=properties.contains("rendition:page-spread-center") || properties.contains("rendition:spread-none");
    s.title = QFileInfo(item.path).fileName();
    s.fixed =
        ref.properties.contains("rendition:layout-pre-paginated") ||
        (fixed && !ref.properties.contains("rendition:layout-reflowable"));
    const auto bytes = p.resources.value(item.path);
    QStringDecoder decoder(QStringDecoder::encodingForData(bytes).value_or(
        QStringConverter::Utf8));
    s.html = decoder(bytes);
    if (decoder.hasError()) {
      p.error = QCoreApplication::translate("KomaMimic", "EPUB本文の文字コードがUTF-8 / UTF-16として解釈できません。");
      return;
    }
    s.html = epubHtml(s.html, p.error);
    if (!p.error.isEmpty())
      return;
    s.plain = xmlText(bytes, &s.anchors);
    QXmlStreamReader markup(bytes);
    while (!markup.atEnd()) {
      markup.readNext();
      if (!markup.isStartElement())
        continue;
      const auto a = markup.attributes();
      if (markup.name() == u"meta" && a.value("name") == u"viewport") {
        const auto content = a.value("content").toString();
        QRegularExpression width("width\\s*=\\s*(\\d+)"),
            height("height\\s*=\\s*(\\d+)");
        s.viewport = {width.match(content).captured(1).toInt(),
                      height.match(content).captured(1).toInt()};
      }
      if (item.type == "image/svg+xml" && markup.name() == u"svg") {
        const auto box = a.value("viewBox").toString().split(
            QRegularExpression("[,\\s]+"), Qt::SkipEmptyParts);
        if (box.size() == 4)
          s.viewport = {int(box[2].toDouble()), int(box[3].toDouble())};
        else
          s.viewport = {a.value("width").toInt(), a.value("height").toInt()};
      }
    }
    if (item.type == "image/svg+xml") {
      s.fixed = true;
      s.html.remove(QRegularExpression("<\\?xml[^>]*>"));
      s.html = "<html><head></head><body style=\"margin:0\">" + s.html +
               "</body></html>";
    }
    s.imagePath = singleImage(bytes, item.path);
    s.imageSize = resourceImageSize(p, s.imagePath);
    s.wide = !s.imagePath.isEmpty() && s.imageSize.width() > s.imageSize.height()
             && s.imageSize.width() >= 512 && s.imageSize.height() >= 256;
    p.sections.append(s);
  }
  // EPUB 3 navigation, then EPUB 2 NCX. Only spine destinations are exposed.
  const bool isNcx = nav.isEmpty();
  if (isNcx)
    nav = manifest.value(ncx).path;
  QXmlStreamReader navigation(p.resources.value(nav));
  bool inToc = false;
  int navDepth = 0;
  QString label;
  while (!navigation.atEnd()) {
    navigation.readNext();
    if (navigation.isStartElement()) {
      if (!isNcx && navigation.name() == u"nav") {
        ++navDepth;
        for (const auto &a : navigation.attributes())
          if (a.name() == u"type" &&
              a.value().toString().split(' ').contains("toc"))
            inToc = true;
      }
      if (isNcx && navigation.name() == u"text")
        label = navigation.readElementText();
      if ((!isNcx && inToc && navigation.name() == u"a") ||
          (isNcx && navigation.name() == u"content")) {
        const auto href =
            navigation.attributes().value(isNcx ? "src" : "href").toString();
        const auto target = publicationPath(nav, href);
        if (!isNcx)
          label = navigation.readElementText(
              QXmlStreamReader::IncludeChildElements);
        for (const auto &s : p.sections)
          if (s.path == target && !s.auxiliary) {
            p.toc.append({label, target, QUrl(href).fragment(),
                          s.anchors.value(QUrl(href).fragment())});
            break;
          }
      }
    } else if (navigation.isEndElement() && navigation.name() == u"nav" &&
               --navDepth <= 0)
      inToc = false;
  }
  if (p.toc.isEmpty())
    for (const auto &s : p.sections)
      if(!s.auxiliary) p.toc.append({s.title, s.path, {}, 0});
}
} // namespace
ImageResult epubCover(const QString &path, QSize thumbnail,
                       const std::function<bool()> &cancel) {
  Publication p;
  if (!coverEntries(path, p, {"mimetype", "META-INF/container.xml"}, cancel)) return {{}, p.error};
  if (p.resources.value("mimetype").trimmed() != "application/epub+zip") return {{}, QCoreApplication::translate("KomaMimic", "EPUBのmimetypeがありません。")};
  const auto package = packagePath(p);
  if (package.isEmpty() || !coverEntries(path, p, {package}, cancel)) return {{}, QCoreApplication::translate("KomaMimic", "EPUBのpackageを読み込めません。")};
  if (cancel && cancel()) return {{}, QCoreApplication::translate("KomaMimic", "EPUBの読み込みを中止しました。")};
  qint64 bytes = 0;
  int requests = 0;
  const auto fetch = [&](const QString &key) {
    if (key.isEmpty() || !p.error.isEmpty()) return false;
    if (p.resources.contains(key)) return true;
    if (++requests > 64 || bytes > 64*1024*1024) { p.error = QCoreApplication::translate("KomaMimic", "EPUB表紙の読み込み上限を超えています。"); return false; }
    if (!coverEntries(path, p, {key}, cancel)) return false;
    bytes += p.resources.value(key).size();
    if (bytes > 64*1024*1024) { p.error = QCoreApplication::translate("KomaMimic", "EPUB表紙の読み込み上限を超えています。"); return false; }
    return p.resources.contains(key);
  };
  const auto key = coverResource(p, fetch);
  if (!p.error.isEmpty()) return {{}, p.error};
  if (key.isEmpty()) return {{}, QCoreApplication::translate("KomaMimic", "EPUBの表紙画像が見つかりません。")};
  return decodeImage(p.resources.value(key), key, thumbnail);
}
QString markdownReaderStyle() {
  // Qt exports xx-large/x-large/etc. on heading spans. CSS absolute-size
  // keywords refer to browser defaults rather than the reader's body size.
  return "h1{font-size:2em!important}h2{font-size:1.5em!important}"
         "h3{font-size:1.25em!important}h4{font-size:1.125em!important}"
         "h5,h6{font-size:1em!important}"
         "h1,h2,h3,h4,h5,h6{line-height:1.25!important}"
         "h1 span,h2 span,h3 span,h4 span,h5 span,h6 span{font-size:inherit!important;line-height:inherit!important}";
}
std::shared_ptr<const Publication> readPublication(const QString &path,
                                                   const TextOptions &options) {
  auto p = std::make_shared<Publication>();
  p->title = QFileInfo(path).completeBaseName();
  if (QFileInfo(path).suffix().compare("epub", Qt::CaseInsensitive) == 0) {
    p->epub = true;
    epub(*p, path);
    return p;
  }
  QString error;
  const auto text = readDocumentText(path, options, error);
  if (!error.isEmpty()) {
    p->error = error;
    return p;
  }
  return readTextPublication(text, QFileInfo(path).fileName(), options, {}, QFileInfo(path).absolutePath());
}
std::shared_ptr<const Publication> readTextPublication(const QString &text, const QString &name,
    const TextOptions &options, const DocumentResourceLoader &resources, const QString &localRoot) {
  auto p = std::make_shared<Publication>();
  p->title = QFileInfo(name).completeBaseName(); p->localRoot = localRoot;
  PublicationSection section;
  section.path = "text.html";
  section.title = p->title;
  section.plain = text;
  if (isMarkdownDocument(name, options)) {
    p->markdown = true;
    const DocumentResourceLoader safeResources = resources ? resources : localRoot.isEmpty()
      ? DocumentResourceLoader([](const QUrl &) { return QVariant{}; }) : DocumentResourceLoader{};
    LocalTextDocument doc(p->localRoot, safeResources);
    doc.setMarkdown(text, documentMarkdownFeatures(options));
    if (resources) {
      QVector<QPair<int, QTextImageFormat>> images;
      for (auto block = doc.begin(); block.isValid(); block = block.next())
        for (auto item = block.begin(); !item.atEnd(); ++item) {
          const auto fragment = item.fragment();
          if (fragment.isValid() && fragment.charFormat().isImageFormat())
            images.append({fragment.position(), fragment.charFormat().toImageFormat()});
        }
      QHash<QString, QString> mappedImages; qint64 bytes = 0;
      for (const auto &[position, original] : images) {
        auto key = mappedImages.value(original.name());
        if (key.isEmpty() && mappedImages.size() < 256) {
          const auto image = doc.resource(QTextDocument::ImageResource, QUrl(original.name())).value<QImage>();
          QByteArray encoded; QBuffer buffer(&encoded); buffer.open(QIODevice::WriteOnly);
          if (!image.isNull() && image.save(&buffer, "PNG") && bytes + encoded.size() <= 64 * 1024 * 1024) {
            key = "__archive_images__/" + QString::number(mappedImages.size()) + ".png";
            bytes += encoded.size(); p->resources[key] = encoded;
            doc.addResource(QTextDocument::ImageResource, QUrl(key), image);
          } else key = "__missing_archive_image__";
          mappedImages[original.name()] = key;
        }
        if (key.isEmpty()) key = "__missing_archive_image__";
        auto format = original; format.setName(key);
        QTextCursor cursor(&doc); cursor.setPosition(position); cursor.setPosition(position + 1, QTextCursor::KeepAnchor);
        cursor.setCharFormat(format);
      }
    }
    section.html = doc.toHtml();
    section.html.replace("</head>","<style id=\"koma-markdown-headings\">"+markdownReaderStyle()+"</style></head>");
    section.plain = doc.toPlainText();
    // Preserve QTextDocument positions through the browser's DOM layout.
    const QRegularExpression blocks("<(p|h[1-6]|li|pre)(?=[\\s>])[^>]*>",
                                    QRegularExpression::CaseInsensitiveOption);
    auto matches = blocks.globalMatch(section.html);
    QString mapped;
    int previous = 0;
    auto block = doc.begin();
    while (matches.hasNext() && block.isValid()) {
      const auto match = matches.next();
      mapped += section.html.mid(previous, match.capturedEnd() - previous - 1);
      mapped +=
          " data-koma-offset=\"" + QString::number(block.position()) + "\">";
      previous = match.capturedEnd();
      block = block.next();
    }
    mapped += section.html.mid(previous);
    section.html = mapped;
    for (auto block = doc.begin(); block.isValid(); block = block.next()) {
      if (block.blockFormat().headingLevel())
        p->toc.append({block.text(), section.path, {}, block.position()});
    }
  } else {
    section.html = aozoraHtml(text);
    // Positions exclude readings, identically to the standard renderer.
    auto plain = text;
    plain.replace(QRegularExpression(QStringLiteral(
                      "(?:｜([^《\n]+)|([\\p{Han}々]+))《([^》\n]+)》")),
                  "\\1\\2");
    section.plain = plain;
    const QRegularExpression headings("<h[234] data-koma-offset=\"(\\d+)\"[^>]*>(.*?)</h[234]>");
    auto matches=headings.globalMatch(section.html);
    while(matches.hasNext()) {
      const auto match=matches.next();const auto title=xmlText(("<body>"+match.captured(2)+"</body>").toUtf8(),nullptr).trimmed();
      if(!title.isEmpty())p->toc.append({title,section.path,{},match.captured(1).toInt()});
    }
  }
  p->sections.append(section);
  return p;
}
