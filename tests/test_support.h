// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "mainwindow.h"
#include <QApplication>
#include <QBuffer>
#include <QFile>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtTest>

namespace TestData {
bool webEngineIpcAvailable();
QImage solid(QColor color = Qt::red, QSize size = {32, 48});
QByteArray png(const QImage &image);
bool write(const QString &path, const QByteArray &bytes);
bool imageFolder(const QString &path, int count = 5);
struct ZipEntry { QByteArray name, bytes; quint16 flags = 0; };
// Stored ZIP built from the file format, independently of the app's exporter.
QByteArray zip(const QVector<ZipEntry> &entries, const QByteArray &comment = {});
QByteArray apng();
QByteArray tiff();
bool pdf(const QString &path);
void singlePage(MainWindow &window);
}

// Cases use explicit temporary settings paths, also for child processes.
// Redirect XDG config/cache roots too, without using a shared test-mode folder.
template<class Suite> int isolatedTestMain(int argc, char **argv) {
    QTemporaryDir sandbox;
    if (!sandbox.isValid()) return 2;
    qputenv("XDG_CONFIG_HOME", (sandbox.path() + "/config").toUtf8());
    qputenv("XDG_CACHE_HOME", (sandbox.path() + "/cache").toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("KomaMimicTests");
    QCoreApplication::setApplicationName("Regression");
    QThreadPool::globalInstance()->setMaxThreadCount(3);
    int result;
    { Suite suite; result = QTest::qExec(&suite, argc, argv); }
    QThreadPool::globalInstance()->waitForDone();
    return result;
}
#define ISOLATED_TEST_MAIN(Suite) \
    int main(int argc, char **argv) { return isolatedTestMain<Suite>(argc, argv); }
