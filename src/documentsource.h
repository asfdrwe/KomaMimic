// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "media.h"
class QUrl;
using DocumentResourceLoader = std::function<QVariant(const QUrl &)>;
struct TextOptions {
  QString encoding = "UTF-8", family;
  int pointSize = 22, margin = 60, lineSpacing = 8;
  bool vertical = false,ruby=true;
  QColor foreground = Qt::black, background = QColor("#fffdf4");
  bool publisherStyles = true, embeddedFonts = true;
  bool overrideColors = false;
  int writingMode = 0; // 0: publication/default, 1: horizontal, 2: vertical
  int markdownDialect = 1; // 0: CommonMark, 1: GitHub
  int contentMode = 0; // 0: extension, 1: plain/Aozora, 2: Markdown
  int pageLayout = 0; // 0: single, 1: two-page spread
  bool wideImages = true;
};
bool isTextDocumentPath(const QString &path);
bool isArchiveTextPath(const QString &path);
bool isMarkdownDocument(const QString &path, const TextOptions &options);
QString readDocumentText(const QString &path, const TextOptions &options,
                         QString &error);
QString decodeDocumentText(const QByteArray &bytes, const TextOptions &options, QString &error);
bool isDocumentPath(const QString &path);
struct DocumentInfo {
  int pages = 0;
  QString error;
  DocumentInfo(int count = 0, QString message = {})
      : pages(count), error(std::move(message)) {}
};
DocumentInfo inspectDocument(const QString &path, const QString &password = {});
// Internal first-page previews only; document reading always uses DocumentView.
// Byte-backed previews never resolve resources through the host filesystem.
ImageResult renderTextPreview(const QByteArray &bytes, const QString &name,
                              QSize size = {320,480}, const TextOptions &text = {},
                              const DocumentResourceLoader &resources = {},
                              const std::function<bool()> &cancel = {});
ImageResult renderDocumentPreview(const QString &path, QSize size = {320,480},
                                  const TextOptions &text = {},
                                  const std::function<bool()> &cancel = {});
ImageResult renderDocument(const QString &path, int page, QSize size = {},
                           const QString &password = {},
                           const std::function<bool()> &cancel = {});
struct PdfResource {
  int page = 0, index = 0;
  QString label;
};
struct PdfResources {
  QVector<PdfResource> images;
  QString error;
};
PdfResources inspectPdfResources(const QString &path,
                                 const QString &password = {});
ImageResult renderPdfResource(const QString &path, int page, int image,
                              QSize size = {}, const QString &password = {});
