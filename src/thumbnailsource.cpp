// SPDX-License-Identifier: GPL-3.0-only
#include "thumbnailsource.h"
#include "archivepath.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <algorithm>

ThumbnailNamePolicies::ThumbnailNamePolicies(const QSettings &settings) {
    for(const auto &key:settings.allKeys()) {
        if(!key.startsWith("archiveNames/") || !key.endsWith("/encoding"))continue;
        const auto group=key.left(key.size()-9);
        NamePolicy policy{settings.value(key,"CP932").toString(),settings.value(group+"/force",false).toBool()};
        if(!legacyEncodings().contains(policy.legacyEncoding))continue;
        saved.insert(group,policy);if(!previews.contains(policy))previews.append(policy);
    }
}
QString ThumbnailNamePolicies::settingsKey(const QString &path) {
    const QFileInfo info(physicalBookPath(path));
    const QByteArray identity=info.absoluteFilePath().toUtf8()+'\0'+QByteArray::number(info.size())+':'+QByteArray::number(info.lastModified().toMSecsSinceEpoch());
    return "archiveNames/"+QString::fromLatin1(QCryptographicHash::hash(identity,QCryptographicHash::Sha256).toHex());
}
NamePolicy ThumbnailNamePolicies::resolve(const QString &path) const {
    return saved.isEmpty()?NamePolicy{}:saved.value(settingsKey(path));
}

namespace {
const QSize ThumbnailSize{320,480};
QByteArray fingerprint(const QString &path,bool directory=false) {
    const QFileInfo file(path);
    if(directory ? !file.isDir() : !file.isFile()) return {};
    return QJsonDocument(QJsonObject{{"path",file.absoluteFilePath()},{"resolved",file.canonicalFilePath()},
        {"bytes",QString::number(file.size())},{"modified",QString::number(file.lastModified().toMSecsSinceEpoch())}}).toJson(QJsonDocument::Compact);
}
QJsonObject baseKey(const QByteArray &source,const NamePolicy &policy) {
    // Rendering inputs, rather than build/toolkit versions, determine identity.
    return {{"revision",2},
        {"source",QString::fromUtf8(source)},{"encoding",policy.legacyEncoding},{"force",policy.force},
        {"width",ThumbnailSize.width()},{"height",ThumbnailSize.height()}};
}
QByteArray digest(const QByteArray &data) {return QCryptographicHash::hash(data,QCryptographicHash::Sha256).toHex();}
// Purely lexical: never stat/canonicalize the original archive during a disk-cache hit.
QByteArray coverKey(const QString &path,const NamePolicy &policy) {
    const auto absolute=QDir::cleanPath(QFileInfo(physicalBookPath(path)).absoluteFilePath());
    auto key=baseKey(absolute.toUtf8(),policy);
    key["revision"]=3;
    key["kind"]="cover";
    if(parseArchiveEntryPath(path).valid())key["entry"]=bookIdentityPath(path);
    return QJsonDocument(key).toJson(QJsonDocument::Compact);
}
ImageResult cover(const QString &path,const NamePolicy &policy,
                        const std::shared_ptr<ThumbnailDiskCache> &cache,
                        const std::function<bool()> &cancel,bool refresh,int depth,bool cachedOnly,
                        std::function<bool()> *sourceUnchanged=nullptr,const QImage &fallback={}) {
    if(cancel && cancel()) return {};
    const auto bytes=coverKey(path,policy);
    // The fast lane must not even query QFileInfo::isDir/size/mtime/canonicalFilePath.
    if(cachedOnly)return refresh ? ImageResult{} : cache->lookup(bytes,cancel);
    const QFileInfo info(physicalBookPath(path));
    if(info.isDir()) {
        // Save a path-keyed folder cover as well as the actual source image.
        // Cache-only display never enumerates the directory. Validation still
        // repeats the bounded candidate search: a directory's mtime alone
        // cannot detect edits inside existing files or nested directories.
        if(depth>2) return {};
        const auto directorySource=fingerprint(path,true);
        const auto old=refresh?QImage{}:cache->lookup(bytes,cancel).image;
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
        QString failure;
        for(const auto &file:files) {
            if(cancel && cancel()) return {};
            std::function<bool()> childUnchanged;
            auto result=cover(file.absoluteFilePath(),policy,cache,cancel,refresh,depth+1,false,&childUnchanged,
                              old.isNull()?fallback:old);
            if(result.image.isNull()){if(!result.error.isEmpty())failure=result.error;continue;}
            const auto unchanged=[path,directorySource,childUnchanged] {
                return childUnchanged && childUnchanged() && fingerprint(path,true)==directorySource;
            };
            if(sourceUnchanged)*sourceUnchanged=unchanged;
            const auto stamp=result.image.text(QStringLiteral("komamimic-cover-stamp"));
            // Text previews and rejected/changed sources must not become
            // persistent simply because their parent folder has a stable key.
            if(stamp.isEmpty() || !unchanged()) {
                if(!(cancel && cancel()) && !cache->invalidate(bytes,cancel))result.error=QCoreApplication::translate("KomaMimic", "サムネイルの保存先を使用できません。");
                return result;
            }
            if(!old.isNull() && old.text(QStringLiteral("komamimic-cover-stamp"))==stamp)return {old,{}};
            return cache->image(bytes,[result]{return result;},cancel,true,unchanged);
        }
        if(cancel && cancel())return {};
        if(!failure.isEmpty())return {{},failure};
        if(!cache->invalidate(bytes,cancel))return {{},QCoreApplication::translate("KomaMimic", "サムネイルの保存先を使用できません。")};
        return {};
    }
    const auto source=fingerprint(physicalBookPath(path));
    qCDebug(thumbnailCacheLog) << "cover-key" << digest(bytes)
                             << "encoding" << policy.legacyEncoding << "force" << policy.force;
    auto cacheableCover = std::make_shared<bool>(true);
    const auto generate=[&]() -> ImageResult {
        if(cancel && cancel()) return {};
        qCDebug(thumbnailCacheLog)<<"cover-regenerate"<<digest(bytes);
        if(isImagePath(path) && !parseArchiveEntryPath(path).valid()) {
            QFile file(path);
            if(info.size()>MaxEncodedBytes || !file.open(QIODevice::ReadOnly)) return {{},QCoreApplication::translate("KomaMimic", "表紙画像を読めません。")};
            return decodeImage(file.read(MaxEncodedBytes+1),info.fileName(),ThumbnailSize,0,{},cancel);
        }
        if(info.suffix().compare("epub",Qt::CaseInsensitive)==0) return epubCover(path,ThumbnailSize,cancel);
        OpenOptions options;options.cancel=cancel;
        const auto opened=Book::open(path,policy,options);
        if(!opened.book) return {{},opened.error};
        *cacheableCover = !opened.book->hasArchivedText() && !opened.book->archiveDirectory;
        return opened.book->preview(0,ThumbnailSize,cancel);
    };
    // First-page text previews depend on external images and installed fonts.
    const auto cacheBytes=source.isEmpty() || (isTextDocumentPath(path) && info.suffix().compare("epub",Qt::CaseInsensitive)!=0)
        ? QByteArray{} : bytes;
    const auto unchanged=[path,source,cacheableCover,cacheBytes] {
        return !cacheBytes.isEmpty() && *cacheableCover && fingerprint(physicalBookPath(path))==source;
    };
    if(sourceUnchanged)*sourceUnchanged=unchanged;
    const auto stamp=QString::fromLatin1(digest(source));
    if(!refresh && !cacheBytes.isEmpty()) {
        const auto old=cache->lookup(cacheBytes,cancel);
        if(!old.image.isNull() && old.image.text(QStringLiteral("komamimic-cover-stamp"))==stamp)
            return old; // Validation finished. No archive decompression is necessary.
        // The folder PNG itself is sufficient when its chosen source is still
        // current, even if the separate leaf PNG was evicted by the byte limit.
        if(!fallback.isNull() && fallback.text(QStringLiteral("komamimic-cover-stamp"))==stamp)return {fallback,{}};
    }
    const auto stamped=[&]() -> ImageResult {
        auto result=generate();
        if(!result.image.isNull() && !cacheBytes.isEmpty())
            result.image.setText(QStringLiteral("komamimic-cover-stamp"),stamp);
        return result;
    };
    // cacheBytes is stable. A stale PNG must not short-circuit re-generation.
    auto result=cache->image(cacheBytes,stamped,cancel,true,unchanged);
    if(!*cacheableCover && !(cancel && cancel()) && !cache->invalidate(bytes,cancel))result.error=QCoreApplication::translate("KomaMimic", "サムネイルの保存先を使用できません。");
    return result;
}
}
ImageResult cachedCover(const QString &path,const NamePolicy &policy,
                        const std::shared_ptr<ThumbnailDiskCache> &cache,
                        const std::function<bool()> &cancel,bool refresh,int depth,bool cachedOnly) {
    const auto generation=cache->generation();
    const auto cancelled=[cache,generation,cancel] {
        return (cancel && cancel()) || cache->generation()!=generation;
    };
    return cover(path,policy,cache,cancelled,refresh,depth,cachedOnly);
}
ImageResult cachedPageThumbnail(const std::shared_ptr<const Book> &book,int index,
                                const std::shared_ptr<ThumbnailDiskCache> &cache,
                                const std::function<bool()> &cancel,bool refresh,bool cachedOnly) {
    if(!book || index<0 || index>=book->pages.size()) return {{},QCoreApplication::translate("KomaMimic", "ページが範囲外です。")};
    // Cached pixels are provisional. Validation reopens changed sources before
    // using any offsets from a Book snapshot.
    if(!cachedOnly && !book->sourceUnchanged(index)) {
        const auto opened=book->reopenedForThumbnail(index,cancel);
        if(!opened.book)return {{},opened.error};
        const auto &old=book->pages[index];
        for(int i=0;i<opened.book->pages.size();++i) {
            const auto &page=opened.book->pages[i];
            if((old.archiveEntryId.isEmpty()?page.name==old.name:page.archiveEntryId==old.archiveEntryId) && page.documentPage==old.documentPage && page.portion==old.portion && page.pdfResource==old.pdfResource) {
                if(!opened.book->sourceUnchanged(i))return book->image(index,ThumbnailSize,cancel);
                return cachedPageThumbnail(opened.book,i,cache,cancel,refresh,false);
            }
        }
        return book->image(index,ThumbnailSize,cancel);
    }
    const auto &page=book->pages[index];
    const auto path=page.archivedText ? book->path : book->filesystem || page.document ? page.filePath : book->path;
    const auto source=book->thumbnailSource(index);
    auto key=baseKey(source,book->namePolicy);key["kind"]="page";
    qCDebug(thumbnailCacheLog) << "page-key" << digest(source);
    key["identity"]=page.identity;key["name"]=page.name;key["ordinal"]=QString::number(page.archiveIndex);
    key["page"]=page.documentPage;key["pdfResource"]=page.pdfResource;
    key["portion"]=QJsonArray{page.portion.x(),page.portion.y(),page.portion.width(),page.portion.height()};
    key["inline"]=QString::fromLatin1(digest(page.inlineData));
    key["password"]=QString::fromLatin1(digest(book->openOptions.password.toUtf8()));
    const bool cacheable=!page.archivedText && (!source.isEmpty() || !page.inlineData.isEmpty()) && !isTextDocumentPath(page.filePath);
    const auto legacy=cacheable?QJsonDocument(key).toJson(QJsonDocument::Compact):QByteArray{};
    // Stable page identity lets a changed source paint its old preview first,
    // including after restart. Freshness belongs in the pixels, not this key.
    key["source"]=QFileInfo(physicalBookPath(path)).absoluteFilePath();key["revision"]=3;
    if(!page.archiveEntryId.isEmpty())key["identity"]=page.archiveEntryId;
    key.remove("ordinal");
    const auto bytes=cacheable?QJsonDocument(key).toJson(QJsonDocument::Compact):QByteArray{};
    const auto stamp=QString::fromLatin1(digest(source));
    if(cachedOnly) {
        if(refresh)return {};
        auto hit=cache->lookup(bytes,cancel);
        return hit.image.isNull()?cache->lookup(legacy,cancel):hit;
    }
    if(!refresh && cacheable) {
        const auto hit=cache->lookup(bytes,cancel);
        if(!hit.image.isNull() && hit.image.text("komamimic-cover-stamp")==stamp)return hit;
    }
    const auto generate=[&] {
        auto result=refresh?ImageResult{}:cache->lookup(legacy,cancel);
        if(result.image.isNull())result=book->image(index,ThumbnailSize,cancel);
        if(!result.image.isNull())result.image.setText("komamimic-cover-stamp",stamp);
        return result;
    };
    return cache->image(bytes,generate,cancel,true,
        [book,index,path,source]{return book->sourceUnchanged(index) && fingerprint(physicalBookPath(path))==source;});

}
ImageResult cachedCollectionCover(const QString &path,const NamePolicy &policy,const QJsonObject &metadata,bool spine,
                             const std::shared_ptr<ThumbnailDiskCache> &cache,const std::function<bool()> &cancel,bool cachedOnly) {
    if(cancel && cancel()) return {};
    const QString field=spine && metadata.contains("spinePng") ? "spine" : "cover";
    const auto encoded=metadata.value(field+"Png").toString().toLatin1();
    // Registered crops are already persistent in the library DB.
    if(!encoded.isEmpty() && encoded.size()<2*1024*1024) {
        const auto image=decodeImage(QByteArray::fromBase64(encoded),"cover.png",ThumbnailSize).image;
        if(!image.isNull()) return {image,{}};
    }
    const int page=metadata.value(field).toInt();
    if(page==0 || QFileInfo(path).suffix().compare("epub",Qt::CaseInsensitive)==0) return cachedCover(path,policy,cache,cancel,false,0,cachedOnly);
    auto key=baseKey(QFileInfo(physicalBookPath(path)).absoluteFilePath().toUtf8(),policy);
    key["revision"]=3;key["kind"]="registered-page";key["page"]=page;
    const auto bytes=QJsonDocument(key).toJson(QJsonDocument::Compact);
    if(cachedOnly)return cache->lookup(bytes,cancel);
    const auto source=fingerprint(physicalBookPath(path));
    const auto stamp=QString::fromLatin1(digest(source));
    const auto old=cache->lookup(bytes,cancel).image;
    if(!old.isNull() && old.text("komamimic-cover-stamp")==stamp)return {old,{}};
    return cache->image(bytes,[&]() -> ImageResult {
        OpenOptions options;options.cancel=cancel;
        const auto result=Book::open(path,policy,options);
        if(!result.book) return {{},result.error};
        if(result.book->pages.isEmpty()) return {};
        auto image=result.book->image(std::clamp(page,0,int(result.book->pages.size())-1),ThumbnailSize,cancel);
        image.image.setText("komamimic-cover-stamp",stamp);return image;
    },cancel,true,[path,source]{return fingerprint(physicalBookPath(path))==source;});
}
