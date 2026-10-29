// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "book.h"
#include "thumbnailcache.h"
#include <QJsonObject>

ImageResult cachedCover(const QString &path,const NamePolicy &policy,
                        const std::shared_ptr<ThumbnailDiskCache> &cache,
                        const std::function<bool()> &cancel={},bool refresh=false,int depth=0);
ImageResult cachedPageThumbnail(const std::shared_ptr<const Book> &book,int page,
                                const std::shared_ptr<ThumbnailDiskCache> &cache,
                                const std::function<bool()> &cancel={},bool refresh=false);
QImage cachedCollectionCover(const QString &path,const NamePolicy &policy,
                             const QJsonObject &metadata,bool spine,
                             const std::shared_ptr<ThumbnailDiskCache> &cache,
                             const std::function<bool()> &cancel={});
