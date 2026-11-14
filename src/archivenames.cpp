// SPDX-License-Identifier: GPL-3.0-only
#include "archivenames.h"
#include "media.h"
#include <archive.h>
#include <archive_entry.h>
#include <QCoreApplication>
#include <QFile>
#include <QtEndian>
#include <zlib.h>
#include <algorithm>
#include <limits>
#include <memory>
#include <cstring>
#include <cerrno>

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
    ArchiveReadActivity::metadataBytes+=quint64(b.size());
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

ArchiveNames readZipNames(const QString &path,const std::function<bool()> &cancel,bool strictLocal,ZipNamesMode mode,const NamePolicy &policy) {
    if(cancel && cancel())return failure(QCoreApplication::translate("KomaMimic", "書庫読込を取り消しました。"));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly|(mode==ZipNamesMode::Cover?QIODevice::Unbuffered:QIODevice::NotOpen))) return failure(file.errorString());
    const quint64 fileSize = quint64(file.size());
    quint64 tailSize = std::min<quint64>(fileSize, mode==ZipNamesMode::Cover?22:65557);
    auto tail = readAt(file, fileSize - tailSize, tailSize);
    const auto findEnd=[&] {
        for(qsizetype at=tail.size()-22;at>=0;--at)
            if(u32(tail,at)==0x06054b50 && at+22+u16(tail,at+20)==tail.size())return at;
        return qsizetype(-1);
    };
    auto e=findEnd();
    if(e<0 && mode==ZipNamesMode::Cover && tailSize<fileSize) {
        tailSize=std::min<quint64>(fileSize,65557);tail=readAt(file,fileSize-tailSize,tailSize);e=findEnd();
    }
    if (e < 0) return failure(QCoreApplication::translate("KomaMimic", "ZIPの終端headerがありません（分割ZIPは未対応です）。"));
    const quint64 end = fileSize - tailSize + quint64(e);
    if (u16(tail, e + 4) || u16(tail, e + 6) || u16(tail,e+8)!=u16(tail,e+10)) return failure(QCoreApplication::translate("KomaMimic", "分割ZIPは未対応です。"));
    quint64 count = u16(tail, e + 10), directorySize = u32(tail, e + 12), directoryOffset = u32(tail, e + 16);
    if (count == 65535 || directorySize == 0xffffffff || directoryOffset == 0xffffffff) {
        if (end < 20) return failure(QCoreApplication::translate("KomaMimic", "ZIP64 locatorがありません。"));
        const auto locator = readAt(file, end - 20, 20);
        if (locator.size() != 20 || u32(locator, 0) != 0x07064b50 || u32(locator, 4) || u32(locator, 16) != 1)
            return failure(QCoreApplication::translate("KomaMimic", "ZIP64 locatorが不正、または分割ZIPです。"));
        const auto record = readAt(file, u64(locator, 8), 56);
        if (record.size() != 56 || u32(record, 0) != 0x06064b50 || u32(record, 16) || u32(record, 20) || u64(record,24)!=u64(record,32))
            return failure(QCoreApplication::translate("KomaMimic", "ZIP64終端headerが不正です。"));
        count = u64(record, 32); directorySize = u64(record, 40); directoryOffset = u64(record, 48);
    }
    if (count > MaxEntries || directorySize > MaxMetadata || directoryOffset > fileSize
        || directorySize > fileSize - directoryOffset)
        return failure(QCoreApplication::translate("KomaMimic", "ZIPの項目数・名前情報が上限を超えたか、headerが不正です。"));
    const auto directory=readAt(file,directoryOffset,directorySize);
    if(quint64(directory.size())!=directorySize)return failure(QCoreApplication::translate("KomaMimic", "ZIPの名前情報が切れています。"));
    ArchiveNames result;
    result.directoryOffset=qint64(directoryOffset);
    quint64 cursor = directoryOffset;
    for (quint64 i = 0; i < count; ++i) {
        if(cancel && cancel())return failure(QCoreApplication::translate("KomaMimic", "書庫読込を取り消しました。"));
        const auto fixed=directory.mid(qsizetype(cursor-directoryOffset),46);
        if (fixed.size() != 46 || u32(fixed, 0) != 0x02014b50) return failure(QCoreApplication::translate("KomaMimic", "ZIPのcentral headerが不正です。"));
        const quint16 nameSize = u16(fixed, 28), extraSize = u16(fixed, 30), commentSize = u16(fixed, 32);
        const quint64 headerSize = 46 + quint64(nameSize) + extraSize + commentSize;
        if (cursor < directoryOffset || headerSize > directorySize || cursor - directoryOffset > directorySize - headerSize)
            return failure(QCoreApplication::translate("KomaMimic", "ZIPのcentral headerが範囲外です。"));
        ArchiveName name;
        name.rawHeader=directory.mid(qsizetype(cursor-directoryOffset),qsizetype(headerSize));
        if (quint64(name.rawHeader.size()) != headerSize) return failure(QCoreApplication::translate("KomaMimic", "ZIPの名前情報が切れています。"));
        name.rawName = name.rawHeader.mid(46, nameSize);
        name.zipFlags = u16(fixed, 8);
        const auto host=u16(fixed,4)>>8;
        const auto attributes=u32(fixed,38);
        // Only Unix/macOS creators encode POSIX mode in the upper word.
        name.fileType=(host==3 || host==19)?(attributes>>16)&0170000:0;
        if(!name.fileType)name.fileType=(name.rawName.endsWith('/') || (attributes&0x10))?0040000:0100000;
        quint64 offset = u32(fixed, 42), size = u32(fixed, 24), packed = u32(fixed, 20);
        const QByteArray extra = name.rawHeader.mid(46 + nameSize, extraSize);
        bool zip64Offset = offset != 0xffffffff, zip64Size = size != 0xffffffff, zip64Packed=packed!=0xffffffff;
        for (qsizetype p = 0; p + 4 <= extra.size();) {
            const quint16 type = u16(extra, p), length = u16(extra, p + 2); p += 4;
            if (length > extra.size() - p) break;
            if (type == 1) {
                qsizetype z = p;
                if (size == 0xffffffff && z + 8 <= p + length) { size = u64(extra, z); z += 8; zip64Size = true; }
                if (packed == 0xffffffff && z + 8 <= p + length) { packed=u64(extra,z);z+=8;zip64Packed=true; }
                if (offset == 0xffffffff && z + 8 <= p + length) { offset = u64(extra, z); zip64Offset = true; }
            }
            p += length;
        }
        if (!zip64Offset || !zip64Size || !zip64Packed || packed>quint64(std::numeric_limits<qint64>::max()) || offset > quint64(std::numeric_limits<qint64>::max())
            || size > quint64(std::numeric_limits<qint64>::max())) return failure(QCoreApplication::translate("KomaMimic", "ZIP64 entryが不正です。"));
        name.offset = qint64(offset); name.size = qint64(size);
        name.compressedSize=qint64(packed);
        const QString extraName = unicodeExtra(extra, name.rawName, name.warning);
        // Central Unicode/UTF-8 (or forced byte decoding) fixes the ordering.
        // Otherwise local Unicode extra may change even an ASCII filename;
        // retain that legacy name fallback, but read only its header bytes.
        const bool localNeeded=mode==ZipNamesMode::Complete || (!policy.force && !(name.zipFlags&0x800) && extraName.isEmpty());
        const auto local = localNeeded?readAt(file, offset, 30):QByteArray{};
        quint16 localNameSize=0,localExtraSize=0;
        if(localNeeded && (local.size()!=30 || u32(local,0)!=0x04034b50))name.error=QCoreApplication::translate("KomaMimic", "ZIPのlocal headerが不正です。");
        else if(localNeeded) {
            localNameSize=u16(local,26);localExtraSize=u16(local,28);
            name.localHeader=mode==ZipNamesMode::Cover?local+readAt(file,offset+30,quint64(localNameSize)+localExtraSize):
                readAt(file,offset,30+quint64(localNameSize)+localExtraSize);
            if(name.localHeader.size()!=30+localNameSize+localExtraSize)name.error=QCoreApplication::translate("KomaMimic", "ZIPのlocal名前情報が切れています。");
        }
        if(strictLocal && !name.error.isEmpty())return failure(name.error);
        const auto localName = name.localHeader.mid(30, localNameSize);
        if (localNeeded && (localName != name.rawName || (local.size()==30 && ((u16(local, 6) ^ name.zipFlags) & 0x800))))
            name.warning += QCoreApplication::translate("KomaMimic", " local/centralの名前・Unicode宣言が異なります。centralを優先します。");
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

ZipEntryData readZipEntry(const QString &path,const ArchiveNames &names,int index,const std::function<bool()> &cancel,const QString &password) {
    const auto invalid=[] {return QCoreApplication::translate("KomaMimic", "ZIPのlocal headerが不正です。");};
    const auto cancelled=[&] {return cancel && cancel();};
    if(cancelled())return {{},QCoreApplication::translate("KomaMimic", "書庫読込を取り消しました。")};
    if(index<0 || index>=names.entries.size())return {{},invalid()};
    const auto &selected=names.entries[index];
    if(!selected.error.isEmpty())return {{},selected.error};
    if(selected.size<0 || selected.size>MaxEncodedBytes || selected.compressedSize<0 || selected.compressedSize>2*MaxEncodedBytes)
        return {{},QCoreApplication::translate("KomaMimic", "画像の入力上限を超えています。")};
    // Present libarchive with a seekable, single-entry view. Its local header
    // and compressed data map to the original file; its tiny central directory
    // maps to memory. Other entries' headers and payloads are never visited.
    struct View {
        QFile file;QByteArray tail,buffer=QByteArray(64*1024,char(0));
        qint64 start=0,span=0,position=0;
        std::function<bool()> cancel;
        qint64 size() const {return span+tail.size();}
        bool stopped(archive *a) const {
            if(cancel && cancel()){archive_set_error(a,ECANCELED,"Archive read cancelled");return true;}return false;
        }
    } view;
    view.file.setFileName(path);view.cancel=cancel;view.start=selected.offset;
    if(!view.file.open(QIODevice::ReadOnly|QIODevice::Unbuffered))return {{},view.file.errorString()};
    const auto limit=index+1<names.entries.size()?std::min(names.entries[index+1].offset,names.directoryOffset):names.directoryOffset;
    if(view.start<0 || limit<view.start || limit>view.file.size())return {{},invalid()};
    const auto fixed=readAt(view.file,quint64(view.start),30);
    if(fixed.size()!=30 || u32(fixed,0)!=0x04034b50 || selected.rawHeader.size()<46 ||
       u16(fixed,8)!=u16(selected.rawHeader,10) || ((u16(fixed,6)^selected.zipFlags)&0x49))return {{},invalid()};
    const qint64 headerSize=30+qint64(u16(fixed,26))+u16(fixed,28);
    if(headerSize>limit-view.start || selected.compressedSize>limit-view.start-headerSize)return {{},invalid()};
    const qint64 payloadEnd=headerSize+selected.compressedSize;
    view.span=payloadEnd+((selected.zipFlags&8)?std::min<qint64>(24,limit-view.start-payloadEnd):0);
    view.tail=selected.rawHeader;
    qToLittleEndian<quint32>(0,view.tail.data()+42);
    QByteArray end(22,char(0));qToLittleEndian<quint32>(0x06054b50,end.data());
    qToLittleEndian<quint16>(1,end.data()+8);qToLittleEndian<quint16>(1,end.data()+10);
    qToLittleEndian<quint32>(quint32(view.tail.size()),end.data()+12);
    qToLittleEndian<quint32>(quint32(view.span),end.data()+16);view.tail+=end;
    std::unique_ptr<archive,decltype(&archive_read_free)> reader(archive_read_new(),archive_read_free);
    if(!reader)return {{},QCoreApplication::translate("KomaMimic", "書庫リーダを作成できません。")};
    archive_read_support_filter_none(reader.get());archive_read_support_format_zip_seekable(reader.get());
    archive_read_set_format_option(reader.get(),"zip","hdrcharset","CP437");
    archive_read_set_format_option(reader.get(),"zip","mac-ext","0");
    if(!password.isEmpty())archive_read_add_passphrase(reader.get(),password.toUtf8().constData());
    archive_read_set_seek_callback(reader.get(),[](archive *a,void *opaque,la_int64_t offset,int whence)->la_int64_t {
        auto &v=*static_cast<View*>(opaque);if(v.stopped(a))return ARCHIVE_FATAL;
        const auto base=whence==SEEK_SET?0:whence==SEEK_CUR?v.position:v.size();
        if(offset< -base || offset>v.size()-base)return ARCHIVE_FATAL;
        v.position=base+offset;return v.position;
    });
    const auto opened=archive_read_open2(reader.get(),&view,nullptr,
        [](archive *a,void *opaque,const void **buffer)->la_ssize_t {
            auto &v=*static_cast<View*>(opaque);if(v.stopped(a))return ARCHIVE_FATAL;
            if(v.position>=v.size())return 0;
            qint64 count=0;
            if(v.position<v.span) {
                if(!v.file.seek(v.start+v.position))return ARCHIVE_FATAL;
                count=v.file.read(v.buffer.data(),std::min<qint64>(v.buffer.size(),v.span-v.position));
                if(count<=0)return ARCHIVE_FATAL;
                ArchiveReadActivity::previewBytes+=quint64(count);
                *buffer=v.buffer.constData();
            } else {
                count=std::min<qint64>(v.buffer.size(),v.size()-v.position);
                *buffer=v.tail.constData()+v.position-v.span;
            }
            v.position+=count;return count;
        },[](archive *a,void *opaque,la_int64_t distance)->la_int64_t {
            auto &v=*static_cast<View*>(opaque);if(v.stopped(a))return 0;
            const auto count=std::clamp<qint64>(distance,0,v.size()-v.position);v.position+=count;return count;
        },nullptr);
    const auto diagnostic=[&] {const auto message=archive_error_string(reader.get());return message?QString::fromUtf8(message):invalid();};
    if(opened!=ARCHIVE_OK)return {{},diagnostic()};
    archive_entry *entry=nullptr;const auto status=archive_read_next_header(reader.get(),&entry);
    if(status!=ARCHIVE_OK && status!=ARCHIVE_WARN)return {{},diagnostic()};
    if(archive_entry_is_encrypted(entry)>0 && password.isEmpty())return {{},QCoreApplication::translate("KomaMimic", "書庫のパスワードが必要です。")};
    ++ArchiveReadActivity::previewEntries;
    QByteArray bytes;char buffer[64*1024];la_ssize_t count;
    while((count=archive_read_data(reader.get(),buffer,sizeof buffer))>0) {
        if(cancelled())return {{},QCoreApplication::translate("KomaMimic", "書庫読込を取り消しました。")};
        if(count>MaxEncodedBytes-bytes.size())return {{},QCoreApplication::translate("KomaMimic", "画像の入力上限を超えています。")};
        bytes.append(buffer,count);
    }
    if(count<0)return {{},diagnostic()};
    // AE-2 uses authenticated encryption instead of the central CRC field.
    bool authenticated=false;
    if(u16(selected.rawHeader,10)==99) {
        const auto extra=selected.rawHeader.mid(46+u16(selected.rawHeader,28),u16(selected.rawHeader,30));
        for(qsizetype p=0;p+4<=extra.size();) {
            const auto type=u16(extra,p),length=u16(extra,p+2);p+=4;if(length>extra.size()-p)break;
            if(type==0x9901 && length>=7 && u16(extra,p)==2)authenticated=true;
            p+=length;
        }
    }
    if(bytes.size()!=selected.size || (!authenticated && quint32(crc32(0,reinterpret_cast<const Bytef*>(bytes.constData()),uInt(bytes.size())))!=u32(selected.rawHeader,16)))
        return {{},invalid()};
    return {bytes,{}};
}

ArchiveNames readLhaNames(const QString &path,const std::function<bool()> &cancel) {
    if(cancel && cancel())return failure(QCoreApplication::translate("KomaMimic", "書庫読込を取り消しました。"));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return failure(file.errorString());
    ArchiveNames result;
    quint64 offset = 0, metadataBytes = 0;
    while (offset < quint64(file.size())) {
        if(cancel && cancel())return failure(QCoreApplication::translate("KomaMimic", "書庫読込を取り消しました。"));
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
                if(cancel && cancel())return failure(QCoreApplication::translate("KomaMimic", "書庫読込を取り消しました。"));
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
