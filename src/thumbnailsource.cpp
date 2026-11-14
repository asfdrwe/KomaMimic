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

std::atomic<quint64> ThumbnailActivity::metadataChecks{0},ThumbnailActivity::folderScans{0},ThumbnailActivity::generations{0};
static QByteArray sourceRecord(const QString &path,const QJsonObject &entry) {
    if(!entry.contains("bytes") || !entry.contains("modified"))return {};
    return QJsonDocument(QJsonObject{{"path",path},{"resolved",entry.value("resolved").toString(path)},
        {"bytes",entry.value("bytes")},{"modified",entry.value("modified")}}).toJson(QJsonDocument::Compact);
}
static QString recordStamp(const QString &path,const QJsonObject &entry) {
    const auto record=sourceRecord(path,entry);
    return record.isEmpty()?QString{}:QString::fromLatin1(QCryptographicHash::hash(record,QCryptographicHash::Sha256).toHex());
}
QJsonObject thumbnailEntry(const QFileInfo &entry,const QString &resolvedDirectory) {
    const bool dir=entry.isDir();
    QJsonObject value{{"directory",dir},{"modified",QString::number(entry.lastModified().toMSecsSinceEpoch())}};
    if(!dir) {
        value["bytes"]=QString::number(entry.size());
        // Match Book's opened snapshot, including the final file symlink.
        // This runs while collecting source information, never on a PNG hit.
        value["resolved"]=entry.isSymLink()?entry.canonicalFilePath():
            resolvedDirectory.isEmpty()?entry.absoluteFilePath():QDir(resolvedDirectory).filePath(entry.fileName());
        value["stamp"]=recordStamp(entry.absoluteFilePath(),value);
    }
    return value;
}
QJsonObject thumbnailDependencies(const ImageResult &result,const QString &fallback) {
    auto deps=result.thumbnailEntries;
    if(!fallback.isEmpty()) {
        const auto path=QFileInfo(physicalBookPath(fallback)).absoluteFilePath();
        auto item=deps.value(path).toObject();
        item["cacheStamp"]=result.image.text("komamimic-cover-stamp");
        item["cacheCreated"]=QString::number(result.cacheCreatedMs);
        deps[path]=item;
    }
    return deps;
}
void ThumbnailChanges::observe(const QFileInfo &entry,const QString &resolvedDirectory) {
    const auto path=entry.absoluteFilePath();const auto value=thumbnailEntry(entry,resolvedDirectory);
    if(entries.contains(path) && entries.value(path)!=value)changed(path);
    entries[path]=value;entrySerials[path]=serial;
}
void ThumbnailChanges::changed(const QString &path) {notices[path]=++serial;}
static QJsonObject cacheIdentity(const QJsonObject &value) {
    return {{"stamp",value.value("cacheStamp").toString(value.value("stamp").toString())},
            {"created",value.value("cacheCreated")}};
}
bool ThumbnailChanges::needsUpdate(const QJsonObject &deps) const {
    for(auto i=deps.begin();i!=deps.end();++i) {
        if(notices.contains(i.key()))return true;
        if(!entries.contains(i.key()))continue;
        const auto now=entries.value(i.key()),saved=i.value().toObject();
        if(checked.value(i.key())==qMakePair(cacheIdentity(saved),now))continue;
        const auto created=saved.value("cacheCreated").toString().toLongLong();
        if(now.value("directory").toBool()) {
            if(created>0 && now.value("modified").toString().toLongLong()>created)return true;
            continue;
        }
        const auto stamp=saved.value("cacheStamp").toString(saved.value("stamp").toString());
        if(!stamp.isEmpty() && !now.value("stamp").toString().isEmpty()) {
            if(stamp!=now.value("stamp").toString())return true;
        } else if(created>0 && now.value("modified").toString().toLongLong()>created)return true;
    }
    return false;
}
void ThumbnailChanges::acknowledge(const QJsonObject &deps,quint64 through) {
    for(auto i=deps.begin();i!=deps.end();++i)if(notices.value(i.key())<=through) {
        notices.remove(i.key());checked[i.key()]=qMakePair(cacheIdentity(i.value().toObject()),entries.value(i.key()));
    }
}

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
QJsonObject suppliedEntry(const QString &path) {
    const QFileInfo file(path);
    if(!file.isDir())++ThumbnailActivity::metadataChecks;
    return thumbnailEntry(file,file.absoluteDir().canonicalPath());
}
QJsonObject listedFile(const QString &path,const std::function<bool()> &cancel={}) {
    ++ThumbnailActivity::folderScans;
    const QFileInfo lexical(path);QDir parent(lexical.absolutePath());
    const auto resolved=parent.canonicalPath();
    QDirIterator entries(parent.absolutePath(),QDir::Files|QDir::Dirs|QDir::Readable|QDir::NoDotAndDotDot);
    while(entries.hasNext()){if(cancel && cancel())return {};entries.next();if(entries.fileName()==lexical.fileName())return thumbnailEntry(entries.fileInfo(),resolved);}
    return {};
}
QJsonObject baseKey(const QByteArray &source,const NamePolicy &policy) {
    // Rendering inputs, rather than build/toolkit versions, determine identity.
    return {{"revision",2},
        {"source",QString::fromUtf8(source)},{"encoding",policy.legacyEncoding},{"force",policy.force},
        {"width",ThumbnailSize.width()},{"height",ThumbnailSize.height()}};
}
QByteArray digest(const QByteArray &data) {return QCryptographicHash::hash(data,QCryptographicHash::Sha256).toHex();}
bool savedCoverMatches(const QImage &image,const QString &path,const QJsonObject &entry) {
    const auto saved=image.text("komamimic-cover-stamp");
    if(saved.isEmpty())return false;
    if(saved==recordStamp(path,entry))return true;
    // Older link covers fingerprinted the resolved parent plus the link name.
    // Recognize that fingerprint during a requested verification without
    // rewriting existing PNGs or changing their lexical cache keys.
    const QFileInfo file(path);
    if(!file.isSymLink())return false;
    auto legacy=entry;
    legacy["resolved"]=QDir(file.absoluteDir().canonicalPath()).filePath(file.fileName());
    return saved==recordStamp(path,legacy);
}
// Purely lexical: never stat/canonicalize the original archive during a disk-cache hit.
QByteArray coverKey(const QString &path,const NamePolicy &policy) {
    const auto absolute=QDir::cleanPath(QFileInfo(physicalBookPath(path)).absoluteFilePath());
    auto key=baseKey(absolute.toUtf8(),policy);
    key["revision"]=3;
    key["kind"]="cover";
    if(parseArchiveEntryPath(path).valid())key["entry"]=bookIdentityPath(path);
    return QJsonDocument(key).toJson(QJsonDocument::Compact);
}
ImageResult cover(const QString &path,const NamePolicy &policy,const std::shared_ptr<ThumbnailDiskCache> &cache,
                  const std::function<bool()> &cancel,bool refresh,int depth,int maximum,bool cachedOnly,bool verify,
                  QJsonObject info={},ImageResult fallback={}) {
    if(cancel && cancel())return {};
    const auto bytes=coverKey(path,policy);
    auto old=refresh?ImageResult{}:cache->lookup(bytes,cancel);
    if(cachedOnly || (!refresh && !verify && (!old.image.isNull() || !old.error.isEmpty())))return old;
    if(!old.error.isEmpty())return old;
    const auto physical=QFileInfo(physicalBookPath(path)).absoluteFilePath();
    if(info.isEmpty())info=verify?listedFile(physical,cancel):suppliedEntry(physical);
    if(info.value("directory").toBool()) {
        if(depth>maximum)return old;
        ++ThumbnailActivity::folderScans;
        const QFileInfo directory(physical);const auto resolved=directory.canonicalFilePath();
        QJsonObject seen{{physical,thumbnailEntry(directory)}};
        QList<QPair<QString,QJsonObject>> candidates;
        bool cut=false;
        QDirIterator entries(physical,QDir::Files|QDir::Dirs|QDir::NoDotAndDotDot|QDir::Readable);
        while(entries.hasNext()) {
            if(cancel && cancel())return {};
            entries.next();const auto entry=entries.fileInfo();
            const bool dir=entry.isDir();
            if(dir && depth>=maximum){cut=true;continue;}
            if(!dir && !isImagePath(entry.fileName()) && !isArchivePath(entry.fileName()) && !isDocumentPath(entry.fileName()))continue;
            auto item=qMakePair(entry.absoluteFilePath(),thumbnailEntry(entry,resolved));
            auto before=[](const auto &x,const auto &y){
                const bool xd=x.second.value("directory").toBool(),yd=y.second.value("directory").toBool();
                return xd!=yd?!xd:naturalLess(QFileInfo(x.first).fileName(),QFileInfo(y.first).fileName());
            };
            candidates.insert(std::upper_bound(candidates.begin(),candidates.end(),item,before),item);
            if(candidates.size()>16)candidates.removeLast();
        }
        QString failure;
        const auto previous=old.image.isNull()?fallback:old;
        for(const auto &candidate:candidates) {
            if(cancel && cancel())return {};
            seen[candidate.first]=candidate.second;
            // Compare the candidate directly using information already obtained
            // by directory enumeration. No source stat/open or PNG rewrite.
            if(!refresh && !previous.image.isNull() && !candidate.second.value("stamp").toString().isEmpty() &&
               candidate.second.value("stamp").toString()==previous.image.text("komamimic-cover-stamp")) {
                auto result=previous;result.thumbnailEntries=seen;return result;
            }
            auto result=cover(candidate.first,policy,cache,cancel,refresh,depth+1,maximum,false,true,candidate.second,previous);
            cut=cut || result.thumbnailCutoff;
            for(auto i=result.thumbnailEntries.begin();i!=result.thumbnailEntries.end();++i)seen[i.key()]=i.value();
            if(result.image.isNull()){if(!result.error.isEmpty())failure=result.error;continue;}
            if(!refresh && !old.image.isNull() && old.image.text("komamimic-cover-stamp")==result.image.text("komamimic-cover-stamp")) {
                old.thumbnailEntries=seen;return old;
            }
            result.thumbnailEntries=seen;
            if(cancel && cancel())return result;
            return cache->image(bytes,[result]{return result;},cancel,true);
        }
        if(!previous.image.isNull() && (cut || !failure.isEmpty())) {
            auto result=previous;result.thumbnailEntries=seen;result.error=failure;return result;
        }
        ImageResult result;result.thumbnailEntries=seen;result.error=failure;result.thumbnailCutoff=cut;
        if(failure.isEmpty() && !cut && !(cancel && cancel()))cache->invalidate(bytes,cancel);
        return result;
    }
    if(verify && (info.isEmpty() || info.value("_refresh").toBool()))info=listedFile(physical,cancel);
    if(info.isEmpty())return {{},QCoreApplication::translate("KomaMimic", "表紙画像を読めません。")};
    const auto stamp=recordStamp(physical,info);
    const auto previous=old.image.isNull()?fallback:old;
    if(!refresh && !previous.image.isNull() && (savedCoverMatches(previous.image,physical,info) ||
       (previous.image.text("komamimic-cover-stamp").isEmpty() && previous.cacheCreatedMs>0 && info.value("modified").toString().toLongLong()<=previous.cacheCreatedMs))) {
        auto result=previous;result.thumbnailEntries={{physical,info}};return result;
    }
    const auto failureKey=bytes+stamp.toUtf8();
    if(!refresh)if(const auto error=cache->failedGeneration(failureKey);!error.isEmpty()) {
        ImageResult result{{},error};result.thumbnailEntries={{physical,info}};return result;
    }
    auto generate=[&]() -> ImageResult {
        if(cancel && cancel())return {};
        ++ThumbnailActivity::generations;
        ImageResult result;
        if(isImagePath(path) && !parseArchiveEntryPath(path).valid()) {
            QFile file(path);
            if(info.value("bytes").toString().toLongLong()>MaxEncodedBytes || !file.open(QIODevice::ReadOnly))result.error=QCoreApplication::translate("KomaMimic", "表紙画像を読めません。");
            else result=decodeImage(file.read(MaxEncodedBytes+1),QFileInfo(path).fileName(),ThumbnailSize,0,{},cancel);
        } else if(QFileInfo(path).suffix().compare("epub",Qt::CaseInsensitive)==0)result=epubCover(path,ThumbnailSize,cancel);
        else {
            OpenOptions options;options.cancel=cancel;const auto opened=Book::open(path,policy,options);
            result=opened.book?opened.book->preview(0,ThumbnailSize,cancel):ImageResult{{},opened.error};
        }
        if(!result.image.isNull())result.image.setText("komamimic-cover-stamp",stamp);
        result.thumbnailEntries={{physical,info}};
        if(!result.error.isEmpty() && !(cancel && cancel()))cache->rememberFailure(failureKey,result.error);
        return result;
    };
    return cache->image(bytes,generate,cancel,true);
}
}
ImageResult cachedCover(const QString &path,const NamePolicy &policy,const std::shared_ptr<ThumbnailDiskCache> &cache,
                        const std::function<bool()> &cancel,bool refresh,int depth,bool cachedOnly,bool verify,int maxDepth,const QJsonObject &observed) {
    const auto generation=cache->generation();
    const auto cancelled=[cache,generation,cancel]{return (cancel && cancel()) || cache->generation()!=generation;};
    return cover(path,policy,cache,cancelled,refresh,depth,std::clamp(maxDepth,0,8),cachedOnly,verify,observed);
}
ImageResult cachedPageThumbnail(const std::shared_ptr<const Book> &book,int index,
                                const std::shared_ptr<ThumbnailDiskCache> &cache,
                                const std::function<bool()> &cancel,bool refresh,bool cachedOnly,bool verify,int maxDepth,const QJsonObject &observed) {
    if(!book || index<0 || index>=book->pages.size()) return {{},QCoreApplication::translate("KomaMimic", "ページが範囲外です。")};
    const auto &page=book->pages[index];
    const auto source=book->thumbnailSource(index);
    const auto path=QJsonDocument::fromJson(source).object().value("path").toString(
        page.archivedText?book->path:book->filesystem || page.document?page.filePath:book->path);
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
    if(cachedOnly || (!verify && !refresh)) {
        if(refresh)return {};
        auto hit=cache->lookup(bytes,cancel);
        if(hit.image.isNull())hit=cache->lookup(legacy,cancel);
        if(cachedOnly || !hit.image.isNull() || !hit.error.isEmpty())return hit;
    }
    Q_UNUSED(maxDepth);
    auto actual=observed;
    if(verify && (actual.isEmpty() || actual.value("_refresh").toBool()))actual=listedFile(QFileInfo(physicalBookPath(path)).absoluteFilePath(),cancel);
    const auto freshSource=sourceRecord(QFileInfo(physicalBookPath(path)).absoluteFilePath(),actual);
    if(!refresh && !freshSource.isEmpty()) {
        auto hit=cache->lookup(bytes,cancel);
        if(!hit.image.isNull() && hit.image.text("komamimic-cover-stamp")==QString::fromLatin1(digest(freshSource))) {
            hit.thumbnailEntries={{QFileInfo(physicalBookPath(path)).absoluteFilePath(),actual}};return hit;
        }
    }
    // Cached pixels are provisional. Validation reopens changed sources before
    // using any offsets from a Book snapshot.
    if(!freshSource.isEmpty() && freshSource!=source) {
        const auto opened=book->reopenedForThumbnail(index,cancel);
        if(!opened.book)return {{},opened.error};
        const auto &old=book->pages[index];
        for(int i=0;i<opened.book->pages.size();++i) {
            const auto &page=opened.book->pages[i];
            if((!old.sourceBook || old.name==page.name) &&
               (old.archiveEntryId.isEmpty()?page.name==old.name:page.archiveEntryId==old.archiveEntryId) && page.documentPage==old.documentPage && page.portion==old.portion && page.pdfResource==old.pdfResource) {
                if(!opened.book->sourceUnchanged(i))return book->image(index,ThumbnailSize,cancel);
                // A stale observation (or a source changing during open) must
                // not reopen recursively with the same mismatched record.
                if(opened.book->thumbnailSource(i)!=freshSource)
                    return {{},QCoreApplication::translate("KomaMimic", "画像・書庫が変更されています。開き直してください。")};
                return cachedPageThumbnail(opened.book,i,cache,cancel,refresh,false,true,0,actual);
            }
        }
        return book->image(index,ThumbnailSize,cancel);
    }
    if(!refresh && cacheable) {
        const auto hit=cache->lookup(bytes,cancel);
        if(!hit.image.isNull() && hit.image.text("komamimic-cover-stamp")==stamp)return hit;
    }
    QByteArray reusableData;
    if(!refresh && page.sourceBook && page.inlineData.isEmpty() && !page.document) {
        // Older container pages included a payload hash in their PNG key.
        // Recover that key only on a visible miss, never in cache-only lookup.
        const auto data=book->encoded(index,cancel);
        if(!data.error.isEmpty())return {{},data.error};
        reusableData=data.bytes;
        auto oldKey=key;oldKey["inline"]=QString::fromLatin1(digest(data.bytes));
        const auto oldSource=book->filesystem?QByteArray{}:source;
        const auto oldPath=book->filesystem?page.filePath:path;
        oldKey["source"]=QFileInfo(physicalBookPath(oldPath)).absoluteFilePath();
        auto hit=cache->lookup(QJsonDocument(oldKey).toJson(QJsonDocument::Compact),cancel);
        if(hit.image.isNull()) {
            oldKey["revision"]=2;oldKey["source"]=QString::fromUtf8(oldSource);
            oldKey["identity"]=page.identity;oldKey["ordinal"]=QString::number(page.archiveIndex);
            hit=cache->lookup(QJsonDocument(oldKey).toJson(QJsonDocument::Compact),cancel);
        }
        if(!hit.image.isNull()) {
            hit.image.setText("komamimic-cover-stamp",stamp);
            hit.thumbnailEntries={{QFileInfo(physicalBookPath(path)).absoluteFilePath(),QJsonDocument::fromJson(source).object()}};
            return hit; // Keep the saved PNG and its creation time unchanged.
        }
    }
    const auto generate=[&] {
        auto result=refresh?ImageResult{}:cache->lookup(legacy,cancel);
        if(result.image.isNull()) {
            ++ThumbnailActivity::generations;
            if(reusableData.isEmpty())result=book->image(index,ThumbnailSize,cancel);
            else {
                // Reuse the selected payload even when the file cache is off.
                auto decodedPage=page;decodedPage.inlineData=reusableData;decodedPage.sourceBook.reset();
                result=book->withPages({decodedPage})->image(0,ThumbnailSize,cancel);
            }
        }
        if(!result.image.isNull()) {
            result.image.setText("komamimic-cover-stamp",stamp);
            const auto json=QJsonDocument::fromJson(source).object();
            QJsonObject attr{{"directory",false},{"modified",json.value("modified")},{"bytes",json.value("bytes")},{"resolved",json.value("resolved")}};
            attr["stamp"]=stamp;result.thumbnailEntries={{QFileInfo(physicalBookPath(path)).absoluteFilePath(),attr}};
        }
        return result;
    };
    return cache->image(bytes,generate,cancel,true);

}
ImageResult cachedCollectionCover(const QString &path,const NamePolicy &policy,const QJsonObject &metadata,bool spine,
                             const std::shared_ptr<ThumbnailDiskCache> &cache,const std::function<bool()> &cancel,bool cachedOnly,bool verify,int maxDepth,const QJsonObject &observed) {
    if(cancel && cancel()) return {};
    const QString field=spine && metadata.contains("spinePng") ? "spine" : "cover";
    const auto encoded=metadata.value(field+"Png").toString().toLatin1();
    // Registered crops are already persistent in the library DB.
    if(!encoded.isEmpty() && encoded.size()<2*1024*1024) {
        const auto image=decodeImage(QByteArray::fromBase64(encoded),"cover.png",ThumbnailSize).image;
        if(!image.isNull()) return {image,{}};
    }
    const int page=metadata.value(field).toInt();
    if(page==0 || QFileInfo(path).suffix().compare("epub",Qt::CaseInsensitive)==0) return cachedCover(path,policy,cache,cancel,false,0,cachedOnly,verify,maxDepth,observed);
    auto key=baseKey(QFileInfo(physicalBookPath(path)).absoluteFilePath().toUtf8(),policy);
    key["revision"]=3;key["kind"]="registered-page";key["page"]=page;
    const auto bytes=QJsonDocument(key).toJson(QJsonDocument::Compact);
    if(cachedOnly || !verify) {
        auto hit=cache->lookup(bytes,cancel);if(cachedOnly || !hit.image.isNull() || !hit.error.isEmpty())return hit;
    }
    const auto info=observed.isEmpty() || observed.value("_refresh").toBool()?listedFile(physicalBookPath(path),cancel):observed;
    const auto source=sourceRecord(QFileInfo(physicalBookPath(path)).absoluteFilePath(),info);
    const auto stamp=QString::fromLatin1(digest(source));
    const auto old=cache->lookup(bytes,cancel).image;
    if(!old.isNull() && savedCoverMatches(old,QFileInfo(physicalBookPath(path)).absoluteFilePath(),info))return {old,{}};
    return cache->image(bytes,[&]() -> ImageResult {
        ++ThumbnailActivity::generations;
        OpenOptions options;options.cancel=cancel;
        const auto result=Book::open(path,policy,options);
        if(!result.book) return {{},result.error};
        if(result.book->pages.isEmpty()) return {};
        auto image=result.book->image(std::clamp(page,0,int(result.book->pages.size())-1),ThumbnailSize,cancel);
        image.image.setText("komamimic-cover-stamp",stamp);
        image.thumbnailEntries={{QFileInfo(physicalBookPath(path)).absoluteFilePath(),info}};return image;
    },cancel,true);
}
