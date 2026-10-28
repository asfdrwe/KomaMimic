// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QString>
struct ArchiveEntryPath {
    QString archive, id, name;
    bool valid() const { return !archive.isEmpty(); }
};
// Absolute logical paths, never host extraction paths. Names are URL-escaped.
QString archiveEntryPath(const QString &archive, const QString &id, const QString &name);
ArchiveEntryPath parseArchiveEntryPath(const QString &path);
QString physicalBookPath(const QString &path);
QString bookDisplayName(const QString &path);
QString bookIdentityPath(const QString &path);
bool bookPathExists(const QString &path);
