// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QPalette>
// 0 follows the platform, 1 light, 2 dark. Applies to application chrome;
// image pixels and the book's separately configured text colors are preserved.
void setApplicationAppearance(int mode);
QPalette appearancePalette(bool dark);
