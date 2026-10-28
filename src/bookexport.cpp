// SPDX-License-Identifier: GPL-3.0-only
#include "bookexport.h"
#include <QCoreApplication>
#include <QBuffer>
#include <QDir>
#include <QFileInfo>
#include <QImageWriter>
#include <QPainter>
#include <QPdfWriter>
#include <QSaveFile>
#include <QRegularExpression>
#include <QColorSpace>
#include <QSet>
#include <QtEndian>
#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
namespace {
void word(QByteArray &bytes, int value) {
  bytes.append(char(value & 255));
  bytes.append(char((value >> 8) & 255));
}
QByteArray gifFrame(const QImage &image, int delayMs, bool first) {
  const auto rgb = image.convertToFormat(QImage::Format_ARGB32);
  QByteArray output;
  if (first) {
    output += "GIF89a";
    word(output, image.width());
    word(output, image.height());
    output += QByteArray::fromHex("f70000");
    for (int i = 0; i < 256; ++i) {
      output.append(char(((i >> 5) & 7) * 255 / 7));
      output.append(char(((i >> 2) & 7) * 255 / 7));
      output.append(char((i & 3) * 255 / 3));
    }
    output += QByteArray::fromHex("21ff0b");
    output += "NETSCAPE2.0";
    output += QByteArray::fromHex("0301000000");
  }
  output += QByteArray::fromHex("21f90409");
  word(output, std::clamp(delayMs / 10, 1, 65535));
  output.append(char(1));
  output.append(char(0));
  output.append(',');
  word(output, 0);
  word(output, 0);
  word(output, image.width());
  word(output, image.height());
  output.append(char(0));
  output.append(char(8));
  QByteArray codes;
  quint32 bits = 0;
  int count = 0;
  const auto code = [&](int value) {
    bits |= quint32(value) << count;
    count += 9;
    while (count >= 8) {
      codes.append(char(bits & 255));
      bits >>= 8;
      count -= 8;
    }
  };
  code(256);
  int sinceClear = 0;
  for (int y = 0; y < rgb.height(); ++y)
    for (int x = 0; x < rgb.width(); ++x) {
      if (sinceClear == 200) {
        code(256);
        sinceClear = 0;
      }
      const auto pixel = rgb.pixel(x, y);
      int value = ((qRed(pixel) >> 5) << 5) | ((qGreen(pixel) >> 5) << 2) |
                  (qBlue(pixel) >> 6);
      value = qAlpha(pixel) < 128 ? 1 : value == 1 ? 0 : value;
      code(value);
      ++sinceClear;
    }
  code(257);
  if (count)
    codes.append(char(bits & 255));
  for (int offset = 0; offset < codes.size(); offset += 255) {
    const auto block = codes.mid(offset, 255);
    output.append(char(block.size()));
    output += block;
  }
  output.append(char(0));
  return output;
}
bool writeImage(QIODevice *device, const QImage &image, QByteArray format,
                int quality, QString &error) {
  if (format.isEmpty())
    format = "png";
  QImageWriter writer(device, format);
  writer.setQuality(quality);
  writer.setOptimizedWrite(true);
  if (writer.write(image))
    return true;
  error = writer.errorString();
  return false;
}
} // namespace
ExportPlan planExport(const Book &book, const QVector<int> &pages, const QString &directory,
                      const ExportNaming &n, bool spread) {
  ExportPlan plan; QSet<QString> used;
  if(n.mode<0 || n.mode>2 || n.digits<1 || n.digits>9 || n.start<0 || n.conflict<0 || n.conflict>2 ||
     !QStringList{"png","jpg","webp","tiff"}.contains(n.format)) { plan.error=QCoreApplication::translate("KomaMimic", "出力の命名・形式設定が不正です。"); return plan; }
  const auto clean=[](QString name) {
    name.replace(QRegularExpression("[<>:\"/\\\\|?*\\x00-\\x1f]"), "_");
    while(name.endsWith('.') || name.endsWith(' ')) name.chop(1);
    if(name.isEmpty() || name=="." || name=="..") name="page";
    if(QRegularExpression("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\.|$)",QRegularExpression::CaseInsensitiveOption).match(name).hasMatch()) name.prepend('_');
    return name.left(160);
  };
  for(int i=0;i<pages.size();i+=spread?2:1) {
    if(pages[i]<0 || pages[i]>=book.pages.size()) {plan.error=QCoreApplication::translate("KomaMimic", "出力ページが範囲外です。");return plan;}
    const auto number=QString::number(qint64(n.start)+plan.names.size()).rightJustified(n.digits,'0');
    const auto base=clean(QFileInfo(book.pages[pages[i]].name).completeBaseName());
    const auto suffix=n.mode==0 ? number : "_"+number;
    const auto stem=n.mode==1 ? clean(n.prefix+base) :
        (n.mode==0 && n.prefix.isEmpty() ? QString{} : clean(n.prefix+(n.mode==2 ? base : QString{})).left(160-suffix.size()))+suffix;
    const auto name=stem+'.'+n.format, folded=name.normalized(QString::NormalizationForm_C).toCaseFolded();
    if(used.contains(folded)) {plan.error=QCoreApplication::translate("KomaMimic", "出力名が重複します：")+name+QCoreApplication::translate("KomaMimic", "（元名＋連番を選択してください）");return plan;}
    used.insert(folded); plan.names.append(name);
    if(!directory.isEmpty()) {
      const QFileInfo target(QDir(directory).filePath(name));
      if(target.isDir()) {plan.error=QCoreApplication::translate("KomaMimic", "出力名と同名のフォルダがあります：")+name;return plan;}
      if(target.exists() || target.isSymLink())plan.conflicts.append(name);
    }
  }
  return plan;
}
QString exportPages(const std::shared_ptr<const Book> &book,
                    const QVector<int> &pages, const QString &path,
                    ExportKind kind, const ImageOptions &options, int quality,
                    int delayMs, const std::function<bool(int, int)> &progress,
                    bool spread, bool rightToLeft, const ExportNaming &naming) {
  if (!book || pages.isEmpty())
    return QCoreApplication::translate("KomaMimic", "出力するページがありません。");
  if ((kind == ExportKind::Image || kind == ExportKind::Original) &&
      pages.size() != 1 &&
      !(kind == ExportKind::Image && spread && pages.size() == 2))
    return QCoreApplication::translate("KomaMimic", "単画像の保存では1ページを選択してください。");
  for (int page : pages)
    if (page < 0 || page >= book->pages.size())
      return QCoreApplication::translate("KomaMimic", "出力ページが範囲外です。");
  const auto plan=planExport(*book,pages,kind==ExportKind::Directory ? path : QString{},naming,spread);
  if(kind==ExportKind::Directory || kind==ExportKind::Zip) {
    if(!plan.error.isEmpty()) return plan.error;
    if(!QImageWriter::supportedImageFormats().contains(naming.format.toLatin1()))return QCoreApplication::translate("KomaMimic", "出力codecが利用できません：")+naming.format;
    if(kind==ExportKind::Directory && naming.conflict==0 && !plan.conflicts.isEmpty())
      return QCoreApplication::translate("KomaMimic", "同名ファイルがあります：")+plan.conflicts.join(", ");
  }
  if (kind == ExportKind::Directory && !QDir().mkpath(path))
    return QCoreApplication::translate("KomaMimic", "出力フォルダを作成できません。");
  QSaveFile file(path);
  if (kind != ExportKind::Directory && !file.open(QIODevice::WriteOnly))
    return file.errorString();
  if (kind == ExportKind::Original) {
    if (book->pages[pages.first()].document)
      return QCoreApplication::translate("KomaMimic", "文書ページは加工画像としてPNG等へ保存してください。");
    const auto data = book->encoded(pages.first());
    if (!data.error.isEmpty())
      return data.error;
    if (file.write(data.bytes) != data.bytes.size() || !file.commit())
      return file.errorString();
    return {};
  }
  archive *zip = nullptr;
  if (kind == ExportKind::Zip) {
    zip = archive_write_new();
    archive_write_set_format_zip(zip);
    archive_write_set_bytes_per_block(zip, 0);
    archive_write_set_bytes_in_last_block(zip, 1);
    if (archive_write_open(
            zip, &file, nullptr,
            [](archive *, void *context, const void *data,
               size_t size) -> la_ssize_t {
              return static_cast<QSaveFile *>(context)->write(
                  static_cast<const char *>(data), qint64(size));
            },
            nullptr) != ARCHIVE_OK) {
      archive_write_free(zip);
      return QCoreApplication::translate("KomaMimic", "ZIP出力を作成できません。");
    }
  }
  struct ZipGuard {
    archive *value;
    ~ZipGuard() {
      if (value)
        archive_write_free(value);
    }
  } guard{zip};
  std::unique_ptr<QPdfWriter> pdf;
  QPainter painter;
  if (kind == ExportKind::Pdf) {
    pdf = std::make_unique<QPdfWriter>(&file);
    pdf->setResolution(144);
    pdf->setCreator("KomaMimic");
  }
  QSize gifSize;
  bool firstGif = true;
  const int step = spread && kind != ExportKind::Original ? 2 : 1;
  for (int i = 0; i < pages.size(); i += step) {
    if (progress && !progress(i, pages.size()))
      return QCoreApplication::translate("KomaMimic", "出力をキャンセルしました。");
    if(kind==ExportKind::Directory && naming.conflict==1 && (QFileInfo::exists(QDir(path).filePath(plan.names[i/step])) || QFileInfo(QDir(path).filePath(plan.names[i/step])).isSymLink())) continue;
    const auto cancelled=[&] { return progress && !progress(i,pages.size()); };
    auto media = processMedia(book->image(pages[i],{},cancelled), options,cancelled);
    if (!media.error.isEmpty() || media.image.isNull())
      return media.error.isEmpty() ? QCoreApplication::translate("KomaMimic", "画像を読めません。") : media.error;
    if (step == 2 && i + 1 < pages.size()) {
      const auto next = processMedia(book->image(pages[i + 1],{},cancelled), options,cancelled);
      if (next.image.isNull() || !next.error.isEmpty())
        return next.error.isEmpty() ? QCoreApplication::translate("KomaMimic", "画像を読めません。") : next.error;
      media.image = composeSpread(media.image, next.image, rightToLeft);
      media.frames.clear();
      media.delays.clear();
      if (media.image.isNull())
        return QCoreApplication::translate("KomaMimic", "結合画像が大きすぎます。");
    }
    if(cancelled())return QCoreApplication::translate("KomaMimic", "出力をキャンセルしました。");
    // A fresh pixel buffer removes text/EXIF/XMP from processed output.
    QImage image(media.image.size(), QImage::Format_ARGB32);
    { QPainter pixels(&image); pixels.setCompositionMode(QPainter::CompositionMode_Source); pixels.drawImage(0,0,media.image); }
    if(naming.keepColorProfile) image.setColorSpace(media.image.colorSpace());
    if (kind == ExportKind::Pdf) {
      pdf->setPageSize(QPageSize(
          QSizeF(image.width() / 144.0 * 25.4, image.height() / 144.0 * 25.4),
          QPageSize::Millimeter));
      pdf->setPageMargins(QMarginsF(0, 0, 0, 0));
      if (i == 0) {
        if (!painter.begin(pdf.get()))
          return QCoreApplication::translate("KomaMimic", "PDFに描画できません。");
      } else if (!pdf->newPage())
        return QCoreApplication::translate("KomaMimic", "PDFの改ページに失敗しました。");
      painter.drawImage(QRect(0, 0, pdf->width(), pdf->height()), image);
    } else if (kind == ExportKind::Gif) {
      if (firstGif)
        gifSize = image.size();
      if (gifSize.width() > 65535 || gifSize.height() > 65535)
        return QCoreApplication::translate("KomaMimic", "GIFの寸法上限を超えています。");
      auto frames = media.frames;
      if (frames.isEmpty())
        frames.append(image);
      for (int frame = 0; frame < frames.size(); ++frame) {
        QImage canvas(gifSize, QImage::Format_ARGB32);
        canvas.fill(Qt::transparent);
        QPainter composite(&canvas);
        const auto size =
            frames[frame].size().scaled(gifSize, Qt::KeepAspectRatio);
        composite.drawImage(
            QRect(QPoint((gifSize.width() - size.width()) / 2,
                         (gifSize.height() - size.height()) / 2),
                  size),
            frames[frame]);
        composite.end();
        const auto bytes = gifFrame(
            canvas, media.delays.size() > frame ? media.delays[frame] : delayMs,
            firstGif);
        firstGif = false;
        if (file.write(bytes) != bytes.size())
          return file.errorString();
      }
    } else {
      QString error;
      if (kind == ExportKind::Zip) {
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        if (!writeImage(&buffer, image, naming.format.toLatin1(), quality, error))
          return error;
        archive_entry *entry = archive_entry_new();
        const auto name =
            plan.names[i/step].toUtf8();
        archive_entry_set_pathname(entry, name.constData());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        archive_entry_set_size(entry, bytes.size());
        const int status = archive_write_header(zip, entry);
        archive_entry_free(entry);
        if (status != ARCHIVE_OK ||
            archive_write_data(zip, bytes.constData(), bytes.size()) !=
                bytes.size())
          return QCoreApplication::translate("KomaMimic", "ZIPへの書込みに失敗しました。");
      } else if (kind == ExportKind::Directory) {
        if(naming.conflict==0 && (QFileInfo::exists(QDir(path).filePath(plan.names[i/step])) || QFileInfo(QDir(path).filePath(plan.names[i/step])).isSymLink())) return QCoreApplication::translate("KomaMimic", "出力中に同名ファイルが作成されました。");
        QSaveFile output(QDir(path).filePath(
            plan.names[i/step]));
        if (!output.open(QIODevice::WriteOnly) ||
            !writeImage(&output, image, naming.format.toLatin1(), quality, error) ||
            !output.commit())
          return error.isEmpty() ? output.errorString() : error;
      } else if (!writeImage(&file, image, QFileInfo(path).suffix().toLatin1(),
                             quality, error))
        return error;
    }
  }
  if (zip && archive_write_close(zip) != ARCHIVE_OK)
    return QCoreApplication::translate("KomaMimic", "ZIPの確定に失敗しました。");
  if (painter.isActive())
    painter.end();
  pdf.reset();
  if (kind == ExportKind::Gif && file.write(";", 1) != 1)
    return file.errorString();
  if (kind != ExportKind::Directory && !file.commit())
    return file.errorString();
  if (progress)
    progress(pages.size(), pages.size());
  return {};
}

QImage composeSpread(const QImage &first, const QImage &second,
                     bool rightToLeft) {
  if (first.isNull() || second.isNull())
    return {};
  const int height = std::max(first.height(), second.height());
  const qint64 width =
      qRound(double(height) * first.width() / first.height()) +
      qRound(double(height) * second.width() / second.height());
  if (width > 16384 || height > 16384 || width * height > 48 * 1024 * 1024)
    return {};
  const auto a = first.scaledToHeight(height, Qt::SmoothTransformation),
             b = second.scaledToHeight(height, Qt::SmoothTransformation);
  QImage image(int(width), height, QImage::Format_ARGB32);
  image.fill(Qt::white);
  QPainter painter(&image);
  painter.drawImage(0, 0, rightToLeft ? b : a);
  painter.drawImage(rightToLeft ? b.width() : a.width(), 0,
                    rightToLeft ? a : b);
  return image;
}
