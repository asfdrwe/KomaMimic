// SPDX-License-Identifier: GPL-3.0-only
// Poppler is GPL-licensed; the linked application is distributed under GPLv3.
// Include GLib before Qt to avoid Qt's signals macro in GLib headers.
#include <poppler.h>
#include <QCoreApplication>

#include "pdfsource.h"
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <QThread>

namespace {
constexpr int MaxPages = 100000;
// Serialize Poppler work; documents/pages never escape this translation unit.
QMutex pdfMutex;
struct Unref {
  void operator()(gpointer object) const { g_object_unref(object); }
};
using Document = std::unique_ptr<PopplerDocument, Unref>;
using Page = std::unique_ptr<PopplerPage, Unref>;
using Surface =
    std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)>;
using Context = std::unique_ptr<cairo_t, decltype(&cairo_destroy)>;
using Mapping =
    std::unique_ptr<GList, decltype(&poppler_page_free_image_mapping)>;

Document openPdf(const QString &path, const QString &password, QString &error) {
  const QFileInfo info(path);
  if (!info.isFile() || !info.isReadable()) {
    error = QCoreApplication::translate("KomaMimic", "PDFファイルを読み込めません。");
    return {};
  }
  const auto uri = QUrl::fromLocalFile(info.absoluteFilePath()).toEncoded();
  const auto secret = password.toUtf8();
  GError *failure = nullptr;
  Document document(poppler_document_new_from_file(
      uri.constData(), password.isEmpty() ? nullptr : secret.constData(),
      &failure));
  if (failure) {
    error =
        failure->domain == POPPLER_ERROR &&
                failure->code == POPPLER_ERROR_ENCRYPTED
            ? QCoreApplication::translate("KomaMimic", "PDFのパスワードが必要か、パスワードが違います。")
            : QString::fromUtf8(failure->message);
    g_error_free(failure);
  }
  if (!document) {
    if (error.isEmpty())
      error = QCoreApplication::translate("KomaMimic", "PDFの読み込みに失敗しました。");
    return {};
  }
  const int pages = poppler_document_get_n_pages(document.get());
  if (pages <= 0 || pages > MaxPages) {
    error = QCoreApplication::translate("KomaMimic", "PDFのページ数が範囲外です。");
    return {};
  }
  return document;
}
Page getPage(PopplerDocument *document, int index, QString &error) {
  if (index < 0 || index >= poppler_document_get_n_pages(document)) {
    error = QCoreApplication::translate("KomaMimic", "PDFのページが範囲外です。");
    return {};
  }
  Page page(poppler_document_get_page(document, index));
  if (!page)
    error = QCoreApplication::translate("KomaMimic", "PDFページを読み込めません。");
  return page;
}
bool validSize(int width, int height) {
  return width > 0 && height > 0 && qint64(width) * height <= MaxPixels;
}

// Paint into Qt-owned native-endian ARGB32 storage; no dangling Cairo buffers.
ImageResult copyImage(cairo_surface_t *source, QSize target) {
  if (!source || cairo_surface_status(source) != CAIRO_STATUS_SUCCESS ||
      cairo_surface_get_type(source) != CAIRO_SURFACE_TYPE_IMAGE)
    return {{}, QCoreApplication::translate("KomaMimic", "PDF画像を取り出せません。")};
  const int width = cairo_image_surface_get_width(source);
  const int height = cairo_image_surface_get_height(source);
  if (!validSize(width, height))
    return {{}, QCoreApplication::translate("KomaMimic", "PDF画像の画素数が上限を超えています。")};
  QSize size(width, height);
  if (!target.isEmpty())
    size.scale(target.boundedTo(QSize(8192, 8192)), Qt::KeepAspectRatio);
  if (!validSize(size.width(), size.height()))
    return {{}, QCoreApplication::translate("KomaMimic", "PDF画像の出力サイズが範囲外です。")};
  QImage image(size, QImage::Format_ARGB32_Premultiplied);
  if (image.isNull())
    return {{}, QCoreApplication::translate("KomaMimic", "PDF画像用のメモリを確保できません。")};
  image.fill(Qt::transparent);
  Surface surface(cairo_image_surface_create_for_data(
                      image.bits(), CAIRO_FORMAT_ARGB32, image.width(),
                      image.height(), int(image.bytesPerLine())),
                  cairo_surface_destroy);
  Context context(cairo_create(surface.get()), cairo_destroy);
  cairo_scale(context.get(), double(size.width()) / width,
              double(size.height()) / height);
  cairo_set_source_surface(context.get(), source, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(context.get()), CAIRO_FILTER_BEST);
  cairo_paint(context.get());
  cairo_surface_flush(surface.get());
  if (cairo_status(context.get()) != CAIRO_STATUS_SUCCESS ||
      cairo_surface_status(surface.get()) != CAIRO_STATUS_SUCCESS)
    return {{}, QCoreApplication::translate("KomaMimic", "PDF画像の変換に失敗しました。")};
  return {image, {}};
}
} // namespace

DocumentInfo inspectPdfDocument(const QString &path, const QString &password) {
  QMutexLocker lock(&pdfMutex);
  QString error;
  auto document = openPdf(path, password, error);
  return document
             ? DocumentInfo{poppler_document_get_n_pages(document.get()), {}}
             : DocumentInfo{0, error};
}

ImageResult renderPdfDocument(const QString &path, int index, QSize target,
                              const QString &password, const std::function<bool()> &cancel) {
  std::unique_lock<QMutex> lock(pdfMutex,std::defer_lock);
  while(!lock.try_lock()) {if(cancel && cancel())return {{},QCoreApplication::translate("KomaMimic", "PDF描画を取り消しました。")};QThread::msleep(10);}
  if(cancel && cancel())return {{},QCoreApplication::translate("KomaMimic", "PDF描画を取り消しました。")};
  QString error;
  auto document = openPdf(path, password, error);
  if (!document)
    return {{}, error};
  auto page = getPage(document.get(), index, error);
  if (!page)
    return {{}, error};
  double width = 0, height = 0;
  poppler_page_get_size(page.get(), &width, &height);
  if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 ||
      height <= 0)
    return {{}, QCoreApplication::translate("KomaMimic", "PDFページの寸法が不正です。")};
  const int edge =
      target.isEmpty()
          ? 2400
          : std::clamp(std::max(target.width(), target.height()), 32, 8192);
  const double scale = edge / std::max(width, height);
  QSize size(std::max(1, int(std::ceil(width * scale))),
             std::max(1, int(std::ceil(height * scale))));
  if (!target.isEmpty())
    size.scale(target.boundedTo(QSize(8192, 8192)), Qt::KeepAspectRatio);
  if (!validSize(size.width(), size.height()))
    return {{}, QCoreApplication::translate("KomaMimic", "PDFページの画素数が上限を超えています。")};
  QImage image(size, QImage::Format_ARGB32_Premultiplied);
  if (image.isNull())
    return {{}, QCoreApplication::translate("KomaMimic", "PDFページ用のメモリを確保できません。")};
  image.fill(Qt::white);
  Surface surface(cairo_image_surface_create_for_data(
                      image.bits(), CAIRO_FORMAT_ARGB32, size.width(),
                      size.height(), int(image.bytesPerLine())),
                  cairo_surface_destroy);
  Context context(cairo_create(surface.get()), cairo_destroy);
  cairo_scale(context.get(), size.width() / width, size.height() / height);
  if(cancel && cancel())return {{},QCoreApplication::translate("KomaMimic", "PDF描画を取り消しました。")};
  poppler_page_render(page.get(), context.get());
  if(cancel && cancel())return {{},QCoreApplication::translate("KomaMimic", "PDF描画を取り消しました。")};
  cairo_surface_flush(surface.get());
  if (cairo_status(context.get()) != CAIRO_STATUS_SUCCESS ||
      cairo_surface_status(surface.get()) != CAIRO_STATUS_SUCCESS)
    return {{}, QCoreApplication::translate("KomaMimic", "PDFページの描画に失敗しました。")};
  return {image, {}};
}

PdfResources inspectPdfResources(const QString &path, const QString &password) {
  QMutexLocker lock(&pdfMutex);
  QString error;
  auto document = openPdf(path, password, error);
  if (!document)
    return {{}, error};
  PdfResources result;
  for (int index = 0; index < poppler_document_get_n_pages(document.get());
       ++index) {
    auto page = getPage(document.get(), index, error);
    if (!page)
      return {{}, error};
    Mapping mappings(poppler_page_get_image_mapping(page.get()),
                     poppler_page_free_image_mapping);
    QVector<int> ids;
    for (auto *entry = mappings.get(); entry; entry = entry->next) {
      if (result.images.size() + ids.size() >= MaxPages)
        return {{}, QCoreApplication::translate("KomaMimic", "PDFの画像数が上限を超えています。")};
      ids.append(static_cast<PopplerImageMapping *>(entry->data)->image_id);
    }
    std::sort(ids.begin(), ids.end());
    for (int id : ids)
      result.images.append(
          {index, id,
           QCoreApplication::translate("KomaMimic", "ページ%1・画像%2").arg(index + 1).arg(id + 1)});
  }
  if (result.images.isEmpty())
    result.error = QCoreApplication::translate("KomaMimic", "PDF内に抽出できる画像リソースがありません。");
  return result;
}

ImageResult renderPdfResource(const QString &path, int index, int image,
                              QSize target, const QString &password) {
  QMutexLocker lock(&pdfMutex);
  QString error;
  auto document = openPdf(path, password, error);
  if (!document)
    return {{}, error};
  auto page = getPage(document.get(), index, error);
  if (!page)
    return {{}, error};
  Mapping mappings(poppler_page_get_image_mapping(page.get()),
                   poppler_page_free_image_mapping);
  bool found = false;
  for (auto *entry = mappings.get(); entry; entry = entry->next)
    found |= static_cast<PopplerImageMapping *>(entry->data)->image_id == image;
  if (!found)
    return {{}, QCoreApplication::translate("KomaMimic", "PDFの画像リソースを見つけられません。")};
  Surface surface(poppler_page_get_image(page.get(), image),
                  cairo_surface_destroy);
  return copyImage(surface.get(), target);
}
