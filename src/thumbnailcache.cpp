// SPDX-License-Identifier: GPL-3.0-only
#include "thumbnailcache.h"
#include <QCoreApplication>
#include <QBuffer>
#include <QColorSpace>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QLockFile>
#include <QMutexLocker>
#include <QPainter>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>

Q_LOGGING_CATEGORY(thumbnailCacheLog, "komamimic.cache", QtWarningMsg)

namespace {
constexpr qint64 MaxCacheFile=4*1024*1024;
bool managed(const QString &name) {
    static const QRegularExpression pattern("^[a-f0-9]{64}\\.png$");
    return pattern.match(name).hasMatch();
}
QImage committedImage(const QString &path) {
    const QFileInfo info(path);
    if(info.isSymLink() || !info.isFile() || info.size()>MaxCacheFile)return {};
    QImageReader reader(path,"PNG");const auto size=reader.size();
    if(size.width()<=0 || size.height()<=0 || size.width()>1024 || size.height()>1024)return {};
    return reader.read();
}
QByteArray revision(const QString &directory) {
    QFile file(directory+"/.revision");
    return file.open(QIODevice::ReadOnly) ? file.read(64) : QByteArray{};
}
}
QString ThumbnailDiskCache::defaultDirectory() {
    const auto root=QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    return root.isEmpty() ? QString{} : QDir(root).filePath("thumbnails-v1");
}
ThumbnailDiskCache::ThumbnailDiskCache(QString directory):m_directory(std::move(directory)) {}
void ThumbnailDiskCache::configure(bool enabled,int megabytes) {
    const auto limit=qint64(std::clamp(megabytes,16,16384))*1024*1024;
    if(m_enabled.load()==enabled && m_limit.load()==limit)return;
    discardPending();
    m_enabled.store(enabled);
    m_limit.store(limit);
}
void ThumbnailDiskCache::discardPending() {
    m_generation.fetch_add(1);
    QMutexLocker guard(&m_pendingMutex);m_pending.clear();m_pendingBytes=0;m_recentImages.clear();
}
void ThumbnailDiskCache::enqueue(Pending pending,bool retry) {
    QMutexLocker guard(&m_pendingMutex);
    if(pending.epoch!=m_generation.load() || !m_enabled.load())return;
    for(auto i=m_pending.begin();i!=m_pending.end();++i)if(i->key==pending.key) {
        // A retry must never displace a newer request for the same key.
        if(retry)return;
        // A timestamp update must not replace an image waiting for persistence.
        if(pending.image.isNull())return;
        m_pendingBytes-=i->image.sizeInBytes();m_pending.erase(i);break;
    }
    const auto cost=pending.image.sizeInBytes();
    constexpr qint64 limit=32LL*1024*1024;
    if(cost>limit)return;
    while(!m_pending.isEmpty() && (m_pendingBytes+cost>limit || m_pending.size()>=256)) {
        m_pendingBytes-=m_pending.first().image.sizeInBytes();m_pending.removeFirst();
        qCDebug(thumbnailCacheLog)<<"deferred-store-dropped";
    }
    if(!pending.image.isNull())m_recentImages.insert(pending.key,new QImage(pending.image),int((cost+1023)/1024));
    m_pendingBytes+=cost;m_pending.append(std::move(pending));
}
bool ThumbnailDiskCache::hasPendingWrites() const {
    QMutexLocker guard(&m_pendingMutex);return !m_pending.isEmpty();
}
ImageResult ThumbnailDiskCache::lookup(const QByteArray &key,const std::function<bool()> &cancel) {
    const auto epoch=m_generation.load();
    if(!m_enabled.load() || key.isEmpty() || (cancel && cancel()) || m_directory.isEmpty() || QFileInfo(m_directory).isSymLink())return {};
    if(m_deferred.load()) {
        QImage recent;
        {QMutexLocker guard(&m_pendingMutex);if(const auto *pixels=m_recentImages.object(key))recent=*pixels;}
        if(!recent.isNull() && epoch==m_generation.load() && !(cancel && cancel()))return {recent,{}};
    }
    const auto name=QString::fromLatin1(QCryptographicHash::hash(key,QCryptographicHash::Sha256).toHex())+".png";
    const auto image=committedImage(QDir(m_directory).filePath(name));
    if(image.isNull() || (cancel && cancel()) || epoch!=m_generation.load())return {};
    if(m_deferred.load())enqueue({key,{},epoch,{}});
    qCDebug(thumbnailCacheLog)<<"hit"<<name<<"display-lookup";
    return {image,{}};
}
bool ThumbnailDiskCache::store(const Pending &pending,const std::function<bool()> &cancel) {
    const auto stale=[&]{return pending.epoch!=m_generation.load() || !m_enabled.load();};
    if(stale())return true;
    if(cancel && cancel())return false;
    if(pending.sourceUnchanged && !pending.sourceUnchanged())return true;
    QByteArray bytes;
    if(!pending.image.isNull()) {
        QImage pixels(pending.image.size(),QImage::Format_RGBA8888);pixels.fill(Qt::transparent);
        pixels.setColorSpace(pending.image.colorSpace());
        {QPainter painter(&pixels);painter.drawImage(pixels.rect(),pending.image);}
        QBuffer buffer(&bytes);buffer.open(QIODevice::WriteOnly);
        if(!pixels.save(&buffer,"PNG") || bytes.size()>MaxCacheFile)return true;
    }
    if(!m_mutex.tryLock())return false;
    const auto unlock=qScopeGuard([this]{m_mutex.unlock();});
    if(stale())return true;
    QString error;if(!prepare(!pending.image.isNull(),error))return true;
    QLockFile lock(m_directory+"/.lock");if(!lock.tryLock(0))return false;
    if(cancel && cancel())return false;
    const auto name=QString::fromLatin1(QCryptographicHash::hash(pending.key,QCryptographicHash::Sha256).toHex())+".png";
    const auto destination=QDir(m_directory).filePath(name);
    if(QFileInfo(destination).isSymLink())return true;
    if(pending.image.isNull()) {
        const QFileInfo info(destination);
        if(!info.isFile())return true;
        const auto now=QDateTime::currentMSecsSinceEpoch();auto diskUsed=info.lastModified().toMSecsSinceEpoch();
        if(now-diskUsed>=60000) {QFile file(destination);if(file.open(QIODevice::ReadWrite|QIODevice::ExistingOnly) && file.setFileTime(QDateTime::fromMSecsSinceEpoch(now),QFileDevice::FileModificationTime))diskUsed=now;}
        const auto old=m_entries.value(name).bytes;m_bytes+=info.size()-old;m_entries[name]={info.size(),now,diskUsed};
        return true;
    }
    if(!inventory(cancel))return false;
    // Never evict an existing entry until the replacement PNG is committed.
    // The per-file maximum is smaller than the minimum configurable cache limit.
    if(bytes.size()>m_limit.load() || stale())return true;
    if(cancel && cancel())return false;
    if(pending.sourceUnchanged && !pending.sourceUnchanged())return true;
    const auto existing=m_entries.value(name).bytes;
    QSaveFile file(destination);
    if(file.open(QIODevice::WriteOnly) && file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner) && file.write(bytes)==bytes.size() &&
       !stale() && !(cancel && cancel()) && (!pending.sourceUnchanged || pending.sourceUnchanged()) && file.commit()) {
        const auto now=QDateTime::currentMSecsSinceEpoch();m_bytes+=bytes.size()-existing;m_entries[name]={bytes.size(),now,now};
        // Delete only after the replacement is durable.  Do not cancel cleanup
        // after commit: an interrupted flush must not leave the cache oversized.
        trim(m_limit.load(),{},name);
        changed();
        qCDebug(thumbnailCacheLog)<<"stored"<<name<<"deferred";
    }
    return true;
}
bool ThumbnailDiskCache::flushPending(const std::function<bool()> &cancel) {
    Pending pending;
    {QMutexLocker guard(&m_pendingMutex);if(m_pending.isEmpty())return false;
        pending=m_pending.takeFirst();m_pendingBytes-=pending.image.sizeInBytes();}
    const bool complete=store(pending,cancel);
    if(!complete)enqueue(std::move(pending),true);
    else if(!pending.image.isNull()) {
        QMutexLocker guard(&m_pendingMutex);
        // Do not keep serving a rejected replacement from the recent-image
        // cache. A newer pending write of the same key may already exist.
        bool newer=false;
        for(const auto &item:m_pending)if(item.key==pending.key){newer=true;break;}
        if(!newer)m_recentImages.remove(pending.key);
    }
    return complete;
}
bool ThumbnailDiskCache::prepare(bool create,QString &error) {
    if(m_directory.isEmpty() || QFileInfo(m_directory).isSymLink()) {
        error=QCoreApplication::translate("KomaMimic", "サムネイルの保存先を使用できません。");return false;
    }
    const bool existed=QFileInfo::exists(m_directory);
    if(!existed && !create) return false;
    if(!QDir().mkpath(m_directory) || !QFileInfo(m_directory).isDir()) {
        error=QCoreApplication::translate("KomaMimic", "サムネイルの保存先を作成できません。");return false;
    }
    if(!existed) QFile::setPermissions(m_directory,QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner);
    return true;
}
bool ThumbnailDiskCache::inventory(const std::function<bool()> &cancel) {
    if(cancel && cancel()) return false;
    const auto current=revision(m_directory);
    if(m_initialized && current==m_revision) return true;
    QMap<QString,Entry> entries;qint64 bytes=0;
    QDirIterator files(m_directory,{"*.png"},QDir::Files|QDir::NoSymLinks);
    while(files.hasNext()) {
        if(cancel && cancel()) return false;
        files.next();const auto file=files.fileInfo();
        if(!managed(file.fileName())) continue;
        const auto diskUsed=file.lastModified().toMSecsSinceEpoch();
        const auto old=m_entries.value(file.fileName());
        entries.insert(file.fileName(),{file.size(),old.bytes==file.size()?std::max(old.used,diskUsed):diskUsed,diskUsed});
        bytes+=file.size();
    }
    if(cancel && cancel()) return false;
    m_entries=std::move(entries);m_bytes=bytes;m_revision=current;m_initialized=true;
    qCDebug(thumbnailCacheLog) << "inventory" << m_entries.size() << "files" << m_bytes << "bytes";
    return true;
}
void ThumbnailDiskCache::changed() {
    QSaveFile file(m_directory+"/.revision");
    const auto value=QUuid::createUuid().toByteArray(QUuid::Id128);
    if(file.open(QIODevice::WriteOnly) && file.write(value)==value.size() && file.commit()) m_revision=value;
    else m_initialized=false;
}
bool ThumbnailDiskCache::remove(const QString &name) {
    const auto path=QDir(m_directory).filePath(name);
    if(QFileInfo::exists(path) && !QFile::remove(path)) return false;
    const auto found=m_entries.find(name);
    if(found!=m_entries.end()) {m_bytes-=found->bytes;m_entries.erase(found);}
    return true;
}
bool ThumbnailDiskCache::trim(qint64 limit,const std::function<bool()> &cancel,const QString &protectedName) {
    if(m_bytes<=limit) return false;
    bool removed=false;
    auto order=m_entries.keys();
    std::sort(order.begin(),order.end(),[this](const QString &a,const QString &b) {
        const auto x=m_entries.value(a).used,y=m_entries.value(b).used;
        return x==y ? a<b : x<y;
    });
    for(const auto &name:order) {
        if(cancel && cancel()) break;
        if(m_bytes<=limit) break;
        if(name==protectedName)continue;
        if(remove(name)) {
            removed=true;
            qCDebug(thumbnailCacheLog) << "evict" << name;
        }
    }
    if(m_bytes>limit) qCWarning(thumbnailCacheLog) << "eviction-incomplete" << m_bytes << "limit" << limit;
    return removed;
}
ImageResult ThumbnailDiskCache::image(const QByteArray &key,const std::function<ImageResult()> &generate,
                                     const std::function<bool()> &cancel,bool refresh,
                                     const std::function<bool()> &sourceUnchanged) {
    const auto generation=m_generation.load();
    const auto cancelled=[&] {return cancel && cancel();};
    if(cancelled()) return {{},QCoreApplication::translate("KomaMimic", "サムネイルの読み込みを取り消しました。")};
    const bool use=m_enabled.load() && !key.isEmpty();
    if(m_deferred.load() && !refresh) {auto recent=lookup(key,cancel);if(!recent.image.isNull())return recent;}
    const auto name=QString::fromLatin1(QCryptographicHash::hash(key,QCryptographicHash::Sha256).toHex())+".png";
    qCDebug(thumbnailCacheLog) << "lookup" << name << "directory" << m_directory
                             << "enabled" << use << "refresh" << refresh;
    if(use && !refresh && !m_directory.isEmpty() && !QFileInfo(m_directory).isSymLink()) {
        const auto path=QDir(m_directory).filePath(name);
        // QSaveFile publishes complete PNGs by replacement. Reading committed
        // pixels needs neither the directory lock nor a complete inventory.
        // Busy writers, maintenance and surviving locks must not turn a hit
        // into a fresh decode of the source archive.
        const auto cached=committedImage(path);
        if(!cached.isNull() && !cancelled() && generation==m_generation.load()) {
            // Approximate LRU is best-effort. A busy cache must not delay a hit.
            if(m_deferred.load())enqueue({key,{},generation,{}});
            else if(m_mutex.tryLock()) {
                const auto unlock=qScopeGuard([this]{m_mutex.unlock();});
                QLockFile lock(m_directory+"/.lock");
                if(lock.tryLock(0)) {
                    const auto now=QDateTime::currentMSecsSinceEpoch();
                    const QFileInfo info(path);auto diskUsed=info.lastModified().toMSecsSinceEpoch();
                    if(!info.isSymLink() && info.isFile() && now-diskUsed>=60000) {
                        QFile file(path);
                        if(file.open(QIODevice::ReadWrite|QIODevice::ExistingOnly) && file.setFileTime(QDateTime::fromMSecsSinceEpoch(now),QFileDevice::FileModificationTime))diskUsed=now;
                    }
                    if(!info.isSymLink() && info.isFile() && info.size()<=MaxCacheFile) {
                        const auto old=m_entries.value(name).bytes;
                        m_bytes+=info.size()-old;m_entries[name]={info.size(),now,diskUsed};
                    }
                }
            }
            if(cancelled() || generation!=m_generation.load())return {};
            qCDebug(thumbnailCacheLog) << "hit" << name << "committed-read";
            return {cached,{}};
        }
    }
    if(m_deferred.load()) {
        if(cancelled())return {};
        auto result=generate();
        if(use && !cancelled() && generation==m_generation.load() && m_enabled.load() && result.error.isEmpty() && !result.image.isNull() && result.image.width()<=1024 && result.image.height()<=1024 && (!sourceUnchanged || sourceUnchanged()))
            enqueue({key,result.image,generation,sourceUnchanged});
        return result;
    }
    if(use && !refresh) {
        QMutexLocker guard(&m_mutex);QString error;
        if(prepare(false,error)) {
            QLockFile lock(m_directory+"/.lock");
            if(lock.tryLock(100)) {
                if(!inventory(cancelled)) return {};
                const auto path=QDir(m_directory).filePath(name);
                const QFileInfo info(path);
                if(!info.isSymLink() && info.isFile() && info.size()<=MaxCacheFile) {
                    QImageReader reader(path,"PNG");const auto size=reader.size();
                    QImage cached;
                    if(size.width()>0 && size.height()>0 && size.width()<=1024 && size.height()<=1024) cached=reader.read();
                    if(!cached.isNull()) {
                        const auto now=QDateTime::currentMSecsSinceEpoch();
                        auto &entry=m_entries[name];
                        // Approximate LRU, with at most one timestamp write/minute.
                        if(now-entry.diskUsed>=60000) {
                            QFile file(path);
                            if(file.open(QIODevice::ReadWrite) && file.setFileTime(QDateTime::fromMSecsSinceEpoch(now),QFileDevice::FileModificationTime)) entry.diskUsed=now;
                        }
                        entry.used=now;
                        if(!cancelled() && generation==m_generation.load()) {
                            qCDebug(thumbnailCacheLog) << "hit" << name;
                            return {cached,{}};
                        }
                    } else if(remove(name)) {
                        qCDebug(thumbnailCacheLog) << "invalid-png" << name;
                        changed();
                    }
                }
            } else {
                qCDebug(thumbnailCacheLog) << "lock-unavailable" << name << lock.error();
            }
        }
    }
    if(cancelled()) return {};
    qCDebug(thumbnailCacheLog) << "generate" << name;
    auto result=generate();
    if(!use || cancelled() || generation!=m_generation.load() || !m_enabled.load() ||
       !result.error.isEmpty() || result.image.isNull() || (sourceUnchanged && !sourceUnchanged())) return result;
    const auto &original=result.image;
    if(original.width()>1024 || original.height()>1024) return result;
    // Retain pixels only, rather than source EXIF/text/location metadata.
    QImage pixels(original.size(),QImage::Format_RGBA8888);pixels.fill(Qt::transparent);
    pixels.setColorSpace(original.colorSpace());
    {QPainter painter(&pixels);painter.drawImage(pixels.rect(),original);}
    QByteArray bytes;QBuffer buffer(&bytes);buffer.open(QIODevice::WriteOnly);
    if(!pixels.save(&buffer,"PNG") || bytes.size()>MaxCacheFile) return result;
    QMutexLocker guard(&m_mutex);QString error;
    if(cancelled() || generation!=m_generation.load() || !m_enabled.load() || !prepare(true,error)) return result;
    QLockFile lock(m_directory+"/.lock");if(!lock.tryLock(100)) return result;
    if(!inventory(cancelled)) return result;
    if(bytes.size()>m_limit.load()) return result;
    const auto destination=QDir(m_directory).filePath(name);
    if(QFileInfo(destination).isSymLink()) return result;
    const auto existing=m_entries.value(name).bytes;
    QSaveFile file(destination);
    if(file.open(QIODevice::WriteOnly) && file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner) && file.write(bytes)==bytes.size() &&
       !cancelled() && generation==m_generation.load() && (!sourceUnchanged || sourceUnchanged()) && file.commit()) {
        const auto now=QDateTime::currentMSecsSinceEpoch();
        m_bytes+=bytes.size()-existing;m_entries[name]={bytes.size(),now,now};
        // QSaveFile keeps the old PNG intact on failure.  Never remove cache
        // entries to make room until the new image has been committed.
        trim(m_limit.load(),{},name);
        changed();
        qCDebug(thumbnailCacheLog) << "stored" << name;
    }
    return result;
}
ThumbnailDiskCache::Stats ThumbnailDiskCache::stats(const std::function<bool()> &cancel) {
    QMutexLocker guard(&m_mutex);Stats result;
    if(!prepare(false,result.error)) return result;
    QLockFile lock(m_directory+"/.lock");
    if(!lock.tryLock(100)) {result.error=QCoreApplication::translate("KomaMimic", "サムネイルキャッシュを別の処理が使用中です。");return result;}
    // Explicit usage requests also account for externally removed files.
    m_initialized=false;
    if(!inventory(cancel)) {
        result.error=QCoreApplication::translate("KomaMimic", "サムネイルキャッシュの集計を取り消しました。");
        return result;
    }
    result.bytes=m_bytes;result.entries=m_entries.size();return result;
}
QString ThumbnailDiskCache::clear() {
    discardPending();
    QMutexLocker guard(&m_mutex);QString error;
    if(!prepare(false,error)) return error;
    QLockFile lock(m_directory+"/.lock");
    if(!lock.tryLock(1000)) return QCoreApplication::translate("KomaMimic", "サムネイルキャッシュを別の処理が使用中です。");
    m_initialized=false;inventory();const auto names=m_entries.keys();
    for(const auto &name:names) if(!remove(name)) error=QCoreApplication::translate("KomaMimic", "一部のサムネイルを削除できませんでした。");
    changed();return error;
}
QString ThumbnailDiskCache::maintain(const std::function<bool()> &cancel) {
    if(!m_enabled.load() || (cancel && cancel())) return {};
    QMutexLocker guard(&m_mutex);QString error;
    if(!prepare(false,error)) return error;
    QLockFile lock(m_directory+"/.lock");
    if(!lock.tryLock(1000)) return QCoreApplication::translate("KomaMimic", "サムネイルキャッシュを別の処理が使用中です。");
    if(!inventory(cancel)) return {};
    // Even if cancelled partway through eviction, publish any deletions to
    // other cache instances before leaving the maintenance lock.
    const bool removed=trim(m_limit.load(),cancel);
    if(removed)changed();
    return {};
}
