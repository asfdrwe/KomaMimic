// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QJsonObject>
struct ZipMetadata {
  QJsonObject data;
  QByteArray originalComment;
  QString error;
};
ZipMetadata readZipMetadata(const QString &path);
QString writeZipMetadata(const QString &source, const QString &destination,
                         const QJsonObject &metadata);
