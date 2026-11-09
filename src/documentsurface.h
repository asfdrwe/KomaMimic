// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QWidget>

// Keep WebEngine's RHI composition inside this native child. The surrounding
// DocumentView stays an ordinary widget so sibling views keep their surfaces.
class DocumentSurface final : public QWidget {
public:
  explicit DocumentSurface(QWidget *parent) : QWidget(parent) {
    setObjectName("documentSurface");
    setAttribute(Qt::WA_DontCreateNativeAncestors);
    setAttribute(Qt::WA_NativeWindow);
  }
};
