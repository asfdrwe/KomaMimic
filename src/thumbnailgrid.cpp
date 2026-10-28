// SPDX-License-Identifier: GPL-3.0-only
#include "thumbnailgrid.h"
#include <QCoreApplication>
#include <QApplication>
#include <QCursor>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QTextLayout>
#include <QTimer>
#include <cmath>
#include <algorithm>
namespace {
constexpr int ItemMargin = 8, CaptionGap = 4, MinimumIconSize = 24;
int captionHeight(const QFont &font) { return 2 * QFontMetrics(font).lineSpacing() + 4; }

class ThumbnailDelegate final : public QStyledItemDelegate {
public:
    explicit ThumbnailDelegate(QListWidget *grid) : QStyledItemDelegate(grid), m_grid(grid) {}
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override { return m_grid->viewMode()==QListView::IconMode?m_grid->gridSize():QStyledItemDelegate::sizeHint(option,index); }
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        if(m_grid->viewMode()!=QListView::IconMode){QStyledItemDelegate::paint(painter,option,index);return;}
        QStyleOptionViewItem opt(option); initStyleOption(&opt, index);
        QString text = opt.text;
        text.replace('\n', ' '); text.replace('\r', ' '); text.replace('\t', ' ');
        const auto icon = opt.icon;
        opt.text.clear(); opt.icon = {};
        opt.features &= ~(QStyleOptionViewItem::HasDisplay | QStyleOptionViewItem::HasDecoration);
        auto *style = opt.widget ? opt.widget->style() : QApplication::style();
        painter->save(); painter->setClipRect(opt.rect, Qt::IntersectClip);
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

        const QRect content = opt.rect.adjusted(ItemMargin, ItemMargin, -ItemMargin, -ItemMargin);
        const QRect caption(content.left(), content.bottom() - captionHeight(opt.font) + 1,
                            content.width(), captionHeight(opt.font));
        const QRect decoration(content.left(), content.top(), content.width(), caption.top() - CaptionGap - content.top());
        const bool enabled = opt.state & QStyle::State_Enabled, selected = opt.state & QStyle::State_Selected;
        icon.paint(painter, decoration, Qt::AlignCenter, !enabled ? QIcon::Disabled : selected ? QIcon::Selected : QIcon::Normal);
        if (caption.width() > 0) {
            // Qt's default icon delegate can consume the caption area with the
            // decoration. Lay out at most two lines in a separate, fixed area.
            QTextLayout layout(text, opt.font);
            QTextOption wrapping; wrapping.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
            layout.setTextOption(wrapping); layout.beginLayout();
            auto first = layout.createLine();
            if (first.isValid()) first.setLineWidth(caption.width());
            const int split = first.isValid() ? first.textLength() : 0;
            layout.endLayout();
            const QFontMetrics metrics(opt.font);
            const QStringList lines{ text.left(split), text.mid(split) };
            painter->setFont(opt.font);
            painter->setPen(opt.palette.color(enabled ? QPalette::Normal : QPalette::Disabled, selected ? QPalette::HighlightedText : QPalette::Text));
            for (int i = 0; i < lines.size(); ++i) {
                const QRect line(caption.left(), caption.top() + i * metrics.lineSpacing(), caption.width(), metrics.height() + 4);
                painter->drawText(line, Qt::AlignHCenter | Qt::AlignTop | Qt::TextSingleLine,
                                  metrics.elidedText(lines[i], Qt::ElideRight, caption.width()));
            }
        }
        painter->restore();
    }
private:
    QListWidget *m_grid;
};
}
ThumbnailGrid::ThumbnailGrid(QWidget *parent) : QListWidget(parent) {
    setViewMode(QListView::IconMode); setResizeMode(QListView::Adjust); setMovement(QListView::Static);
    setUniformItemSizes(true); setWordWrap(true); setSpacing(0); setSelectionMode(QAbstractItemView::SingleSelection);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel); setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setMouseTracking(true); viewport()->setMouseTracking(true);
    resetMousePosition();
    setItemDelegate(new ThumbnailDelegate(this));
    updateGrid();
}
void ThumbnailGrid::resetMousePosition() { m_lastMousePosition = QCursor::pos(); }
void ThumbnailGrid::setMouseSelectionEnabled(bool enabled) {
    m_mouseSelectionEnabled = enabled;
    resetMousePosition();
}
bool ThumbnailGrid::event(QEvent *event) {
    switch (event->type()) {
    case QEvent::KeyPress:
    case QEvent::FocusIn:
    case QEvent::FocusOut:
    case QEvent::WindowActivate:
    case QEvent::WindowDeactivate:
        resetMousePosition();
        break;
    default: break;
    }
    return QListWidget::event(event);
}
void ThumbnailGrid::mouseMoveEvent(QMouseEvent *event) {
    const QPoint position = event->globalPosition().toPoint();
    const bool moved = position != m_lastMousePosition;
    m_lastMousePosition = position;
    QListWidget::mouseMoveEvent(event);
    // A stationary pointer must not undo keyboard navigation or select an item
    // that scrolled/reflowed underneath it. Hover never takes keyboard focus.
    if (!m_mouseSelectionEnabled || !moved || !hasFocus() || !window()->isActiveWindow()
        || event->buttons() != Qt::NoButton || event->modifiers() != Qt::NoModifier
        || state() != NoState || QApplication::activePopupWidget() || QApplication::activeModalWidget()) return;
    const auto local = event->position().toPoint();
    auto *item = viewport()->rect().contains(local) ? itemAt(local) : nullptr;
    if (!item || item == currentItem()) return;
    // Even a partly visible item can be selected without moving the content
    // under the pointer. Keyboard navigation still scrolls its selection into view.
    const bool autoScroll = hasAutoScroll();
    setAutoScroll(false);
    setCurrentItem(item);
    setAutoScroll(autoScroll);
}
QIcon ThumbnailGrid::thumbnailIcon(const QImage &image, bool directory) {
    static QIcon filePlaceholder, directoryPlaceholder;
    auto &placeholder = directory ? directoryPlaceholder : filePlaceholder;
    if (image.isNull() && !placeholder.isNull()) return placeholder;
    QPixmap pixmap(144, 186); pixmap.fill(Qt::transparent); QPainter p(&pixmap);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    if (directory) {
        p.setBrush(QColor("#b88a42")); p.setPen(Qt::NoPen); p.drawRoundedRect(8, 7, 65, 23, 4, 4);
        p.setBrush(QColor("#dcc08c")); p.drawRoundedRect(5, 22, 134, 158, 5, 5);
    } else {
        p.fillRect(QRect(11, 7, 124, 171), QColor("#334d69")); p.fillRect(QRect(6, 7, 5, 171), QColor("#203249"));
    }
    if (!image.isNull()) {
        const QRect space = directory ? QRect(13, 29, 118, 141) : QRect(16, 13, 113, 157);
        const QSize size = image.size().scaled(space.size(), Qt::KeepAspectRatio);
        p.drawImage(QRect(space.center() - QPoint(size.width()/2, size.height()/2), size), image);
    } else {
        p.setPen(directory ? QColor("#634518") : QColor(Qt::white));
        p.drawText(QRect(8, 32, 126, 126), Qt::AlignCenter, directory ? QCoreApplication::translate("KomaMimic", "フォルダ") : QCoreApplication::translate("KomaMimic", "画像・書庫"));
    }
    const QIcon icon(pixmap);
    if (image.isNull()) placeholder = icon;
    return icon;
}
void ThumbnailGrid::setLayoutMode(Layout layout, int columns) {
    m_layout = layout; m_columns = std::clamp(columns, 1, 20); updateGrid(); requestVisible();
}
void ThumbnailGrid::updateGrid() {
    const int caption = captionHeight(font()), padding = 2 * ItemMargin + CaptionGap;
    const int minimumHeight = std::max(90, MinimumIconSize + caption + padding);
    const int w = std::max(64, viewport()->width() - 24), h = std::max(minimumHeight, viewport()->height() - 24);
    int columns = m_layout == Layout::EightColumns ? 8 : m_columns;
    int cellWidth = 0, cellHeight = 0;
    if (m_layout == Layout::Fit && count() > 0) {
        columns = std::max(1, int(std::ceil(std::sqrt(double(count()) * w / (h * 0.7)))));
        const int rows = (count() + columns - 1) / columns;
        cellWidth = std::max(64, w / columns); cellHeight = std::max(minimumHeight, h / rows);
    } else {
        cellWidth = std::max(48, w / columns); cellHeight = std::clamp(qRound(cellWidth * 1.30) + caption + padding, minimumHeight, std::max(310, minimumHeight));
    }
    setGridSize(QSize(cellWidth, cellHeight));
    setIconSize(QSize(std::max(MinimumIconSize, cellWidth - 2 * ItemMargin), std::max(MinimumIconSize, cellHeight - caption - padding)));
}
QVector<int> ThumbnailGrid::visibleRows() const {
    QVector<int> rows;
    if (!isVisible() || !count()) return rows;
    const int columns = std::max(1, viewport()->width() / std::max(1, gridSize().width()));
    const int top = std::max(0, verticalScrollBar()->value() / std::max(1, gridSize().height()) - 1);
    const int first = std::min(count() - 1, top * columns);
    const int n = (viewport()->height() / std::max(1, gridSize().height()) + 4) * columns;
    for (int i = first; i < std::min(count(), first + n); ++i) rows.append(i);
    return rows;
}
void ThumbnailGrid::clearItems() { m_cached.clear(); clear(); }
void ThumbnailGrid::invalidateThumbnails() {
    m_cached.clear();
    for (int row = 0; row < count(); ++row) {
        item(row)->setData(ThumbnailRoleReady, false); item(row)->setData(PendingRole, false);
        item(row)->setData(ThumbnailRefreshRole,true);
        item(row)->setIcon(thumbnailIcon({}, item(row)->data(DirectoryRole).toBool()));
    }
    requestVisible();
}
void ThumbnailGrid::setThumbnail(int row, const QImage &image, const QString &error) {
    if (row < 0 || row >= count()) return;
    auto *entry = item(row); entry->setData(PendingRole, false); entry->setData(ThumbnailRoleReady, true);
    entry->setData(ThumbnailRefreshRole,false);
    if (!error.isEmpty()) entry->setToolTip(entry->toolTip() + '\n' + error);
    if (image.isNull()) return;
    const auto icon = thumbnailIcon(image, entry->data(DirectoryRole).toBool());
    entry->setIcon(QIcon(icon.pixmap(iconSize().boundedTo(QSize(144, 186))))); m_cached.enqueue(row);
    // Icon pixels are bounded independently of the number of list entries.
    const int keep = std::max(160, int(visibleRows().size()) + 20);
    while (m_cached.size() > keep) {
        const int old = m_cached.dequeue();
        if (old >= count()) continue;
        item(old)->setIcon(thumbnailIcon({}, item(old)->data(DirectoryRole).toBool()));
        item(old)->setData(ThumbnailRoleReady, false);
    }
}
void ThumbnailGrid::moveSelection(int dx, int dy) {
    if (!count()) return;
    const int columns = std::max(1, viewport()->width() / std::max(1, gridSize().width()));
    const int row = currentRow() < 0 ? 0 : std::clamp(currentRow() + dx + dy * columns, 0, count() - 1);
    setCurrentRow(row); scrollToItem(currentItem(), QAbstractItemView::EnsureVisible);
    resetMousePosition();
}
void ThumbnailGrid::selectFirst() { if (count()) { setCurrentRow(0); scrollToItem(currentItem()); } resetMousePosition(); }
void ThumbnailGrid::selectLast() { if (count()) { setCurrentRow(count() - 1); scrollToItem(currentItem()); } resetMousePosition(); }
void ThumbnailGrid::requestVisible() {
    if (m_scheduled) return;
    m_scheduled = true;
    QTimer::singleShot(0, this, [this] { m_scheduled = false; updateGrid(); emit visibleRowsChanged(); });
}
void ThumbnailGrid::resizeEvent(QResizeEvent *event) { QListWidget::resizeEvent(event); resetMousePosition(); requestVisible(); }
void ThumbnailGrid::changeEvent(QEvent *event) {
    QListWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) requestVisible();
}
void ThumbnailGrid::showEvent(QShowEvent *event) { QListWidget::showEvent(event); resetMousePosition(); requestVisible(); }
void ThumbnailGrid::scrollContentsBy(int dx, int dy) { QListWidget::scrollContentsBy(dx, dy); resetMousePosition(); requestVisible(); }

void installThumbnailDelegate(QListWidget *list){list->setItemDelegate(new ThumbnailDelegate(list));}
