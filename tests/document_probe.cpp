// SPDX-License-Identifier: GPL-3.0-only
// Exercise the real document renderer after deployment, without QtTest or user data.
#include "documentview.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTimer>
#include <cstdio>
#include <functional>

int main(int argc, char **argv) {
    DocumentView::registerScheme();
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("KomaMimicDocumentProbe");
    QCoreApplication::setApplicationName("DeploymentCheck");
    QCommandLineParser parser;
    parser.addOption({"parse-only", "Parse documents without starting WebEngine."});
    parser.addOption({"output", "Write the JSON report to a separate file.", "file"});
    parser.addPositionalArgument("path", "Documents or generated fixture root.", "path...");
    parser.process(app);
    auto paths = parser.positionalArguments();
    const bool parseOnly = parser.isSet("parse-only");
    const auto outputPath = parser.value("output");
    if (paths.size() == 1 && QFileInfo(paths.first()).isDir()) {
        const auto directory = QDir(paths.first()).filePath("documents/");
        paths.clear();
        for (const auto *name : {"aozora-utf8.txt", "sample.md", "reflow.epub", "fixed.epub"})
            paths.append(directory + name);
    }
    if (paths.isEmpty()) return 2;
    QJsonArray results;
    bool failed = false;
    int index = 0;
    std::unique_ptr<DocumentView> view;
    QTimer timeout;
    timeout.setSingleShot(true);
    std::function<void()> next;
    const auto record = [&](bool ok, const QString &error, int pages) {
        timeout.stop();
        failed |= !ok;
        results.append(QJsonObject{{"file", QFileInfo(paths[index]).fileName()},
                                   {"pass", ok}, {"error", error}, {"pages", pages}});
        std::fprintf(stderr, "%s document %d/%d\n", ok ? "PASS" : "FAIL", index + 1, int(paths.size()));
        ++index;
        QTimer::singleShot(0, &app, next);
    };
    next = [&] {
        if (index >= paths.size()) {
            const auto bytes = QJsonDocument(QJsonObject{{"all_passed", !failed},
                {"rendered", !parseOnly}, {"results", results}}).toJson();
            if (outputPath.isEmpty()) {
                if (std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout) != size_t(bytes.size()) ||
                    std::fflush(stdout) != 0) {
                    app.exit(3);
                    return;
                }
            } else {
                // Chromium subprocesses can inherit stdout and write their own
                // diagnostics there. Keep the machine-readable report separate.
                QSaveFile output(outputPath);
                if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) {
                    std::fprintf(stderr, "Cannot write document report: %s\n", qPrintable(output.errorString()));
                    app.exit(3);
                    return;
                }
            }
            app.exit(failed ? 1 : 0);
            return;
        }
        const auto publication = readPublication(paths[index], {});
        if (!publication->error.isEmpty() || publication->sections.isEmpty()) {
            record(false, publication->error, 0);
            return;
        }
        if (publication->epub) {
            const auto cover = epubCover(paths[index]);
            if (!cover.error.isEmpty()) {
                record(false, cover.error, 0);
                return;
            }
        }
        if (parseOnly) {
            record(true, {}, int(publication->sections.size()));
            return;
        }
        if (!view) {
            view = std::make_unique<DocumentView>();
            view->resize(900, 700);
            view->show();
            QObject::connect(view.get(), &DocumentView::laidOut, &app,
                [&](const QVector<DocumentLocation> &locations) {
                    if (timeout.isActive()) record(!locations.isEmpty(), {}, int(locations.size()));
                });
            QObject::connect(view.get(), &DocumentView::failed, &app,
                [&](const QString &error) { if (timeout.isActive()) record(false, error, 0); });
        }
        timeout.start(25000);
        view->open(publication, {});
    };
    QObject::connect(&timeout, &QTimer::timeout, &app, [&] { record(false, "Document renderer timed out", 0); });
    QTimer::singleShot(0, &app, next);
    const int code = app.exec();
    view.reset();
    return code;
}
