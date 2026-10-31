// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QWidget>
#include <QImage>
#include <QVector>
#include <QElapsedTimer>
#include "media.h"
#include "imageprocessing.h"
#include <QCache>
#include <atomic>
#include <memory>
class QTimer;
class PageView : public QWidget {
    Q_OBJECT
public:
    explicit PageView(QWidget *parent = nullptr);
    ~PageView() override;
    void setPages(const QVector<QImage> &images, bool rightToLeft, const QString &caption);
    void setMedia(const QVector<ImageResult> &media, bool rightToLeft, const QString &caption);
    void setAnimationPaused(bool paused);
    void stopAnimations();
    void restartAnimations();
    void setAnimationRepeat(bool enabled);
    int animationFrame(int page = 0) const;
    void setMessage(const QString &message);
    void setResizeAlgorithm(ResizeAlgorithm algorithm);
    ResizeAlgorithm resizeAlgorithm() const { return m_algorithm; }
    void setScaleMode(ScaleMode mode);
    ScaleMode scaleMode() const { return m_scaleMode; }
    void setNoUpscale(bool enabled);
    bool noUpscale() const { return m_noUpscale; }
    void zoomBy(double factor);
    double zoomFactor() const { return m_zoom; }
    bool scaleReady() const { return m_scaleReady; }
    QVector<QRectF> pageRects() const;
    QRectF imageArea() const;
    void setFilenameVisible(bool visible);
    bool filenameVisible() const { return m_filenameVisible; }
    QVector<QImage> images() const { return m_images; }
    MemoryCacheInfo resizeCacheInfo() const { return {int(m_scaleCache.size()),qint64(m_scaleCache.totalCost())*1024,qint64(m_scaleCache.maxCost())*1024}; }
    QPointF pan() const { return m_pan; }
    void setPan(QPointF value);
    void setSourceOrigin(QPointF value);
    bool scrollBy(QPointF delta, int pixelsPerSecond = 0);
    void setBackgroundColor(const QColor &color) { m_background=color; update(); }
    bool scan(int direction, bool rightToLeft, int overlapPercent = 15);
    void enterScan(int direction, bool rightToLeft);
    void applyScanEntry();
    void setLoupe(bool visible, int radius = 100, double zoom = 2, bool round = true);
    void cancelPointer();
    void setLeadingBlank(bool enabled);
    void setTransition(int kind,int duration);
signals:
    void nextRequested();
    void previousRequested();
    void animationChanged(bool available, bool paused);
    void scaleChanged();
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void hideEvent(QHideEvent *) override;
    void showEvent(QShowEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
private:
    void animate();
    void requestScale();
    void startScale();
    void clampPan();
    ResizeAlgorithm m_algorithm = ResizeAlgorithm::Smooth;
    ScaleMode m_scaleMode = ScaleMode::Fit;
    bool m_noUpscale = false, m_scaleReady = false, m_scaleBusy = false, m_scalePending = false, m_dragging = false;
    double m_zoom = 1;
    QPointF m_pan, m_pressPosition, m_pressPan;
    QPointF m_scrollTarget;
    QTimer *m_scrollTimer;
    QElapsedTimer m_scrollClock;
    int m_scrollSpeed=4000;
    QColor m_background;
    Qt::MouseButton m_pressedButton = Qt::NoButton;
    QVector<QImage> m_scaled;
    QString m_scaleError;
    QTimer *m_scaleTimer;
    quint64 m_scaleGeneration = 0;
    std::shared_ptr<std::atomic<quint64>> m_scaleToken;
    QCache<QString, QImage> m_scaleCache;
    struct Playback { qint64 elapsed = 0; int frame = 0; bool finished = false; };
    QVector<ImageResult> m_media;
    QVector<Playback> m_playback;
    QTimer *m_timer;
    QElapsedTimer m_clock;
    bool m_paused = false, m_repeat = false;
    QVector<QImage> m_images;
    QString m_message, m_caption;
    int m_scanEntry = 0; bool m_scanRtl = true;
    bool m_rtl = true;
    bool m_filenameVisible = false;
    int m_wheelRemainder = 0;
    bool m_loupe = false, m_loupeRound = true;
    int m_loupeRadius = 100;
    double m_loupeZoom = 2;
    QPointF m_pointer;
    bool m_leadingBlank = false;
    QImage m_transitionImage;
    QElapsedTimer m_transitionClock;QTimer *m_transitionTimer;
    int m_transitionKind=0,m_transitionDuration=180;
};
