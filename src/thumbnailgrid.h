// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QListWidget>
#include <QQueue>
#include <QCache>
#include <functional>
struct CustomThumbnailSettings {
    int columns = 0, width = 64, height = 64, horizontalGap = 0, verticalGap = 0, filenameLines = 1;
    bool rightOrigin = false, frames = false;
    bool operator==(const CustomThumbnailSettings &) const = default;
};
enum ThumbnailRole { PathRole = Qt::UserRole, DirectoryRole, ThumbnailRoleReady, PendingRole, PageRole, ThumbnailRefreshRole, ThumbnailPageNameRole, ThumbnailSourceStampRole,
                     ThumbnailCoverStampRole, ThumbnailCoverValidatedRole, ThumbnailNextCheckRole, ThumbnailCacheNextCheckRole, ThumbnailDependenciesRole, ThumbnailHasPixelsRole, ThumbnailSearchDepthRole };
class ThumbnailGrid : public QListWidget {
    Q_OBJECT
public:
    enum class Layout { Custom, EightColumns, Fit };
    explicit ThumbnailGrid(QWidget *parent = nullptr);
    ~ThumbnailGrid() override;
    void setMouseSelectionEnabled(bool enabled);
    bool mouseSelectionEnabled() const { return m_mouseSelectionEnabled; }
    void setLayoutMode(Layout layout, int columns = 0);
    void setCustomSettings(CustomThumbnailSettings settings);
    CustomThumbnailSettings customSettings() const { return m_custom; }
    int filenameLines() const { return m_layout == Layout::Custom ? m_custom.filenameLines : 2; }
    Layout layoutMode() const { return m_layout; }
    int customColumns() const { return m_columns; }
    QVector<int> visibleRows() const;
    void clearItems();
    void setThumbnail(int row, const QImage &image, const QString &error);
    void setMemoryRetention(bool enabled, qint64 bytes);
    qint64 offscreenThumbnailBytes() const;
    void invalidateThumbnails();
    void moveSelection(int dx, int dy);
    void selectFirst();
    void selectLast();
    void requestVisible();
    QIcon placeholderIcon(bool directory = false) const { return displayIcon({},directory); }
    static QIcon thumbnailIcon(const QImage &image = {}, bool directory = false);
signals:
    void visibleRowsChanged();
protected:
    bool event(QEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void changeEvent(QEvent *) override;
    void showEvent(QShowEvent *) override;
    void scrollContentsBy(int dx, int dy) override;
private:
    void resetMousePosition();
    void updateGrid();
    QIcon displayIcon(const QImage &image, bool directory) const;
    void repaintIcons();
    Layout m_layout = Layout::Custom;
    int m_columns = 0;
    CustomThumbnailSettings m_custom;
    bool m_scheduled = false;
    bool m_mouseSelectionEnabled = true;
    QPoint m_lastMousePosition;
    QQueue<int> m_cached;
    struct SourceImage : QImage {
        SourceImage(const QImage &image,std::function<void()> released):QImage(image),released(std::move(released)){}
        ~SourceImage(){released();}
        std::function<void()> released;
    };
    QCache<int, SourceImage> m_sourceImages{32 * 1024};
    void trimThumbnails();
    void recordThumbnailCost(int row,const QImage &image,const QIcon &icon);
    QHash<int,qint64> m_thumbnailCosts;
    qint64 m_thumbnailBytes=0,m_memoryLimit=128LL*1024*1024;
    bool m_retainAll=false;
};

void installThumbnailDelegate(QListWidget *list);
