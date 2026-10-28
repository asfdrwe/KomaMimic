// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>

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
    qint64 offset = 0;
    qint64 size = 0;
};
struct DecodedName {
    QString text, encoding, origin, warning;
    bool assumed = false;
};
QStringList legacyEncodings();
bool decodeStrict(const QByteArray &bytes, const QString &encoding, QString &text);
QString escapedName(const QByteArray &bytes);
DecodedName decodeName(const ArchiveName &name, const NamePolicy &policy);
