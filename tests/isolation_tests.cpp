// SPDX-License-Identifier: GPL-3.0-only
#include "test_support.h"
#include "test_paths.h"

class IsolationTests : public QObject {
    Q_OBJECT
private slots:
    void newDirectoriesRemainInsideTheProfile() {
        QTemporaryDir temporary;
        const auto root = temporary.filePath("profile");
        QVERIFY(QDir().mkpath(root));
        const auto missing = root + "/home/Library/Caches/設定 space/Application";
        QVERIFY(!QFileInfo::exists(missing));
        QVERIFY(testPathIsInside(root, missing));
        QVERIFY(testPathIsInside(root, root));
        QVERIFY(!testPathIsInside(root, root + "-outside/new/cache"));
        QVERIFY(!testPathIsInside(root, {}));
        QVERIFY(!testPathIsInside(root, "relative/path"));
        QVERIFY(!QFileInfo::exists(missing)); // Validation never creates directories.
    }
#ifdef Q_OS_UNIX
    void aliasesOfTheSameRootAreAccepted() {
        QTemporaryDir temporary;
        const auto real = temporary.filePath("private/var");
        const auto alias = temporary.filePath("var");
        QVERIFY(QDir().mkpath(real));
        QVERIFY(QFile::link(real, alias));
        const auto missing = "/home/Library/Caches/KomaMimicTests/Regression";
        QVERIFY(testPathIsInside(real, alias + missing));
        QVERIFY(testPathIsInside(alias, real + missing));
        QCOMPARE(resolvedTestPath(alias + missing), QDir(real).canonicalPath() + missing);
    }
    void symlinkEscapeIsRejected() {
        QTemporaryDir temporary;
        const auto root = temporary.filePath("profile");
        const auto outside = temporary.filePath("normal-home");
        QVERIFY(QDir().mkpath(root));
        QVERIFY(QDir().mkpath(outside));
        QVERIFY(QFile::link(outside, root + "/escape"));
        QVERIFY(!testPathIsInside(root, root + "/escape/Library/Caches/new"));
    }
#endif
};
ISOLATED_TEST_MAIN(IsolationTests)
#include "isolation_tests.moc"
