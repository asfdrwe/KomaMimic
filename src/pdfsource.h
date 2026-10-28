// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "documentsource.h"

DocumentInfo inspectPdfDocument(const QString &path, const QString &password);
ImageResult renderPdfDocument(const QString &path, int page, QSize target,
                              const QString &password, const std::function<bool()> &cancel = {});
