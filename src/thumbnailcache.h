// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "media.h"
#include <QMap>
#include <QMutex>
#include <QList>
#include <QCache>
#include <QLoggingCategory>
#include <atomic>

Q_DECLARE_LOGGING_CATEGORY(thumbnailCacheLog)

// Shared by GUI owners and pool jobs. Live display I/O runs on workers;
// normal shutdown checkpoints after they have stopped.
class ThumbnailDiskCache {
public:
    struct Stats { qint64 bytes=0; int entries=0; QString error; };
    struct LookupActivity { quint64 memoryHits=0,pngDecodes=0; };
    LookupActivity lookupActivity() const {return {m_memoryHits.load(),m_pngDecodes.load()};}
    explicit ThumbnailDiskCache(QString directory = defaultDirectory());
    static QString defaultDirectory();
    QString directory() const { return m_directory; }
    bool enabled() const { return m_enabled.load(); }
    qint64 limitBytes() const { return m_limit.load(); }
    // Configuration itself performs no disk I/O.
    void configure(bool enabled, int megabytes);
    // Live display uses nonblocking lookup and a separate persistence worker.
    void setDeferredWrites(bool enabled) { m_deferred.store(enabled); }
    ImageResult lookup(const QByteArray &key,const std::function<bool()> &cancel={});
    bool hasPendingWrites() const;
    bool flushPending(const std::function<bool()> &cancel={});
    void discardPending();
    // Persist deferred pixels as a replayable journal before normal shutdown.
    bool checkpoint();
    void recoverPending();
    bool recoveryPending() const {return !m_recovered.load();}
    bool invalidate(const QByteArray &key,const std::function<bool()> &cancel={});
    qint64 pendingMemoryBytes() const;
    QString failedGeneration(const QByteArray &identity);
    void rememberFailure(const QByteArray &identity,const QString &error);
    // Composite source requests share one invalidation boundary, including
    // folder searches that make multiple child-cache calls.
    quint64 generation() const { return m_generation.load(); }
    ImageResult image(const QByteArray &key, const std::function<ImageResult()> &generate,
                      const std::function<bool()> &cancel = {}, bool refresh = false,
                      const std::function<bool()> &sourceUnchanged = {});
    Stats stats(const std::function<bool()> &cancel={});
    QString clear();
    QString maintain(const std::function<bool()> &cancel = {});
private:
    struct Pending {
        QByteArray key; QImage image; quint64 epoch=0;
        QString staged;
        quint64 version=0;
        bool reserved=false;
    };
    enum class StoreResult { Stored, Obsolete, Retry, Error };
    bool enqueue(Pending pending,bool retry=false);
    bool stage(Pending &pending);
    StoreResult store(const Pending &,const std::function<bool()> &cancel,qint64 *created=nullptr);
    mutable QMutex m_pendingMutex;
    QMutex m_recentMutex;
    QHash<QByteArray,QString> m_failures;
    ImageResult recentImage(const QByteArray &key);
    void rememberRecent(const QByteArray &key,const ImageResult &image,bool pending=false,const quint64 *revision=nullptr);
    void forgetRecent(const QByteArray &key={});
    QList<Pending> m_pending;
    struct RecentImage {ImageResult result;bool pending=false;};
    QCache<QByteArray,RecentImage> m_recentImages{32*1024};
    std::atomic<quint64> m_recentRevision{0};
    std::atomic<quint64> m_memoryHits{0},m_pngDecodes{0};
    QHash<QByteArray,int> m_activeStores; // Protected by m_pendingMutex.
    qint64 m_pendingBytes=0;
    QHash<QByteArray,quint64> m_versions;
    quint64 m_nextVersion=0; // Request order; m_versions contains only completed replacements.
    std::atomic<bool> m_recovered{false};
    std::atomic<bool> m_deferred{false};
    struct Entry { qint64 bytes=0, created=0; };
    bool prepare(bool create, QString &error);
    bool inventory(const std::function<bool()> &cancel = {});
    void changed();
    bool remove(const QString &name);
    // Disk retention is determined by bytes, not the number of thumbnails.
    bool trim(qint64 limit, const std::function<bool()> &cancel = {}, const QString &protectedName = {});
    QString m_directory;
    QMutex m_mutex;
    QMap<QString,Entry> m_entries;
    QByteArray m_revision;
    qint64 m_bytes=0;
    bool m_initialized=false;
    std::atomic<bool> m_enabled{true};
    std::atomic<qint64> m_limit{512LL*1024*1024};
    std::atomic<quint64> m_generation{0};
};
