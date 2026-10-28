// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QImage>
#include <QVector>
#include <QStringList>
#include <QVariantMap>
#include <functional>
// Cache accounting in bytes; container costs are rounded up to KiB.
struct MemoryCacheInfo { int entries=0; qint64 bytes=0,limitBytes=0; };
struct ImageResult {
    ImageResult(QImage first = {}, QString diagnostic = {}) : image(std::move(first)), error(std::move(diagnostic)) {}
    QImage image;
    QString error;
    QVector<QImage> frames;
    QVector<int> delays;
    // Qt: -1 = forever, 0 = one playback, N = N extra repeats.
    int loopCount = 0;
    qint64 memoryCost() const;
};
constexpr qint64 MaxEncodedBytes = 64 * 1024 * 1024;
constexpr qint64 MaxPixels = 48 * 1024 * 1024;
QStringList imageExtensions();
bool isImagePath(const QString &path);
bool isArchivePath(const QString &path);
QString openFileFilter();
ImageResult decodeImage(const QByteArray &bytes, const QString &name, QSize thumbnail = {}, int documentPage = 0,
                        const QVariantMap &scriptSettings = {}, const std::function<bool()> &cancel = {});
int documentPages(const QByteArray &bytes);
