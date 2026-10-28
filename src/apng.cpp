// SPDX-License-Identifier: GPL-3.0-only
#include "apng.h"
#include <QCoreApplication>
#include <QPainter>
#include <QtEndian>
#include <zlib.h>
namespace {
const QByteArray Signature = QByteArray::fromHex("89504e470d0a1a0a");
void chunk(QByteArray &output, const QByteArray &type, const QByteArray &data) {
  char value[4];
  qToBigEndian<quint32>(quint32(data.size()), value);
  output.append(value, 4);
  output += type;
  output += data;
  const auto crc = crc32(
      crc32(0, reinterpret_cast<const Bytef *>(type.constData()), 4),
      reinterpret_cast<const Bytef *>(data.constData()), uInt(data.size()));
  qToBigEndian<quint32>(quint32(crc), value);
  output.append(value, 4);
}
struct Frame {
  quint32 width = 0, height = 0, x = 0, y = 0;
  int delay = 100, dispose = 0, blend = 0;
  QByteArray data;
};
} // namespace
ImageResult decodeApng(const QByteArray &bytes, bool &recognized) {
  recognized = false;
  ImageResult result;
  if (!bytes.startsWith(Signature))
    return result;
  QByteArray header, globals;
  QVector<Frame> frames;
  Frame current;
  quint32 sequence = 0, declared = 0, plays = 0;
  bool hasFrame = false, seenData = false;
  const auto fail = [&](const QString &error) {
    result.error = error;
    if (recognized && result.image.isNull() && header.size() == 13) {
      const auto width = qFromBigEndian<quint32>(header.constData()),
                 height = qFromBigEndian<quint32>(header.constData() + 4);
      if (width && height && quint64(width) * height <= quint64(MaxPixels))
        result.image = QImage::fromData(bytes, "PNG");
    }
    result.frames.clear();
    result.delays.clear();
    return result;
  };
  for (qsizetype offset = 8, chunks = 0; offset + 12 <= bytes.size();
       ++chunks) {
    if (chunks > 100000)
      return fail(QCoreApplication::translate("KomaMimic", "APNGのチャンク数が上限を超えています。"));
    const quint32 length = qFromBigEndian<quint32>(bytes.constData() + offset);
    if (quint64(length) + 12 > quint64(bytes.size() - offset))
      return fail(QCoreApplication::translate("KomaMimic", "APNGのチャンクが壊れています。"));
    const auto type = bytes.mid(offset + 4, 4),
               data = bytes.mid(offset + 8, length);
    if (type == "acTL")
      recognized = true;
    const quint32 actual = quint32(crc32(
        0, reinterpret_cast<const Bytef *>(bytes.constData() + offset + 4),
        length + 4));
    if (actual !=
        qFromBigEndian<quint32>(bytes.constData() + offset + 8 + length))
      return fail(QCoreApplication::translate("KomaMimic", "APNGのCRCが一致しません。"));
    if (type == "IHDR") {
      if (data.size() != 13)
        return fail(QCoreApplication::translate("KomaMimic", "PNGヘッダが不正です。"));
      header = data;
    } else if (type == "acTL") {
      if (data.size() != 8 || seenData)
        return fail(QCoreApplication::translate("KomaMimic", "APNGの制御ヘッダが不正です。"));
      declared = qFromBigEndian<quint32>(data.constData());
      plays = qFromBigEndian<quint32>(data.constData() + 4);
      if (!declared || declared > 2000)
        return fail(QCoreApplication::translate("KomaMimic", "APNGのフレーム数が上限を超えています。"));
    } else if (type == "fcTL") {
      if (!recognized || data.size() != 26 ||
          qFromBigEndian<quint32>(data.constData()) != sequence++)
        return fail(QCoreApplication::translate("KomaMimic", "APNGのフレーム制御が不正です。"));
      if (hasFrame) {
        frames.append(current);
        if (frames.size() > 2000)
          return fail(QCoreApplication::translate("KomaMimic", "APNGのフレーム数が上限を超えています。"));
        current = {};
      }
      hasFrame = true;
      current.width = qFromBigEndian<quint32>(data.constData() + 4);
      current.height = qFromBigEndian<quint32>(data.constData() + 8);
      current.x = qFromBigEndian<quint32>(data.constData() + 12);
      current.y = qFromBigEndian<quint32>(data.constData() + 16);
      const int numerator = qFromBigEndian<quint16>(data.constData() + 20),
                denominator = qFromBigEndian<quint16>(data.constData() + 22);
      current.delay = std::max(
          10, qRound(numerator * 1000.0 / (denominator ? denominator : 100)));
      current.dispose = quint8(data[24]);
      current.blend = quint8(data[25]);
    } else if (type == "IDAT") {
      seenData = true;
      if (hasFrame)
        current.data += data;
    } else if (type == "fdAT") {
      if (!hasFrame || data.size() < 4 ||
          qFromBigEndian<quint32>(data.constData()) != sequence++)
        return fail(QCoreApplication::translate("KomaMimic", "APNGの画像データが不正です。"));
      current.data += data.mid(4);
    } else if (type == "PLTE" || type == "tRNS" || type == "gAMA" ||
               type == "cHRM" || type == "sRGB" || type == "iCCP" ||
               type == "sBIT") {
      if (!seenData)
        chunk(globals, type, data);
    } else if (type == "IEND") {
      if (hasFrame)
        frames.append(current);
      break;
    }
    offset += length + 12;
  }
  if (!recognized)
    return {};
  if (header.size() != 13 || frames.size() != declared)
    return fail(QCoreApplication::translate("KomaMimic", "APNGのフレーム数が一致しません。"));
  const quint32 width = qFromBigEndian<quint32>(header.constData()),
                height = qFromBigEndian<quint32>(header.constData() + 4);
  if (!width || !height || quint64(width) * height > quint64(MaxPixels))
    return fail(QCoreApplication::translate("KomaMimic", "APNGの寸法が上限を超えています。"));
  QImage canvas(int(width), int(height), QImage::Format_ARGB32_Premultiplied);
  canvas.fill(Qt::transparent);
  qint64 memory = 0;
  for (const auto &frame : frames) {
    if (!frame.width || !frame.height ||
        quint64(frame.x) + frame.width > width ||
        quint64(frame.y) + frame.height > height || frame.dispose > 2 ||
        frame.blend > 1)
      return fail(QCoreApplication::translate("KomaMimic", "APNGのフレーム範囲が不正です。"));
    auto frameHeader = header;
    qToBigEndian<quint32>(frame.width, frameHeader.data());
    qToBigEndian<quint32>(frame.height, frameHeader.data() + 4);
    QByteArray png = Signature;
    chunk(png, "IHDR", frameHeader);
    png += globals;
    chunk(png, "IDAT", frame.data);
    chunk(png, "IEND", {});
    const auto image = QImage::fromData(png, "PNG");
    if (image.isNull())
      return fail(QCoreApplication::translate("KomaMimic", "APNGのフレームを展開できません。"));
    const QImage previous = frame.dispose == 2 ? canvas : QImage{};
    QPainter painter(&canvas);
    painter.setCompositionMode(frame.blend == 0
                                   ? QPainter::CompositionMode_Source
                                   : QPainter::CompositionMode_SourceOver);
    painter.drawImage(int(frame.x), int(frame.y), image);
    painter.end();
    if (result.image.isNull())
      result.image = canvas;
    memory += canvas.sizeInBytes();
    if (memory > 128 * 1024 * 1024)
      return fail(
          QCoreApplication::translate("KomaMimic", "APNGの展開メモリが128 MiBを超えています。先頭画像を表示します。"));
    result.frames.append(canvas);
    result.delays.append(frame.delay);
    if (frame.dispose == 1) {
      QPainter clear(&canvas);
      clear.setCompositionMode(QPainter::CompositionMode_Source);
      clear.fillRect(QRect(int(frame.x), int(frame.y), int(frame.width),
                           int(frame.height)),
                     Qt::transparent);
    } else if (frame.dispose == 2)
      canvas = previous;
  }
  result.loopCount =
      plays == 0 ? -1 : int(std::min<quint32>(plays - 1, 2147483647));
  return result;
}
