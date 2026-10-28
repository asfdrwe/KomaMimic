// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "filenames.h"
#include <QVector>
struct ArchiveNames {
    QVector<ArchiveName> entries;
    QString error;
};
ArchiveNames readZipNames(const QString &path);
ArchiveNames readLhaNames(const QString &path);
