// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "media.h"
ImageResult decodeApng(const QByteArray &bytes, bool &recognized,const std::function<bool()> &cancel={});
