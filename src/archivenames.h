// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "filenames.h"
#include <QVector>
#include <functional>
#include <atomic>
#include <QSet>
struct ArchiveReadActivity {
    inline static std::atomic<quint64> metadataBytes{0},previewBytes{0},previewEntries{0};
};
struct ArchiveNames {
    QVector<ArchiveName> entries;
    QString error;
    qint64 directoryOffset=-1;
};
enum class ZipNamesMode { Complete, Cover };
ArchiveNames readZipNames(const QString &path,const std::function<bool()> &cancel={},bool strictLocal=true,
                         ZipNamesMode mode=ZipNamesMode::Complete,const NamePolicy &policy={});
struct ZipCoverNames {ArchiveNames archive;QString label;};
ZipCoverNames selectZipCover(const QString &path,const NamePolicy &policy,const QSet<QString> &imageSuffixes,
                            NameDecoder &decoder,const std::function<bool()> &cancel={});
struct ZipEntryData {QByteArray bytes;QString error;};
ZipEntryData readZipEntry(const QString &path,const ArchiveNames &names,int index,
                         const std::function<bool()> &cancel={},const QString &password={});
ArchiveNames readLhaNames(const QString &path,const std::function<bool()> &cancel={});
