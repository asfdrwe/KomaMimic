// SPDX-License-Identifier: GPL-3.0-only
#include "foldertree.h"
#include <QCoreApplication>
#include <QFileSystemModel>
#include <QFileIconProvider>
#include <QDir>
#include <QHeaderView>
#include <QKeyEvent>
#include <QTimer>
#include <QScrollBar>
namespace {
class DirectoryTreeIcons final : public QObject, public QFileIconProvider {
public:
    explicit DirectoryTreeIcons(QObject *parent) : QObject(parent) {
        setOptions(QFileIconProvider::DontUseCustomDirectoryIcons);
    }
    QString type(const QFileInfo &) const override {
        // Only names are displayed in this directory tree. QFileSystemModel
        // also inspects filtered-out files; their unused MIME comments can
        // enter the Windows system-locale code while scanning a directory.
        return {};
    }
};
}
FolderTree::FolderTree(QWidget *parent) : QTreeView(parent), m_files(new QFileSystemModel(this)) {
    setObjectName("folderTree"); setAccessibleName(QCoreApplication::translate("KomaMimic", "フォルダツリー"));
    m_files->setReadOnly(true); m_files->setFilter(QDir::AllDirs | QDir::NoDotAndDotDot | QDir::Drives);
    m_files->setOption(QFileSystemModel::DontUseCustomDirectoryIcons);
    // The model does not own its provider. Parenting it to the model keeps it
    // alive through the model's shutdown, including its information gatherer.
    m_files->setIconProvider(new DirectoryTreeIcons(m_files));
    m_files->setRootPath(QDir::rootPath()); setModel(m_files);
    for (int col = 1; col < m_files->columnCount(); ++col) hideColumn(col);
    setHeaderHidden(true); setUniformRowHeights(true); setEditTriggers(QAbstractItemView::NoEditTriggers);
    header()->setStretchLastSection(false);
    header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    connect(this, &QTreeView::clicked, this, [this](const QModelIndex &index) { emit directoryActivated(m_files->filePath(index)); });
    connect(this, &QTreeView::activated, this, [this](const QModelIndex &index) { emit directoryActivated(m_files->filePath(index)); });
    connect(m_files, &QFileSystemModel::directoryLoaded, this, [this](const QString &directory) {
        const auto selected = selectedPath();
        const auto prefix = directory.endsWith('/') ? directory : directory + '/';
        if (selected == directory || selected.startsWith(prefix)) QTimer::singleShot(0, this, [this] {
            // Asynchronously inserted ancestor rows can move the selected
            // folder below the viewport after the initial scrollTo().
            const auto path = selectedPath(); if (!path.isEmpty()) selectPath(path);
        });
    });
}
void FolderTree::selectPath(const QString &path) {
    const auto index = m_files->index(path);
    if (!index.isValid()) return;
    auto parent = index.parent();
    while (parent.isValid()) { expand(parent); parent = parent.parent(); }
    setCurrentIndex(index); doItemsLayout(); resizeColumnToContents(0); scrollTo(index, QAbstractItemView::PositionAtCenter);
    int depth=0;for(auto ancestor=index.parent();ancestor.isValid();ancestor=ancestor.parent())++depth;horizontalScrollBar()->setValue(std::max(0,depth*indentation()-viewport()->width()/3));
}
QString FolderTree::selectedPath() const { return m_files->filePath(currentIndex()); }
void FolderTree::navigate(int key) {
    QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier); QTreeView::keyPressEvent(&event);
}
