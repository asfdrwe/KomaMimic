// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "filenames.h"
#include <QVector>
#include <functional>
struct ArchiveNames {
    QVector<ArchiveName> entries;
    QString error;
};
ArchiveNames readZipNames(const QString &path,const std::function<bool()> &cancel={},bool strictLocal=true);
ArchiveNames readLhaNames(const QString &path,const std::function<bool()> &cancel={});
