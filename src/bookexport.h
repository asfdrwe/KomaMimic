// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "book.h"
#include "imageprocessing.h"
#include <functional>
struct ExportNaming {
    int mode = 0, digits = 6, start = 1; // sequence / source basename / basename+sequence
    QString prefix, format = "png";
    int conflict = 0; // reject / skip / overwrite
    bool keepColorProfile = true;
};
struct ExportPlan { QStringList names, conflicts; QString error; };
ExportPlan planExport(const Book &book, const QVector<int> &pages, const QString &directory,
                      const ExportNaming &naming = {}, bool spread = false);
enum class ExportKind { Image, Directory, Zip, Pdf, Gif, Original };
QString exportPages(const std::shared_ptr<const Book> &book,
                    const QVector<int> &pages, const QString &path,
                    ExportKind kind, const ImageOptions &options = {},
                    int quality = 90, int delayMs = 1000,
                    const std::function<bool(int, int)> &progress = {},
                    bool spread = false, bool rightToLeft = true,
                    const ExportNaming &naming = {});
QImage composeSpread(const QImage &first, const QImage &second,
                     bool rightToLeft);
