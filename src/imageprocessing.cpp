// SPDX-License-Identifier: GPL-3.0-only
#include "imageprocessing.h"
#include <QCoreApplication>
#include "scriptreader.h"
#include <QPainter>
#include <QTextOption>
#include <QTransform>
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace {
bool cancelled(const CancelCheck &check) { return check && check(); }
QRect contentBounds(const QImage &source, const QRect &region, int tolerance, const CancelCheck &cancel) {
    const auto image = source.convertToFormat(QImage::Format_ARGB32);
    int left = region.right() + 1, top = region.bottom() + 1, right = -1, bottom = -1;
    const int white = 255 - std::clamp(tolerance, 0, 100);
    for (int y = region.top(); y <= region.bottom(); ++y) {
        if (cancelled(cancel)) return {};
        const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = region.left(); x <= region.right(); ++x) {
            const auto p = row[x];
            if (qAlpha(p) > 16 && (qRed(p) < white || qGreen(p) < white || qBlue(p) < white)) {
                left = std::min(left, x); right = std::max(right, x); top = std::min(top, y); bottom = y;
            }
        }
    }
    return right >= left && bottom >= top ? QRect(QPoint(left, top), QPoint(right, bottom)) : QRect{};
}
QImage sharpenImage(const QImage &source, double amount, const CancelCheck &cancel) {
    QImage image = source.convertToFormat(QImage::Format_ARGB32);
    QImage out(image.size(), QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y) {
        if (cancelled(cancel)) return {};
        auto *dst = reinterpret_cast<QRgb *>(out.scanLine(y));
        const auto *center = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            double r = 0, g = 0, b = 0, weight = 0;
            // Ignore fully transparent neighbours when estimating the blur.
            for (int dy = -1; dy <= 1; ++dy) {
                const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(std::clamp(y + dy, 0, image.height() - 1)));
                for (int dx = -1; dx <= 1; ++dx) {
                    const auto pixel = row[std::clamp(x + dx, 0, image.width() - 1)];
                    const double w = (dx == 0 ? 2 : 1) * (dy == 0 ? 2 : 1) * qAlpha(pixel) / 255.0;
                    r += qRed(pixel) * w; g += qGreen(pixel) * w; b += qBlue(pixel) * w; weight += w;
                }
            }
            const auto pixel = center[x];
            if (weight == 0) { dst[x] = pixel; continue; }
            const auto channel = [&](int c, double blurred) { return std::clamp(qRound(c + amount * (c - blurred / weight)), 0, 255); };
            dst[x] = qRgba(channel(qRed(pixel), r), channel(qGreen(pixel), g), channel(qBlue(pixel), b), qAlpha(pixel));
        }
    }
    return out;
}
struct Tap { int pixel; double weight; };
using Table = QVector<QVector<Tap>>;
double kernel(double x, ResizeAlgorithm algorithm) {
    x = std::abs(x);
    if (algorithm == ResizeAlgorithm::Bilinear) return std::max(0.0, 1 - x);
    if (algorithm == ResizeAlgorithm::Bicubic) {
        // Mitchell-Netravali, B=C=1/3.
        constexpr double b = 1.0 / 3, c = 1.0 / 3;
        if (x < 1) return ((12 - 9*b - 6*c)*x*x*x + (-18 + 12*b + 6*c)*x*x + 6 - 2*b) / 6;
        if (x < 2) return ((-b - 6*c)*x*x*x + (6*b + 30*c)*x*x + (-12*b - 48*c)*x + 8*b + 24*c) / 6;
        return 0;
    }
    if (x < 1e-10) return 1;
    if (x >= 3) return 0;
    const double p = std::numbers::pi * x;
    return std::sin(p) * std::sin(p / 3) / (p * p / 3);
}
Table contributions(int from, int to, ResizeAlgorithm algorithm, const CancelCheck &cancel) {
    Table table(to);
    const double ratio = double(from) / to;
    const double stretch = std::max(1.0, ratio);
    const bool area = algorithm == ResizeAlgorithm::Area || algorithm == ResizeAlgorithm::AreaSharpWeak || algorithm == ResizeAlgorithm::AreaSharpStrong;
    const double radius = algorithm == ResizeAlgorithm::Bilinear ? 1 : algorithm == ResizeAlgorithm::Bicubic ? 2 : 3;
    for (int out = 0; out < to; ++out) {
        if (cancelled(cancel)) return {};
        auto &taps = table[out];
        if (algorithm == ResizeAlgorithm::Nearest) { taps.append({std::min(from - 1, int((out + 0.5) * ratio)), 1}); continue; }
        const double center = (out + 0.5) * ratio - 0.5;
        const int first = area ? int(std::floor(out * ratio)) : int(std::ceil(center - radius * stretch));
        const int last = area ? int(std::ceil((out + 1) * ratio)) - 1 : int(std::floor(center + radius * stretch));
        double total = 0;
        for (int p = first; p <= last; ++p) {
            const double weight = area ? std::max(0.0, std::min((out + 1) * ratio, double(p + 1)) - std::max(out * ratio, double(p)))
                                       : kernel((center - p) / stretch, algorithm);
            if (std::abs(weight) < 1e-12) continue;
            taps.append({std::clamp(p, 0, from - 1), weight}); total += weight;
        }
        if (std::abs(total) < 1e-12) taps = {{std::clamp(qRound(center), 0, from - 1), 1}};
        else for (auto &tap : taps) tap.weight /= total;
    }
    return table;
}
QImage resampleAxis(const QImage &source, int size, bool horizontal, ResizeAlgorithm algorithm, const CancelCheck &cancel) {
    const auto table = contributions(horizontal ? source.width() : source.height(), size, algorithm, cancel);
    if (table.isEmpty()) return {};
    QImage out(horizontal ? QSize(size, source.height()) : QSize(source.width(), size), QImage::Format_ARGB32_Premultiplied);
    if (out.isNull()) return {};
    for (int y = 0; y < out.height(); ++y) {
        if (cancelled(cancel)) return {};
        auto *dst = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < out.width(); ++x) {
            double a = 0, r = 0, g = 0, b = 0;
            for (const auto &tap : table[horizontal ? x : y]) {
                const auto *row = reinterpret_cast<const QRgb *>(source.constScanLine(horizontal ? y : tap.pixel));
                const auto pixel = row[horizontal ? tap.pixel : x];
                a += qAlpha(pixel)*tap.weight; r += qRed(pixel)*tap.weight; g += qGreen(pixel)*tap.weight; b += qBlue(pixel)*tap.weight;
            }
            const int alpha = std::clamp(qRound(a), 0, 255);
            dst[x] = qRgba(std::clamp(qRound(r), 0, alpha), std::clamp(qRound(g), 0, alpha), std::clamp(qRound(b), 0, alpha), alpha);
        }
    }
    return out;
}
}
QString resizeAlgorithmName(ResizeAlgorithm algorithm) {
    switch (algorithm) {
    case ResizeAlgorithm::Smooth: return QCoreApplication::translate("KomaMimic", "HALFTONE相当（Qt平滑・近似）");
    case ResizeAlgorithm::Area: return QCoreApplication::translate("KomaMimic", "平均画素法");
    case ResizeAlgorithm::Lanczos3: return "Lanczos 3";
    case ResizeAlgorithm::Bicubic: return QCoreApplication::translate("KomaMimic", "バイキュービック（Mitchell）");
    case ResizeAlgorithm::Bilinear: return QCoreApplication::translate("KomaMimic", "線形補間");
    case ResizeAlgorithm::AreaSharpWeak: return QCoreApplication::translate("KomaMimic", "平均画素法＋シャープ弱");
    case ResizeAlgorithm::AreaSharpStrong: return QCoreApplication::translate("KomaMimic", "平均画素法＋シャープ強");
    case ResizeAlgorithm::Nearest: return QCoreApplication::translate("KomaMimic", "最近傍（補間なし）");
    }
    return {};
}
QImage resizeImage(const QImage &source, QSize target, ResizeAlgorithm algorithm, const CancelCheck &cancel) {
    if (source.isNull() || !target.isValid() || target.width() > 16384 || target.height() > 16384
        || qint64(target.width()) * target.height() > 16 * 1024 * 1024 || cancelled(cancel)) return {};
    if (source.size() == target) return source;
    if (algorithm == ResizeAlgorithm::Smooth) return source.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (source.width() > 65536 || source.height() > 65536) return {};
    QImage image = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const auto pass = [&](QImage input, bool horizontal) {
        const int size = horizontal ? target.width() : target.height();
        if ((horizontal ? input.width() : input.height()) == size) return input;
        return resampleAxis(input, size, horizontal, algorithm, cancel);
    };
    const bool horizontalFirst = qint64(target.width()) * source.height() <= qint64(source.width()) * target.height();
    image = pass(image, horizontalFirst);
    if (image.isNull() || cancelled(cancel)) return {};
    image = pass(image, !horizontalFirst);
    if (algorithm == ResizeAlgorithm::AreaSharpWeak || algorithm == ResizeAlgorithm::AreaSharpStrong)
        image = sharpenImage(image, algorithm == ResizeAlgorithm::AreaSharpWeak ? 0.4 : 1.0, cancel);
    return image;
}
QImage processImage(const QImage &source, const ImageOptions &options, const CancelCheck &cancel) {
    if (!options.enabled || source.isNull()) return source;
    QImage image = source;
    const int horizontal=int(image.width()*std::clamp(options.cropPercent,0.0,40.0)/100.0);
    const int vertical=int(image.height()*std::clamp(options.cropPercent,0.0,40.0)/100.0);
    const auto m = options.crop+QMargins(horizontal,vertical,horizontal,vertical);
    const QRect crop(m.left(), m.top(), image.width() - m.left() - m.right(), image.height() - m.top() - m.bottom());
    if (!crop.isValid() || m.left() < 0 || m.top() < 0 || m.right() < 0 || m.bottom() < 0) return {};
    if (!m.isNull()) image = image.copy(crop);
    if (options.autoCrop) {
        const auto bounds = contentBounds(image, image.rect(), options.whiteTolerance, cancel);
        if (cancelled(cancel)) return {};
        if (bounds.isValid()) image = image.copy(bounds);
    }
    const int turns = ((options.rotation % 4) + 4) % 4;
    if (turns) image = image.transformed(QTransform().rotate(turns * 90));
    if (options.flipH || options.flipV) image = image.transformed(QTransform::fromScale(options.flipH ? -1 : 1, options.flipV ? -1 : 1));
    const bool tone = options.blackPoint || options.whitePoint != 255 || options.outputBlack || options.outputWhite != 255 || options.curveShadows != 64 || options.curveMidtones != 128 || options.curveHighlights != 192 || options.brightness || options.contrast || std::abs(options.gamma - 1) > 1e-8 || options.gray || options.binary || options.invert;
    if (tone) {
        image = image.convertToFormat(QImage::Format_ARGB32);
        std::array<int, 256> lut{};
        const double contrast = std::pow(2.0, std::clamp(options.contrast, -100, 100) / 50.0);
        const double gamma = std::clamp(options.gamma, 0.1, 4.0);
        for (int c = 0; c < 256; ++c) {
            const double value = std::clamp(((std::clamp((c - options.blackPoint) / double(std::max(1, options.whitePoint-options.blackPoint)),0.0,1.0) - 0.5) * contrast + 0.5) + std::clamp(options.brightness, -100, 100) / 100.0, 0.0, 1.0);
            const double v=255*std::pow(value,1.0/gamma);
            const int knots[]={0,std::clamp(options.curveShadows,0,255),std::clamp(options.curveMidtones,0,255),std::clamp(options.curveHighlights,0,255),255};
            const int segment=std::min(3,int(v)/64);const double part=(v-segment*64)/(segment==3?63.0:64.0);
            const double curved=knots[segment]*(1-part)+knots[segment+1]*part;
            lut[c]=std::clamp(qRound(options.outputBlack+curved/255*(options.outputWhite-options.outputBlack)),0,255);
        }
        for (int y = 0; y < image.height(); ++y) {
            if (cancelled(cancel)) return {};
            auto *row = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                const auto p = row[x]; int r = lut[qRed(p)], g = lut[qGreen(p)], b = lut[qBlue(p)];
                if (options.gray || options.binary) r = g = b = (77*r + 150*g + 29*b + 128) >> 8;
                if (options.binary) r = g = b = r >= options.threshold ? 255 : 0;
                if (options.invert) { r = 255 - r; g = 255 - g; b = 255 - b; }
                row[x] = qRgba(r, g, b, qAlpha(p));
            }
        }
    }
    if(options.denoise || options.lineWidth){
        image=image.convertToFormat(QImage::Format_ARGB32);
        const int passes=std::clamp(std::abs(options.lineWidth),0,3)+(options.denoise?1:0);
        for(int pass=0;pass<passes;++pass){
            QImage next=image.copy();
            for(int y=0;y<image.height();++y){if(cancelled(cancel))return {};auto *row=reinterpret_cast<QRgb*>(next.scanLine(y));
                for(int x=0;x<image.width();++x){int rr[9],gg[9],bb[9],n=0;
                    for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){const auto color=image.pixel(std::clamp(x+dx,0,image.width()-1),std::clamp(y+dy,0,image.height()-1));rr[n]=qRed(color);gg[n]=qGreen(color);bb[n++]=qBlue(color);}
                    const bool median=options.denoise && pass==0;
                    const int rank=median?4:options.lineWidth>0?0:8;
                    std::sort(rr,rr+9);std::sort(gg,gg+9);std::sort(bb,bb+9);
                    const auto old=row[x];row[x]=qRgba(rr[rank],gg[rank],bb[rank],qAlpha(old));
                }
            }image=next;
        }
    }
    if(options.resizeToHeight>0) {
        const int height=std::clamp(options.resizeToHeight,1,8192);
        const qint64 width=qRound64(double(image.width())*height/image.height());
        if(width<1 || width>16384 || width*height>MaxPixels)return {};
        image=resizeImage(image,{int(width),height},ResizeAlgorithm::Lanczos3,cancel);if(image.isNull())return {};
    }
    if(options.resize.isValid()){
        QSize size=image.size();size.scale(options.resize,Qt::KeepAspectRatio);
        image=resizeImage(image,size,ResizeAlgorithm::Lanczos3,cancel);if(image.isNull())return {};
    }
    if (options.sharpen) image = sharpenImage(image, options.sharpen == 1 ? 0.4 : 1.0, cancel);
    const auto &b=options.border;
    const qint64 width=qint64(image.width())+b.left()+b.right(),height=qint64(image.height())+b.top()+b.bottom();
    if(!b.isNull()){
        if(b.left()<0||b.top()<0||b.right()<0||b.bottom()<0||width>16384||height>16384||width*height>48*1024*1024)return {};
        QImage canvas(int(width),int(height),QImage::Format_ARGB32);if(canvas.isNull())return {};canvas.fill(options.borderColor);
        QPainter painter(&canvas);painter.drawImage(b.left(),b.top(),image);painter.end();image=canvas;
    }
    if(!options.overlay.isEmpty()){
        QPainter painter(&image);QFont font;font.setPixelSize(std::clamp(options.textSize,8,200));painter.setFont(font);painter.setPen(options.textColor);
        painter.drawText(image.rect().adjusted(12,12,-12,-12),Qt::AlignLeft|Qt::AlignBottom|Qt::TextWordWrap,options.overlay.left(4096));
    }
    return image;
}
ImageResult processMedia(const ImageResult &source, const ImageOptions &options, const CancelCheck &cancel) {
    if (source.image.isNull() || !options.enabled || options == ImageOptions{}) return source;
    ImageResult out = source;
    if (source.frames.isEmpty()) out.image = processImage(source.image, options, cancel);
    else {
        out.frames.clear();
        // Auto-crop must use one stable rectangle for all animation frames.
        ImageOptions stable = options;
        if (options.autoCrop) {
            stable.autoCrop = false;
            QRect bounds;
            const auto size = source.image.size();
            const int horizontal=int(size.width()*std::clamp(options.cropPercent,0.0,40.0)/100.0);
            const int vertical=int(size.height()*std::clamp(options.cropPercent,0.0,40.0)/100.0);
            const auto m=options.crop+QMargins(horizontal,vertical,horizontal,vertical);
            const QRect region(m.left(), m.top(), size.width() - m.left() - m.right(), size.height() - m.top() - m.bottom());
            if (region.isValid() && source.image.rect().contains(region)) {
                for (const auto &frame : source.frames) {
                    if (frame.size() != size || cancelled(cancel)) return {{}, QCoreApplication::translate("KomaMimic", "アニメーションのcanvas寸法が一致しません。")};
                    bounds = bounds.united(contentBounds(frame, region, options.whiteTolerance, cancel));
                }
                if (bounds.isValid()) {stable.crop = QMargins(bounds.left(), bounds.top(), size.width() - 1 - bounds.right(), size.height() - 1 - bounds.bottom());stable.cropPercent=0;}
            }
        }
        for (const auto &frame : source.frames) {
            if (cancelled(cancel)) return {};
            const auto filtered = processImage(frame, stable, cancel);
            if (filtered.isNull()) { out.frames.clear(); out.image = {}; break; }
            out.frames.append(filtered);
        }
        if (!out.frames.isEmpty()) out.image = out.frames.first();
    }
    if(!options.scriptFilter.isEmpty() && !out.image.isNull()) {
      if(out.frames.isEmpty()) {
        const auto filtered=applyScriptFilter(options.scriptFilter,out.image,options.scriptSettings,cancel);
        if(!filtered.error.isEmpty())return filtered;
        out.image=filtered.image;
      } else {
        for(auto &frame:out.frames) {
          const auto filtered=applyScriptFilter(options.scriptFilter,frame,options.scriptSettings,cancel);
          if(!filtered.error.isEmpty())return filtered;
          frame=filtered.image;
        }
        out.image=out.frames.first();
      }
    }
    if (out.image.isNull() && !cancelled(cancel)) out.error = QCoreApplication::translate("KomaMimic", "切り抜き範囲が画像の外側です。画像加工をリセットしてください。");
    return out;
}

QVariantMap advancedImageSettings(const ImageOptions &o){
    return {{"cropPercent",o.cropPercent},{"resizeToHeight",o.resizeToHeight},{"blackPoint",o.blackPoint},{"whitePoint",o.whitePoint},{"outputBlack",o.outputBlack},{"outputWhite",o.outputWhite},{"curveShadows",o.curveShadows},{"curveMidtones",o.curveMidtones},{"curveHighlights",o.curveHighlights},{"denoise",o.denoise},{"lineWidth",o.lineWidth},{"textSize",o.textSize},{"overlay",o.overlay},{"resizeWidth",o.resize.width()},{"resizeHeight",o.resize.height()},{"borderLeft",o.border.left()},{"borderTop",o.border.top()},{"borderRight",o.border.right()},{"borderBottom",o.border.bottom()},{"borderColor",o.borderColor.name(QColor::HexArgb)},{"textColor",o.textColor.name(QColor::HexArgb)}};
}
void restoreAdvancedImageSettings(ImageOptions &o,const QVariantMap &v){
    const double crop=v.value("cropPercent",0).toDouble();o.cropPercent=std::isfinite(crop)?std::clamp(crop,0.0,40.0):0;
    o.resizeToHeight=std::clamp(v.value("resizeToHeight",0).toInt(),0,8192);
    auto integer=[&](const char *key,int initial,int low,int high){return std::clamp(v.value(key,initial).toInt(),low,high);};
    o.blackPoint=integer("blackPoint",0,0,254);o.whitePoint=integer("whitePoint",255,o.blackPoint+1,255);
    o.outputBlack=integer("outputBlack",0,0,255);o.outputWhite=integer("outputWhite",255,0,255);
    o.curveShadows=integer("curveShadows",64,0,255);o.curveMidtones=integer("curveMidtones",128,0,255);o.curveHighlights=integer("curveHighlights",192,0,255);
    o.denoise=integer("denoise",0,0,1);o.lineWidth=integer("lineWidth",0,-3,3);o.textSize=integer("textSize",24,8,200);o.overlay=v.value("overlay").toString().left(4096);
    o.resize={integer("resizeWidth",-1,-1,8192),integer("resizeHeight",-1,-1,8192)};if(!o.resize.isValid()||o.resize.isEmpty())o.resize={};
    o.border={integer("borderLeft",0,0,4096),integer("borderTop",0,0,4096),integer("borderRight",0,0,4096),integer("borderBottom",0,0,4096)};
    o.borderColor=QColor(v.value("borderColor","#ffffffff").toString());if(!o.borderColor.isValid())o.borderColor=Qt::white;
    o.textColor=QColor(v.value("textColor","#ff000000").toString());if(!o.textColor.isValid())o.textColor=Qt::black;
}
