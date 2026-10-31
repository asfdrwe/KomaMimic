// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QListWidget>
#include <QQueue>
#include <QCache>
struct CustomThumbnailSettings {
    int columns = 0, width = 64, height = 64, horizontalGap = 0, verticalGap = 0, filenameLines = 1;
    bool rightOrigin = false, frames = false;
    bool operator==(const CustomThumbnailSettings &) const = default;
};
enum ThumbnailRole { PathRole = Qt::UserRole, DirectoryRole, ThumbnailRoleReady, PendingRole, PageRole, ThumbnailRefreshRole, ThumbnailPageNameRole };
class ThumbnailGrid : public QListWidget {
    Q_OBJECT
public:
    enum class Layout { Custom, EightColumns, Fit };
    explicit ThumbnailGrid(QWidget *parent = nullptr);
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
    QCache<int, QImage> m_sourceImages{32 * 1024};
};

void installThumbnailDelegate(QListWidget *list);
