// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "mainwindow.h"
#include "documentview.h"
#include "test_paths.h"
#include <QApplication>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtTest>

namespace TestData {
bool webEngineIpcAvailable();
DocumentView *currentDocumentView(const MainWindow &window);
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
    // Preserve the Python launcher's profile on macOS: Foundation may cache
    // CFFIXED_USER_HOME before QApplication while resolving the temp directory.
    const auto outer = qEnvironmentVariable("KOMAMIMIC_TEST_ROOT");
    const auto root = QDir(outer.isEmpty() ? sandbox.path() : outer).canonicalPath();
    if (root.isEmpty()) return 2;
    qputenv("KOMAMIMIC_TEST_ROOT", root.toUtf8());
    const auto redirect = [&](const char *key, const QString &directory) {
        QDir().mkpath(directory);
        qputenv(key, directory.toUtf8());
    };
    redirect("HOME", root + "/home");
    redirect("XDG_CONFIG_HOME", root + "/config");
    redirect("XDG_CACHE_HOME", root + "/cache");
    redirect("XDG_DATA_HOME", root + "/data");
    redirect("XDG_STATE_HOME", root + "/state");
    redirect("XDG_RUNTIME_DIR", root + "/run");
    QFile::setPermissions(root + "/run", QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    for (const auto *key : {"TMPDIR", "TMP", "TEMP"}) redirect(key, root);
#ifdef Q_OS_MACOS
    redirect("CFFIXED_USER_HOME", root + "/home");
#endif
#ifdef Q_OS_WIN
    const auto home = root + "/home";
    redirect("USERPROFILE", home);
    redirect("APPDATA", home + "/AppData/Roaming");
    redirect("LOCALAPPDATA", home + "/AppData/Local");
    const auto drive = home.size() > 1 && home[1] == ':' ? home.left(2) : QString{};
    qputenv("HOMEDRIVE", drive.toUtf8());
    qputenv("HOMEPATH", home.mid(drive.size()).toUtf8());
#endif
#ifdef Q_OS_WIN
    // The offscreen platform does not always discover installed Windows fonts.
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR")) {
        const QByteArray windowsDir = qgetenv("WINDIR");
        if (!windowsDir.isEmpty())
            qputenv("QT_QPA_FONTDIR", windowsDir + "/Fonts");
    }
    // Avoid GPU-context instability in headless Qt WebEngine tests.
    if (qEnvironmentVariableIsEmpty("QTWEBENGINE_CHROMIUM_FLAGS"))
        qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--disable-gpu");
#endif
    qputenv("QT_QPA_PLATFORM", "offscreen");
    DocumentView::registerScheme();
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("KomaMimicTests");
    QCoreApplication::setApplicationName("Regression");
    const auto inside=[&](const QString &path) {
        return testPathIsInside(root, path);
    };
    for (auto kind : {QStandardPaths::CacheLocation, QStandardPaths::AppConfigLocation,
                      QStandardPaths::AppDataLocation, QStandardPaths::GenericDataLocation}) {
        const auto path = QStandardPaths::writableLocation(kind);
        if (!inside(path)) {
            qCritical().noquote()<<"OS directory is outside the test environment:"<<path;
            return 2;
        }
    }
    if (!inside(QDir::tempPath())) return 2;
    app.setProperty("komamimicTestRoot", root);
    QThreadPool::globalInstance()->setMaxThreadCount(3);
    int result;
    { Suite suite; result = QTest::qExec(&suite, argc, argv); }
    QThreadPool::globalInstance()->waitForDone();
    return result;
}
#define ISOLATED_TEST_MAIN(Suite) \
    int main(int argc, char **argv) { return isolatedTestMain<Suite>(argc, argv); }
