// SPDX-License-Identifier: GPL-3.0-only
#include "readertoolbar.h"
#include <QCoreApplication>
#include <QAction>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <algorithm>

QStringList ReaderToolBar::defaultLeft() {
    return {"open", "first", "previousBook", "previous", "previousOne", "nextOne", "next", "nextBook", "last"};
}
QStringList ReaderToolBar::defaultRight() {
    return {"loupe", "thumbnails", "library", "bookLists", "filterSettings", "fullscreenGlobal"};
}
ReaderToolBar::ReaderToolBar(const QHash<QString, QAction *> &commands, QWidget *parent)
    : QToolBar(QCoreApplication::translate("KomaMimic", "読書操作（下部）"), parent), m_commands(commands) {
    setObjectName("readerToolbar"); setMovable(false); setToolButtonStyle(Qt::ToolButtonIconOnly);
    auto *panel = new QWidget(this); panel->setObjectName("pagePositionPanel");
    panel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *layout = new QHBoxLayout(panel); layout->setContentsMargins(5, 0, 5, 0);
    m_slider = new QSlider(Qt::Horizontal, panel); m_slider->setObjectName("pageSlider");
    m_slider->setAccessibleName(QCoreApplication::translate("KomaMimic", "ページ位置")); m_slider->setMinimumWidth(120);
    m_slider->setFocusPolicy(Qt::NoFocus); layout->addWidget(m_slider, 1);
    m_position = new QLabel(panel); m_position->setObjectName("toolbarPagePosition"); layout->addWidget(m_position);
    m_pageAction = new QWidgetAction(this); m_pageAction->setDefaultWidget(panel);
    setCommands(defaultLeft(), defaultRight(), true); setPosition(0, 0, false);
}
void ReaderToolBar::setCommands(const QStringList &left, const QStringList &right, bool showPage) {
    // Keep the shared command actions and page widgets alive while rebuilding.
    for (auto *action : actions()) {
        removeAction(action);
        if (action->isSeparator() && action->parent() == this) delete action;
    }
    const auto append = [this](const QStringList &ids) {
        QStringList accepted;
        for (const auto &id : ids) {
            if (id == "|") { addSeparator(); accepted.append(id); }
            else if (auto *action = m_commands.value(id); action && !actions().contains(action)) { addAction(action); accepted.append(id); }
        }
        return accepted;
    };
    m_left = append(left); m_showPage = showPage;
    addAction(m_pageAction); m_pageAction->setVisible(showPage);
    m_right = append(right);
}
void ReaderToolBar::setPosition(int count, int index, bool enabled) {
    const QSignalBlocker blocker(m_slider);
    m_slider->setRange(0, std::max(0, count - 1)); m_slider->setValue(index);
    m_slider->setEnabled(enabled && count > 0);
    m_position->setText(count > 0 ? QString("%1 / %2").arg(index + 1).arg(count) : QString("—"));
}
void ReaderToolBar::restore(const QSettings &settings) {
    if (settings.contains("toolbar/leftActions") || settings.contains("toolbar/rightActions"))
        setCommands(settings.value("toolbar/leftActions").toStringList(), settings.value("toolbar/rightActions").toStringList(), settings.value("toolbar/showPageBar", true).toBool());
    else if (settings.contains("toolbar/actions"))
        setCommands(settings.value("toolbar/actions").toStringList(), {}, settings.value("toolbar/showPageBar", true).toBool());
    else setCommands(defaultLeft(), defaultRight(), settings.value("toolbar/showPageBar", true).toBool());
}
void ReaderToolBar::configure(QSettings &settings) {
    QDialog dialog(this); dialog.setObjectName("toolbarSettingsDialog");
    dialog.setWindowTitle(QCoreApplication::translate("KomaMimic", "ツールバーの設定")); dialog.resize(960, 600);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(QCoreApplication::translate("KomaMimic", "操作を左右へ追加し、ドラッグして順番を変更します。設定はOKで保存します。"), &dialog));
    auto *columns = new QHBoxLayout; layout->addLayout(columns, 1);
    const auto list = [&](const QString &name, const QString &title) {
        auto *column = new QVBoxLayout; columns->addLayout(column, 1);
        column->addWidget(new QLabel(title, &dialog));
        auto *result = new QListWidget(&dialog); result->setObjectName(name); column->addWidget(result, 1);
        return result;
    };
    auto *available = list("toolbarAvailable", QCoreApplication::translate("KomaMimic", "使用できる操作"));
    auto *left = list("toolbarLeft", QCoreApplication::translate("KomaMimic", "左のボタン")); auto *right = list("toolbarRight", QCoreApplication::translate("KomaMimic", "右のボタン"));
    for (auto *target : {left, right}) target->setDragDropMode(QAbstractItemView::InternalMove);
    QStringList ids = m_commands.keys(); std::sort(ids.begin(), ids.end());
    for (const auto &id : ids) {
        auto label = m_commands[id]->text(); label.remove('&');
        auto *item = new QListWidgetItem(m_commands[id]->icon(), label, available); item->setData(Qt::UserRole, id);
    }
    const auto append = [&](QListWidget *target, const QString &id) {
        if (id != "|")
            for (auto *group : {left, right})
                for (int row = 0; row < group->count(); ++row)
                    if (group->item(row)->data(Qt::UserRole).toString() == id) return;
        auto label = id == "|" ? QCoreApplication::translate("KomaMimic", "── 区切り ──") : m_commands.value(id)->text(); label.remove('&');
        auto *item = new QListWidgetItem(label, target); item->setData(Qt::UserRole, id);
    };
    for (const auto &id : m_left) append(left, id);
    for (const auto &id : m_right) append(right, id);
    auto *search = new QLineEdit(&dialog); search->setObjectName("toolbarSearch"); search->setPlaceholderText(QCoreApplication::translate("KomaMimic", "操作名で検索")); layout->addWidget(search);
    connect(search, &QLineEdit::textChanged, &dialog, [available](const QString &text) {
        for (int i = 0; i < available->count(); ++i) available->item(i)->setHidden(!available->item(i)->text().contains(text, Qt::CaseInsensitive));
    });
    auto *buttons = new QHBoxLayout; layout->addLayout(buttons);
    for (auto *target : {left, right}) {
        auto *add = new QPushButton(target == left ? QCoreApplication::translate("KomaMimic", "左へ追加") : QCoreApplication::translate("KomaMimic", "右へ追加"), &dialog); buttons->addWidget(add);
        connect(add, &QPushButton::clicked, &dialog, [&, target] {
            if (auto *item = available->currentItem()) append(target, item->data(Qt::UserRole).toString());
        });
        auto *separator = new QPushButton(target == left ? QCoreApplication::translate("KomaMimic", "左に区切り") : QCoreApplication::translate("KomaMimic", "右に区切り"), &dialog); buttons->addWidget(separator);
        connect(separator, &QPushButton::clicked, &dialog, [&, target] { append(target, "|"); });
        auto *remove = new QPushButton(target == left ? QCoreApplication::translate("KomaMimic", "左から削除") : QCoreApplication::translate("KomaMimic", "右から削除"), &dialog); buttons->addWidget(remove);
        connect(remove, &QPushButton::clicked, &dialog, [target] { delete target->takeItem(target->currentRow()); });
    }
    auto *show = new QCheckBox(QCoreApplication::translate("KomaMimic", "中央にページバーを表示"), &dialog); show->setObjectName("toolbarShowPage"); show->setChecked(m_showPage); layout->addWidget(show);
    auto *previewLabel = new QLabel(QCoreApplication::translate("KomaMimic", "配置の見本（ボタンを押しても操作しません）"), &dialog); layout->addWidget(previewLabel);
    QHash<QString, QAction *> copies;
    for (auto it = m_commands.cbegin(); it != m_commands.cend(); ++it) {
        auto *copy = new QAction(it.value()->icon(), it.value()->text(), &dialog); copy->setIconText(it.value()->iconText()); copies[it.key()] = copy;
    }
    auto *preview = new ReaderToolBar(copies, &dialog); preview->setObjectName("toolbarPreview"); preview->setPosition(100, 49, false); layout->addWidget(preview);
    const auto commands = [](QListWidget *source) { QStringList result; for (int i = 0; i < source->count(); ++i) result << source->item(i)->data(Qt::UserRole).toString(); return result; };
    const auto update = [&] { preview->setCommands(commands(left), commands(right), show->isChecked()); };
    for (auto *target : {left, right}) {
        connect(target->model(), &QAbstractItemModel::rowsInserted, &dialog, update);
        connect(target->model(), &QAbstractItemModel::rowsRemoved, &dialog, update);
        connect(target->model(), &QAbstractItemModel::rowsMoved, &dialog, update);
        connect(target->model(), &QAbstractItemModel::dataChanged, &dialog, update);
    }
    connect(show, &QCheckBox::toggled, &dialog, update); update();
    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults, &dialog); layout->addWidget(box);
    connect(box, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(box->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, &dialog, [&] {
        left->clear(); right->clear(); for (const auto &id : defaultLeft()) append(left, id); for (const auto &id : defaultRight()) append(right, id); show->setChecked(true);
    });
    if (dialog.exec() != QDialog::Accepted) return;
    setCommands(commands(left), commands(right), show->isChecked());
    settings.setValue("toolbar/leftActions", m_left); settings.setValue("toolbar/rightActions", m_right);
    settings.setValue("toolbar/showPageBar", m_showPage);
    auto flattened = m_left + m_right; flattened.removeAll("|"); settings.setValue("toolbar/actions", flattened);
}
