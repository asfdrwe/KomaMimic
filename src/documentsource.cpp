// SPDX-License-Identifier: GPL-3.0-only
#include "documentsource.h"
#include <QCoreApplication>
#include "pdfsource.h"
#include "publication.h"
#include <QAbstractTextDocumentLayout>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextBoundaryFinder>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <algorithm>
#include <cmath>
#include <unicode/ucnv.h>
namespace {
QString decodeText(const QByteArray &bytes, const TextOptions &options, QString &error) {
  if (bytes.size() > 16 * 1024 * 1024) {
    error = QCoreApplication::translate("KomaMimic", "テキストは16 MiBまでです。");
    return {};
  }
  QStringDecoder decoder(QStringDecoder::Utf8);
  QString result = decoder(bytes);
  if (options.encoding != "UTF-8" || decoder.hasError()) {
    const auto encoding =
        options.encoding == "UTF-8" || options.encoding == "CP932"
            ? QByteArray("windows-932")
            : options.encoding.toLatin1();
    UErrorCode status = U_ZERO_ERROR;
    int32_t needed = 0;
    ucnv_convert("UTF-8", encoding.constData(), nullptr, 0, bytes.constData(),
                 int(bytes.size()), &status);
    status = U_ZERO_ERROR;
    QByteArray output(bytes.size() * 4 + 16, '\0');
    needed = ucnv_convert("UTF-8", encoding.constData(), output.data(),
                          int(output.size()), bytes.constData(),
                          int(bytes.size()), &status);
    if (U_FAILURE(status)) {
      error = QCoreApplication::translate("KomaMimic", "テキストの文字コードを解釈できません。");
      return {};
    }
    result = QString::fromUtf8(output.constData(), needed);
  }
  if (options.vertical && result.size() > 1000000) {
    error = QCoreApplication::translate("KomaMimic", "縦書きテキストは100万文字までです。");
    return {};
  }
  if (result.startsWith(QChar(0xfeff)))
    result.remove(0, 1);
  if (result.contains(QChar(0))) {
    error = QCoreApplication::translate("KomaMimic", "バイナリを含むテキスト／MMT形式は解釈できません。");
    return {};
  }
  return result;
}
QString readText(const QString &path, const TextOptions &options, QString &error) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) { error = file.errorString(); return {}; }
  return decodeText(file.read(16 * 1024 * 1024 + 1), options, error);
}
QFont textFont(const TextOptions &o) {
  QFont font(o.family);
  font.setPixelSize(std::clamp(o.pointSize, 8, 120));
  return font;
}
constexpr int PageWidth = 1200, PageHeight = 1600;
struct Ruby {
  int position, length;
  QString reading;
};
struct RubyText {
  QString plain;
  QVector<Ruby> ruby;
};
RubyText rubyText(const QString &text, bool enabled) {
  RubyText result;
  const QRegularExpression expression(
      QStringLiteral("(?:｜([^《\n]+)|([\\p{Han}々]+))《([^》\n]+)》"));
  auto matches = expression.globalMatch(text);
  int previous = 0;
  while (matches.hasNext()) {
    const auto match = matches.next();
    result.plain += text.mid(previous, match.capturedStart() - previous);
    const auto base =
        match.captured(1).isEmpty() ? match.captured(2) : match.captured(1);
    if (enabled) result.ruby.append(
        {int(result.plain.size()), int(base.size()), match.captured(3)});
    result.plain += base;
    previous = match.capturedEnd();
  }
  result.plain += text.mid(previous);
  return result;
}
struct TextCell {
  QString text;
  int page, column, row, position;
};
QVector<TextCell> verticalCells(const QString &text, const TextOptions &o) {
  const int margin = std::clamp(o.margin, 0, 300),
            step = std::clamp(o.pointSize, 8, 120),
            columns = std::max(1, (PageWidth - 2 * margin) /
                                      (step + std::max(0, o.lineSpacing))),
            rows = std::max(1, (PageHeight - 2 * margin) / step);
  QVector<TextCell> cells;
  QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
  int previous = 0, page = 0, column = 0, row = 0;
  while (true) {
    const int end = finder.toNextBoundary();
    if (end < 0)
      break;
    if (page > 0) break;
    const auto glyph = text.mid(previous, end - previous);
    previous = end;
    if (glyph.contains('\n') || glyph.contains('\r')) {
      row = 0;
      ++column;
    } else if (glyph == "\f") {
      ++page;
      column = 0;
      row = 0;
    } else {
      cells.append({glyph, page, column, row, previous - int(glyph.size())});
      if (++row >= rows) {
        row = 0;
        ++column;
      }
    }
    if (column >= columns) {
      column = 0;
      row = 0;
      ++page;
    }
  }
  return cells;
}
void prepare(QTextDocument &document, const QString &text,
             const TextOptions &options, const QString &path) {
  document.setDefaultFont(textFont(options));
  document.setDocumentMargin(std::clamp(options.margin, 0, 300));
  document.setPageSize(QSizeF(PageWidth, PageHeight));
  if (isMarkdownDocument(path, options)) {
    document.setMarkdown(text, documentMarkdownFeatures(options));
    return;
  }
  document.setPlainText(rubyText(text, options.ruby).plain);
  QTextCursor cursor(&document);
  cursor.select(QTextCursor::Document);
  QTextBlockFormat format;
  format.setLineHeight(
      std::clamp(options.pointSize, 8, 120) +
          std::max(options.ruby ? options.pointSize / 2 + 2 : 0,
                   std::clamp(options.lineSpacing, 0, 80)),
      QTextBlockFormat::FixedHeight);
  cursor.mergeBlockFormat(format);
}
} // namespace
QString readDocumentText(const QString &path, const TextOptions &options, QString &error) {
  return readText(path, options, error);
}
QString decodeDocumentText(const QByteArray &bytes, const TextOptions &options, QString &error) {
  return decodeText(bytes, options, error);
}
bool isMarkdownDocument(const QString &path, const TextOptions &options) {
  const auto ext = QFileInfo(path).suffix().toLower();
  return options.contentMode == 2 || (options.contentMode == 0 && (ext == "md" || ext == "markdown"));
}
bool isArchiveTextPath(const QString &path) {
  const auto ext = QFileInfo(path).suffix().toLower();
  return ext == "txt" || ext == "mmt" || ext == "md" || ext == "markdown";
}
bool isTextDocumentPath(const QString &path) {
  const auto ext = QFileInfo(path).suffix().toLower();
  return ext == "txt" || ext == "mmt" || ext == "md" || ext == "markdown" || ext == "epub";
}
bool isDocumentPath(const QString &path) {
  const auto ext = QFileInfo(path).suffix().toLower();
  return ext == "pdf" || isTextDocumentPath(path);
}
DocumentInfo inspectDocument(const QString &path, const QString &password) {
  return inspectPdfDocument(path, password);
}
static ImageResult renderDecodedText(const QString &text, const QString &path, QSize target,
                                      const TextOptions &options, const DocumentResourceLoader &resources,
                                      const std::function<bool()> &cancel) {
  if (cancel && cancel()) return {{}, QCoreApplication::translate("KomaMimic", "文書描画を取り消しました。")};
  constexpr int page = 0; // Cover preview only; no alternate reading renderer.
  QSize size(PageWidth, PageHeight);
  size.scale(target.isEmpty() ? QSize(320,480) : target.boundedTo({320,480}), Qt::KeepAspectRatio);
  QImage image(size, QImage::Format_ARGB32_Premultiplied);
  image.fill(options.background);
  QPainter painter(&image);
  painter.scale(double(size.width()) / PageWidth,
                double(size.height()) / PageHeight);
  painter.setFont(textFont(options));
  painter.setPen(options.foreground);
  if (options.vertical) {
    const int step = std::clamp(options.pointSize, 8, 120),
              margin = std::clamp(options.margin, 0, 300);
    const auto prepared = rubyText(text, options.ruby);
    const auto cells = verticalCells(prepared.plain, options);
    const QString rotated =
        QStringLiteral("ー―〜～…‥（）［］｛｝〈〉《》「」『』【】");
    for (const auto &cell : cells) {
      if (cell.page < page)
        continue;
      if (cell.page > page)
        break;
      const QRect box(PageWidth - margin -
                          (cell.column + 1) *
                              (step + std::max(0, options.lineSpacing)),
                      margin + cell.row * step, step, step);
      if (cell.text.size() == 1 && rotated.contains(cell.text)) {
        painter.save();
        painter.translate(box.center());
        painter.rotate(90);
        painter.drawText(QRect(-step / 2, -step / 2, step, step),
                         Qt::AlignCenter, cell.text);
        painter.restore();
      } else
        painter.drawText(box, Qt::AlignCenter, cell.text);
      for (const auto &ruby : prepared.ruby)
        if (ruby.position == cell.position) {
          painter.save();
          QFont font = textFont(options);
          font.setPixelSize(std::max(6, step / 2));
          painter.setFont(font);
          const auto graphemes = ruby.reading.toUcs4();
          for (int k = 0; k < graphemes.size(); ++k) {
            const char32_t scalar = graphemes[k];
            const auto letter = QString::fromUcs4(&scalar, 1);
            const int spacing =
                std::max(1, step * ruby.length /
                                int(std::max<qsizetype>(1, graphemes.size())));
            painter.drawText(QRect(box.right(), box.top() + k * spacing,
                                   step / 2 + 2, spacing),
                             Qt::AlignCenter, letter);
          }
          painter.restore();
        }
    }
  } else {
    LocalTextDocument document(QFileInfo(path).absolutePath(), resources);
    prepare(document, text, options, path);
    if (cancel && cancel()) return {{}, QCoreApplication::translate("KomaMimic", "文書描画を取り消しました。")};
    painter.translate(0, -page * PageHeight);
    QAbstractTextDocumentLayout::PaintContext context;
    context.palette.setColor(QPalette::Text, options.foreground);
    context.clip = QRectF(0, page * PageHeight, PageWidth, PageHeight);
    document.documentLayout()->draw(&painter, context);
    const auto prepared = rubyText(text, options.ruby && !isMarkdownDocument(path, options));
    painter.save();
    QFont font = textFont(options);
    const int small = std::max(6, options.pointSize / 2);
    font.setPixelSize(small);
    painter.setFont(font);
    for (const auto &ruby : prepared.ruby) {
      if (isMarkdownDocument(path, options)) break;
      const auto block = document.findBlock(ruby.position);
      if (!block.isValid() || !block.layout())
        continue;
      const auto *layout = block.layout();
      const int offset = ruby.position - block.position();
      const auto line = layout->lineForTextPosition(offset);
      if (!line.isValid())
        continue;
      const auto bounds = document.documentLayout()->blockBoundingRect(block);
      const double x = line.cursorToX(offset),
                   end = line.cursorToX(
                       std::min(offset + ruby.length,
                                line.textStart() + line.textLength()));
      const double y = bounds.top() + line.y() - small;
      if (y + small < page * PageHeight || y > (page + 1) * PageHeight)
        continue;
      painter.drawText(
          QRectF(bounds.left() + x, y, std::max(1.0, end - x), small + 2),
          Qt::AlignCenter, ruby.reading);
    }
    painter.restore();
  }
  if (cancel && cancel()) return {{}, QCoreApplication::translate("KomaMimic", "文書描画を取り消しました。")};
  return {image, {}};
}
ImageResult renderDocument(const QString &path, int page, QSize target, const QString &password,
                           const std::function<bool()> &cancel) {
  return renderPdfDocument(path, page, target, password, cancel);
}
ImageResult renderDocumentPreview(const QString &path, QSize target, const TextOptions &options,
                                  const std::function<bool()> &cancel) {
  QString error; const auto text = readText(path, options, error);
  auto preview = options;
  if (isMarkdownDocument(path, preview)) preview.vertical = false;
  return error.isEmpty() ? renderDecodedText(text, path, target, preview, {}, cancel) : ImageResult{{}, error};
}
ImageResult renderTextPreview(const QByteArray &bytes, const QString &name, QSize target,
                             const TextOptions &options, const DocumentResourceLoader &resources,
                             const std::function<bool()> &cancel) {
  QString error; const auto text = decodeText(bytes, options, error);
  const DocumentResourceLoader safeResources = resources ? resources : DocumentResourceLoader([](const QUrl &) { return QVariant{}; });
  auto preview = options;
  if (isMarkdownDocument(name, preview)) preview.vertical = false;
  return error.isEmpty() ? renderDecodedText(text, name, target, preview, safeResources, cancel) : ImageResult{{}, error};
}
