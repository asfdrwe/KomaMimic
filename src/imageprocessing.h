// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "media.h"
#include <functional>
#include <QMargins>
#include <QVariantMap>
#include <QColor>
enum class ResizeAlgorithm { Smooth, Area, Lanczos3, Bicubic, Bilinear, AreaSharpWeak, AreaSharpStrong, Nearest };
enum class ScaleMode { Fit, Width, Height, Actual, Zoom, Fill, EqualHeight };
using CancelCheck = std::function<bool()>;
struct ImageOptions {
    bool enabled = true;
    int rotation = 0;
    bool flipH = false, flipV = false, gray = false, invert = false, binary = false, autoCrop = false;
    int brightness = 0, contrast = 0, threshold = 128, whiteTolerance = 12, sharpen = 0;
    double gamma = 1.0;
    QMargins crop, border;
    double cropPercent=0;
    int resizeToHeight=0;
    QColor borderColor = Qt::white, textColor = Qt::black;
    int blackPoint = 0, whitePoint = 255, outputBlack = 0, outputWhite = 255;
    int curveShadows = 64, curveMidtones = 128, curveHighlights = 192;
    int denoise = 0, lineWidth = 0, textSize = 24;
    QString overlay, scriptFilter;
    QVariantMap scriptSettings;
    QSize resize;
    bool operator==(const ImageOptions &) const = default;
};
QString resizeAlgorithmName(ResizeAlgorithm algorithm);
QImage resizeImage(const QImage &source, QSize target, ResizeAlgorithm algorithm, const CancelCheck &cancel = {});
QImage processImage(const QImage &source, const ImageOptions &options, const CancelCheck &cancel = {});
ImageResult processMedia(const ImageResult &source, const ImageOptions &options, const CancelCheck &cancel = {});

QVariantMap advancedImageSettings(const ImageOptions &options);
void restoreAdvancedImageSettings(ImageOptions &options, const QVariantMap &settings);
