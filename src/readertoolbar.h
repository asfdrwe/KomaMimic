// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QHash>
#include <QToolBar>
class QLabel;
class QSlider;
class QSettings;
class QWidgetAction;
class ReaderToolBar : public QToolBar {
public:
    explicit ReaderToolBar(const QHash<QString, QAction *> &commands, QWidget *parent = nullptr);
    QSlider *slider() const { return m_slider; }
    QStringList leftCommands() const { return m_left; }
    QStringList rightCommands() const { return m_right; }
    bool pageBarVisible() const { return m_showPage; }
    void setCommands(const QStringList &left, const QStringList &right, bool showPage);
    void setPosition(int count, int index, bool enabled);
    void restore(const QSettings &settings);
    void configure(QSettings &settings);
    static QStringList defaultLeft();
    static QStringList defaultRight();
private:
    QHash<QString, QAction *> m_commands;
    QStringList m_left, m_right;
    bool m_showPage = true;
    QSlider *m_slider;
    QLabel *m_position;
    QWidgetAction *m_pageAction;
};
