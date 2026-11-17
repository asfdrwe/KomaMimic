// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QDir>
#include <QFileInfo>
#include <QStringList>

// OS APIs may return /var while the launcher uses /private/var. Resolve the
// existing ancestor, also when the final application/cache directory is new.
inline QString resolvedTestPath(const QString &path) {
    if (path.isEmpty() || !QDir::isAbsolutePath(path)) return {};
    QString ancestor = path;
    QStringList tail;
    for (;;) {
        const QFileInfo info(ancestor);
        const auto real = info.canonicalFilePath();
        if (!real.isEmpty())
            return tail.isEmpty() ? real : QDir(real).filePath(tail.join('/'));
        if (info.isSymLink()) return {}; // An unresolved link cannot prove isolation.
        const auto parent = info.absolutePath();
        if (parent == ancestor || info.fileName().isEmpty()) return {};
        tail.prepend(info.fileName());
        ancestor = parent;
    }
}

inline bool testPathIsInside(const QString &root, const QString &path) {
    if (root.isEmpty() || !QDir::isAbsolutePath(root)) return false;
    const auto parent = QDir(root).canonicalPath();
    const auto child = resolvedTestPath(path);
    if (parent.isEmpty() || child.isEmpty()) return false;
    const auto relative = QDir(parent).relativeFilePath(child);
    return !QDir::isAbsolutePath(relative) && relative != ".." &&
           !relative.startsWith("../");
}
