// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <memory>
#include <atomic>

struct NamePolicy {
    QString legacyEncoding = "CP932";
    bool force = false;
    bool operator==(const NamePolicy &) const = default;
};

// Retain bytes and declarations separately from the displayed Unicode name.
// ZIP entries are ordered by local-header offset; LHA entries by header order.
struct ArchiveName {
    QByteArray rawName, rawDirectory, rawHeader, localHeader;
    QString declaredName, nameEncoding, directoryEncoding;
    QString warning;
    bool hasDeclaredName = false;
    bool invalidUnicode = false;
    bool lhaDirectory = false;
    quint16 zipFlags = 0;
    quint32 fileType = 0;
    QString error;
    qint64 offset = 0;
    qint64 size = 0;
    qint64 compressedSize = -1; // ZIP only; other formats retain their existing reader.
    qint64 endOffset = -1; // Next physical ZIP entry or central directory.
};
struct DecodedName {
    QString text, encoding, origin, warning;
    bool assumed = false;
};
QStringList legacyEncodings();
struct NameDecodeActivity {inline static std::atomic<quint64> converterOpens{0};};
// One worker/request owns a decoder; no ICU state is shared between workers.
class NameDecoder {
public:
    NameDecoder();
    ~NameDecoder();
    NameDecoder(const NameDecoder &)=delete;
    NameDecoder &operator=(const NameDecoder &)=delete;
    bool decode(const QByteArray &bytes,const QString &encoding,QString &text);
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
bool decodeStrict(const QByteArray &bytes, const QString &encoding, QString &text,NameDecoder *decoder=nullptr);
QString escapedName(const QByteArray &bytes);
DecodedName decodeName(const ArchiveName &name, const NamePolicy &policy,NameDecoder *decoder=nullptr);
