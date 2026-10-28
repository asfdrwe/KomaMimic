// SPDX-License-Identifier: GPL-3.0-only
#include "test_support.h"
#ifdef Q_OS_UNIX
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <QAction>
#include <QDir>
#include <QPainter>
#include <QPdfWriter>
#include <QtEndian>
#include <zlib.h>

namespace {
void le16(QByteArray &b, quint16 n) { char v[2]; qToLittleEndian(n, v); b.append(v, 2); }
void le32(QByteArray &b, quint32 n) { char v[4]; qToLittleEndian(n, v); b.append(v, 4); }
void be32(QByteArray &b, quint32 n) { char v[4]; qToBigEndian(n, v); b.append(v, 4); }
void chunk(QByteArray &b, const QByteArray &type, const QByteArray &data) {
    be32(b, quint32(data.size())); b += type; b += data;
    const auto payload = type + data;
    be32(b, quint32(crc32(0, reinterpret_cast<const Bytef *>(payload.constData()), uInt(payload.size()))));
}
}
namespace TestData {
bool webEngineIpcAvailable() {
#ifdef Q_OS_UNIX
    int fd[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fd) != 0) return false;
    const bool ok = ::shutdown(fd[0], SHUT_RD) == 0;
    ::close(fd[0]); ::close(fd[1]);
    return ok;
#else
    return true;
#endif
}
QImage solid(QColor color, QSize size) {
    QImage image(size, QImage::Format_ARGB32); image.fill(color); return image;
}
QByteArray png(const QImage &image) {
    QByteArray b; QBuffer buffer(&b); buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG")) return {};
    return b;
}
bool write(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
bool imageFolder(const QString &path, int count) {
    if (!QDir().mkpath(path)) return false;
    for (int i = 0; i < count; ++i)
        if (!write(path + QString("/%1.png").arg(i + 1), png(solid(QColor::fromHsv(i * 40 % 360, 255, 255))))) return false;
    return true;
}
QByteArray zip(const QVector<ZipEntry> &entries, const QByteArray &comment) {
    QByteArray b, central;
    for (const auto &e : entries) {
        const auto offset = quint32(b.size()), size = quint32(e.bytes.size());
        const auto crc = quint32(crc32(0, reinterpret_cast<const Bytef *>(e.bytes.constData()), uInt(size)));
        le32(b, 0x04034b50); le16(b, 20); le16(b, e.flags); le16(b, 0);
        le16(b, 0); le16(b, 0x21); le32(b, crc); le32(b, size); le32(b, size);
        le16(b, quint16(e.name.size())); le16(b, 0); b += e.name; b += e.bytes;
        le32(central, 0x02014b50); le16(central, 20); le16(central, 20);
        le16(central, e.flags); le16(central, 0); le16(central, 0); le16(central, 0x21);
        le32(central, crc); le32(central, size); le32(central, size);
        le16(central, quint16(e.name.size())); le16(central, 0); le16(central, 0);
        le16(central, 0); le16(central, 0); le32(central, 0); le32(central, offset); central += e.name;
    }
    const auto start = quint32(b.size()); b += central;
    le32(b, 0x06054b50); le16(b, 0); le16(b, 0);
    le16(b, quint16(entries.size())); le16(b, quint16(entries.size()));
    le32(b, quint32(central.size())); le32(b, start); le16(b, quint16(comment.size())); b += comment;
    return b;
}
QByteArray apng() {
    QByteArray b = QByteArray::fromHex("89504e470d0a1a0a"), header;
    be32(header, 2); be32(header, 1); header += QByteArray::fromHex("0806000000");
    chunk(b, "IHDR", header); QByteArray control; be32(control, 2); be32(control, 0); chunk(b, "acTL", control);
    quint32 sequence = 0;
    for (const auto &color : {QByteArray::fromHex("ff0000ff"), QByteArray::fromHex("0000ffff")}) {
        QByteArray frame; be32(frame, sequence++); be32(frame, 2); be32(frame, 1); be32(frame, 0); be32(frame, 0);
        frame += QByteArray::fromHex("0001000a0000"); chunk(b, "fcTL", frame);
        QByteArray raw(1, char(0)); raw += color; raw += color;
        uLongf size = compressBound(uLong(raw.size())); QByteArray data(qsizetype(size), char(0));
        if (compress2(reinterpret_cast<Bytef *>(data.data()), &size,
                      reinterpret_cast<const Bytef *>(raw.constData()), uLong(raw.size()), 6) != Z_OK) return {};
        data.resize(qsizetype(size));
        if (sequence == 1) chunk(b, "IDAT", data);
        else { QByteArray numbered; be32(numbered, sequence++); numbered += data; chunk(b, "fdAT", numbered); }
    }
    chunk(b, "IEND", {}); return b;
}
QByteArray tiff() {
    // Two uncompressed 1x1 grayscale IFDs; black then white.
    QByteArray b("II"); le16(b, 42); le32(b, 8);
    constexpr quint32 ifdSize = 2 + 9 * 12 + 4;
    for (int i = 0; i < 2; ++i) {
        le16(b, 9);
        const auto tag = [&](quint16 id, quint16 type, quint32 value) { le16(b, id); le16(b, type); le32(b, 1); le32(b, value); };
        tag(256, 4, 1); tag(257, 4, 1); tag(258, 3, 8); tag(259, 3, 1); tag(262, 3, 1);
        tag(273, 4, 8 + 2 * ifdSize + quint32(i)); tag(277, 3, 1); tag(278, 4, 1); tag(279, 4, 1);
        le32(b, i == 0 ? 8 + ifdSize : 0);
    }
    b.append(char(0)); b.append(char(255)); return b;
}
bool pdf(const QString &path) {
    QPdfWriter writer(path); writer.setResolution(72);
    writer.setPageSize(QPageSize(QSizeF(72, 108), QPageSize::Point));
    writer.setPageMargins(QMarginsF(0, 0, 0, 0)); QPainter p;
    if (!p.begin(&writer)) return false;
    p.drawImage(QRect(0, 0, writer.width(), writer.height()), solid(Qt::red));
    if (!writer.newPage()) return false;
    p.drawImage(QRect(0, 0, writer.width(), writer.height()), solid(Qt::blue));
    return p.end();
}
void singlePage(MainWindow &w) {
    if (w.command("autoSpread")->isChecked()) w.command("autoSpread")->trigger();
    if (w.command("spread")->isChecked()) w.command("spread")->trigger();
}
}
