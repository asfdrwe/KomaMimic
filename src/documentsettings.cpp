// SPDX-License-Identifier: GPL-3.0-only
#include "documentsettings.h"
#include "archivepath.h"
#include <QCryptographicHash>
#include <QFileInfo>
QString documentBookKey(const QString &path) {
  return "documents/books/" +
         QString::fromLatin1(QCryptographicHash::hash(
                                 bookIdentityPath(path).toUtf8(),
                                 QCryptographicHash::Sha256)
                                 .toHex()) +
         '/';
}
QString documentFormatKey(const QString &path) {
  auto ext = QFileInfo(path).suffix().toLower();
  if (ext == "markdown")
    ext = "md";
  if (ext == "mmt")
    ext = "txt";
  return "documents/formats/" + ext + '/';
}
QVariantMap documentOptionDefaults() {
  return {{"ruby", true},
          {"vertical", false},
          {"encoding", "UTF-8"},
          {"font", ""},
          {"size", 22},
          {"margin", 60},
          {"spacing", 8},
          {"foreground", "#000000"},
          {"background", "#fffdf4"},
          {"publisherStyles", true},
          {"embeddedFonts", true},
          {"overrideColors", false},
          {"writingMode", 0},
          {"pageLayout", 0},
          {"wideImages", true},
          {"markdownDialect", 1},
          {"contentMode", 0}};
}
QVariantMap documentOptions(const QSettings &s, const QString &path, bool book,
                            bool format) {
  auto result = documentOptionDefaults();
  for (auto i = result.begin(); i != result.end(); ++i) {
    if (s.contains("text/" + i.key()))
      i.value() = s.value("text/" + i.key());
    if (format && s.contains(documentFormatKey(path) + i.key()))
      i.value() = s.value(documentFormatKey(path) + i.key());
    if (book && s.contains(documentBookKey(path) + i.key()))
      i.value() = s.value(documentBookKey(path) + i.key());
  }
  return result;
}
void saveDocumentOptions(QSettings &s, const QString &prefix,
                         const QVariantMap &values,
                         const QVariantMap &inherited) {
  for (auto i = values.begin(); i != values.end(); ++i) {
    if (!documentOptionDefaults().contains(i.key()))
      continue;
    if (i.value() == inherited.value(i.key()))
      s.remove(prefix + i.key());
    else
      s.setValue(prefix + i.key(), i.value());
  }
  s.sync();
}
TextOptions textOptions(const QVariantMap &v) {
  TextOptions o;
  o.ruby = v.value("ruby", true).toBool();
  o.vertical = v.value("vertical").toBool();
  o.encoding = v.value("encoding", "UTF-8").toString();
  o.family = v.value("font").toString();
  o.pointSize = std::clamp(v.value("size", 22).toInt(), 8, 120);
  o.margin = std::clamp(v.value("margin", 60).toInt(), 0, 300);
  o.lineSpacing = std::clamp(v.value("spacing", 8).toInt(), 0, 80);
  o.foreground = QColor(v.value("foreground", "#000000").toString());
  o.background = QColor(v.value("background", "#fffdf4").toString());
  if (!o.foreground.isValid())
    o.foreground = Qt::black;
  if (!o.background.isValid())
    o.background = QColor("#fffdf4");
  o.publisherStyles = v.value("publisherStyles", true).toBool();
  o.embeddedFonts = v.value("embeddedFonts", true).toBool();
  o.overrideColors = v.value("overrideColors").toBool();
  o.writingMode = std::clamp(v.value("writingMode").toInt(), 0, 2);
  o.markdownDialect = std::clamp(v.value("markdownDialect", 1).toInt(), 0, 1);
  o.contentMode = std::clamp(v.value("contentMode").toInt(), 0, 2);
  o.pageLayout = std::clamp(v.value("pageLayout").toInt(), 0, 1);
  o.wideImages = v.value("wideImages", true).toBool();
  return o;
}
QVector<int> documentVisiblePages(int count, int anchor, int pageLayout,
                                 const QSet<int> &wide, const QHash<int,int> &sides, const QSet<int> &solo, bool rtl) {
  if (anchor < 0 || anchor >= count)
    return {};
  QVector<int> result{anchor};
  const int firstSide=rtl ? 1 : -1;
  const bool secondOnly=sides.value(anchor) && sides.value(anchor)!=firstSide;
  const bool incompatible=sides.value(anchor+1)==firstSide;
  if (pageLayout == 1 && anchor + 1 < count && !wide.contains(anchor) && !wide.contains(anchor + 1)
      && !solo.contains(anchor) && !solo.contains(anchor+1) && !secondOnly && !incompatible)
    result.append(anchor + 1);
  return result;
}
int advanceDocumentPage(int count, int anchor, int direction, int pageLayout,
                        bool onePage, const QSet<int> &wide, const QHash<int,int> &sides, const QSet<int> &solo, bool rtl) {
  if (count <= 0)
    return 0;
  anchor = std::clamp(anchor, 0, count - 1);
  const int step = !onePage ? documentVisiblePages(count, anchor, pageLayout, wide,sides,solo,rtl).size() : 1;
  if (direction > 0)
    return anchor + step < count ? anchor + step : anchor;
  if(!onePage && pageLayout==1 && (!sides.isEmpty() || !solo.isEmpty())) {
    int previous=0;
    for(int start=0; start<anchor;) {
      previous=start;
      start+=documentVisiblePages(count,start,pageLayout,wide,sides,solo,rtl).size();
    }
    return previous;
  }
  if (!onePage && pageLayout == 1) {
    if (wide.contains(anchor - 1)) return anchor - 1;
    if (wide.contains(anchor)) {
      int start = anchor - 1;
      while (start >= 0 && !wide.contains(start)) --start;
      ++start;
      return start + std::max(0, (anchor - start - 1) / 2) * 2;
    }
    if (wide.contains(anchor - 2)) return anchor - 1;
    return std::max(0, anchor - 2);
  }
  return std::max(0, anchor - step);
}

void moveDocumentSettings(QSettings &s, const QString &oldPath, const QString &newPath) {
  const auto oldKey = documentBookKey(oldPath), newKey = documentBookKey(newPath);
  if (oldKey == newKey) return;
  QVariantMap source;
  for (const auto &key : s.allKeys())
    if (key.startsWith(oldKey)) source.insert(key.mid(oldKey.size()), s.value(key));
  for (auto i = source.cbegin(); i != source.cend(); ++i) s.setValue(newKey + i.key(), i.value());
  s.sync();
  if (s.status() == QSettings::NoError) { s.remove(oldKey.chopped(1)); s.sync(); }
}
