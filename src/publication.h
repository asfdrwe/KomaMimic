// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "documentsource.h"
#include <QHash>
#include <QJsonObject>
#include <QTextDocument>
#include <QUrl>
#include <memory>
#include <functional>

struct DocumentLocation {
  QString section;
  int offset = 0;
  QString quote;
  bool wide = false;
  int side = 0; bool solo = false;
  QJsonObject json() const;
  static DocumentLocation fromJson(const QJsonObject &value);
};
struct PublicationSection {
  QString path, title, html, plain;
  bool fixed = false;
  QSize viewport;
  QHash<QString, int> anchors;
  QString imagePath;
  QSize imageSize;
  bool wide = false;
  int side = 0; bool solo = false, auxiliary = false;
};
struct PublicationToc {
  QString title, section, fragment;
  int offset = 0;
};
struct Publication {
  QString title, error;
  QStringList warnings;
  QVector<PublicationSection> sections;
  QVector<PublicationToc> toc;
  QHash<QString, QByteArray> resources;
  QString localRoot;
  bool epub = false, rightToLeft = false, markdown = false;
};
std::shared_ptr<const Publication> readPublication(const QString &path,
                                                   const TextOptions &options);
std::shared_ptr<const Publication> readTextPublication(const QString &text, const QString &name,
    const TextOptions &options, const DocumentResourceLoader &resources, const QString &localRoot = {});
// Resolves publication references; rejects external URLs and paths above root.
QString publicationPath(const QString &base, const QString &reference);
QString aozoraHtml(const QString &text,
    const std::function<QString(const QString &)> &imageSource = {});
// No Book or WebEngine view is needed to retrieve a cover.
ImageResult epubCover(const QString &path, QSize thumbnail = {240, 320},
                       const std::function<bool()> &cancel = {});
QTextDocument::MarkdownFeatures
documentMarkdownFeatures(const TextOptions &options);
QString markdownReaderStyle();
// Local Markdown resources are bounded to its directory, including symlinks.
QVariant documentResource(const QString &root, const QUrl &url);
class LocalTextDocument : public QTextDocument {
public:
  explicit LocalTextDocument(const QString &root, DocumentResourceLoader resources = {})
      : m_root(root), m_resources(std::move(resources)) {}

protected:
  QVariant loadResource(int type, const QUrl &url) override;

private:
  QString m_root;
  DocumentResourceLoader m_resources;
  qint64 m_bytes = 0;
  int m_count = 0;
};
