// SPDX-License-Identifier: GPL-3.0-only
#include "pageview.h"
#include <QCoreApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <QTimer>
#include <QPainterPath>
#include <QApplication>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>
PageView::PageView(QWidget *parent) : QWidget(parent), m_scaleToken(std::make_shared<std::atomic<quint64>>(0)), m_scaleCache(32 * 1024) {
    m_scrollTimer=new QTimer(this);m_scrollTimer->setInterval(16);
    connect(m_scrollTimer,&QTimer::timeout,this,[this] {
        const auto delta=m_scrollTarget-m_pan;
        const double length=std::hypot(delta.x(),delta.y());
        const double step=m_scrollSpeed*std::min<qint64>(m_scrollClock.restart(),100)/1000.0;
        if(length<=step || length<0.1) {m_pan=m_scrollTarget;m_scrollTimer->stop();}
        else m_pan+=delta*(step/length);
        clampPan();update();
    });
    setObjectName("pageView");
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(240, 180);
    setContextMenuPolicy(Qt::PreventContextMenu);
    setAccessibleName(QCoreApplication::translate("KomaMimic", "読書画面"));
    m_timer = new QTimer(this); m_timer->setInterval(16);
    connect(m_timer, &QTimer::timeout, this, &PageView::animate);
    m_transitionTimer=new QTimer(this);m_transitionTimer->setInterval(16);connect(m_transitionTimer,&QTimer::timeout,this,[this]{if(m_transitionClock.elapsed()>=m_transitionDuration){m_transitionTimer->stop();m_transitionImage={};}update();});
    m_scaleTimer = new QTimer(this); m_scaleTimer->setSingleShot(true); m_scaleTimer->setInterval(30);
    connect(m_scaleTimer, &QTimer::timeout, this, &PageView::startScale);
    setMessage(QCoreApplication::translate("KomaMimic", "左の書庫・画像をダブルクリックして開きます\n\n左クリック: 次へ　右クリック: 前へ\nショートカットは「ツール」で変更できます"));
}
PageView::~PageView() { m_scaleToken->fetch_add(1); }
void PageView::setPages(const QVector<QImage> &images, bool rtl, const QString &caption) {
    QVector<ImageResult> media;
    for (const auto &image : images) media.append(ImageResult(image));
    setMedia(media, rtl, caption);
}
void PageView::setMedia(const QVector<ImageResult> &media, bool rtl, const QString &caption) {
    m_scrollTimer->stop();
    m_transitionTimer->stop();m_transitionImage={};
    if(m_transitionKind&&isVisible()&&!m_images.isEmpty()&&qint64(width())*height()<8*1024*1024){m_transitionImage=grab().toImage();m_transitionClock.start();m_transitionTimer->start();}
    m_timer->stop(); m_media = media; m_playback = QVector<Playback>(media.size());
    m_images.clear(); bool animated = false;
    for (const auto &asset : media) { m_images.append(asset.image); animated |= asset.frames.size() > 1; }
    m_rtl = rtl; m_caption = caption; m_message.clear(); m_paused = false;
    m_clock.start(); if (animated && isVisible()) m_timer->start();
    m_pan = {};
    emit animationChanged(animated, false); requestScale(); update();
}
void PageView::setAnimationPaused(bool paused) {
    m_paused = paused; m_clock.restart();
    bool animated = false;
    for (const auto &asset : m_media) animated |= asset.frames.size() > 1;
    if (animated && !paused) {
        bool finished = true;
        for (int i = 0; i < m_media.size(); ++i) if (m_media[i].frames.size() > 1 && !m_playback[i].finished) finished = false;
        if (finished) {
            m_playback = QVector<Playback>(m_media.size());
            for (int i = 0; i < m_media.size(); ++i) m_images[i] = m_media[i].image;
            requestScale();
            update();
        }
    }
    if (animated && !paused && isVisible()) m_timer->start(); else m_timer->stop();
    emit animationChanged(animated, paused);
}
void PageView::stopAnimations() {
    m_playback = QVector<Playback>(m_media.size());
    for (int i = 0; i < m_media.size(); ++i) m_images[i] = m_media[i].image;
    setAnimationPaused(true); requestScale(); update();
}
void PageView::restartAnimations() { stopAnimations(); setAnimationPaused(false); }
void PageView::setAnimationRepeat(bool enabled) { m_repeat = enabled; restartAnimations(); }
int PageView::animationFrame(int page) const { return page >= 0 && page < m_playback.size() ? m_playback[page].frame : -1; }
void PageView::animate() {
    const qint64 delta = m_clock.restart();
    if (m_paused || !isVisible() || window()->isMinimized() || QApplication::activeModalWidget()) return;
    bool changed = false, running = false;
    for (int i = 0; i < m_media.size(); ++i) {
        const auto &asset = m_media[i]; auto &state = m_playback[i];
        if (asset.frames.size() < 2 || state.finished) continue;
        qint64 duration = 0;
        for (int delay : asset.delays) duration += delay;
        if (!duration) continue;
        state.elapsed += delta;
        qint64 time = state.elapsed % duration;
        int frame = 0;
        if (!m_repeat && asset.loopCount >= 0 && state.elapsed / duration >= qint64(asset.loopCount) + 1) {
            state.finished = true; frame = asset.frames.size() - 1;
        } else {
            while (frame + 1 < asset.delays.size() && time >= asset.delays[frame]) { time -= asset.delays[frame]; ++frame; }
            running = true;
        }
        if (frame != state.frame) { state.frame = frame; m_images[i] = asset.frames[frame]; changed = true; }
    }
    if (changed) { requestScale(); update(); }
    if (!running) { m_timer->stop(); m_paused = true; emit animationChanged(true, true); }
}
void PageView::setMessage(const QString &message) {
    m_scanEntry=0;
    m_timer->stop(); m_media.clear(); m_playback.clear(); emit animationChanged(false, false);
    m_images.clear(); m_caption.clear(); m_message = message; update();
    requestScale();
}
void PageView::hideEvent(QHideEvent *event) {
    m_scrollTimer->stop(); m_timer->stop(); m_scaleTimer->stop(); m_scaleToken->store(++m_scaleGeneration); m_scalePending = true;
    QWidget::hideEvent(event);
}
void PageView::showEvent(QShowEvent *event) {
    QWidget::showEvent(event); m_clock.start();
    for (const auto &asset : m_media) if (asset.frames.size() > 1 && !m_paused) { m_timer->start(); break; }
    requestScale();
}
void PageView::setResizeAlgorithm(ResizeAlgorithm algorithm) { m_algorithm = algorithm; requestScale(); emit scaleChanged(); update(); }
void PageView::setScaleMode(ScaleMode mode) { m_scrollTimer->stop(); m_scaleMode = mode; m_zoom = 1; m_pan = {}; requestScale(); emit scaleChanged(); update(); }
void PageView::setNoUpscale(bool enabled) { m_noUpscale = enabled; clampPan(); requestScale(); emit scaleChanged(); update(); }
void PageView::zoomBy(double factor) {
    m_scrollTimer->stop();
    if (!std::isfinite(factor) || factor <= 0) return;
    const auto rects = pageRects();
    const double current = rects.isEmpty() ? m_zoom : rects[0].width() * devicePixelRatioF() / (m_images[0].width()/m_images[0].devicePixelRatio());
    m_zoom = std::clamp(current * factor, 0.05, 16.0); m_scaleMode = ScaleMode::Zoom; m_pan = {};
    requestScale(); emit scaleChanged(); update();
}
QRectF PageView::imageArea() const {
    return QRectF(20,16,std::max(1,width()-40),std::max(1,height()-(m_filenameVisible?58:32)));
}
void PageView::setFilenameVisible(bool visible) {
    if (m_filenameVisible == visible) return;
    m_filenameVisible = visible;
    m_scrollTimer->stop(); clampPan(); requestScale(); emit scaleChanged(); update();
}
QVector<QRectF> PageView::pageRects() const {
    QVector<QRectF> rects(m_images.size());
    if (m_images.isEmpty()) return rects;
    const QRectF available = imageArea();
    const bool blank = m_leadingBlank && m_images.size() == 1;
    const double gap = m_images.size() > 1 || blank ? 6 : 0, dpr = devicePixelRatioF();
    double aspect = 0, minimumHeight = 1e20;
    for (const auto &image : m_images) { aspect += double(image.width()) / image.height(); minimumHeight = std::min(minimumHeight, image.height() / image.devicePixelRatio() / dpr); }
    if (blank) aspect *= 2;
    double h = std::max(1.0, std::min(available.height(), (available.width() - gap) / aspect));
    if (m_scaleMode == ScaleMode::Width) h = std::max(1.0, (available.width() - gap) / aspect);
    else if (m_scaleMode == ScaleMode::Fill) h = std::max(1.0,std::max(available.height(),(available.width()-gap)/aspect));
    else if (m_scaleMode == ScaleMode::EqualHeight) h = std::min(minimumHeight,h);
    else if (m_scaleMode == ScaleMode::Height) h = std::max(1.0, available.height());
    const bool native = m_scaleMode == ScaleMode::Actual || m_scaleMode == ScaleMode::Zoom;
    const double scale = m_scaleMode == ScaleMode::Actual ? 1 : m_zoom;
    if (!native && m_noUpscale) h = std::min(h, minimumHeight);
    double width = gap;
    for (const auto &image : m_images) width += native ? image.width() / image.devicePixelRatio() / dpr * (m_noUpscale ? std::min(1.0, scale) : scale) : h * image.width() / image.height();
    if (blank) width = 2 * (width - gap) + gap;
    double x = available.center().x() - width / 2 + m_pan.x();
    for (int n = 0; n < m_images.size(); ++n) {
        const int i = m_rtl ? m_images.size() - 1 - n : n;
        const auto &image = m_images[i];
        const double height = native ? image.height() / image.devicePixelRatio() / dpr * (m_noUpscale ? std::min(1.0, scale) : scale) : h;
        const double w = height * image.width() / image.height();
        rects[i] = QRectF(x + (blank && !m_rtl ? w + gap : 0), available.center().y() - height/2 + m_pan.y(), w, height); x += w + gap;
    }
    return rects;
}
void PageView::clampPan() {
    const auto rects = pageRects(); if (rects.isEmpty()) { m_pan = {}; return; }
    QRectF total;
    for (const auto &r : rects) total = total.united(r);
    const double maxX = std::max(0.0, (total.width() - imageArea().width()) / 2);
    const double maxY = std::max(0.0, (total.height() - imageArea().height()) / 2);
    m_pan.setX(std::clamp(m_pan.x(), -maxX, maxX)); m_pan.setY(std::clamp(m_pan.y(), -maxY, maxY));
}
void PageView::requestScale() {
    m_scaleToken->store(++m_scaleGeneration); m_scaleReady = false; m_scalePending = true;
    m_scaled.clear(); m_scaleError.clear();
    if (isVisible()) m_scaleTimer->start();
}
void PageView::startScale() {
    if (m_scaleBusy || !isVisible()) return;
    m_scalePending = false;
    if (m_images.isEmpty()) { m_scaleReady = true; return; }
    const auto rects = pageRects(); const double dpr = devicePixelRatioF();
    QVector<QSize> targets; QStringList keys; QVector<QImage> cached;
    for (int i = 0; i < m_images.size(); ++i) {
        const QSize target(std::max(1, qRound(rects[i].width()*dpr)), std::max(1, qRound(rects[i].height()*dpr)));
        targets.append(target);
        const QString key = QString("%1:%2:%3:%4").arg(m_images[i].cacheKey()).arg(target.width()).arg(target.height()).arg(int(m_algorithm));
        keys.append(key); cached.append(m_scaleCache.contains(key) ? *m_scaleCache.object(key) : QImage{});
    }
    m_scaleBusy = true;
    const quint64 generation = m_scaleGeneration;
    auto *watcher = new QFutureWatcher<QVector<QImage>>(this);
    connect(watcher, &QFutureWatcher<QVector<QImage>>::finished, this, [this, watcher, generation, keys] {
        const auto images = watcher->result(); watcher->deleteLater(); m_scaleBusy = false;
        if (generation == m_scaleGeneration) {
            m_scaled = images; m_scaleReady = true;
            for (int i = 0; i < images.size(); ++i) {
                if (images[i].isNull()) { m_scaleError = QCoreApplication::translate("KomaMimic", "拡縮処理の上限のため標準描画で表示"); continue; }
                const qint64 kb = (images[i].sizeInBytes() + 1023) / 1024;
                if (kb <= m_scaleCache.maxCost()) m_scaleCache.insert(keys[i], new QImage(images[i]), int(kb));
            }
            update();
        }
        if (m_scalePending && isVisible()) m_scaleTimer->start(0);
    });
    watcher->setFuture(QtConcurrent::run([images = m_images, targets, cached, algorithm = m_algorithm, token = m_scaleToken, generation] {
        QVector<QImage> scaled;
        const auto cancel = [token, generation] { return token->load() != generation; };
        for (int i = 0; i < images.size(); ++i) {
            if (cancel()) return QVector<QImage>{};
            scaled.append(cached[i].isNull() ? resizeImage(images[i], targets[i], algorithm, cancel) : cached[i]);
        }
        return scaled;
    }));
}
void PageView::resizeEvent(QResizeEvent *event) { QWidget::resizeEvent(event); clampPan(); requestScale(); emit scaleChanged(); }
void PageView::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), m_background.isValid()?m_background:palette().color(QPalette::Base));
    p.setRenderHint(QPainter::SmoothPixmapTransform, m_algorithm != ResizeAlgorithm::Nearest);
    if (m_images.isEmpty()) {
        p.setPen(palette().color(QPalette::Text));
        p.drawText(rect().adjusted(24, 24, -24, -24), Qt::AlignCenter | Qt::TextWordWrap, m_message);
        return;
    }
    const auto rects = pageRects();
    p.save(); p.setClipRect(imageArea());
    for (int i = 0; i < m_images.size(); ++i) {
        p.fillRect(rects[i], Qt::white);
        const auto &image = m_scaleReady && i < m_scaled.size() && !m_scaled[i].isNull() ? m_scaled[i] : m_images[i];
        p.drawImage(rects[i], image);
    }
    p.restore();
    if(!m_transitionImage.isNull()){const double progress=std::clamp(m_transitionClock.elapsed()/double(m_transitionDuration),0.0,1.0);p.save();p.setOpacity(1-progress);p.drawImage(QPointF(m_transitionKind==2?(m_rtl?1:-1)*progress*width():0,0),m_transitionImage);p.restore();}
    if (m_loupe) for (int i = 0; i < rects.size(); ++i) if (rects[i].contains(m_pointer)) {
        const auto area = imageArea();
        const int radius = std::max(1, std::min({m_loupeRadius, int(area.width()/2), int(area.height()/2)}));
        const QPointF center(std::clamp(m_pointer.x(), area.left()+radius, std::max(area.left()+radius,area.right()-radius)),
                             std::clamp(m_pointer.y(), area.top()+radius, std::max(area.top()+radius,area.bottom()-radius)));
        const QRectF glass(center.x() - radius, center.y() - radius, 2 * radius, 2 * radius);
        const QPointF source((m_pointer.x() - rects[i].left()) / rects[i].width() * m_images[i].width(), (m_pointer.y() - rects[i].top()) / rects[i].height() * m_images[i].height());
        const double half = radius / m_loupeZoom * devicePixelRatioF() * m_images[i].devicePixelRatio(); QPainterPath clip;
        if (m_loupeRound) clip.addEllipse(glass); else clip.addRect(glass);
        p.save(); p.setClipPath(clip); p.fillRect(glass, Qt::white);
        p.drawImage(glass, m_images[i], QRectF(source.x() - half, source.y() - half, 2 * half, 2 * half)); p.restore();
        p.setPen(QPen(Qt::white, 2)); p.setBrush(Qt::NoBrush); if (m_loupeRound) p.drawEllipse(glass); else p.drawRect(glass); break;
    }
    p.setPen(palette().color(QPalette::PlaceholderText));
    if (m_filenameVisible)
        p.drawText(QRect(12, height() - 30, width() - 24, 24), Qt::AlignCenter,
                   fontMetrics().elidedText(m_caption, Qt::ElideMiddle, std::max(1,width()-30)));
    // Scaling warnings remain visible even when the filename band is disabled.
    if (!m_scaleError.isEmpty()) {
        const QRect warning = imageArea().toAlignedRect().adjusted(4,4,-4,-4);
        const QRect line(warning.left(), warning.top(), warning.width(), fontMetrics().height()+8);
        p.fillRect(line, palette().color(QPalette::Base));
        p.drawText(line, Qt::AlignCenter, fontMetrics().elidedText(m_scaleError,Qt::ElideRight,line.width()));
    }
}
void PageView::mousePressEvent(QMouseEvent *event) {
    m_scrollTimer->stop();
    setFocus();
    m_pressedButton = event->button(); m_pressPosition = event->position(); m_pressPan = m_pan; m_dragging = false;
    event->accept();
}
void PageView::mouseMoveEvent(QMouseEvent *event) {
    m_pointer = event->position(); if (m_loupe) update();
    if ((m_pressedButton == Qt::LeftButton || m_pressedButton == Qt::MiddleButton)
        && (event->position() - m_pressPosition).manhattanLength() > QApplication::startDragDistance()) {
        m_dragging = true; m_pan = m_pressPan + event->position() - m_pressPosition; clampPan(); setCursor(Qt::ClosedHandCursor); update();
    }
    event->accept();
}
void PageView::setPan(QPointF value) { m_scrollTimer->stop(); m_pan = value; clampPan(); update(); }
bool PageView::scrollBy(QPointF delta, int pixelsPerSecond) {
    if(pixelsPerSecond<=0) {
        m_scrollTimer->stop();const auto previous=m_pan;setPan(m_pan+delta);return previous!=m_pan;
    }
    const auto previous=m_pan;
    m_pan=(m_scrollTimer->isActive()?m_scrollTarget:m_pan)+delta;clampPan();
    m_scrollTarget=m_pan;m_pan=previous;
    if(m_scrollTarget==m_pan)return false;
    m_scrollSpeed=std::clamp(pixelsPerSecond,100,20000);
    if(!m_scrollTimer->isActive()){m_scrollClock.start();m_scrollTimer->start();}
    return true;
}
void PageView::setLoupe(bool visible, int radius, double zoom, bool round) {
    m_loupe = visible; m_loupeRadius = std::clamp(radius, 20, 800); m_loupeZoom = std::clamp(zoom, 0.25, 16.0); m_loupeRound = round; setMouseTracking(true); update();
}
void PageView::cancelPointer() { m_pressedButton = Qt::NoButton; m_dragging = false; unsetCursor(); }
void PageView::setLeadingBlank(bool enabled) { if (m_leadingBlank == enabled) return; m_leadingBlank = enabled; requestScale(); update(); }
void PageView::mouseReleaseEvent(QMouseEvent *event) {
    if (!m_dragging && event->button() == m_pressedButton) {
        if (event->button() == Qt::LeftButton) emit nextRequested();
        else if (event->button() == Qt::RightButton) emit previousRequested();
    }
    m_pressedButton = Qt::NoButton; m_dragging = false; unsetCursor(); event->accept();
}
void PageView::wheelEvent(QWheelEvent *event) {
    if (event->modifiers() & Qt::ControlModifier) {
        const int delta = event->angleDelta().y() ? event->angleDelta().y() : event->pixelDelta().y();
        if (delta) zoomBy(delta > 0 ? 1.25 : 0.8);
        event->accept(); return;
    }
    m_wheelRemainder += event->angleDelta().y();
    if (m_wheelRemainder >= 120) { m_wheelRemainder = 0; emit previousRequested(); }
    else if (m_wheelRemainder <= -120) { m_wheelRemainder = 0; emit nextRequested(); }
    event->accept();
}

void PageView::setTransition(int kind,int duration){m_transitionKind=std::clamp(kind,0,2);m_transitionDuration=std::clamp(duration,50,1500);}

void PageView::setSourceOrigin(QPointF value){const auto boxes=pageRects();if(boxes.isEmpty()||m_images.isEmpty())return;const double scale=boxes[0].width()/(m_images[0].width()/m_images[0].devicePixelRatio());setPan(m_pan+QPointF(imageArea().left()-boxes[0].left()-value.x()*scale,imageArea().top()-boxes[0].top()-value.y()*scale));}

namespace {
QVector<QPointF> scanStops(const QVector<QRectF> &rects, QSizeF viewport, bool rtl, int overlap) {
    QRectF total; for (const auto &r : rects) total = total.united(r);
    const double x = std::max(0.0, (total.width() - viewport.width())/2);
    const double y = std::max(0.0, (total.height() - viewport.height())/2);
    const double fraction = 1 - std::clamp(overlap, 0, 80)/100.0;
    const int columns = std::max(1, int(std::ceil(2*x / std::max(1.0, viewport.width()*fraction))));
    const int rows = std::max(1, int(std::ceil(2*y / std::max(1.0, viewport.height()*fraction))));
    QVector<QPointF> result;
    for (int c=0; c <= (x > 0 ? columns : 0); ++c)
      for (int r=0; r <= (y > 0 ? rows : 0); ++r)
        result.append({(rtl ? -1 : 1)*x*(1-2.0*c/columns), y*(1-2.0*r/rows)});
    return result;
}
}
bool PageView::scan(int direction, bool rtl, int overlap) {
    if (m_images.isEmpty()) return false;
    const auto stops = scanStops(pageRects(), imageArea().size(), rtl, overlap);
    int nearest=0; double distance=1e100;
    for (int i=0;i<stops.size();++i) {
      const double d = (stops[i]-m_pan).manhattanLength();
      if(d < distance) { distance=d; nearest=i; }
    }
    // A manually panned position enters the nearest stop before continuing.
    const int target = nearest + (distance > 1 ? 0 : (direction > 0 ? 1 : -1));
    if (target<0 || target>=stops.size()) return false;
    setPan(stops[target]); return true;
}
void PageView::enterScan(int direction, bool rtl) { m_scanRtl=rtl; m_scanEntry=direction; }
void PageView::applyScanEntry() {
    if(!m_scanEntry)return;
    const auto stops=scanStops(pageRects(), imageArea().size(), m_scanRtl, 15);
    if(!stops.isEmpty()) setPan(m_scanEntry>0 ? stops.first() : stops.last());
    m_scanEntry=0;
}
