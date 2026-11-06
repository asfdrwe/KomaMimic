// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "book.h"
#include "thumbnailcache.h"
#include <QJsonObject>
#include <QMap>
class QSettings;
struct ThumbnailNamePolicies {
    explicit ThumbnailNamePolicies(const QSettings &settings);
    static QString settingsKey(const QString &path);
    NamePolicy resolve(const QString &path) const;
    QList<NamePolicy> previews{ NamePolicy{} };
    QMap<QString,NamePolicy> saved;
};

ImageResult cachedCover(const QString &path,const NamePolicy &policy,
                        const std::shared_ptr<ThumbnailDiskCache> &cache,
                        const std::function<bool()> &cancel={},bool refresh=false,int depth=0,bool cachedOnly=false);
ImageResult cachedPageThumbnail(const std::shared_ptr<const Book> &book,int page,
                                const std::shared_ptr<ThumbnailDiskCache> &cache,
                                const std::function<bool()> &cancel={},bool refresh=false,bool cachedOnly=false);
ImageResult cachedCollectionCover(const QString &path,const NamePolicy &policy,
                             const QJsonObject &metadata,bool spine,
                             const std::shared_ptr<ThumbnailDiskCache> &cache,
                             const std::function<bool()> &cancel={},bool cachedOnly=false);
