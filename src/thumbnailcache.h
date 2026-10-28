// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "media.h"
#include <QMap>
#include <QMutex>
#include <atomic>

// Shared by GUI owners and pool jobs. All disk work belongs on a worker.
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
    ImageResult image(const QByteArray &key, const std::function<ImageResult()> &generate,
                      const std::function<bool()> &cancel = {}, bool refresh = false,
                      const std::function<bool()> &sourceUnchanged = {});
    Stats stats();
    QString clear();
    QString maintain();
private:
    struct Entry { qint64 bytes=0, used=0, diskUsed=0; };
    bool prepare(bool create, QString &error);
    void inventory();
    void changed();
    bool remove(const QString &name);
    void trim(qint64 limit, int count=20000);
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
