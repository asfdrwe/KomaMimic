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
    bool invalidate(const QByteArray &key,const std::function<bool()> &cancel={});
    qint64 pendingMemoryBytes() const;
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
        std::function<bool()> sourceUnchanged;
        QString staged;
        quint64 version=0;
        bool reserved=false;
    };
    enum class StoreResult { Stored, Obsolete, Retry, Error };
    bool enqueue(Pending pending,bool retry=false);
    bool stage(Pending &pending);
    StoreResult store(const Pending &,const std::function<bool()> &cancel);
    mutable QMutex m_pendingMutex;
    QList<Pending> m_pending;
    QCache<QByteArray,QImage> m_recentImages{32*1024};
    qint64 m_pendingBytes=0;
    QHash<QByteArray,quint64> m_versions;
    std::atomic<bool> m_recovered{false};
    std::atomic<bool> m_deferred{false};
    struct Entry { qint64 bytes=0, used=0, diskUsed=0; };
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
