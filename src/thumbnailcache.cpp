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
QImage committedImage(const QString &path,qint64 *created=nullptr) {
    const QFileInfo info(path);
    if(info.isSymLink() || !info.isFile() || info.size()>MaxCacheFile)return {};
    QFile file(path);
    // Another writer can commit/remove a staged PNG between stat and open.
    // Cache reads are best-effort: a failed open must still allow fallback.
    if(!file.open(QIODevice::ReadOnly))return {};
    QImageReader reader(&file,"PNG");const auto size=reader.size();
    if(size.width()<=0 || size.height()<=0 || size.width()>1024 || size.height()>1024)return {};
    if(created)*created=info.lastModified().toMSecsSinceEpoch();
    return reader.read();
}
QByteArray revision(const QString &directory) {
    QFile file(directory+"/.revision");
    return file.open(QIODevice::ReadOnly) ? file.read(64) : QByteArray{};
}
}
QString ThumbnailDiskCache::defaultDirectory() {
    const auto root=QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    // v2 deliberately does not read/migrate the v1 fingerprint-keyed thumbnails.
    return root.isEmpty() ? QString{} : QDir(root).filePath("thumbnails-v2");
}
ThumbnailDiskCache::ThumbnailDiskCache(QString directory):m_directory(std::move(directory)) {}
void ThumbnailDiskCache::configure(bool enabled,int megabytes) {
    // Capacity/enable changes do not mean "forget generated work".
    m_enabled.store(enabled);
    m_limit.store(qint64(std::clamp(megabytes,16,16384))*1024*1024);
}
ImageResult ThumbnailDiskCache::recentImage(const QByteArray &key) {
    QMutexLocker guard(&m_recentMutex);
    const auto *image=m_recentImages.object(QCryptographicHash::hash(key,QCryptographicHash::Sha256).toHex());return image?image->result:ImageResult{};
}
void ThumbnailDiskCache::rememberRecent(const QByteArray &key,const ImageResult &image,bool pending,const quint64 *revision) {
    QMutexLocker guard(&m_recentMutex);
    if(revision && *revision!=m_recentRevision.load())return;
    m_recentImages.insert(QCryptographicHash::hash(key,QCryptographicHash::Sha256).toHex(),new RecentImage{image,pending},int((image.image.sizeInBytes()+1023)/1024));
}
void ThumbnailDiskCache::forgetRecent(const QByteArray &key) {
    QMutexLocker guard(&m_recentMutex);++m_recentRevision;
    if(key.isEmpty())m_recentImages.clear();else m_recentImages.remove(QCryptographicHash::hash(key,QCryptographicHash::Sha256).toHex());
}
QString ThumbnailDiskCache::failedGeneration(const QByteArray &identity) {
    QMutexLocker guard(&m_recentMutex);return m_failures.value(identity);
}
void ThumbnailDiskCache::rememberFailure(const QByteArray &identity,const QString &error) {
    QMutexLocker guard(&m_recentMutex);
    if(m_failures.size()>=1024)m_failures.clear();
    if(!error.isEmpty())m_failures[identity]=error;
}
void ThumbnailDiskCache::discardPending() {
    m_generation.fetch_add(1);
    QMutexLocker guard(&m_pendingMutex);
    for(const auto &pending:m_pending)if(!pending.staged.isEmpty())QFile::remove(pending.staged);
    m_pending.clear();m_pendingBytes=0;forgetRecent();m_versions.clear();
}
qint64 ThumbnailDiskCache::pendingMemoryBytes() const {
    QMutexLocker guard(&m_pendingMutex);return m_pendingBytes;
}
bool ThumbnailDiskCache::stage(Pending &pending) {
    if(pending.image.isNull())return true;
    const auto root=m_directory+"/pending";
    if(m_directory.isEmpty() || QFileInfo(m_directory).isSymLink() || QFileInfo(root).isSymLink() || !QDir().mkpath(root))return false;
    QFile::setPermissions(m_directory,QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner);
    QFile::setPermissions(root,QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner);
    const auto path=root+'/'+QString::fromLatin1(QCryptographicHash::hash(pending.key,QCryptographicHash::Sha256).toHex())+".png";
    if(QFileInfo(path).isSymLink())return false;
    // Strip arbitrary source metadata just as the committed PNG writer does.
    QImage pixels(pending.image.size(),QImage::Format_RGBA8888);pixels.fill(Qt::transparent);
    pixels.setColorSpace(pending.image.colorSpace());
    {QPainter painter(&pixels);painter.drawImage(pixels.rect(),pending.image);}
    pixels.setText("komamimic-cover-stamp",pending.image.text("komamimic-cover-stamp"));
    pixels.setText("komamimic-pending-key",QString::fromLatin1(pending.key.toBase64()));
    QSaveFile file(path);
    if(!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner) || !pixels.save(&file,"PNG") || !file.commit())return false;
    pending.staged=path;return true;
}
bool ThumbnailDiskCache::checkpoint() {
    QMutexLocker guard(&m_pendingMutex);bool ok=true;
    for(auto &pending:m_pending)if(!pending.image.isNull()) {
        if(stage(pending)){m_pendingBytes-=pending.image.sizeInBytes();pending.image={};}
        else {ok=false;qCWarning(thumbnailCacheLog)<<"checkpoint-failed"<<m_directory;}
    }
    forgetRecent();return ok;
}
void ThumbnailDiskCache::recoverPending() {
    if(m_recovered.exchange(true))return;
    QMutexLocker guard(&m_pendingMutex);
    const auto root=m_directory+"/pending";
    if(QFileInfo(m_directory).isSymLink() || QFileInfo(root).isSymLink())return;
    QDirIterator files(root,{"*.png"},QDir::Files|QDir::NoSymLinks);
    while(files.hasNext()) {
        files.next();if(!managed(files.fileName()))continue;
        const auto image=committedImage(files.filePath());
        const auto key=QByteArray::fromBase64(image.text("komamimic-pending-key").toLatin1());
        if(image.isNull() || key.isEmpty())continue;
        const auto name=QString::fromLatin1(QCryptographicHash::hash(key,QCryptographicHash::Sha256).toHex())+".png";
        if(name!=files.fileName())continue;
        bool found=false;for(const auto &pending:m_pending)if(pending.key==key){found=true;break;}
        if(!found && !m_versions.contains(key)) {
            m_versions[key]=0; // Recovered pixels precede every new generation.
            m_pending.append({key,{},m_generation.load(),files.filePath(),0});
        }
    }
}
bool ThumbnailDiskCache::enqueue(Pending pending,bool retry) {
    m_pendingMutex.lock();
    const auto unlock=qScopeGuard([this]{m_pendingMutex.unlock();});
    if(pending.epoch!=m_generation.load())return true;
    if(pending.reserved){m_pendingBytes-=pending.image.sizeInBytes();pending.reserved=false;}
    const bool pixels=!pending.image.isNull() || !pending.staged.isEmpty();
    if(retry?pending.version!=m_versions.value(pending.key):pending.version<m_versions.value(pending.key))return true;
    for(auto i=m_pending.begin();i!=m_pending.end();++i)if(i->key==pending.key) {
        if(retry || !pixels)return true;
        m_pendingBytes-=i->image.sizeInBytes();
        if(!i->staged.isEmpty())QFile::remove(i->staged);
        m_pending.erase(i);break;
    }
    const auto cost=pending.image.sizeInBytes();
    constexpr qint64 limit=32LL*1024*1024;
    while(m_pendingBytes+cost>limit) {
        auto old=std::find_if(m_pending.begin(),m_pending.end(),[](const Pending &item){return !item.image.isNull();});
        if(old==m_pending.end() || !stage(*old)) {
            qCWarning(thumbnailCacheLog)<<"pending-journal-unavailable"<<m_directory;return false;
        }
        m_pendingBytes-=old->image.sizeInBytes();forgetRecent(old->key);old->image={};
    }
    if(pixels && !retry)m_versions[pending.key]=pending.version;
    if(!pending.image.isNull())rememberRecent(pending.key,ImageResult{pending.image,{}},true);
    m_pendingBytes+=cost;m_pending.append(std::move(pending));return true;
}
bool ThumbnailDiskCache::hasPendingWrites() const {
    // Called by the GUI timer: recovery/staging must never block painting.
    if(!m_pendingMutex.tryLock())return true;
    const bool pending=!m_pending.isEmpty();m_pendingMutex.unlock();return pending;
}
ImageResult ThumbnailDiskCache::lookup(const QByteArray &key,const std::function<bool()> &cancel) {
    const auto epoch=m_generation.load();
    const auto memoryRevision=m_recentRevision.load();
    if(!m_enabled.load() || key.isEmpty() || (cancel && cancel()) || m_directory.isEmpty() || QFileInfo(m_directory).isSymLink())return {};
    const auto recent=recentImage(key);
    if(!recent.image.isNull() && epoch==m_generation.load() && !(cancel && cancel())){++m_memoryHits;return recent;}
    // PNG lookup never waits for staging or persistence locks. Version checks
    // only control optional RAM promotion, not delivery of a saved thumbnail.
    quint64 version=0;const bool tracked=m_pendingMutex.tryLock();
    if(tracked){version=m_versions.value(key);m_pendingMutex.unlock();}
    const auto name=QString::fromLatin1(QCryptographicHash::hash(key,QCryptographicHash::Sha256).toHex())+".png";
    const auto staged=QDir(m_directory).filePath("pending/"+name),committed=QDir(m_directory).filePath(name);
    qint64 created=0;
    auto image=committedImage(staged,&created);
    if(image.isNull())image=committedImage(committed,&created);
    if((cancel && cancel()) || epoch!=m_generation.load())return {};
    if(image.isNull())return {};
    ++m_pngDecodes;
    // Reads never alter cache creation order.
    qCDebug(thumbnailCacheLog)<<"hit"<<name<<"display-lookup";
    ImageResult result{image,{}};result.cacheCreatedMs=created;
    if(tracked && m_pendingMutex.tryLock()) {
        const auto unlock=qScopeGuard([this]{m_pendingMutex.unlock();});
        if(epoch==m_generation.load() && version==m_versions.value(key) && !m_activeStores.contains(key) &&
           std::none_of(m_pending.cbegin(),m_pending.cend(),[&key](const Pending &pending){return pending.key==key;})) {
            const auto newer=recentImage(key);
            if(!newer.image.isNull())return newer;
            rememberRecent(key,result,false,&memoryRevision);
        }
    }
    return result;
}
ThumbnailDiskCache::StoreResult ThumbnailDiskCache::store(const Pending &pending,const std::function<bool()> &cancel,qint64 *created) {
    const auto stale=[&]{
        QMutexLocker guard(&m_pendingMutex);
        return pending.epoch!=m_generation.load() || pending.version!=m_versions.value(pending.key);
    };
    if(stale())return StoreResult::Obsolete;
    if(cancel && cancel())return StoreResult::Retry;
    QByteArray bytes;
    if(!pending.image.isNull()) {
        QImage pixels(pending.image.size(),QImage::Format_RGBA8888);pixels.fill(Qt::transparent);
        pixels.setColorSpace(pending.image.colorSpace());
        // Only the application-generated fingerprint is retained; source EXIF/text is stripped.
        const auto stamp=pending.image.text(QStringLiteral("komamimic-cover-stamp"));
        if(!stamp.isEmpty())pixels.setText(QStringLiteral("komamimic-cover-stamp"),stamp);
            {QPainter painter(&pixels);painter.drawImage(pixels.rect(),pending.image);}
        QBuffer buffer(&bytes);buffer.open(QIODevice::WriteOnly);
        if(!pixels.save(&buffer,"PNG") || bytes.size()>MaxCacheFile)return StoreResult::Error;
    }
    if(!m_mutex.tryLock())return StoreResult::Retry;
    const auto unlock=qScopeGuard([this]{m_mutex.unlock();});
    if(stale())return StoreResult::Obsolete;
    QString error;if(!prepare(!pending.image.isNull(),error))return pending.image.isNull() && pending.staged.isEmpty()?StoreResult::Obsolete:StoreResult::Error;
    QLockFile lock(m_directory+"/.lock");if(!lock.tryLock(0))return StoreResult::Retry;
    if(cancel && cancel())return StoreResult::Retry;
    const auto name=QString::fromLatin1(QCryptographicHash::hash(pending.key,QCryptographicHash::Sha256).toHex())+".png";
    const auto destination=QDir(m_directory).filePath(name);
    if(QFileInfo(destination).isSymLink())return StoreResult::Error;
    if(pending.image.isNull())return StoreResult::Obsolete;
    if(!inventory(cancel))return StoreResult::Retry;
    // Never evict an existing entry until the replacement PNG is committed.
    // The per-file maximum is smaller than the minimum configurable cache limit.
    if(bytes.size()>m_limit.load() || stale())return StoreResult::Obsolete;
    if(cancel && cancel())return StoreResult::Retry;
    const auto existing=m_entries.value(name).bytes;
    QSaveFile file(destination);
    if(file.open(QIODevice::WriteOnly) && file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner) && file.write(bytes)==bytes.size() &&
       !stale() && !(cancel && cancel()) && file.commit()) {
        const auto now=QFileInfo(destination).lastModified().toMSecsSinceEpoch();m_bytes+=bytes.size()-existing;m_entries[name]={bytes.size(),now};
        if(created)*created=now;
        // Delete only after the replacement is durable.  Do not cancel cleanup
        // after commit: an interrupted flush must not leave the cache oversized.
        trim(m_limit.load(),{},name);
        changed();
        qCDebug(thumbnailCacheLog)<<"stored"<<name<<"deferred";
        return StoreResult::Stored;
    }
    if(stale())return StoreResult::Obsolete;
    return (cancel && cancel())?StoreResult::Retry:StoreResult::Error;
}
bool ThumbnailDiskCache::flushPending(const std::function<bool()> &cancel) {
    recoverPending();
    Pending pending;
    {QMutexLocker guard(&m_pendingMutex);if(m_pending.isEmpty())return false;
        auto next=std::find_if(m_pending.begin(),m_pending.end(),[](const Pending &item){return !item.image.isNull() || !item.staged.isEmpty();});
        if(next==m_pending.end())next=m_pending.begin();
        pending=std::move(*next);m_pending.erase(next);pending.reserved=!pending.image.isNull();++m_activeStores[pending.key];}
    const auto active=qScopeGuard([this,key=pending.key]{QMutexLocker guard(&m_pendingMutex);if(--m_activeStores[key]==0)m_activeStores.remove(key);});
    if(pending.image.isNull() && !pending.staged.isEmpty())pending.image=committedImage(pending.staged);
    qint64 created=0;const auto result=store(pending,cancel,&created);
    const bool complete=result==StoreResult::Stored || result==StoreResult::Obsolete;
    if(!complete) {
        if(result==StoreResult::Error)qCWarning(thumbnailCacheLog)<<"store-retry"<<m_directory;
        if(!pending.staged.isEmpty() && !pending.reserved)pending.image={};
        enqueue(std::move(pending),true);
    } else {
        QMutexLocker guard(&m_pendingMutex);
        if(pending.reserved && pending.epoch==m_generation.load())m_pendingBytes-=pending.image.sizeInBytes();
        if(pending.epoch==m_generation.load() && m_versions.value(pending.key)==pending.version) {
            const auto staged=pending.staged.isEmpty()?m_directory+"/pending/"+QString::fromLatin1(QCryptographicHash::hash(pending.key,QCryptographicHash::Sha256).toHex())+".png":pending.staged;
            if(!QFileInfo(staged).isSymLink())QFile::remove(staged);
            if(result==StoreResult::Stored){ImageResult image{pending.image,{}};image.cacheCreatedMs=created;rememberRecent(pending.key,image);}
            else forgetRecent(pending.key);
        }
    }
    return complete;
}
bool ThumbnailDiskCache::invalidate(const QByteArray &key,const std::function<bool()> &cancel) {
    if(cancel && cancel())return false;
    qCDebug(thumbnailCacheLog)<<"remove-confirmed-no-cover";
    QMutexLocker diskGuard(&m_mutex);QString error;
    QMutexLocker pendingGuard(&m_pendingMutex);m_versions[key]=++m_nextVersion;
    for(auto i=m_pending.begin();i!=m_pending.end();) {
        if(i->key!=key){++i;continue;}
        m_pendingBytes-=i->image.sizeInBytes();i=m_pending.erase(i);
    }
    forgetRecent(key);
    if(!prepare(false,error))return error.isEmpty();
    QLockFile lock(m_directory+"/.lock");if(!lock.tryLock(0))return false;
    const auto name=QString::fromLatin1(QCryptographicHash::hash(key,QCryptographicHash::Sha256).toHex())+".png";
    const auto staged=m_directory+"/pending/"+name;
    if(QFileInfo::exists(staged) && !QFile::remove(staged))return false;
    const bool existed=QFileInfo::exists(m_directory+'/'+name);
    const bool ok=remove(name);if(ok && existed)changed();return ok;
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
        const auto created=file.lastModified().toMSecsSinceEpoch();
        entries.insert(file.fileName(),{file.size(),created});
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
    {QMutexLocker guard(&m_recentMutex);++m_recentRevision;
        const auto key=name.toLatin1().chopped(4);const auto *image=m_recentImages.object(key);
        // Evict committed pixels, preserving a newer pending replacement.
        if(image && !image->pending)m_recentImages.remove(key);}
    const auto found=m_entries.find(name);
    if(found!=m_entries.end()) {m_bytes-=found->bytes;m_entries.erase(found);}
    return true;
}
bool ThumbnailDiskCache::trim(qint64 limit,const std::function<bool()> &cancel,const QString &protectedName) {
    if(m_bytes<=limit) return false;
    bool removed=false;
    auto order=m_entries.keys();
    std::sort(order.begin(),order.end(),[this](const QString &a,const QString &b) {
        const auto x=m_entries.value(a).created,y=m_entries.value(b).created;
        return x==y ? a<b : x<y;
    });
    for(const auto &name:order) {
        if(cancel && cancel()) break;
        if(m_bytes<=limit) break;
        if(name==protectedName)continue;
        if(remove(name)) {
            removed=true;
            qCDebug(thumbnailCacheLog) << "remove-capacity-oldest-created" << name;
        }
    }
    if(m_bytes>limit) qCWarning(thumbnailCacheLog) << "eviction-incomplete" << m_bytes << "limit" << limit;
    return removed;
}
ImageResult ThumbnailDiskCache::image(const QByteArray &key,const std::function<ImageResult()> &generate,
                                     const std::function<bool()> &cancel,bool refresh,
                                     const std::function<bool()> &sourceUnchanged) {
    const auto epoch=m_generation.load();
    if(cancel && cancel())return {};
    const bool use=m_enabled.load() && !key.isEmpty();
    if(use && !refresh) {
        auto hit=lookup(key,cancel);
        if(!hit.image.isNull() || !hit.error.isEmpty())return hit;
    }
    // Reserve order before generation: a cancelled old decoder may finish
    // after its replacement, but must never supersede newer completed pixels.
    // Starting a replacement must not discard already completed pending pixels.
    quint64 version=0;
    if(use){QMutexLocker guard(&m_pendingMutex);if(epoch!=m_generation.load())return {};version=++m_nextVersion;}
    // No inventory, journal recovery or source verification precedes a hit.
    auto result=generate();
    if(!use || epoch!=m_generation.load() || !m_enabled.load() || !result.error.isEmpty() || result.image.isNull() ||
       result.image.width()>1024 || result.image.height()>1024)return result;
    // This guard belongs to generation only. Store receives immutable pixels,
    // never a callback that can access the original archive or directory again.
    if(sourceUnchanged && !sourceUnchanged())return result;
    if(!enqueue({key,result.image,epoch,{},version})) {
        result.error=QCoreApplication::translate("KomaMimic", "サムネイルの保存先を使用できません。");return result;
    }
    if(!m_deferred.load()) {
        flushPending(cancel);
        // Explicit synchronous users also retain completed pixels on lock/I/O
        // failure. Application display requests always use deferred writes.
        if(hasPendingWrites())checkpoint();
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
    recoverPending();
    {QMutexLocker guard(&m_recentMutex);m_failures.clear();}
    qCDebug(thumbnailCacheLog)<<"remove-explicit-clear";
    discardPending();
    QMutexLocker guard(&m_mutex);QString error;
    if(!prepare(false,error)) return error;
    QLockFile lock(m_directory+"/.lock");
    if(!lock.tryLock(1000)) return QCoreApplication::translate("KomaMimic", "サムネイルキャッシュを別の処理が使用中です。");
    m_initialized=false;inventory();const auto names=m_entries.keys();
    for(const auto &name:names) if(!remove(name)) error=QCoreApplication::translate("KomaMimic", "一部のサムネイルを削除できませんでした。");
    const auto journal=m_directory+"/pending";
    if(!QFileInfo(journal).isSymLink()) {
        QDirIterator pending(journal,{"*.png"},QDir::Files|QDir::NoSymLinks);
        while(pending.hasNext()) {
            pending.next();if(managed(pending.fileName()) && !QFile::remove(pending.filePath()))
                error=QCoreApplication::translate("KomaMimic", "一部のサムネイルを削除できませんでした。");
        }
    }
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
