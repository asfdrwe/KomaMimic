// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QListWidget>
#include <QQueue>
enum ThumbnailRole { PathRole = Qt::UserRole, DirectoryRole, ThumbnailRoleReady, PendingRole, PageRole, ThumbnailRefreshRole };
class ThumbnailGrid : public QListWidget {
    Q_OBJECT
public:
    enum class Layout { Custom, EightColumns, Fit };
    explicit ThumbnailGrid(QWidget *parent = nullptr);
    void setMouseSelectionEnabled(bool enabled);
    bool mouseSelectionEnabled() const { return m_mouseSelectionEnabled; }
    void setLayoutMode(Layout layout, int columns = 5);
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
    Layout m_layout = Layout::Custom;
    int m_columns = 5;
    bool m_scheduled = false;
    bool m_mouseSelectionEnabled = true;
    QPoint m_lastMousePosition;
    QQueue<int> m_cached;
};

void installThumbnailDelegate(QListWidget *list);
