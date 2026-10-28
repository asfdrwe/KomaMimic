// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QTreeView>
class QFileSystemModel;
class FolderTree : public QTreeView {
    Q_OBJECT
public:
    explicit FolderTree(QWidget *parent = nullptr);
    void selectPath(const QString &path);
    QString selectedPath() const;
    void navigate(int key);
signals:
    void directoryActivated(const QString &path);
private:
    QFileSystemModel *m_files;
};
