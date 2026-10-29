// SPDX-License-Identifier: GPL-3.0-only
#include "thumbnailsource.h"
#include "archivepath.h"
#include <QCoreApplication>
#include "scriptreader.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>

namespace {
const QSize ThumbnailSize{320,480};
QByteArray fingerprint(const QString &path) {
    const QFileInfo file(path);
    if(!file.isFile()) return {};
    return QJsonDocument(QJsonObject{{"path",file.absoluteFilePath()},{"resolved",file.canonicalFilePath()},
        {"bytes",QString::number(file.size())},{"modified",QString::number(file.lastModified().toMSecsSinceEpoch())}}).toJson(QJsonDocument::Compact);
}
QJsonObject baseKey(const QByteArray &source,const NamePolicy &policy) {
    return {{"revision",2},{"version",KOMAMIMIC_VERSION},{"qt",qVersion()},
        {"source",QString::fromUtf8(source)},{"encoding",policy.legacyEncoding},{"force",policy.force},
        {"scripts",QString::fromLatin1(scriptReaderFingerprint().toHex())},{"width",ThumbnailSize.width()},{"height",ThumbnailSize.height()}};
}
QByteArray digest(const QByteArray &data) {return QCryptographicHash::hash(data,QCryptographicHash::Sha256).toHex();}
}
ImageResult cachedCover(const QString &path,const NamePolicy &policy,
                        const std::shared_ptr<ThumbnailDiskCache> &cache,
                        const std::function<bool()> &cancel,bool refresh,int depth) {
    if(cancel && cancel()) return {};
    const QFileInfo info(physicalBookPath(path));
    if(info.isDir()) {
        // Cache actual source files, so changing an image within a directory
        // cannot leave its old synthetic folder cover on disk indefinitely.
        if(depth>2) return {};
        const auto before=[](const QFileInfo &a,const QFileInfo &b) {
            if(a.isDir()!=b.isDir()) return !a.isDir();
            return naturalLess(a.fileName(),b.fileName());
        };
        // Keep only the first 16 candidates in the same natural order. A full
        // entryInfoList/sort cannot observe cancellation in a large directory.
        QList<QFileInfo> files;
        QDirIterator entries(path,QDir::Files|QDir::Dirs|QDir::NoDotAndDotDot|QDir::Readable);
        while(entries.hasNext()) {
            if(cancel && cancel()) return {};
            entries.next();const auto file=entries.fileInfo();
            if(!file.isDir() && !isImagePath(file.fileName()) && !isArchivePath(file.fileName()) && !isDocumentPath(file.fileName())) continue;
            if(files.size()==16 && !before(file,files.constLast())) continue;
            files.insert(std::upper_bound(files.begin(),files.end(),file,before),file);
            if(files.size()>16) files.removeLast();
        }
        for(const auto &file:files) {
            if(cancel && cancel()) return {};
            auto result=cachedCover(file.absoluteFilePath(),policy,cache,cancel,refresh,depth+1);
            if(!result.image.isNull()) return result;
        }
        return {};
    }
    const auto source=fingerprint(physicalBookPath(path));
    auto key=baseKey(source,policy);key["kind"]="cover";
    if (parseArchiveEntryPath(path).valid()) key["entry"] = bookIdentityPath(path);
    bool cacheableCover = true;
    const auto generate=[&]() -> ImageResult {
        if(cancel && cancel()) return {};
        if(isImagePath(path) && !parseArchiveEntryPath(path).valid()) {
            QFile file(path);
            if(info.size()>MaxEncodedBytes || !file.open(QIODevice::ReadOnly)) return {{},QCoreApplication::translate("KomaMimic", "表紙画像を読めません。")};
            return decodeImage(file.read(MaxEncodedBytes+1),info.fileName(),ThumbnailSize,0,{},cancel);
        }
        if(info.suffix().compare("epub",Qt::CaseInsensitive)==0) return epubCover(path,ThumbnailSize,cancel);
        OpenOptions options;options.cancel=cancel;
        const auto opened=Book::open(path,policy,options);
        if(!opened.book) return {{},opened.error};
        cacheableCover = !opened.book->hasArchivedText() && !opened.book->archiveDirectory;
        return opened.book->preview(0,ThumbnailSize,cancel);
    };
    // First-page text previews depend on external images and installed fonts.
    const auto bytes=source.isEmpty() || (isTextDocumentPath(path) && info.suffix().compare("epub",Qt::CaseInsensitive)!=0)
        ? QByteArray{} : QJsonDocument(key).toJson(QJsonDocument::Compact);
    return cache->image(bytes,generate,cancel,refresh,[path,source,&cacheableCover]{return cacheableCover && fingerprint(physicalBookPath(path))==source;});
}
ImageResult cachedPageThumbnail(const std::shared_ptr<const Book> &book,int index,
                                const std::shared_ptr<ThumbnailDiskCache> &cache,
                                const std::function<bool()> &cancel,bool refresh) {
    if(!book || index<0 || index>=book->pages.size()) return {{},QCoreApplication::translate("KomaMimic", "ページが範囲外です。")};
    // A disk hit must not bypass Book's stale-source check when an old Book
    // survives while the archive is replaced and another job opens its new version.
    if(!book->sourceUnchanged(index)) return book->image(index,ThumbnailSize,cancel);
    const auto &page=book->pages[index];
    const auto path=page.archivedText ? book->path : book->filesystem || page.document ? page.filePath : book->path;
    const auto source=fingerprint(physicalBookPath(path));
    auto key=baseKey(source,book->namePolicy);key["kind"]="page";
    key["identity"]=page.identity;key["name"]=page.name;key["ordinal"]=QString::number(page.archiveIndex);
    key["page"]=page.documentPage;key["pdfResource"]=page.pdfResource;
    key["portion"]=QJsonArray{page.portion.x(),page.portion.y(),page.portion.width(),page.portion.height()};
    key["inline"]=QString::fromLatin1(digest(page.inlineData));
    key["settings"]=QJsonObject::fromVariantMap(book->openOptions.scriptSettings);
    key["password"]=QString::fromLatin1(digest(book->openOptions.password.toUtf8()));
    const auto bytes=page.archivedText || (source.isEmpty() && page.inlineData.isEmpty()) || isTextDocumentPath(page.filePath)
        ? QByteArray{} : QJsonDocument(key).toJson(QJsonDocument::Compact);
    return cache->image(bytes,[book,index,cancel]{return book->image(index,ThumbnailSize,cancel);},cancel,refresh,
        [book,index,path,source]{return book->sourceUnchanged(index) && fingerprint(physicalBookPath(path))==source;});
}
QImage cachedCollectionCover(const QString &path,const NamePolicy &policy,const QJsonObject &metadata,bool spine,
                             const std::shared_ptr<ThumbnailDiskCache> &cache,const std::function<bool()> &cancel) {
    if(cancel && cancel()) return {};
    const QString field=spine && metadata.contains("spinePng") ? "spine" : "cover";
    const auto encoded=metadata.value(field+"Png").toString().toLatin1();
    // Registered crops are already persistent in the library DB.
    if(!encoded.isEmpty() && encoded.size()<2*1024*1024) {
        const auto image=decodeImage(QByteArray::fromBase64(encoded),"cover.png",ThumbnailSize).image;
        if(!image.isNull()) return image;
    }
    const int page=metadata.value(field).toInt();
    if(page==0 || QFileInfo(path).suffix().compare("epub",Qt::CaseInsensitive)==0) return cachedCover(path,policy,cache,cancel).image;
    const auto source=fingerprint(physicalBookPath(path));auto key=baseKey(source,policy);
    key["kind"]="registered-page";key["page"]=page;
    return cache->image(source.isEmpty()?QByteArray{}:QJsonDocument(key).toJson(QJsonDocument::Compact),[&]() -> ImageResult {
        OpenOptions options;options.cancel=cancel;
        const auto result=Book::open(path,policy,options);
        if(!result.book) return {{},result.error};
        if(result.book->pages.isEmpty()) return {};
        return result.book->image(std::clamp(page,0,int(result.book->pages.size())-1),ThumbnailSize,cancel);
    },cancel,false,[path,source]{return fingerprint(physicalBookPath(path))==source;}).image;
}
