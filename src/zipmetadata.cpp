// SPDX-License-Identifier: GPL-3.0-only
#include "zipmetadata.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QtEndian>
namespace {
const QByteArray marker = "\nKomaMimic-Metadata-v1:";
struct End {
  qint64 offset = -1;
  QByteArray comment;
  QString error;
};
End end(QFile &file) {
  if (file.size() < 22)
    return {-1, {}, QCoreApplication::translate("KomaMimic", "ZIP終端がありません。")};
  const auto start = std::max<qint64>(0, file.size() - 65557);
  if (!file.seek(start))
    return {-1, {}, file.errorString()};
  const auto tail = file.read(65557);
  for (qsizetype i = tail.size() - 22; i >= 0; --i) {
    if (tail.mid(i, 4) != "PK\5\6")
      continue;
    const int length = qFromLittleEndian<quint16>(
        reinterpret_cast<const uchar *>(tail.constData() + i + 20));
    if (i + 22 + length > tail.size())
      continue;
    bool zero = true;
    for (qsizetype at = i + 22 + length; at < tail.size(); ++at)
      if (tail[at] != 0) {
        zero = false;
        break;
      }
    if (!zero)
      continue;
    return {start + i, tail.mid(i + 22, length), {}};
  }
  return {-1, {}, QCoreApplication::translate("KomaMimic", "ZIP終端／コメントが不正か、末尾に未知のデータがあります。")};
}
} // namespace
ZipMetadata readZipMetadata(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return {{}, {}, file.errorString()};
  const auto e = end(file);
  if (!e.error.isEmpty())
    return {{}, {}, e.error};
  const auto at = e.comment.lastIndexOf(marker);
  if (at < 0)
    return {{}, e.comment, {}};
  const auto encoded = e.comment.mid(at + marker.size());
  const auto raw =
      QByteArray::fromBase64(encoded, QByteArray::AbortOnBase64DecodingErrors);
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(raw, &error);
  if (error.error != QJsonParseError::NoError || !document.isObject())
    return {{}, e.comment, QCoreApplication::translate("KomaMimic", "KomaMimicメタデータが壊れています。")};
  return {document.object(), e.comment.left(at), {}};
}
QString writeZipMetadata(const QString &source, const QString &destination,
                         const QJsonObject &metadata) {
  const QFileInfo before(source);
  const auto modified = before.lastModified();
  const auto size = before.size();
  QFile input(source);
  if (!input.open(QIODevice::ReadOnly))
    return input.errorString();
  const auto e = end(input);
  if (!e.error.isEmpty())
    return e.error;
  auto original = readZipMetadata(source);
  if (!original.error.isEmpty())
    return original.error;
  const auto comment =
      original.originalComment + marker +
      QJsonDocument(metadata).toJson(QJsonDocument::Compact).toBase64();
  if (comment.size() > 65535)
    return QCoreApplication::translate("KomaMimic", "ZIPコメントは64 KiB未満にしてください。");
  QSaveFile output(destination);
  if (!output.open(QIODevice::WriteOnly))
    return output.errorString();
  input.seek(0);
  qint64 remaining = e.offset + 20;
  while (remaining > 0) {
    const auto bytes = input.read(std::min<qint64>(remaining, 1024 * 1024));
    if (bytes.isEmpty() || output.write(bytes) != bytes.size())
      return QCoreApplication::translate("KomaMimic", "ZIPを複製できません。");
    remaining -= bytes.size();
  }
  char length[2];
  qToLittleEndian<quint16>(quint16(comment.size()),
                           reinterpret_cast<uchar *>(length));
  if (output.write(length, 2) != 2 || output.write(comment) != comment.size())
    return output.errorString();
  const QFileInfo after(source);
  if (after.size() != size || after.lastModified() != modified)
    return QCoreApplication::translate("KomaMimic", "保存中に元のZIPが変更されました。やり直してください。");
  input.close();
  return output.commit() ? QString{} : output.errorString();
}
