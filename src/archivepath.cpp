// SPDX-License-Identifier: GPL-3.0-only
#include "archivepath.h"
#include "media.h"
#include <QFileInfo>
#include <QRegularExpression>
#include <QUrl>
namespace { const QString Marker = "/__komamimic_archive__/"; }
QString archiveEntryPath(const QString &archive, const QString &id, const QString &name) {
    return QFileInfo(archive).absoluteFilePath() + Marker + id + '/' +
        QString::fromLatin1(QUrl::toPercentEncoding(QFileInfo(name).fileName()));
}
ArchiveEntryPath parseArchiveEntryPath(const QString &path) {
    const auto position = path.lastIndexOf(Marker);
    if (position < 0) return {};
    const auto archive = path.left(position), tail = path.mid(position + Marker.size());
    const auto id = tail.section('/', 0, 0), escaped = tail.section('/', 1);
    static const QRegularExpression validId("^[0-9a-f]{64}(?:-[1-9][0-9]*)?$");
    if (!isArchivePath(archive) || !QFileInfo(archive).isAbsolute() || !validId.match(id).hasMatch() ||
        escaped.isEmpty() || escaped.contains('/')) return {};
    const auto name = QUrl::fromPercentEncoding(escaped.toLatin1());
    if (name.contains(QChar(0)) || name.contains('/') || name.isEmpty()) return {};
    return {archive, id, name};
}
QString physicalBookPath(const QString &path) {
    const auto entry = parseArchiveEntryPath(path);
    return entry.valid() ? entry.archive : path;
}
QString bookDisplayName(const QString &path) {
    const auto entry = parseArchiveEntryPath(path);
    return entry.valid() ? entry.name : QFileInfo(path).fileName();
}
QString bookIdentityPath(const QString &path) {
    const auto entry = parseArchiveEntryPath(path);
    return entry.valid() ? entry.archive + Marker + entry.id : QFileInfo(path).absoluteFilePath();
}
bool bookPathExists(const QString &path) { return QFileInfo::exists(physicalBookPath(path)); }
