// SPDX-License-Identifier: GPL-3.0-only
#include "test_support.h"
#include "i18n.h"
#include "documentview.h"
#include "expression.h"
#include "readertoolbar.h"
#include "shortcuts.h"
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QFontComboBox>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QStatusBar>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>

class I18nTests : public QObject {
    Q_OBJECT
private slots:
    void init() { QCOMPARE(I18n::initialize("en"), QString("en")); }
    void cleanup() { QThreadPool::globalInstance()->waitForDone(); I18n::initialize("ja"); }
    void languageRegistryAndPreferenceOrder() {
        QStringList codes;
        for (const auto &entry : I18n::languages()) { codes << entry.code; QVERIFY(!entry.name.isEmpty()); }
        QVERIFY(codes.contains("ja")); QVERIFY(codes.contains("en"));
        QCOMPARE(I18n::resolve("system", {"ja-JP", "en-US"}), QString("ja"));
        QCOMPARE(I18n::resolve("system", {"de-DE", "en-GB", "ja"}), QString("en"));
        QCOMPARE(I18n::resolve("system", {"de-DE"}), QString("en"));
        QCOMPARE(I18n::resolve("ja", {"en-US"}), QString("ja"));
        QCOMPARE(I18n::resolve("en", {"ja-JP"}), QString("en"));
        QCOMPARE(I18n::resolve("unknown", {"ja-JP"}), QString("ja"));
        QTemporaryDir paths; QVERIFY(paths.isValid());
        const auto a=paths.filePath("a.ini"), b=paths.filePath("b.ini"), c=paths.filePath("c.ini");
        QCOMPARE(I18n::startupConfigPath({"app", "--config="+a}), a);
        QCOMPARE(I18n::startupConfigPath({"app", "-i", b}), b);
        QCOMPARE(I18n::startupConfigPath({"app", "-i"+c}), c);
    }
    void translatedCommandsErrorsAndReaderControls() {
        QHash<QString, QKeySequence> englishKeys;
        for (const auto &spec : shortcutSpecs()) englishKeys[spec.id] = spec.defaultKey;
        QCOMPARE(evaluateExpression("1/0", {}).error, QString("Division by zero."));
        QCOMPARE(DocumentView::readerMessages().value("back").toString(), QString("Return to link origin"));
        QCOMPARE(I18n::formatLabel("画像フォルダ"), QString("Image folder"));
        I18n::initialize("ja");
        for (const auto &spec : shortcutSpecs()) QCOMPARE(spec.defaultKey, englishKeys.value(spec.id));
        QCOMPARE(evaluateExpression("1/0", {}).error, QString("ゼロ除算です。"));
        QCOMPARE(DocumentView::readerMessages().value("back").toString(), QString("リンク元へ戻る"));
    }
    void textSyntaxAndUserContentAreNotTranslated() {
        QTemporaryDir d;
        const auto path = d.filePath("日本語の本.txt");
        QVERIFY(TestData::write(path, QString("［＃ここから大見出し］\n日本語の章\n［＃ここで大見出し終わり］\n｜漢字《かんじ》と本文").toUtf8()));
        auto opened = Book::open(path); QVERIFY2(opened.book, qPrintable(opened.error));
        QVERIFY(opened.book->publication); const auto &publication = *opened.book->publication;
        QVERIFY(publication.sections.first().plain.contains("日本語の章"));
        QVERIFY(publication.sections.first().plain.contains("漢字"));
        QVERIFY(publication.sections.first().html.contains("<h2"));
        QVERIFY(!publication.sections.first().html.contains("［＃ここから大見出し］"));
        QCOMPARE(opened.book->path, path);
        auto generated = Book::fromPages({BookPage{}}); QVERIFY(!generated.book);
    }
    void englishMenuLinksUseStableSettingsPageIds() {
        QTemporaryDir d; MainWindow w(d.filePath("settings.ini"));
        QCOMPARE(w.menuBar()->actions().first()->text(), QString("&File"));
        QCOMPARE(w.command("next")->text(), QString("Next"));
        QString page;
        QTimer::singleShot(0, &w, [&] {
            auto *dialog = w.findChild<QDialog *>("preferencesDialog");
            if (!dialog) return;
            auto *tabs = dialog->findChild<QTabWidget *>();
            page = tabs->currentWidget()->property("settingsPageId").toString(); dialog->reject();
        });
        w.findChild<QAction *>("menuCacheSettings")->trigger(); QCOMPARE(page, QString("cache"));
        w.close();
    }
    void savedLanguageAppliesAfterRestartAndPreservesSettings() {
        QTemporaryDir d; const auto config = d.filePath("settings.ini");
        {
            MainWindow w(config); auto keys = w.bindings(); keys["next"] = QKeySequence("Ctrl+Alt+N"); QVERIFY(w.applyBindings(keys));
            int restartNotices = 0; QString restartText;
            QTimer dismiss;
            connect(&dismiss, &QTimer::timeout, &w, [&] {
                if (auto *notice = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                    if (notice->objectName() == "languageRestartNotice") { ++restartNotices; restartText = notice->text(); }
                    notice->accept();
                }
            });
            dismiss.start(10);
            QTimer::singleShot(0, &w, [&] {
                auto *dialog = w.findChild<QDialog *>("preferencesDialog"); if (!dialog) return;
                auto *language = dialog->findChild<QComboBox *>("ui/language");
                language->setCurrentIndex(language->findData("ja")); QCOMPARE(language->currentData().toString(), QString("ja")); dialog->accept();
            });
            w.command("preferences")->trigger(); QVERIFY2(w.statusBar()->currentMessage().contains("Preferences saved"), qPrintable(w.statusBar()->currentMessage())); QCOMPARE(I18n::currentLanguage(), QString("en"));
            QCOMPARE(w.menuBar()->actions().first()->text(), QString("&File"));
            QCOMPARE(restartNotices, 1); QVERIFY(restartText.contains("restart", Qt::CaseInsensitive));
            for (bool cancel : {false, true}) {
                QTimer::singleShot(0, &w, [&, cancel] {
                    auto *dialog = w.findChild<QDialog *>("preferencesDialog"); if (!dialog) return;
                    if (cancel) {
                        auto *language = dialog->findChild<QComboBox *>("ui/language");
                        language->setCurrentIndex(language->findData("en")); dialog->reject();
                    } else dialog->accept();
                });
                w.command("preferences")->trigger(); QCOMPARE(restartNotices, 1);
            }
            dismiss.stop(); w.close();
        }
        QSettings settings(config, QSettings::IniFormat); QCOMPARE(settings.value("ui/language").toString(), QString("ja"));
        I18n::initialize(settings.value("ui/language").toString());
        MainWindow restored(config); QCOMPARE(restored.menuBar()->actions().first()->text(), QString("ファイル(&F)"));
        QCOMPARE(restored.bindings().value("next"), QKeySequence("Ctrl+Alt+N")); restored.close();
    }
    void documentFontAndColorPreviewDoesNotSaveOnCancel() {
        QTemporaryDir d; MainWindow w(d.filePath("settings.ini")); bool checked = false;
        QTimer::singleShot(0, &w, [&] {
            auto *dialog = w.findChild<QDialog *>("documentSettingsDialog");
            if (!dialog) dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            dialog->findChild<QSpinBox *>("document_size")->setValue(40);
            dialog->findChild<QLineEdit *>("document_foreground")->setText("#123456");
            dialog->findChild<QLineEdit *>("document_background")->setText("#ddeeff");
            auto *preview = dialog->findChild<QLabel *>("documentFontPreview");
            checked = preview && preview->font().pixelSize() == 40 && preview->palette().color(QPalette::WindowText) == QColor("#123456") && preview->palette().color(QPalette::Window) == QColor("#ddeeff");
            dialog->reject();
        });
        w.command("documentSettings")->trigger(); QVERIFY(checked); w.close();
        QSettings settings(d.filePath("settings.ini"), QSettings::IniFormat);
        QVERIFY(!settings.contains("text/size"));
    }
    void toolbarGroupsPreviewAndPersistentPageWidget() {
        QTemporaryDir d; const auto config = d.filePath("settings.ini");
        {
            MainWindow w(config); w.show(); w.command("toolbarToggle")->setChecked(true);
            auto *bar = static_cast<ReaderToolBar *>(w.findChild<QToolBar *>("readerToolbar")); QVERIFY(bar);
            auto *slider = w.findChild<QSlider *>("pageSlider"); QVERIFY(slider); QVERIFY(bar->isAncestorOf(slider));
            QVERIFY(!w.statusBar()->isAncestorOf(slider));
            QSignalSpy next(w.command("next"), &QAction::triggered); bool previewDidNotRun = false;
            QTimer::singleShot(0, &w, [&] {
                auto *dialog = w.findChild<QDialog *>("toolbarSettingsDialog"); if (!dialog) return;
                auto *left = dialog->findChild<QListWidget *>("toolbarLeft"); auto *right = dialog->findChild<QListWidget *>("toolbarRight");
                left->clear(); right->clear();
                for (const auto &id : {"previous", "|", "next"}) {
                    auto *item = new QListWidgetItem(id, left); item->setData(Qt::UserRole, id);
                }
                auto *item = new QListWidgetItem("fullscreenGlobal", right); item->setData(Qt::UserRole, "fullscreenGlobal");
                auto *preview = static_cast<ReaderToolBar *>(dialog->findChild<QToolBar *>("toolbarPreview"));
                QCOMPARE(preview->leftCommands(), QStringList({"previous", "|", "next"}));
                QCOMPARE(preview->rightCommands(), QStringList({"fullscreenGlobal"}));
                for (auto *action : preview->actions()) if (action->text() == w.command("next")->text()) action->trigger();
                previewDidNotRun = next.isEmpty();
                dialog->findChild<QCheckBox *>("toolbarShowPage")->setChecked(false); dialog->accept();
            });
            w.command("toolbarSettings")->trigger(); QVERIFY(previewDidNotRun);
            QCOMPARE(bar->leftCommands(), QStringList({"previous", "|", "next"}));
            QCOMPARE(bar->rightCommands(), QStringList({"fullscreenGlobal"})); QVERIFY(!bar->pageBarVisible());
            QCOMPARE(bar->slider(), slider); w.close();
        }
        MainWindow restored(config); auto *bar = static_cast<ReaderToolBar *>(restored.findChild<QToolBar *>("readerToolbar"));
        QCOMPARE(bar->leftCommands(), QStringList({"previous", "|", "next"})); QVERIFY(!bar->pageBarVisible()); restored.close();
    }
    void legacyToolbarOrderSurvivesMigration() {
        QTemporaryDir d; const auto config = d.filePath("settings.ini");
        { QSettings settings(config, QSettings::IniFormat); settings.setValue("toolbar/actions", QStringList{"next", "previous", "open"}); }
        MainWindow w(config); auto *bar = static_cast<ReaderToolBar *>(w.findChild<QToolBar *>("readerToolbar"));
        QCOMPARE(bar->leftCommands(), QStringList({"next", "previous", "open"})); QVERIFY(bar->rightCommands().isEmpty()); QVERIFY(bar->pageBarVisible());
        bar->setCommands({"next", "next"}, {"next", "open"}, true);
        QCOMPARE(bar->leftCommands(), QStringList({"next"})); QCOMPARE(bar->rightCommands(), QStringList({"open"})); w.close();
    }
    void commandCategoriesAndResetPreserveOtherBindings() {
        QHash<QString, QKeySequence> bindings;
        for (const auto &spec : shortcutSpecs()) bindings[spec.id] = spec.defaultKey;
        bindings["next"] = QKeySequence("Ctrl+Alt+N"); bindings["textSizeIncrease"] = QKeySequence("Ctrl+Alt+T");
        ShortcutDialog dialog(bindings); auto *category = dialog.findChild<QComboBox *>("shortcutCategory"); QVERIFY(category);
        category->setCurrentIndex(category->findData("documents"));
        dialog.findChild<QPushButton *>("resetVisibleShortcuts")->click();
        QCOMPARE(dialog.bindings().value("next"), QKeySequence("Ctrl+Alt+N")); QVERIFY(dialog.bindings().value("textSizeIncrease").isEmpty());
    }
    void inputCategoriesResetOnlyVisibleMappings() {
        for (const bool gamepad : {false, true}) {
            QTemporaryDir d; const auto config = d.filePath("settings.ini");
            const QString prefix = gamepad ? "input/pad/" : "input/mouse/";
            const QString button = gamepad ? "a" : "left", motion = gamepad ? "leftx+" : "wheelUp";
            { QSettings settings(config, QSettings::IniFormat); settings.setValue(prefix + button, "quit"); settings.setValue(prefix + motion, "last"); }
            MainWindow w(config); bool checked = false;
            QTimer::singleShot(0, &w, [&] {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); if (!dialog) return;
                auto *category = dialog->findChild<QComboBox *>("inputCategory");
                auto *table = dialog->findChild<QTableWidget *>("inputBindings");
                category->setCurrentIndex(category->findData("motion"));
                checked = table->isRowHidden(0);
                dialog->findChild<QPushButton *>("resetVisibleInputs")->click(); dialog->accept();
            });
            w.command(gamepad ? "gamepadSettings" : "mouseSettings")->trigger(); QVERIFY(checked); w.close();
            QSettings settings(config, QSettings::IniFormat);
            QCOMPARE(settings.value(prefix + button).toString(), QString("quit"));
            QCOMPARE(settings.value(prefix + motion).toString(), gamepad ? QString("next") : QString("wheelScrollUp"));
        }
    }
    void commandLineLanguageSelectionPreservesIds() {
        QTemporaryDir d; QStringList ids; QString firstLabel;
        for (const auto &language : {"ja", "en"}) {
            const auto config = d.filePath(QString(language) + ".ini");
            { QSettings settings(config, QSettings::IniFormat); settings.setValue("ui/language", language); }
            QProcess process; process.start(KOMAMIMIC_EXECUTABLE, {"--config", config, "--commands"});
            QVERIFY(process.waitForFinished(15000)); QCOMPARE(process.exitCode(), 0);
            QStringList current;
            const auto output = QString::fromUtf8(process.readAllStandardOutput());
            const auto lines = output.split('\n', Qt::SkipEmptyParts);
            QVERIFY(!lines.isEmpty());
            const auto label = lines.first().section('\t', 1, 1);
            if (firstLabel.isEmpty()) firstLabel = label; else QVERIFY(label != firstLabel);
            for (const auto &line : lines) current << line.section('\t', 0, 0);
            if (ids.isEmpty()) ids = current; else QCOMPARE(current, ids);
        }
    }
};
ISOLATED_TEST_MAIN(I18nTests)
#include "i18n_tests.moc"
