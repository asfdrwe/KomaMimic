// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "media.h"
#include <QMap>
#include <QMutex>
#include <QCache>
#include <QLoggingCategory>
#include <atomic>
#include <limits>

Q_DECLARE_LOGGING_CATEGORY(thumbnailCacheLog)

// Memory lookup never waits for a worker writing a normal cache PNG.
class ThumbnailDiskCache {
public:
    struct Stats { qint64 bytes=0; int entries=0; QString error; };
    struct LookupActivity { quint64 memoryHits=0,pngDecodes=0; };
    LookupActivity lookupActivity() const {return {m_memoryHits.load(),m_pngDecodes.load()};}
    explicit ThumbnailDiskCache(QString directory=defaultDirectory());
    static QString defaultDirectory();
    QString directory() const {return m_directory;}
    bool enabled() const {return m_enabled.load();}
    qint64 limitBytes() const {return m_limit.load();}
    // Only the byte counter's representable range limits a user-selected capacity.
    static constexpr qint64 MaximumMegabytes=std::numeric_limits<qint64>::max()/(1024*1024);
    void configure(bool enabled,qint64 megabytes);
    ImageResult lookupMemory(const QByteArray &key,const std::function<bool()> &cancel={});
    ImageResult lookup(const QByteArray &key,const std::function<bool()> &cancel={});
    quint64 availabilityVersion(const QByteArray &key);
    void clearMemory();
    quint64 generation() const {return m_generation.load();}
    ImageResult image(const QByteArray &key,const std::function<ImageResult()> &generate,
                      const std::function<bool()> &cancel={},bool refresh=false);
    QString failedGeneration(const QByteArray &identity);
    void rememberFailure(const QByteArray &identity,const QString &error);
    Stats stats(const std::function<bool()> &cancel={});
    QString clear();
    bool invalidate(const QByteArray &key,const std::function<bool()> &cancel={});
    QString maintain(const std::function<bool()> &cancel={});
private:
    struct WritingImage {ImageResult result;quint64 version=0;};
    QMutex m_memoryMutex,m_mutex;
    QCache<QByteArray,ImageResult> m_recentImages{32*1024};
    QHash<QByteArray,WritingImage> m_writing;
    QHash<QByteArray,quint64> m_versions;
    QHash<QByteArray,QString> m_failures;
    std::atomic<quint64> m_nextVersion{0},m_generation{0};
    std::atomic<quint64> m_memoryHits{0},m_pngDecodes{0};
    bool store(const QByteArray &key,const ImageResult &image,quint64 epoch,quint64 version,const std::function<bool()> &cancel,qint64 &created);
    struct Entry {qint64 bytes=0,created=0;};
    bool prepare(bool create,QString &error);
    bool inventory(const std::function<bool()> &cancel={});
    void changed();
    bool remove(const QString &name);
    bool trim(qint64 limit,const std::function<bool()> &cancel={},const QString &protectedName={});
    QString m_directory;
    QMap<QString,Entry> m_entries;
    QByteArray m_revision;
    qint64 m_bytes=0;
    bool m_initialized=false;
    std::atomic<bool> m_enabled{true};
    std::atomic<qint64> m_limit{512LL*1024*1024};
};
