// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QTreeView>
#include <QWidget>
#include <QSettings>
class FolderModel;
class QTreeWidget;
class QTreeWidgetItem;
class QSplitter;
class QLabel;
class FolderTree : public QTreeView {
    Q_OBJECT
public:
    explicit FolderTree(QWidget *parent = nullptr);
    void selectPath(const QString &path);
    QString selectedPath() const;
    void navigate(int key);
    void setBrowseRoot(const QString &location, const QString &label = {});
    void setTwoPane(bool enabled);
    QString browseRoot() const;
    void activateSelection();
    static QString topLocation();
    static QString locationName(const QString &location);
protected:
    void keyPressEvent(QKeyEvent *event) override;
signals:
    void directoryActivated(const QString &path);
    void contentsChanged(const QString &path);
private:
    FolderModel *m_files;
    bool m_twoPane = true;
    QString m_pendingSelection;
};

// The left browser has its own position. Right-hand browsing never rewrites it.
class FolderBrowser : public QWidget {
    Q_OBJECT
public:
    explicit FolderBrowser(QSettings &settings, QWidget *parent = nullptr);
    FolderTree *tree() const { return m_tree; }
    void restoreSettings();
    void saveSettings();
    void setTwoPane(bool enabled);
    bool twoPane() const { return m_twoPane; }
    QString location() const { return m_location; }
    void openLocation(const QString &location, const QString &label = {}, bool refresh = false);
    void navigate(int key);
    void activateSelection();
    void focusTree();
signals:
    void directoryActivated(const QString &path);
    void fileActivated(const QString &path);
    void errorOccurred(const QString &message);
private:
    void activateUpper(QTreeWidgetItem *item);
    void goParent();
    QSettings &m_settings;
    FolderTree *m_tree;
    QTreeWidget *m_upper;
    QWidget *m_upperPanel;
    QSplitter *m_splitter;
    QLabel *m_locationLabel;
    QString m_location, m_requestedLocation;
    QStringList m_parents;
    quint64 m_generation = 0;
    bool m_twoPane = true, m_restoring = false;
};
