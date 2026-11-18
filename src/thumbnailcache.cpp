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
#include <QJsonDocument>
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
    if(!info.isFile() || info.size()>MaxCacheFile)return {};
    QFile file(path);
    // Another writer can replace a PNG between stat and open.
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
void ThumbnailDiskCache::configure(bool enabled,qint64 megabytes) {
    // Capacity/enable changes do not mean "forget generated work".
    m_enabled.store(enabled);
    m_limit.store(std::clamp<qint64>(megabytes,16,MaximumMegabytes)*1024*1024);
}
quint64 ThumbnailDiskCache::availabilityVersion(const QByteArray &key) {
    QMutexLocker guard(&m_memoryMutex);return m_versions.value(key);
}
ImageResult ThumbnailDiskCache::lookupMemory(const QByteArray &key,const std::function<bool()> &cancel) {
    if(!enabled() || key.isEmpty() || (cancel && cancel()))return {};
    QMutexLocker guard(&m_memoryMutex);
    if(const auto writing=m_writing.constFind(key);writing!=m_writing.cend()){++m_memoryHits;return writing->result;}
    if(const auto *image=m_recentImages.object(QCryptographicHash::hash(key,QCryptographicHash::Sha256).toHex())){++m_memoryHits;return *image;}
    return {};
}
ImageResult ThumbnailDiskCache::lookup(const QByteArray &key,const std::function<bool()> &cancel) {
    const auto epoch=generation();
    if(!enabled() || key.isEmpty() || (cancel && cancel()))return {};
    if(auto hit=lookupMemory(key,cancel);!hit.image.isNull())return hit;
    if(m_directory.isEmpty())return {};
    quint64 version;{QMutexLocker guard(&m_memoryMutex);version=m_versions.value(key);}
    const auto name=QCryptographicHash::hash(key,QCryptographicHash::Sha256).toHex();
    qint64 created=0;auto pixels=committedImage(m_directory+'/'+QString::fromLatin1(name)+".png",&created);
    if(pixels.isNull() || epoch!=generation() || (cancel && cancel()))return {};
    ++m_pngDecodes;ImageResult result{pixels,{}};result.cacheCreatedMs=created;
    result.thumbnailEntries=QJsonDocument::fromJson(pixels.text("komamimic-thumbnail-files").toUtf8()).object();
    QMutexLocker guard(&m_memoryMutex);
    if(epoch!=generation())return {};
    if(const auto writing=m_writing.constFind(key);writing!=m_writing.cend())return writing->result;
    if(m_versions.value(key)==version)m_recentImages.insert(name,new ImageResult(result),int((pixels.sizeInBytes()+1023)/1024));
    return result;
}
QString ThumbnailDiskCache::failedGeneration(const QByteArray &identity) {
    QMutexLocker guard(&m_memoryMutex);return m_failures.value(identity);
}
void ThumbnailDiskCache::rememberFailure(const QByteArray &identity,const QString &error) {
    QMutexLocker guard(&m_memoryMutex);if(m_failures.size()>=1024)m_failures.clear();
    if(!error.isEmpty())m_failures[identity]=error;
}
void ThumbnailDiskCache::clearMemory() {
    QMutexLocker guard(&m_memoryMutex);++m_generation;
    m_recentImages.clear();m_writing.clear();m_versions.clear();m_failures.clear();
}
bool ThumbnailDiskCache::store(const QByteArray &key,const ImageResult &image,quint64 epoch,quint64 version,const std::function<bool()> &cancel,qint64 &created) {
    const auto obsolete=[&]{QMutexLocker guard(&m_memoryMutex);return epoch!=generation() || m_versions.value(key)!=version;};
    if(obsolete() || (cancel && cancel()))return false;
    QImage pixels(image.image.size(),QImage::Format_RGBA8888);pixels.fill(Qt::transparent);pixels.setColorSpace(image.image.colorSpace());
    {QPainter painter(&pixels);painter.drawImage(pixels.rect(),image.image);}
    const auto stamp=image.image.text("komamimic-cover-stamp");if(!stamp.isEmpty())pixels.setText("komamimic-cover-stamp",stamp);
    QJsonObject sources;
    for(auto i=image.thumbnailEntries.begin();i!=image.thumbnailEntries.end();++i) {
        const auto entry=i.value().toObject();
        if(!entry.value("directory").toBool() && entry.value("stamp").toString()==stamp)sources[i.key()]=QJsonObject{{"directory",false}};
    }
    if(!sources.isEmpty())pixels.setText("komamimic-thumbnail-files",QString::fromUtf8(QJsonDocument(sources).toJson(QJsonDocument::Compact)));
    QByteArray bytes;QBuffer buffer(&bytes);buffer.open(QIODevice::WriteOnly);
    if(!pixels.save(&buffer,"PNG") || bytes.size()>MaxCacheFile)return false;
    QMutexLocker guard(&m_mutex);
    QString error;if(obsolete() || (cancel && cancel()) || !prepare(true,error))return false;
    QLockFile lock(m_directory+"/.lock");if(!lock.tryLock(1000))return false;
    if(!inventory(cancel) || obsolete() || (cancel && cancel()))return false;
    const auto name=QString::fromLatin1(QCryptographicHash::hash(key,QCryptographicHash::Sha256).toHex())+".png";
    const auto destination=m_directory+'/'+name;if(QFileInfo(destination).isSymLink())return false;
    QSaveFile file(destination);
    if(!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner) || file.write(bytes)!=bytes.size() || obsolete() || (cancel && cancel()) || !file.commit())return false;
    created=QFileInfo(destination).lastModified().toMSecsSinceEpoch();
    m_bytes+=bytes.size()-m_entries.value(name).bytes;m_entries[name]={bytes.size(),created};
    trim(m_limit.load(),{},name);changed();return true;
}
ImageResult ThumbnailDiskCache::image(const QByteArray &key,const std::function<ImageResult()> &generate,const std::function<bool()> &cancel,bool refresh) {
    const auto epoch=generation();if(cancel && cancel())return {};
    const bool use=enabled() && !key.isEmpty();
    if(use && !refresh)if(auto hit=lookup(key,cancel);!hit.image.isNull() || !hit.error.isEmpty())return hit;
    const auto version=++m_nextVersion;
    auto result=generate();
    if(!use || epoch!=generation() || !enabled() || !result.error.isEmpty() || result.image.isNull() || result.image.width()>1024 || result.image.height()>1024)return result;
    // Publish the completed pixels before encoding or saving the normal PNG.
    result.cacheCreatedMs=0;
    {QMutexLocker guard(&m_memoryMutex);
        if(epoch!=generation() || m_versions.value(key)>version)return result;
        m_versions[key]=version;m_writing[key]={result,version};}
    qint64 created=result.cacheCreatedMs;
    store(key,result,epoch,version,cancel,created);result.cacheCreatedMs=created;
    {QMutexLocker guard(&m_memoryMutex);
        if(epoch==generation() && m_versions.value(key)==version){
            m_recentImages.insert(QCryptographicHash::hash(key,QCryptographicHash::Sha256).toHex(),new ImageResult(result),int((result.image.sizeInBytes()+1023)/1024));
            m_writing.remove(key);
        }}
    return result;
}
bool ThumbnailDiskCache::invalidate(const QByteArray &key,const std::function<bool()> &cancel) {
    if(cancel && cancel())return false;
    {QMutexLocker guard(&m_memoryMutex);m_versions[key]=++m_nextVersion;m_writing.remove(key);m_recentImages.remove(QCryptographicHash::hash(key,QCryptographicHash::Sha256).toHex());}
    QMutexLocker guard(&m_mutex);QString error;if(!prepare(false,error))return error.isEmpty();
    QLockFile lock(m_directory+"/.lock");if(!lock.tryLock(1000))return false;
    if(!inventory(cancel))return false;
    const auto name=QString::fromLatin1(QCryptographicHash::hash(key,QCryptographicHash::Sha256).toHex())+".png";
    const bool existed=m_entries.contains(name),ok=remove(name);if(ok && existed)changed();return ok;
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
    QFile file(m_directory+"/.revision");
    const auto value=QUuid::createUuid().toByteArray(QUuid::Id128);
    if(file.open(QIODevice::WriteOnly|QIODevice::Truncate) && file.write(value)==value.size()) m_revision=value;
    else m_initialized=false;
}
bool ThumbnailDiskCache::remove(const QString &name) {
    const auto path=QDir(m_directory).filePath(name);
    if(QFileInfo::exists(path) && !QFile::remove(path)) return false;
    {QMutexLocker guard(&m_memoryMutex);
        m_recentImages.remove(name.toLatin1().chopped(4));}
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
    clearMemory();QMutexLocker guard(&m_mutex);QString error;
    if(!prepare(false,error))return error;
    QLockFile lock(m_directory+"/.lock");if(!lock.tryLock(1000))return QCoreApplication::translate("KomaMimic", "サムネイルキャッシュを別の処理が使用中です。");
    m_initialized=false;inventory();const auto names=m_entries.keys();
    for(const auto &name:names)if(!remove(name))error=QCoreApplication::translate("KomaMimic", "一部のサムネイルを削除できませんでした。");
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
