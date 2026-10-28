// SPDX-License-Identifier: GPL-3.0-only
#include "media.h"
#include <QCoreApplication>
#include "documentsource.h"
#include "apng.h"
#include "scriptreader.h"
#include <QBuffer>
#include <QFileInfo>
#include <QImageReader>
#include <QtEndian>
#include <algorithm>
namespace {
QSize jpeg2000Size(const QByteArray &bytes) {
    // The installed JasPer Qt handler has no predecode Size option. Validate
    // the JP2 codestream's SIZ dimensions before allowing it to allocate.
    const auto u32 = [&](qsizetype offset) { return qFromBigEndian<quint32>(bytes.constData() + offset); };
    qsizetype start = 0, end = bytes.size();
    if (!bytes.startsWith(QByteArray::fromHex("ff4fff51"))) {
        bool found = false;
        for (qsizetype offset = 0; offset + 8 <= bytes.size();) {
            quint64 size = u32(offset); qsizetype header = 8;
            if (size == 1) {
                if (offset + 16 > bytes.size()) return {};
                size = qFromBigEndian<quint64>(bytes.constData() + offset + 8); header = 16;
            } else if (!size) size = bytes.size() - offset;
            if (size < quint64(header) || size > quint64(bytes.size() - offset)) return {};
            if (bytes.mid(offset + 4, 4) == "jp2c") { start = offset + header; end = offset + qsizetype(size); found = true; break; }
            offset += qsizetype(size);
        }
        if (!found) return {};
    }
    if (end - start < 42 || bytes.mid(start, 4) != QByteArray::fromHex("ff4fff51")) return {};
    const quint32 x = u32(start + 8), y = u32(start + 12), ox = u32(start + 16), oy = u32(start + 20);
    const quint32 tileX = u32(start + 24), tileY = u32(start + 28);
    if (x <= ox || y <= oy || x - ox > MaxPixels || y - oy > MaxPixels
        || quint64(x - ox) * (y - oy) > MaxPixels || !tileX || !tileY || quint64(tileX) * tileY > MaxPixels) return {};
    return QSize(int(x - ox), int(y - oy));
}
}

qint64 ImageResult::memoryCost() const {
    if (frames.isEmpty()) return image.sizeInBytes();
    qint64 size = 0;
    for (const auto &frame : frames) size += frame.sizeInBytes();
    return size;
}
QStringList imageExtensions() {
    static const QStringList extensions = [] {
    // Admit raster codecs deliberately, not handlers invoking interpreters.
    const QStringList allowed = {"png", "apng", "jpg", "jpeg", "jpe", "jfif", "bmp", "gif", "webp", "tif", "tiff",
        "ico", "cur", "pbm", "pgm", "ppm", "xbm", "xpm", "tga", "avif", "jxl", "jp2", "j2k", "heif", "heic", "qoi"};
    const auto formats = QImageReader::supportedImageFormats();
    QStringList result;
    for (const auto &ext : allowed) {
        const auto canonical = ext == "apng" ? QByteArray("png") : ext == "jfif" || ext == "jpe" ? QByteArray("jpeg") : ext.toLatin1();
        if (formats.contains(canonical)) result.append(ext);
    }
    return result;
    }();
    auto result = extensions;
    for (const auto &extension : scriptImageExtensions()) if (!result.contains(extension)) result.append(extension);
    return result;
}
bool isImagePath(const QString &path) { return imageExtensions().contains(QFileInfo(path).suffix().toLower()); }
bool isArchivePath(const QString &path) {
    const QString name = QFileInfo(path).fileName().toLower();
    static const QStringList endings = {".zip", ".cbz", ".rar", ".cbr", ".7z", ".cb7", ".lha", ".lzh", ".cbl",
        ".tar", ".cbt", ".tar.gz", ".tgz", ".tar.bz2", ".tbz", ".tbz2", ".tar.xz", ".txz", ".tar.zst", ".tzst", ".cab"};
    return std::any_of(endings.cbegin(), endings.cend(), [&](const QString &ending) { return name.endsWith(ending); });
}
QString openFileFilter() {
    QStringList patterns;
    for (const auto &ext : imageExtensions()) patterns += "*." + ext;
    const QString archives = "*.zip *.cbz *.rar *.cbr *.7z *.cb7 *.lha *.lzh *.cbl *.tar *.cbt *.tar.gz *.tgz *.tar.bz2 *.tbz *.tbz2 *.tar.xz *.txz *.tar.zst *.tzst *.cab";
    return QCoreApplication::translate("KomaMimic", "対応する画像・書庫・文書 (") + patterns.join(' ') + ' ' + archives + QCoreApplication::translate("KomaMimic", " *.pdf *.txt *.mmt *.md *.markdown *.epub);;画像 (") + patterns.join(' ') + QCoreApplication::translate("KomaMimic", ");;書庫 (") + archives + QCoreApplication::translate("KomaMimic", ");;文書 (*.pdf *.txt *.mmt *.md *.markdown *.epub);;すべてのファイル (*)");
}
int documentPages(const QByteArray &bytes) {
    QBuffer buffer; buffer.setData(bytes); buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    if (reader.format() != "tiff" && reader.format() != "tif") return 1;
    return std::clamp(reader.imageCount(), 1, 10000);
}
ImageResult decodeImage(const QByteArray &bytes, const QString &name, QSize thumbnail, int documentPage, const QVariantMap &scriptSettings, const std::function<bool()> &cancel) {
    if (bytes.size() > MaxEncodedBytes) return {{}, QCoreApplication::translate("KomaMimic", "画像の入力サイズが上限を超えています。")};
    {
        bool recognized = false;
        const auto scripted = decodeScriptImage(bytes, name, thumbnail, recognized, false,documentPage,scriptSettings,cancel);
        if (recognized) return scripted;
    }
    if (!thumbnail.isValid() && documentPage == 0) { bool apng = false; const auto animation = decodeApng(bytes, apng); if (apng) return animation; }
    ImageResult result;
    QBuffer buffer; buffer.setData(bytes); buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer); reader.setAutoTransform(true);
    if (documentPage == 0 && !reader.canRead()) {
        bool recognized = false;
        const auto scripted = decodeScriptImage(bytes, name, thumbnail, recognized,true,documentPage,scriptSettings,cancel);
        if (recognized) return scripted;
    }
    // Payload signature determines the codec, independently of the suffix.
    // Some TIFF handlers reopen their device during jumpToImage(), so select
    // the IFD before asking size() to open it at the current device position.
    if (documentPage > 0 && !reader.jumpToImage(documentPage))
        return {{}, QCoreApplication::translate("KomaMimic", "文書のページを読めません: %1 [%2]").arg(name).arg(documentPage + 1)};
    QSize size = reader.size();
    if (!size.isValid() && (reader.format() == "jp2" || reader.format() == "j2k")) size = jpeg2000Size(bytes);
    if (!size.isValid() || qint64(size.width()) * size.height() > MaxPixels)
        return {{}, QCoreApplication::translate("KomaMimic", "画像寸法が不正、または上限（約48メガピクセル）を超えています: %1\n%2").arg(name, reader.errorString())};
    if (thumbnail.isValid()) reader.setScaledSize(size.scaled(thumbnail, Qt::KeepAspectRatio));
    result.image = reader.read();
    if (result.image.isNull()) return {{}, QCoreApplication::translate("KomaMimic", "画像を読めません: %1\n%2").arg(name, reader.errorString())};
    result.image.setDevicePixelRatio(1);
    if (qint64(result.image.width()) * result.image.height() > MaxPixels) return {{}, QCoreApplication::translate("KomaMimic", "画像の寸法が上限を超えています。")};
    if (!reader.supportsAnimation() || thumbnail.isValid()) return result;
    result.loopCount = reader.loopCount();
    result.frames.append(result.image);
    result.delays.append(reader.nextImageDelay() <= 0 ? 100 : std::max(10, reader.nextImageDelay()));
    qint64 memory = result.image.sizeInBytes();
    // Bound predecoded animation memory; all decoding stays in workers.
    while (reader.canRead()) {
        const QSize nextSize = reader.size();
        if (!nextSize.isValid() || qint64(nextSize.width()) * nextSize.height() > MaxPixels) {
            result.error = QCoreApplication::translate("KomaMimic", "アニメーションの画像寸法が上限を超えています。先頭画像を表示します。"); break;
        }
        QImage frame = reader.read();
        if (frame.isNull()) { result.error = QCoreApplication::translate("KomaMimic", "アニメーションが壊れています。先頭画像を表示します。"); break; }
        memory += frame.sizeInBytes();
        if (result.frames.size() >= 2000 || memory > 128 * 1024 * 1024) {
            result.error = QCoreApplication::translate("KomaMimic", "アニメーションの上限（2,000フレーム/128 MiB）を超えています。先頭画像を表示します。"); break;
        }
        frame.setDevicePixelRatio(1);
        result.frames.append(frame);
        result.delays.append(reader.nextImageDelay() <= 0 ? 100 : std::max(10, reader.nextImageDelay()));
    }
    if (!result.error.isEmpty() || result.frames.size() <= 1) { result.frames.clear(); result.delays.clear(); }
    return result;
}
