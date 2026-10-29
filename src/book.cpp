// SPDX-License-Identifier: GPL-3.0-only
#include "book.h"
#include "archivepath.h"
#include <QCryptographicHash>
#include <QCoreApplication>
#include "scriptreader.h"
#include <QMutexLocker>
#include <QUuid>
#include "archivenames.h"
#include <archive.h>
#include <archive_entry.h>
#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <algorithm>
#include <cstring>
#include <limits>
#include <cerrno>
#include <locale.h>
#ifdef Q_OS_DARWIN
#include <xlocale.h>
#endif
#include <string>

namespace {
constexpr qint64 MaxEntries = 100000;
struct ArchiveFree { void operator()(archive *a) const { archive_read_free(a); } };
using Archive = std::unique_ptr<archive, ArchiveFree>;
class Utf8ArchiveLocale {
public:
    Utf8ArchiveLocale() {
#ifdef Q_OS_WIN
        m_previousMode = _configthreadlocale(_ENABLE_PER_THREAD_LOCALE);
        m_previous = setlocale(LC_CTYPE, nullptr);
        m_valid = setlocale(LC_CTYPE, ".UTF8") != nullptr;
#else
        for (const auto *name : {"C.UTF-8", "en_US.UTF-8", "UTF-8"}) {
            m_locale = newlocale(LC_CTYPE_MASK, name, nullptr);
            if (m_locale) break;
        }
        if (m_locale) m_previous = uselocale(m_locale);
        m_valid = m_locale && m_previous;
#endif
    }
    ~Utf8ArchiveLocale() {
#ifdef Q_OS_WIN
        setlocale(LC_CTYPE, m_previous.c_str()); _configthreadlocale(m_previousMode);
#else
        if (m_previous) uselocale(m_previous);
        if (m_locale) freelocale(m_locale);
#endif
    }
    bool valid() const { return m_valid; }
private:
    bool m_valid = false;
#ifdef Q_OS_WIN
    int m_previousMode = 0;
    std::string m_previous;
#else
    locale_t m_locale = nullptr, m_previous = nullptr;
#endif
};

DocumentResourceLoader archiveTextResources(const ArchiveTextSource &source,
                                            const std::function<bool()> &cancel) {
    return [&source, cancel](const QUrl &url) -> QVariant {
        if (cancel && cancel()) return {};
        const auto target = publicationPath(source.book->pages[source.index].name, url.toString(QUrl::FullyEncoded));
        const auto found = source.images.constFind(target);
        if (target.isEmpty() || found == source.images.cend()) return {};
        const auto &page = source.book->pages[*found];
        if (page.encodedSize < 0 || page.encodedSize > 16 * 1024 * 1024) return {};
        const auto data = source.book->encoded(*found, cancel);
        if (!data.error.isEmpty() || data.bytes.size() > 16 * 1024 * 1024) return {};
        QBuffer buffer; buffer.setData(data.bytes); buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer);
        // Resource bytes may not match their filename. Accept native raster
        // handlers only, so an SVG disguised as PNG cannot resolve host files.
        static const QSet<QByteArray> rasterFormats{
            "png", "jpeg", "jpg", "bmp", "gif", "webp", "tiff", "tif", "ico", "cur",
            "pbm", "pgm", "ppm", "pbmraw", "pgmraw", "ppmraw", "xbm", "xpm", "tga",
            "avif", "jxl", "jp2", "j2k", "heif", "heic", "qoi"};
        if (!rasterFormats.contains(reader.format())) return {};
        const auto size = reader.size();
        if (!size.isValid() || qint64(size.width()) * size.height() > 16000000) return {};
        return reader.read();
    };
}
struct ArchiveInput {
    QFile file; QByteArray buffer=QByteArray(64*1024,char(0));
    std::shared_ptr<std::function<bool()>> cancel;
    bool cancelled(archive *a) const {
      if(cancel && *cancel && (*cancel)()) {archive_set_error(a,ECANCELED,"Archive read cancelled");return true;}
      return false;
    }
};
Archive reader(const QString &path, QString &error, const QString &password = {},
               std::shared_ptr<std::function<bool()>> cancel = {}) {
    Archive a(archive_read_new());
    if (!a) { error = QCoreApplication::translate("KomaMimic", "書庫リーダを作成できません。"); return {}; }
    archive_read_support_filter_none(a.get());
    archive_read_support_filter_gzip(a.get());
    archive_read_support_filter_bzip2(a.get());
    archive_read_support_filter_xz(a.get());
    archive_read_support_filter_zstd(a.get());
    archive_read_support_format_zip_seekable(a.get());
    archive_read_support_format_rar(a.get());
    archive_read_support_format_rar5(a.get());
    archive_read_support_format_7zip(a.get());
    archive_read_support_format_lha(a.get());
    archive_read_support_format_tar(a.get());
    archive_read_support_format_cab(a.get());
    if (!password.isEmpty()) archive_read_add_passphrase(a.get(), password.toUtf8().constData());
    // ZIP/LHA display names are decoded independently from original metadata.
    // Backend conversions must not change entry identity or hide Mac entries.
    archive_read_set_format_option(a.get(), "zip", "hdrcharset", "CP437");
    archive_read_set_format_option(a.get(), "zip", "mac-ext", "0");
    archive_read_set_format_option(a.get(), "lha", "hdrcharset", "CP932");
    // Checking in the compressed-input callback also cancels scans of solid
    // and filtered archives, without forcing unrelated entries to decompress.
    auto *input=new ArchiveInput;input->file.setFileName(path);input->cancel=std::move(cancel);
    if(!input->file.open(QIODevice::ReadOnly)){error=input->file.errorString();delete input;return {};}
    archive_read_set_seek_callback(a.get(),[](archive *archive,void *opaque,la_int64_t offset,int whence)->la_int64_t {
      auto &in=*static_cast<ArchiveInput*>(opaque);if(in.cancelled(archive))return ARCHIVE_FATAL;
      const auto base=whence==SEEK_SET ? 0 : whence==SEEK_CUR ? in.file.pos() : in.file.size();
      if(offset< -base || offset>std::numeric_limits<qint64>::max()-base || !in.file.seek(base+offset))return ARCHIVE_FATAL;
      return in.file.pos();
    });
    const int opened=archive_read_open2(a.get(),input,nullptr,
      [](archive *a,void *opaque,const void **buffer)->la_ssize_t {
        auto &in=*static_cast<ArchiveInput*>(opaque);if(in.cancelled(a))return ARCHIVE_FATAL;
        const auto read=in.file.read(in.buffer.data(),in.buffer.size());*buffer=in.buffer.constData();return read;
      },
      [](archive *a,void *opaque,la_int64_t distance)->la_int64_t {
        // libarchive 3.8.7 retries negative skip results indefinitely. Decline
        // this optimization on cancellation; the read callback then reports
        // the fatal error without entering the skipper's retry loop.
        auto &in=*static_cast<ArchiveInput*>(opaque);if(in.cancelled(a))return 0;
        const auto begin=in.file.pos();distance=std::clamp<qint64>(distance,0,std::max<qint64>(0,in.file.size()-begin));
        return in.file.seek(begin+distance) ? distance : 0;
      },
      [](archive *,void *opaque)->int {delete static_cast<ArchiveInput*>(opaque);return ARCHIVE_OK;});
    if (opened != ARCHIVE_OK) {
        error = QCoreApplication::translate("KomaMimic", "書庫を開けません: %1").arg(QString::fromUtf8(archive_error_string(a.get())));
        return {};
    }
    return a;
}

QString archiveError(archive *a) {
    const char *message = archive_error_string(a);
    return message ? QString::fromUtf8(message) : QCoreApplication::translate("KomaMimic", "書庫の読み込みに失敗しました。");
}
}

bool naturalLess(const QString &a, const QString &b) {
    qsizetype i = 0, j = 0;
    const auto digit = [](QChar c) { return c >= u'0' && c <= u'9'; };
    while (i < a.size() && j < b.size()) {
        if (digit(a[i]) && digit(b[j])) {
            const qsizetype ai = i, bj = j;
            while (i < a.size() && digit(a[i])) ++i;
            while (j < b.size() && digit(b[j])) ++j;
            qsizetype az = ai, bz = bj;
            while (az < i && a[az] == u'0') ++az;
            while (bz < j && b[bz] == u'0') ++bz;
            if (i - az != j - bz) return i - az < j - bz;
            const int n = QStringView(a).mid(az, i - az).compare(QStringView(b).mid(bz, j - bz));
            if (n != 0) return n < 0;
            if (i - ai != j - bj) return i - ai < j - bj;
        } else {
            const QChar ac = a[i].toLower(), bc = b[j].toLower();
            if (ac != bc) return ac < bc;
            ++i; ++j;
        }
    }
    if (i != a.size() || j != b.size()) return i == a.size();
    return a < b;
}

void Book::sortPages() {
    std::stable_sort(pages.begin(), pages.end(), [](const BookPage &a, const BookPage &b) {
        if (a.name == b.name) {
            if (a.archiveIndex == b.archiveIndex) return a.documentPage < b.documentPage;
            return a.archiveIndex < b.archiveIndex;
        }
        return naturalLess(a.name, b.name);
    });
}
Book::OpenResult Book::open(const QString &path, const NamePolicy &policy, const OpenOptions &options) {
    if (parseArchiveEntryPath(path).valid()) return openArchiveEntry(path, policy, options);
    // libarchive internally transcodes some Unicode names via LC_CTYPE, even
    // when our own adapter supplies display names. Scope UTF-8 to this worker;
    // never mutate the process-global locale while other readers are running.
    Utf8ArchiveLocale locale;
    if (!locale.valid()) return {{}, QCoreApplication::translate("KomaMimic", "書庫のUnicode処理に必要なUTF-8 localeを作成できません。")};
    if (!legacyEncodings().contains(policy.legacyEncoding)) return {{}, QCoreApplication::translate("KomaMimic", "未対応の名前の文字コードです。")};
    const QFileInfo info(path);
    auto book = std::shared_ptr<Book>(new Book);
    book->path = info.absoluteFilePath(); book->namePolicy = policy;
    book->openOptions = options;book->openOptions.cancel={};book->m_encodedCache->values.setMaxCost(std::clamp(options.fileCacheMiB,0,256)*1024);
    book->m_fileSize = info.size(); book->m_modifiedMs = info.lastModified().toMSecsSinceEpoch();
    bool nestedFound = false;
    if (isDocumentPath(path)) {
        if (info.suffix().compare("pdf", Qt::CaseInsensitive) == 0 && options.pdfResources) {
            const auto resources = inspectPdfResources(path, options.password);
            if (!resources.error.isEmpty()) return {{}, resources.error};
            book->format = QT_TRANSLATE_NOOP("KomaMimic", "PDF画像リソース");
            for (const auto &resource : resources.images) {
                BookPage page; page.name=info.fileName()+" / "+resource.label;page.document=true;page.pdfResource=true;
                page.documentPage=resource.page;page.archiveIndex=resource.index;page.filePath=info.absoluteFilePath();
                page.identity=QString("pdf-image:%1:%2").arg(resource.page).arg(resource.index);
                page.encodedSize=info.size();page.modifiedMs=book->m_modifiedMs;page.decodedName={page.name,"PDF resource",QCoreApplication::translate("KomaMimic", "画像リソース"),{},false};book->pages.append(page);
            }
            return {book,{}};
        }
        if (isTextDocumentPath(path)) {
            book->publication = readPublication(path, options.text);
            book->documentContent = book->publication;
            book->warnings += book->publication->warnings;
            if (!book->publication->error.isEmpty()) return {{}, book->publication->error};
            book->format = info.suffix().toUpper();
            for (const auto &section : book->publication->sections) {
                if(section.auxiliary) continue;
                BookPage page; page.name = section.title; page.filePath = info.absoluteFilePath();
                page.document = true; page.documentSection = section.path; page.documentOffset = 0;
                page.identity = "text:" + section.path + ":0"; book->pages.append(page);
            }
            return {book, {}};
        }
        const auto document = inspectDocument(path, options.password);
        if (!document.error.isEmpty()) return {{}, document.error};
        book->format = info.suffix().toUpper();
        
        for (int i = 0; i < document.pages; ++i) {
            BookPage page; page.name = info.fileName(); page.documentPage = i; page.document = true;
            page.identity=QString("document:%1").arg(i);
            page.filePath = info.absoluteFilePath(); page.encodedSize = info.size(); page.modifiedMs = book->m_modifiedMs;
            page.decodedName = {page.name, "document", QCoreApplication::translate("KomaMimic", "文書ページ"), {}, false}; book->pages.append(page);
        }
        return {book, {}};
    }
    if (info.isDir() || isImagePath(path)) {
        book->filesystem = true; book->format = QT_TRANSLATE_NOOP("KomaMimic", "画像フォルダ");
        const QDir directory(info.isDir() ? info.absoluteFilePath() : info.absolutePath());
        QFileInfoList files;
        if (options.recursive) {
            QDirIterator iterator(directory.absolutePath(), QDir::Files | QDir::Readable, QDirIterator::Subdirectories);
            while (iterator.hasNext()) { if(options.cancel&&options.cancel())return {{},QCoreApplication::translate("KomaMimic", "読み込みを取り消しました。")};iterator.next(); files.append(iterator.fileInfo()); if (files.size() > MaxEntries) return {{}, QCoreApplication::translate("KomaMimic", "フォルダの項目数が上限を超えています。")}; }
        } else files = directory.entryInfoList(QDir::Files | QDir::Readable, QDir::NoSort);
        qint64 ordinal = 0,containerBytes=0;
        for (const auto &file : files) {
            if(options.cancel&&options.cancel())return {{},QCoreApplication::translate("KomaMimic", "読み込みを取り消しました。")};
            if(options.folderContainers&&(isArchivePath(file.fileName())||file.suffix().compare("pdf",Qt::CaseInsensitive)==0)){
                auto childOptions=options;childOptions.folderContainers=false;childOptions.archiveAsPages=true;const auto child=Book::open(file.absoluteFilePath(),policy,childOptions);
                if(!child.book){book->warnings.append(file.fileName()+": "+child.error);continue;}
                for(int i=0;i<child.book->pages.size();++i){auto page=child.book->pages[i];if(!page.document){const auto data=child.book->encoded(i, options.cancel);if(!data.error.isEmpty())return {{},data.error};containerBytes+=data.bytes.size();if(containerBytes>128*1024*1024)return {{},QCoreApplication::translate("KomaMimic", "フォルダ内書庫の展開が128 MiBを超えています。")};page.inlineData=data.bytes;}
                    page.identity="container:"+file.absoluteFilePath()+"/"+page.identity;page.name=directory.relativeFilePath(file.absoluteFilePath())+"/"+page.name;page.archiveIndex=ordinal++;book->pages.append(page);if(book->pages.size()>MaxEntries)return {{},QCoreApplication::translate("KomaMimic", "フォルダ内のページ数が上限を超えました。")};
                }continue;
            }
            if (!isImagePath(file.fileName())) continue;
            if(options.cancel&&options.cancel())return {{},QCoreApplication::translate("KomaMimic", "読み込みを取り消しました。")};
            if (ordinal >= MaxEntries) return {{}, QCoreApplication::translate("KomaMimic", "フォルダの画像数が上限を超えています。")};
            BookPage page;
            page.name = directory.relativeFilePath(file.absoluteFilePath()); page.filePath = file.absoluteFilePath();
            page.archiveIndex = ordinal++; page.encodedSize = file.size();
            page.modifiedMs = file.lastModified().toMSecsSinceEpoch();
            page.decodedName = {page.name, "filesystem", QCoreApplication::translate("KomaMimic", "OSの名前"), {}, false};
            book->pages.append(page);
        }
    } else {
        QString error;
        auto a = reader(path, error, options.password,std::make_shared<std::function<bool()>>(options.cancel));
        if (!a) return {{}, error};
        ArchiveNames metadata;
        bool metadataRead = false;
        QHash<QByteArray, int> entryOccurrences;
        archive_entry *entry = nullptr;
        qint64 ordinal = 0;
        qint64 nestedBytes = 0;
        int status = ARCHIVE_OK;
        while ((status = archive_read_next_header(a.get(), &entry)) == ARCHIVE_OK || status == ARCHIVE_WARN) {
            if(options.cancel&&options.cancel())return {{},QCoreApplication::translate("KomaMimic", "読み込みを取り消しました。")};
            if (ordinal >= MaxEntries) return {{}, QCoreApplication::translate("KomaMimic", "書庫内の項目数が上限（100,000）を超えています。")};
            if (!metadataRead) {
                const int family = archive_format(a.get()) & ARCHIVE_FORMAT_BASE_MASK;
                if (family == ARCHIVE_FORMAT_ZIP) { metadata = readZipNames(path); book->canReinterpret = true; }
                else if (family == ARCHIVE_FORMAT_LHA) { metadata = readLhaNames(path); book->canReinterpret = true; }
                if (!metadata.error.isEmpty()) return {{}, metadata.error};
                book->format = QString::fromUtf8(archive_format_name(a.get()));
                metadataRead = true;
            }
            if (book->canReinterpret && ordinal >= metadata.entries.size())
                return {{}, QCoreApplication::translate("KomaMimic", "名前情報と書庫内の項目数が一致しません。")};
            if (archive_entry_filetype(entry) == AE_IFREG || archive_entry_filetype(entry) == 0) {
                BookPage page;
                page.archiveIndex = ordinal; page.encodedSize = archive_entry_size(entry);
                if (book->canReinterpret) {
                    page.sourceName = metadata.entries[ordinal];
                    page.decodedName = decodeName(page.sourceName, policy);
                } else {
                    QString name;
                    if (const auto *wide = archive_entry_pathname_w(entry)) name = QString::fromWCharArray(wide);
                    else if (const auto *utf8 = archive_entry_pathname_utf8(entry)) name = QString::fromUtf8(utf8);
                    else if (const auto *raw = archive_entry_pathname(entry)) {
                        page.sourceName.rawName = QByteArray(raw);
                        if (!decodeStrict(page.sourceName.rawName, "UTF-8", name)) name = escapedName(page.sourceName.rawName);
                    }
                    page.decodedName = {name, "backend Unicode", QCoreApplication::translate("KomaMimic", "書庫backend"), {}, false};
                }
                page.name = page.decodedName.text;
                const auto rawIdentity = page.sourceName.rawName.isEmpty() ? page.name.toUtf8() : page.sourceName.rawName;
                page.archiveEntryId = QString::fromLatin1(QCryptographicHash::hash(rawIdentity, QCryptographicHash::Sha256).toHex());
                const int occurrence = entryOccurrences[rawIdentity]++;
                if (occurrence) page.archiveEntryId += '-' + QString::number(occurrence);
                // An invalid declared Unicode name must not prevent reading a
                // payload whose raw suffix still identifies an image candidate.
                if (isImagePath(page.name) || isImagePath(QString::fromLatin1(page.sourceName.rawName)) ||
                    (!options.archiveImagesOnly && !options.archiveAsPages && (isArchiveTextPath(page.name) || isArchiveTextPath(QString::fromLatin1(page.sourceName.rawName)))))
                    book->pages.append(std::move(page));
                else if (options.nestedArchives && options.depth < 3 && isArchivePath(page.name)) {
                    book->pages.append(page);
                    const auto data = book->encoded(book->pages.size() - 1); book->pages.removeLast();
                    if (!data.error.isEmpty()) return {{}, data.error};
                    QTemporaryDir temporary;
                    const auto childPath = temporary.path() + "/archive." + QFileInfo(page.name).suffix();
                    QFile child(childPath);
                    if (!temporary.isValid() || !child.open(QIODevice::WriteOnly) || child.write(data.bytes) != data.bytes.size()) return {{}, QCoreApplication::translate("KomaMimic", "内部書庫を開けません。")};
                    child.close(); auto childOptions = options; childOptions.archiveAsPages=true; ++childOptions.depth;
                    const auto opened = Book::open(childPath, policy, childOptions);
                    if (!opened.book) {
                        if (opened.error == QCoreApplication::translate("KomaMimic", "対応する画像・テキストがありません。暗号化・分割書庫や未対応形式も確認してください。")) {
                            book->warnings.append(page.name + ": " + opened.error);
                            continue;
                        }
                        return {{}, opened.error};
                    }
                    for (int i = 0; i < opened.book->pages.size(); ++i) {
                        auto nested = opened.book->pages[i]; const auto content = opened.book->encoded(i, options.cancel);
                        nestedBytes += content.bytes.size();
                        if (!content.error.isEmpty() || nestedBytes > 128 * 1024 * 1024 || book->pages.size() >= MaxEntries) return {{}, QCoreApplication::translate("KomaMimic", "内部書庫の展開容量が上限を超えたか、読込に失敗しました。")};
                        if (nested.document) {
                            nested.name += QString(" [%1].png").arg(nested.documentPage + 1);
                            nested.document = false; nested.archivedText.reset(); nested.documentPage = 0;
                            nested.portion = {}; nested.documentOffset = -1; nested.documentSection.clear();
                        }
                        nested.name = page.name + "/" + nested.name; nested.inlineData = content.bytes;
                        nested.identity=QString("nested:%1/%2").arg(ordinal).arg(nested.identity);
                        nested.decodedName.text = nested.name; book->pages.append(nested);
                    }
                    nestedFound = true;
                }
            }
            ++ordinal;
            const int skipped = archive_read_data_skip(a.get());
            if (skipped < ARCHIVE_WARN) return {{}, archiveError(a.get())};
        }
        if (status != ARCHIVE_EOF) return {{}, archiveError(a.get())};
        if (book->canReinterpret && ordinal != metadata.entries.size())
            return {{}, QCoreApplication::translate("KomaMimic", "名前情報と書庫内の項目数が一致しません。")};
        if (nestedFound) book->canReinterpret = false;
    }
    book->sortPages();
    if (nestedFound) book->pages.erase(std::remove_if(book->pages.begin(), book->pages.end(),
        [](const BookPage &page) { return isArchiveTextPath(page.name); }), book->pages.end());
    if (book->pages.isEmpty()) return {{}, QCoreApplication::translate("KomaMimic", "対応する画像・テキストがありません。暗号化・分割書庫や未対応形式も確認してください。")};
    if (!book->filesystem && !nestedFound && !options.archiveAsPages && !options.archiveImagesOnly &&
        std::any_of(book->pages.cbegin(), book->pages.cend(), [](const BookPage &page) { return isArchiveTextPath(page.name); })) {
        book->archiveDirectory = true;
        for (auto &page : book->pages) page.identity = "archive-file:" + page.archiveEntryId;
        return {book, {}};
    }
    // Reading sequences contain image/PDF pages. Text documents stay individual
    // publications and are never paginated into an alternate raster reader.
    QVector<BookPage> expanded;
    for (int index = 0; index < book->pages.size(); ++index) {
        const auto page = book->pages[index];
        const auto suffix = QFileInfo(page.name).suffix().toLower();
        if (options.cancel && options.cancel()) return {{}, QCoreApplication::translate("KomaMimic", "読み込みを取り消しました。")};
        int count = 1;
        if (suffix == "tiff" || suffix == "tif" || scriptMultipageExtensions().contains(suffix)) {
            const auto data = book->encoded(index);
            if (!data.error.isEmpty())return {{},data.error};
            QString error;
            const int scripted=scriptPageCount(data.bytes,page.name,error,options.scriptSettings,options.cancel);
            if(scripted<0)return {{},error};
            count=scripted ? scripted : documentPages(data.bytes);
        }
        if (expanded.size() + count > MaxEntries) return {{}, QCoreApplication::translate("KomaMimic", "文書ページ数が上限を超えています。")};
        for (int p = 0; p < count; ++p) { auto copy = page; if(count > 1) copy.documentPage = p; expanded.append(std::move(copy)); }
    }
    book->pages = std::move(expanded);
    if (book->pages.isEmpty()) return {{}, book->warnings.join('\n')};
    for (auto &page : book->pages) if (page.identity.isEmpty()) {
        const auto raw=page.sourceName.rawName.isEmpty()?page.name.toUtf8():page.sourceName.rawName;
        page.identity=(book->filesystem?"file:"+page.filePath:"entry:"+QString::number(page.archiveIndex)+":"+QString::fromLatin1(raw.toHex()))+"#"+QString::number(page.documentPage);
    }
    if (book->filesystem && !info.isDir()) {
        for (int i = 0; i < book->pages.size(); ++i) if (book->pages[i].filePath == info.absoluteFilePath()) { book->initialPage = i; break; }
    }
    return {book, {}};
}
std::shared_ptr<const Book> Book::reinterpretNames(const NamePolicy &policy) const {
    auto copy = std::shared_ptr<Book>(new Book(*this));
    if (!canReinterpret) return copy;
    copy->namePolicy = policy;
    for (auto &page : copy->pages) {
        if(page.sourceName.rawName.isEmpty()&&page.sourceName.rawHeader.isEmpty())continue;
        page.decodedName = decodeName(page.sourceName, policy);
        page.name = page.decodedName.text;
    }
    copy->sortPages();
    return copy;
}
struct Book::ArchiveCursor { Archive handle; qint64 next=0; std::shared_ptr<std::function<bool()>> cancel=std::make_shared<std::function<bool()>>(); };
MemoryCacheInfo Book::encodedCacheInfo() const {
    QMutexLocker lock(&m_encodedCache->mutex);
    return {int(m_encodedCache->values.size()),qint64(m_encodedCache->values.totalCost())*1024,qint64(m_encodedCache->values.maxCost())*1024};
}
bool Book::sourceUnchanged(int index) const {
    if(index<0 || index>=pages.size()) return false;
    const auto &page=pages[index];
    if(!page.inlineData.isEmpty()) return true;
    if(page.archivedText) return page.archivedText->book->sourceUnchanged(page.archivedText->index);
    if (publication && !page.archivedText) {
        const QFileInfo original(path);
        return original.isFile() && original.size()==m_fileSize && original.lastModified().toMSecsSinceEpoch()==m_modifiedMs;
    }
    const bool separate=filesystem || page.document;
    const QFileInfo info(separate?page.filePath:physicalBookPath(path));
    return info.isFile() && info.size()==(separate?page.encodedSize:m_fileSize) &&
        info.lastModified().toMSecsSinceEpoch()==(separate?page.modifiedMs:m_modifiedMs);
}
DataResult Book::encoded(int index, const std::function<bool()> &cancel) const {
    const auto cancelled=[cancel,bookCancel=openOptions.cancel]{return (cancel && cancel()) || (bookCancel && bookCancel());};
    if(cancelled())return {{},QCoreApplication::translate("KomaMimic", "書庫読込を取り消しました。")};
    Utf8ArchiveLocale locale;
    if (!locale.valid()) return {{}, QCoreApplication::translate("KomaMimic", "書庫のUnicode処理に必要なUTF-8 localeを作成できません。")};
    if (index < 0 || index >= pages.size()) return {{}, QCoreApplication::translate("KomaMimic", "ページが範囲外です。")};
    const auto &page = pages[index];
    if (!page.inlineData.isEmpty()) return {page.inlineData, {}};
    if (page.document) {
        const auto image = this->image(index, {}, cancelled);
        if (!image.error.isEmpty()) return {{}, image.error};
        QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly); image.image.save(&buffer, "PNG"); return {bytes, {}};
    }
    const QFileInfo info(filesystem ? page.filePath : physicalBookPath(path));
    if (info.size() != (filesystem ? page.encodedSize : m_fileSize)
        || info.lastModified().toMSecsSinceEpoch() != (filesystem ? page.modifiedMs : m_modifiedMs))
        return {{}, QCoreApplication::translate("KomaMimic", "画像・書庫が変更されています。開き直してください。")};
    {QMutexLocker lock(&m_encodedCache->mutex);if(const auto *value=page.identity.isEmpty()?nullptr:m_encodedCache->values.object(page.identity))return {*value,{}};}
    if (page.encodedSize > MaxEncodedBytes || page.encodedSize < 0)
        return {{}, QCoreApplication::translate("KomaMimic", "画像データの上限（64 MiB）を超えています: %1").arg(page.name)};
    if (filesystem) {
        QFile file(page.filePath);
        if (!file.open(QIODevice::ReadOnly)) return {{}, file.errorString()};
        auto bytes = file.read(MaxEncodedBytes + 1);
        if (bytes.size() > MaxEncodedBytes || file.error() != QFile::NoError) return {{}, QCoreApplication::translate("KomaMimic", "画像データを読めません。")};
        return cacheData(page.identity,bytes);
    }
    QMutexLocker lock(&m_encodedCache->mutex);
    if(cancelled())return {{},QCoreApplication::translate("KomaMimic", "書庫読込を取り消しました。")};
    auto &cursor=m_encodedCache->cursor;
    if(!cursor || cursor->next>page.archiveIndex) {
      cursor=std::make_shared<ArchiveCursor>(); QString error;
      *cursor->cancel=cancelled;
      cursor->handle=reader(physicalBookPath(path),error,openOptions.password,cursor->cancel);
      if(!cursor->handle){cursor.reset();return {{},error};}
    }
    *cursor->cancel=cancelled;
    struct ClearRequestCancel {
      std::shared_ptr<std::function<bool()>> state;
      ~ClearRequestCancel() { *state={}; }
    } clearCancel{cursor->cancel};
    auto *a=cursor->handle.get();
    archive_entry *entry=nullptr; int status=ARCHIVE_OK;
    while((status=archive_read_next_header(a,&entry))==ARCHIVE_OK || status==ARCHIVE_WARN) {
      if(cancelled()){cursor.reset();return {{},QCoreApplication::translate("KomaMimic", "書庫読込を取り消しました。")};}
      const auto ordinal=cursor->next++;
      if(ordinal!=page.archiveIndex) {
        if(archive_read_data_skip(a)<ARCHIVE_WARN){const auto error=archiveError(a);cursor.reset();return {{},error};}
        continue;
      }
      if(archive_entry_is_encrypted(entry)>0 && openOptions.password.isEmpty()){cursor.reset();return {{},QCoreApplication::translate("KomaMimic", "書庫のパスワードが必要です。")};}
      QByteArray bytes; char chunk[64*1024]; la_ssize_t read;
      while((read=archive_read_data(a,chunk,sizeof chunk))>0) {
        if(cancelled()){cursor.reset();return {{},QCoreApplication::translate("KomaMimic", "書庫読込を取り消しました。")};}
        if(bytes.size()+read>MaxEncodedBytes){cursor.reset();return {{},QCoreApplication::translate("KomaMimic", "画像の入力上限を超えています。")};}
        bytes.append(chunk,read);
      }
      if(read<0){const auto error=archiveError(a);cursor.reset();return {{},error};}
      const int cost=int((bytes.size()+1023)/1024);
      if(!page.identity.isEmpty() && cost<=m_encodedCache->values.maxCost())m_encodedCache->values.insert(page.identity,new QByteArray(bytes),cost);
      return {bytes,{}};
    }
    const auto error=status==ARCHIVE_EOF ? QCoreApplication::translate("KomaMimic", "書庫内のページがありません。開き直してください。") : archiveError(a);
    cursor.reset(); return {{},error};
}
ImageResult Book::image(int index, QSize thumbnail, const std::function<bool()> &cancel) const {
    const auto cancelled=[cancel,sourceCancel=openOptions.cancel]{return (cancel && cancel()) || (sourceCancel && sourceCancel());};
    if(cancelled())return {{},QCoreApplication::translate("KomaMimic", "画像読込を取り消しました。")};
    if (index < 0 || index >= pages.size()) return {{}, QCoreApplication::translate("KomaMimic", "ページが範囲外です。")};
    const auto &page = pages[index];
    if ((archiveDirectory && isArchiveTextPath(page.name)) || publication)
        return {{}, QCoreApplication::translate("KomaMimic", "文書の画像出力・ページサムネイルは現在未対応です。")};
    if (page.document) {
        const QFileInfo info(page.filePath);
        if (info.size() != page.encodedSize || info.lastModified().toMSecsSinceEpoch() != page.modifiedMs) return {{}, QCoreApplication::translate("KomaMimic", "文書が変更されています。開き直してください。")};
        auto result=page.pdfResource ? renderPdfResource(page.filePath,page.documentPage,int(page.archiveIndex),thumbnail,openOptions.password) : renderDocument(page.filePath, page.documentPage, thumbnail, openOptions.password,cancelled);
        if(!page.portion.isEmpty()&&!result.image.isNull()){const auto image=result.image;const auto &r=page.portion;result.image=image.copy(QRect(qRound(r.x()*image.width()),qRound(r.y()*image.height()),qRound(r.width()*image.width()),qRound(r.height()*image.height())).intersected(image.rect()));}return result;
    }
    const auto data = encoded(index,cancelled);
    if (!data.error.isEmpty()) return {{}, data.error};
    if (page.portion.isEmpty()) return decodeImage(data.bytes, page.name, thumbnail, page.documentPage,openOptions.scriptSettings,cancelled);
    auto image = decodeImage(data.bytes, page.name, {}, page.documentPage,openOptions.scriptSettings,cancelled);
    const auto crop = [&](QImage &value) {
        if (!page.portion.isEmpty()) {
            const QRect rectangle(qRound(page.portion.x() * value.width()), qRound(page.portion.y() * value.height()),
                                  qRound(page.portion.width() * value.width()), qRound(page.portion.height() * value.height()));
            value = value.copy(rectangle.intersected(value.rect()));
        }
        if (!thumbnail.isEmpty()) value = value.scaled(thumbnail, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    };
    crop(image.image); for (auto &frame : image.frames) crop(frame); return image;
}
ImageResult Book::preview(int index, QSize size, const std::function<bool()> &cancel) const {
    if (cancel && cancel()) return {{}, QCoreApplication::translate("KomaMimic", "画像読込を取り消しました。")};
    if (index < 0 || index >= pages.size()) return {{}, QCoreApplication::translate("KomaMimic", "ページが範囲外です。")};
    const auto &page = pages[index];
    if (archiveDirectory && isArchiveTextPath(page.name)) {
        if (!sourceUnchanged(index)) return {{}, encoded(index, cancel).error};
        auto options = openOptions; options.cancel = cancel;
        const auto opened = openArchiveEntry(entryPath(index), namePolicy, options);
        return opened.book ? opened.book->preview(0, size, cancel) : ImageResult{{}, opened.error};
    }
    if (!publication) return image(index, size, cancel);
    if (!sourceUnchanged(index)) return {{}, QCoreApplication::translate("KomaMimic", "文書が変更されています。開き直してください。")};
    if (page.archivedText) {
        const auto &source = *page.archivedText;
        const auto data = source.book->encoded(source.index, cancel);
        if (!data.error.isEmpty()) return {{}, data.error};
        return renderTextPreview(data.bytes, source.book->pages[source.index].name, size,
                                 source.options, archiveTextResources(source, cancel), cancel);
    }
    if (QFileInfo(path).suffix().compare("epub", Qt::CaseInsensitive)==0) return epubCover(path, size, cancel);
    return renderDocumentPreview(path, size, openOptions.text, cancel);
}

std::shared_ptr<const Book> Book::withPages(const QVector<BookPage> &newPages) const {
    auto copy = std::shared_ptr<Book>(new Book(*this)); copy->pages = newPages; return copy;
}


DataResult Book::cacheData(const QString &key,const QByteArray &data)const{QMutexLocker lock(&m_encodedCache->mutex);const int cost=int((data.size()+1023)/1024);if(cost<=m_encodedCache->values.maxCost())m_encodedCache->values.insert(key,new QByteArray(data),cost);return {data,{}};}
void Book::clearEncodedCache()const{QMutexLocker lock(&m_encodedCache->mutex);m_encodedCache->values.clear();m_encodedCache->cursor.reset();}

Book::OpenResult Book::fromPages(const QVector<BookPage> &pages){if(pages.isEmpty()||pages.size()>100000)return {{},QCoreApplication::translate("KomaMimic", "生成ページ数が範囲外です。")};qint64 used=0;auto book=std::shared_ptr<Book>(new Book);book->format=QT_TRANSLATE_NOOP("KomaMimic", "生成画像");book->path=QT_TRANSLATE_NOOP("KomaMimic", "クリップボード（未保存）");for(auto page:pages){used+=page.inlineData.size();if(page.inlineData.isEmpty()||page.inlineData.size()>MaxEncodedBytes||used>128*1024*1024)return {{},QCoreApplication::translate("KomaMimic", "生成画像の容量が上限を超えています。")};if(page.identity.isEmpty())page.identity=QUuid::createUuid().toString(QUuid::WithoutBraces);book->pages.append(page);}return {book,{}};}

bool browserLess(const QFileInfo &a, const QFileInfo &b, int sort, bool reverse) {
        if (a.isDir() != b.isDir()) return a.isDir();
        int order=0;
        if(sort==1 && a.lastModified()!=b.lastModified()) order=a.lastModified()<b.lastModified() ? -1 : 1;
        else if(sort==2 && a.suffix().compare(b.suffix(),Qt::CaseInsensitive)!=0) order=a.suffix().compare(b.suffix(),Qt::CaseInsensitive);
        else if(sort==3 && a.size()!=b.size()) order=a.size()<b.size() ? -1 : 1;
        else order=naturalLess(a.fileName(),b.fileName()) ? -1 : naturalLess(b.fileName(),a.fileName()) ? 1 : 0;
        return reverse ? order>0 : order<0;
}

bool Book::hasArchivedText() const {
    return std::any_of(pages.cbegin(), pages.cend(), [](const BookPage &page) { return bool(page.archivedText); });
}

QString Book::entryPath(int index) const {
    if (index < 0 || index >= pages.size() || pages[index].archiveEntryId.isEmpty()) return {};
    return archiveEntryPath(physicalBookPath(path), pages[index].archiveEntryId, pages[index].name);
}
Book::OpenResult Book::openArchiveEntry(const QString &path, const NamePolicy &policy, const OpenOptions &options) {
    const auto address = parseArchiveEntryPath(path);
    auto listingOptions = options; listingOptions.archiveAsPages = false; listingOptions.archiveImagesOnly = false;
    listingOptions.nestedArchives = false;
    const auto listing = Book::open(address.archive, policy, listingOptions);
    if (!listing.book) return listing;
    int selected = -1;
    for (int i = 0; i < listing.book->pages.size(); ++i)
        if (listing.book->pages[i].archiveEntryId == address.id) { selected = i; break; }
    const auto missing = QCoreApplication::translate("KomaMimic", "書庫内のファイルが見つかりません。開き直してください。");
    if (selected < 0 || QFileInfo(listing.book->pages[selected].name).suffix().compare(QFileInfo(address.name).suffix(), Qt::CaseInsensitive) != 0)
        return {{}, missing};
    const auto &entry = listing.book->pages[selected];
    if (!isArchiveTextPath(entry.name)) {
        auto imagesOnly = options; imagesOnly.archiveImagesOnly = true; imagesOnly.archiveAsPages = false;
        imagesOnly.nestedArchives = false;
        const auto result = Book::open(address.archive, policy, imagesOnly);
        if (!result.book) return result;
        auto book = std::shared_ptr<Book>(new Book(*result.book)); book->path = QFileInfo(path).absoluteFilePath(); book->archiveParent = listing.book;
        bool found = false;
        for (int i = 0; i < book->pages.size(); ++i) if (book->pages[i].archiveEntryId == address.id) {
            book->initialPage = i; found = true; break;
        }
        return found ? OpenResult{book, {}} : OpenResult{{}, missing};
    }
    if (entry.encodedSize < 0 || entry.encodedSize > 16 * 1024 * 1024)
        return {{}, QCoreApplication::translate("KomaMimic", "テキストは16 MiBまでです。")};
    const auto data = listing.book->encoded(selected, options.cancel);
    if (!data.error.isEmpty()) return {{}, data.error};
    QString error; const auto text = decodeDocumentText(data.bytes, options.text, error);
    if (!error.isEmpty()) return {{}, error};
    auto source = std::make_shared<ArchiveTextSource>(); source->book = listing.book;
    source->index = selected; source->options = options.text;
    QSet<QString> ambiguous;
    for (int i = 0; i < listing.book->pages.size(); ++i) {
        const auto &page = listing.book->pages[i]; if (!isImagePath(page.name)) continue;
        const auto key = publicationPath({}, page.name); if (key.isEmpty() || ambiguous.contains(key)) continue;
        if (source->images.contains(key)) { source->images.remove(key); ambiguous.insert(key); }
        else source->images[key] = i;
    }
    const auto publication = readTextPublication(text, entry.name, options.text, archiveTextResources(*source, options.cancel));
    if (!publication->error.isEmpty()) return {{}, publication->error};
    if (options.cancel && options.cancel()) return {{}, QCoreApplication::translate("KomaMimic", "読み込みを取り消しました。")};
    auto book = std::shared_ptr<Book>(new Book); book->path = QFileInfo(path).absoluteFilePath();
    book->format = QFileInfo(entry.name).suffix().toUpper(); book->namePolicy = policy;
    book->openOptions = options; book->openOptions.cancel = {}; book->documentContent = publication; book->archiveParent = listing.book;
    book->m_encodedCache = listing.book->m_encodedCache;
    book->publication = publication;
    auto page = entry; page.document = true; page.archivedText = source;
    page.filePath = book->path; page.documentSection = "text.html"; page.documentPage = 0;
    page.documentOffset = 0; page.identity = "text:text.html:0";
    book->pages.append(std::move(page));
    return {book, {}};
}
