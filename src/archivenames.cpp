// SPDX-License-Identifier: GPL-3.0-only
#include "archivenames.h"
#include <QCoreApplication>
#include <QFile>
#include <QtEndian>
#include <zlib.h>
#include <algorithm>
#include <limits>

namespace {
constexpr quint64 MaxMetadata = 64 * 1024 * 1024;
constexpr quint64 MaxEntries = 100000;
quint16 u16(const QByteArray &b, qsizetype p) { return qFromLittleEndian<quint16>(b.constData() + p); }
quint32 u32(const QByteArray &b, qsizetype p) { return qFromLittleEndian<quint32>(b.constData() + p); }
quint64 u64(const QByteArray &b, qsizetype p) { return qFromLittleEndian<quint64>(b.constData() + p); }
QByteArray readAt(QFile &f, quint64 offset, quint64 length) {
    if (offset > quint64(f.size()) || length > quint64(f.size()) - offset || length > MaxMetadata) return {};
    if (!f.seek(qint64(offset))) return {};
    const auto b = f.read(qint64(length));
    return quint64(b.size()) == length ? b : QByteArray{};
}
QString unicodeExtra(const QByteArray &extra, const QByteArray &raw, QString &warning) {
    QString accepted; bool found = false, conflict = false;
    for (qsizetype p = 0; p + 4 <= extra.size();) {
        const quint16 type = u16(extra, p), length = u16(extra, p + 2); p += 4;
        if (length > extra.size() - p) { warning += QCoreApplication::translate("KomaMimic", " ZIP拡張headerが切れています。"); break; }
        if (type == 0x7075) {
            QString candidate;
            const quint32 crc = quint32(crc32(0, reinterpret_cast<const Bytef *>(raw.constData()), uInt(raw.size())));
            if (length < 5 || quint8(extra[p]) != 1 || u32(extra, p + 1) != crc
                || !decodeStrict(extra.mid(p + 5, length - 5), "UTF-8", candidate))
                warning += QCoreApplication::translate("KomaMimic", " 無効なUnicode Path extraを無視しました。");
            else if (found && accepted != candidate) { conflict = true; warning += QCoreApplication::translate("KomaMimic", " Unicode Path extraが矛盾しています。"); }
            else { found = true; accepted = candidate; }
        }
        p += length;
    }
    return conflict ? QString{} : accepted;
}
ArchiveNames failure(const QString &message) { return {{}, message}; }
}

ArchiveNames readZipNames(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return failure(file.errorString());
    const quint64 fileSize = quint64(file.size());
    const quint64 tailSize = std::min<quint64>(fileSize, 65557);
    const auto tail = readAt(file, fileSize - tailSize, tailSize);
    qsizetype e = tail.size() - 22;
    for (; e >= 0; --e) {
        if (u32(tail, e) == 0x06054b50 && e + 22 + u16(tail, e + 20) == tail.size()) break;
    }
    if (e < 0) return failure(QCoreApplication::translate("KomaMimic", "ZIPの終端headerがありません（分割ZIPは未対応です）。"));
    const quint64 end = fileSize - tailSize + quint64(e);
    if (u16(tail, e + 4) || u16(tail, e + 6)) return failure(QCoreApplication::translate("KomaMimic", "分割ZIPは未対応です。"));
    quint64 count = u16(tail, e + 10), directorySize = u32(tail, e + 12), directoryOffset = u32(tail, e + 16);
    if (count == 65535 || directorySize == 0xffffffff || directoryOffset == 0xffffffff) {
        if (end < 20) return failure(QCoreApplication::translate("KomaMimic", "ZIP64 locatorがありません。"));
        const auto locator = readAt(file, end - 20, 20);
        if (locator.size() != 20 || u32(locator, 0) != 0x07064b50 || u32(locator, 4) || u32(locator, 16) != 1)
            return failure(QCoreApplication::translate("KomaMimic", "ZIP64 locatorが不正、または分割ZIPです。"));
        const auto record = readAt(file, u64(locator, 8), 56);
        if (record.size() != 56 || u32(record, 0) != 0x06064b50 || u32(record, 16) || u32(record, 20))
            return failure(QCoreApplication::translate("KomaMimic", "ZIP64終端headerが不正です。"));
        count = u64(record, 32); directorySize = u64(record, 40); directoryOffset = u64(record, 48);
    }
    if (count > MaxEntries || directorySize > MaxMetadata || directoryOffset > fileSize
        || directorySize > fileSize - directoryOffset)
        return failure(QCoreApplication::translate("KomaMimic", "ZIPの項目数・名前情報が上限を超えたか、headerが不正です。"));
    ArchiveNames result;
    quint64 cursor = directoryOffset;
    for (quint64 i = 0; i < count; ++i) {
        const auto fixed = readAt(file, cursor, 46);
        if (fixed.size() != 46 || u32(fixed, 0) != 0x02014b50) return failure(QCoreApplication::translate("KomaMimic", "ZIPのcentral headerが不正です。"));
        const quint16 nameSize = u16(fixed, 28), extraSize = u16(fixed, 30), commentSize = u16(fixed, 32);
        const quint64 headerSize = 46 + quint64(nameSize) + extraSize + commentSize;
        if (cursor < directoryOffset || headerSize > directorySize || cursor - directoryOffset > directorySize - headerSize)
            return failure(QCoreApplication::translate("KomaMimic", "ZIPのcentral headerが範囲外です。"));
        ArchiveName name;
        name.rawHeader = readAt(file, cursor, headerSize);
        if (quint64(name.rawHeader.size()) != headerSize) return failure(QCoreApplication::translate("KomaMimic", "ZIPの名前情報が切れています。"));
        name.rawName = name.rawHeader.mid(46, nameSize);
        name.zipFlags = u16(fixed, 8);
        quint64 offset = u32(fixed, 42), size = u32(fixed, 24);
        const QByteArray extra = name.rawHeader.mid(46 + nameSize, extraSize);
        bool zip64Offset = offset != 0xffffffff, zip64Size = size != 0xffffffff;
        for (qsizetype p = 0; p + 4 <= extra.size();) {
            const quint16 type = u16(extra, p), length = u16(extra, p + 2); p += 4;
            if (length > extra.size() - p) break;
            if (type == 1) {
                qsizetype z = p;
                if (size == 0xffffffff && z + 8 <= p + length) { size = u64(extra, z); z += 8; zip64Size = true; }
                if (u32(fixed, 20) == 0xffffffff) z += 8;
                if (offset == 0xffffffff && z + 8 <= p + length) { offset = u64(extra, z); zip64Offset = true; }
            }
            p += length;
        }
        if (!zip64Offset || !zip64Size || offset > quint64(std::numeric_limits<qint64>::max())
            || size > quint64(std::numeric_limits<qint64>::max())) return failure(QCoreApplication::translate("KomaMimic", "ZIP64 entryが不正です。"));
        name.offset = qint64(offset); name.size = qint64(size);
        const auto local = readAt(file, offset, 30);
        if (local.size() != 30 || u32(local, 0) != 0x04034b50) return failure(QCoreApplication::translate("KomaMimic", "ZIPのlocal headerが不正です。"));
        const quint16 localNameSize = u16(local, 26), localExtraSize = u16(local, 28);
        name.localHeader = readAt(file, offset, 30 + quint64(localNameSize) + localExtraSize);
        if (name.localHeader.size() != 30 + localNameSize + localExtraSize) return failure(QCoreApplication::translate("KomaMimic", "ZIPのlocal名前情報が切れています。"));
        const auto localName = name.localHeader.mid(30, localNameSize);
        if (localName != name.rawName || ((u16(local, 6) ^ name.zipFlags) & 0x800))
            name.warning += QCoreApplication::translate("KomaMimic", " local/centralの名前・Unicode宣言が異なります。centralを優先します。");
        const QString extraName = unicodeExtra(extra, name.rawName, name.warning);
        const QString localExtraName = unicodeExtra(name.localHeader.mid(30 + localNameSize), localName, name.warning);
        if (name.zipFlags & 0x800) {
            name.hasDeclaredName = decodeStrict(name.rawName, "UTF-8", name.declaredName);
            name.invalidUnicode = !name.hasDeclaredName;
            if (name.invalidUnicode) name.warning += QCoreApplication::translate("KomaMimic", " UTF-8宣言に対し不正なbyte列です。");
            if (!extraName.isEmpty() && name.hasDeclaredName && extraName != name.declaredName)
                name.warning += QCoreApplication::translate("KomaMimic", " UTF-8名とUnicode extraが異なります。UTF-8 flagを優先します。");
        } else {
            name.declaredName = !extraName.isEmpty() ? extraName : (localName == name.rawName ? localExtraName : QString{});
            name.hasDeclaredName = !name.declaredName.isEmpty();
            if (!extraName.isEmpty() && !localExtraName.isEmpty() && extraName != localExtraName)
                name.warning += QCoreApplication::translate("KomaMimic", " local/centralのUnicode extraが異なります。centralを優先します。");
        }
        result.entries.append(std::move(name));
        cursor += headerSize;
    }
    std::stable_sort(result.entries.begin(), result.entries.end(), [](const ArchiveName &a, const ArchiveName &b) { return a.offset < b.offset; });
    for (qsizetype i = 1; i < result.entries.size(); ++i)
        if (result.entries[i - 1].offset == result.entries[i].offset) return failure(QCoreApplication::translate("KomaMimic", "同じlocal headerを指す重複ZIP entryは読めません。"));
    return result;
}

ArchiveNames readLhaNames(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return failure(file.errorString());
    ArchiveNames result;
    quint64 offset = 0, metadataBytes = 0;
    while (offset < quint64(file.size())) {
        const quint64 remaining = quint64(file.size()) - offset;
        if (remaining < 21) {
            const auto end = readAt(file, offset, remaining);
            if (!end.isEmpty() && end[0] == 0) break;
            return failure(QCoreApplication::translate("KomaMimic", "LHAのheaderが切れています。"));
        }
        auto fixed = readAt(file, offset, 21);
        if (fixed[2] != '-' || fixed[6] != '-') return failure(QCoreApplication::translate("KomaMimic", "LHAのheaderが不正です（SFXは未対応です）。"));
        if (quint64(result.entries.size()) >= MaxEntries) return failure(QCoreApplication::translate("KomaMimic", "LHA内の項目数が上限を超えています。"));
        const int level = quint8(fixed[20]);
        quint64 packed = u32(fixed, 7), totalHeader = 0, chain = 0, baseSize = 0;
        int width = 2;
        ArchiveName name; name.offset = qint64(offset); name.size = u32(fixed, 11);
        QByteArray unicodeFile, unicodeDirectory;
        QString declaredCodepage;
        if (level == 0 || level == 1) {
            baseSize = quint8(fixed[0]) + 2;
            const auto base = readAt(file, offset, baseSize);
            if (base.size() < 22 || quint8(base[21]) > base.size() - 22) return failure(QCoreApplication::translate("KomaMimic", "LHAの基本名前headerが不正です。"));
            name.rawName = base.mid(22, quint8(base[21]));
            totalHeader = baseSize;
            if (level == 1) {
                if (baseSize < 27) return failure(QCoreApplication::translate("KomaMimic", "LHA level 1 headerが不正です。"));
                chain = baseSize - 2;
            }
        } else if (level == 2 || level == 3) {
            const auto base = readAt(file, offset, level == 2 ? 24 : 28);
            if (base.size() != (level == 2 ? 24 : 28)) return failure(QCoreApplication::translate("KomaMimic", "LHAのheaderが切れています。"));
            if (level == 3 && u16(base, 0) != 4) return failure(QCoreApplication::translate("KomaMimic", "LHA level 3のサイズ幅が不正です。"));
            totalHeader = level == 2 ? u16(base, 0) : u32(base, 24);
            baseSize = chain = level == 2 ? 24 : 28;
            width = level == 2 ? 2 : 4;
        } else return failure(QCoreApplication::translate("KomaMimic", "未対応のLHA header levelです。"));
        if (level > 0) {
            quint64 extended = 0;
            for (;;) {
                const auto sizeField = readAt(file, offset + chain, quint64(width));
                if (sizeField.size() != width) return failure(QCoreApplication::translate("KomaMimic", "LHA拡張headerが切れています。"));
                const quint64 size = width == 2 ? u16(sizeField, 0) : u32(sizeField, 0);
                if (!size) { chain += quint64(width); break; }
                if (size <= quint64(width) || size > MaxMetadata || extended > MaxMetadata - size
                    || (level > 1 && (chain > totalHeader || size > totalHeader - chain)))
                    return failure(QCoreApplication::translate("KomaMimic", "LHA拡張headerのサイズが不正です。"));
                const auto ext = readAt(file, offset + chain, size);
                if (quint64(ext.size()) != size) return failure(QCoreApplication::translate("KomaMimic", "LHA拡張headerが切れています。"));
                const int type = quint8(ext[width]);
                const auto data = ext.mid(width + 1);
                if (type == 1) name.rawName = data;
                else if (type == 2) { name.rawDirectory = data; name.lhaDirectory = true; }
                else if (type == 0x44) unicodeFile = data;
                else if (type == 0x45) unicodeDirectory = data;
                else if (type == 0x46 && data.size() == 4) {
                    const quint32 cp = u32(data, 0);
                    declaredCodepage = cp == 65001 ? "UTF-8" : QString("CP%1").arg(cp);
                } else if (type == 0x42 && data.size() == 16) {
                    packed = u64(data, 0);
                    const quint64 size64 = u64(data, 8);
                    if (size64 > quint64(std::numeric_limits<qint64>::max())) return failure(QCoreApplication::translate("KomaMimic", "LHAの画像サイズが不正です。"));
                    name.size = qint64(size64);
                }
                chain += size; extended += size;
            }
            if (level == 1) {
                const quint64 extraBytes = chain - baseSize;
                if (packed < extraBytes) return failure(QCoreApplication::translate("KomaMimic", "LHA level 1の圧縮サイズが不正です。"));
                packed -= extraBytes; totalHeader = chain;
            } else if (chain > totalHeader) return failure(QCoreApplication::translate("KomaMimic", "LHAの拡張headerが範囲外です。"));
        }
        name.nameEncoding = name.directoryEncoding = declaredCodepage;
        if (!unicodeFile.isEmpty()) { name.rawName = unicodeFile; name.nameEncoding = "UTF-16LE"; }
        if (!unicodeDirectory.isEmpty()) { name.rawDirectory = unicodeDirectory; name.directoryEncoding = "UTF-16LE"; name.lhaDirectory = true; }
        if (totalHeader > MaxMetadata || metadataBytes > MaxMetadata - totalHeader || totalHeader > remaining || packed > remaining - totalHeader)
            return failure(QCoreApplication::translate("KomaMimic", "LHAのheader・データが範囲外か、名前情報の上限を超えています。"));
        name.rawHeader = readAt(file, offset, totalHeader);
        metadataBytes += totalHeader;
        result.entries.append(std::move(name));
        offset += totalHeader + packed;
    }
    return result;
}
