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
    m_generation.fetch_add(1);
    m_enabled.store(enabled);
    m_limit.store(qint64(std::clamp(megabytes,16,16384))*1024*1024);
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
void ThumbnailDiskCache::trim(qint64 limit,int count,const std::function<bool()> &cancel) {
    if(m_bytes<=limit && m_entries.size()<=count) return;
    auto order=m_entries.keys();
    std::sort(order.begin(),order.end(),[this](const QString &a,const QString &b) {
        const auto x=m_entries.value(a).used,y=m_entries.value(b).used;
        return x==y ? a<b : x<y;
    });
    for(const auto &name:order) {
        if(cancel && cancel()) break;
        if(m_bytes<=limit && m_entries.size()<=count) break;
        if(remove(name)) qCDebug(thumbnailCacheLog) << "evict" << name;
    }
}
ImageResult ThumbnailDiskCache::image(const QByteArray &key,const std::function<ImageResult()> &generate,
                                     const std::function<bool()> &cancel,bool refresh,
                                     const std::function<bool()> &sourceUnchanged) {
    const auto generation=m_generation.load();
    const auto cancelled=[&] {return cancel && cancel();};
    if(cancelled()) return {{},QCoreApplication::translate("KomaMimic", "サムネイルの読み込みを取り消しました。")};
    const bool use=m_enabled.load() && !key.isEmpty();
    const auto name=QString::fromLatin1(QCryptographicHash::hash(key,QCryptographicHash::Sha256).toHex())+".png";
    qCDebug(thumbnailCacheLog) << "lookup" << name << "directory" << m_directory
                             << "enabled" << use << "refresh" << refresh;
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
    const auto old=m_entries.value(name).bytes;
    trim(m_limit.load()-bytes.size()+old,19999,cancelled);
    const auto existing=m_entries.value(name).bytes;
    if(m_bytes-existing+bytes.size()>m_limit.load()) {changed();return result;}
    const auto destination=QDir(m_directory).filePath(name);
    if(QFileInfo(destination).isSymLink()) return result;
    QSaveFile file(destination);
    if(file.open(QIODevice::WriteOnly) && file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner) && file.write(bytes)==bytes.size() &&
       !cancelled() && generation==m_generation.load() && file.commit()) {
        const auto now=QDateTime::currentMSecsSinceEpoch();
        m_bytes+=bytes.size()-existing;m_entries[name]={bytes.size(),now,now};
        qCDebug(thumbnailCacheLog) << "stored" << name;
    }
    changed();return result;
}
ThumbnailDiskCache::Stats ThumbnailDiskCache::stats() {
    QMutexLocker guard(&m_mutex);Stats result;
    if(!prepare(false,result.error)) return result;
    QLockFile lock(m_directory+"/.lock");
    if(!lock.tryLock(100)) {result.error=QCoreApplication::translate("KomaMimic", "サムネイルキャッシュを別の処理が使用中です。");return result;}
    // Explicit usage requests also account for externally removed files.
    m_initialized=false;inventory();result.bytes=m_bytes;result.entries=m_entries.size();return result;
}
QString ThumbnailDiskCache::clear() {
    m_generation.fetch_add(1);
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
    trim(m_limit.load(),20000,cancel);changed();return {};
}
