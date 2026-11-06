// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "book.h"
#include "thumbnailcache.h"
#include <QJsonObject>
#include <QMap>
class QSettings;
struct ThumbnailActivity {
    static std::atomic<quint64> metadataChecks, folderScans, generations;
};
QJsonObject thumbnailEntry(const QFileInfo &entry,const QString &resolvedDirectory={});
QJsonObject thumbnailDependencies(const ImageResult &result,const QString &fallback={});
struct ThumbnailChanges {
    quint64 serial=0;
    QHash<QString,QJsonObject> entries;
    QHash<QString,quint64> notices,entrySerials;
    QHash<QString,QPair<QJsonObject,QJsonObject>> checked;
    void observe(const QFileInfo &entry,const QString &resolvedDirectory={});
    void changed(const QString &path);
    bool needsUpdate(const QJsonObject &dependencies) const;
    void acknowledge(const QJsonObject &dependencies,quint64 through);
};
struct ThumbnailNamePolicies {
    explicit ThumbnailNamePolicies(const QSettings &settings);
    static QString settingsKey(const QString &path);
    NamePolicy resolve(const QString &path) const;
    QList<NamePolicy> previews{ NamePolicy{} };
    QMap<QString,NamePolicy> saved;
};

ImageResult cachedCover(const QString &path,const NamePolicy &policy,
                        const std::shared_ptr<ThumbnailDiskCache> &cache,
                        const std::function<bool()> &cancel={},bool refresh=false,int depth=0,bool cachedOnly=false,bool verify=false,int maxDepth=0,const QJsonObject &observed={});
ImageResult cachedPageThumbnail(const std::shared_ptr<const Book> &book,int page,
                                const std::shared_ptr<ThumbnailDiskCache> &cache,
                                const std::function<bool()> &cancel={},bool refresh=false,bool cachedOnly=false,bool verify=false,int maxDepth=0,const QJsonObject &observed={});
ImageResult cachedCollectionCover(const QString &path,const NamePolicy &policy,
                             const QJsonObject &metadata,bool spine,
                             const std::shared_ptr<ThumbnailDiskCache> &cache,
                             const std::function<bool()> &cancel={},bool cachedOnly=false,bool verify=false,int maxDepth=0,const QJsonObject &observed={});
