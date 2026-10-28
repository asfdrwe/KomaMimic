// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "filenames.h"
#include "media.h"
#include "documentsource.h"
#include "publication.h"
#include <memory>
#include <functional>
#include <QMutex>
#include <QCache>
#include <QFileInfo>

class Book;
struct ArchiveTextSource {
    // An immutable view of the original entries, before text pagination.
    std::shared_ptr<const Book> book;
    QHash<QString, int> images;
    int index = 0;
    TextOptions options;
};
struct BookPage {
    QString name;
    QString identity;
    QString archiveEntryId;
    qint64 archiveIndex = 0;
    qint64 encodedSize = 0;
    ArchiveName sourceName;
    DecodedName decodedName;
    QString filePath;
    qint64 modifiedMs = 0;
    int documentPage = 0;
    QByteArray inlineData;
    QRectF portion;
    int documentOffset = -1;
    QString documentSection;
    bool document = false;
    bool pdfResource = false;
    std::shared_ptr<const ArchiveTextSource> archivedText;
};
struct OpenOptions {
    bool recursive = false, nestedArchives = false, folderContainers=false;
    bool pdfResources = false;
    // Internal concatenated-folder/nested-archive reading and image-only entry view.
    bool archiveAsPages = false, archiveImagesOnly = false;
    int depth = 0, fileCacheMiB=32;
    std::function<bool()> cancel;
    QString password;
    TextOptions text;
    QVariantMap scriptSettings;
};

struct DataResult { QByteArray bytes; QString error; };

class Book {
public:
    struct OpenResult;
    static OpenResult fromPages(const QVector<BookPage> &pages);
    static OpenResult open(const QString &path, const NamePolicy &policy = {}, const OpenOptions &options = {});
    std::shared_ptr<const Book> reinterpretNames(const NamePolicy &policy) const;
    ImageResult image(int index, QSize thumbnail = {}, const std::function<bool()> &cancel = {}) const;
    // Cover-list previews, including the first page of a text document.
    ImageResult preview(int index = 0, QSize size = {320,480}, const std::function<bool()> &cancel = {}) const;
    bool sourceUnchanged(int index) const;
    DataResult encoded(int index, const std::function<bool()> &cancel = {}) const;
    void clearEncodedCache() const;
    MemoryCacheInfo encodedCacheInfo() const;
    std::shared_ptr<const Publication> publication, documentContent;
    std::shared_ptr<const Book> archiveParent;
    QString path, format;
    QVector<BookPage> pages;
    QStringList warnings;
    NamePolicy namePolicy;
    int initialPage = 0;
    bool filesystem = false;
    bool canReinterpret = false;
    bool archiveDirectory = false;
    OpenOptions openOptions;
    std::shared_ptr<const Book> withPages(const QVector<BookPage> &pages) const;
    bool hasArchivedText() const;
    QString entryPath(int index) const;
private:
    struct ArchiveCursor;
    struct EncodedCache {std::shared_ptr<ArchiveCursor> cursor; QMutex mutex;QCache<QString,QByteArray> values{32*1024};};
    std::shared_ptr<EncodedCache> m_encodedCache=std::make_shared<EncodedCache>();
    DataResult cacheData(const QString &key,const QByteArray &data) const;
    Book() = default;
    qint64 m_fileSize = 0;
    qint64 m_modifiedMs = 0;
    static OpenResult openArchiveEntry(const QString &path, const NamePolicy &policy, const OpenOptions &options);
    void sortPages();
};

struct Book::OpenResult {
    std::shared_ptr<const Book> book;
    QString error;
};

// ASCII numeric runs, Unicode text, independent of the desktop locale.
bool naturalLess(const QString &a, const QString &b);

bool browserLess(const QFileInfo &a, const QFileInfo &b, int sort, bool reverse);
