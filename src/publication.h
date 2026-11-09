// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "documentsource.h"
#include <QHash>
#include <QSet>
#include <QJsonObject>
#include <QTextDocument>
#include <QUrl>
#include <memory>
#include <functional>
#include <atomic>
struct PublicationActivity {inline static std::atomic<quint64> payloadReads{0},payloadBytes{0},imageDecodes{0},sectionLoads{0};};

struct PublicationArchive;
struct PublicationImages;
struct DocumentLocation {
  QString section;
  int offset = 0;
  QString quote;
  bool wide = false;
  int side = 0; bool solo = false;
  bool rightToLeft = false, pending = false;
  QJsonObject json() const;
  static DocumentLocation fromJson(const QJsonObject &value);
};
struct PublicationSection {
  QString path, title, html, plain, error;
  bool fixed = false;
  QSize viewport;
  QHash<QString, int> anchors;
  QString imagePath, locationPath, caption;
  QSize imageSize;
  bool wide = false;
  int side = 0; bool solo = false, auxiliary = false, cover = false, svg = false;
  int baseOffset = 0;
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
  DocumentResourceLoader imageLoader;
  std::function<QVariant(const QUrl &,const std::function<bool()> &)> imageReader;
  QHash<QString,QString> imageSources;
  std::shared_ptr<PublicationImages> images;
  bool epub = false, rightToLeft = false, markdown = false, directionSpecified = false;
  QString coverImage;
  QSet<QString> coverDocuments, blockedResources;
  QHash<QString,QByteArray> fontKeys;
  std::shared_ptr<PublicationArchive> archive;
};
std::shared_ptr<const Publication> readPublication(const QString &path,
                                                   const TextOptions &options, const std::function<bool()> &cancel = {}, bool lazy = false);
std::shared_ptr<const Publication> readTextPublication(const QString &text, const QString &name,
    const TextOptions &options, const DocumentResourceLoader &resources, const QString &localRoot = {}, const std::function<bool()> &cancel = {}, bool lazy = false,
    const std::function<QVariant(const QUrl &,const std::function<bool()> &)> &imageReader = {});
QByteArray publicationResource(const Publication &publication, const QString &path,
    const std::function<bool()> &cancel = {}, QString *error = nullptr);
PublicationSection loadPublicationSection(const Publication &publication, int index,
    const std::function<bool()> &cancel = {});
// Resolves publication references; rejects external URLs and paths above root.
QString publicationPath(const QString &base, const QString &reference);
QString aozoraHtml(const QString &text,
    const std::function<QString(const QString &)> &imageSource = {}, const std::function<bool()> &cancel = {});
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
