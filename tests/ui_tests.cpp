// SPDX-License-Identifier: GPL-3.0-only
#include "test_support.h"
#include "pageview.h"
#include "readerfeatures.h"
#include "librarystore.h"
#include "foldertree.h"
#include "shortcuts.h"
#include "scriptreader.h"
#include "defaultpreferences.h"
#include "thumbnailsource.h"
#include "i18n.h"
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QJsonArray>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QScopeGuard>
#include <QTableWidget>
#include <QWheelEvent>
#include <QCursor>
#include <QSet>
#include <QDialog>
#include <QDir>
#include <QFileSystemWatcher>
#include <QFileSystemModel>
#include <QThreadPool>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSignalSpy>
#include <QTimer>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QTextStream>
#include <QVBoxLayout>

namespace {
// Also deliver an unchanged global position: Qt can send mouse-move events
// after scrolling/reflow even when the physical pointer has not moved.
void thumbnailPointer(ThumbnailGrid *grid, QPoint position,
                      Qt::MouseButtons buttons = Qt::NoButton) {
    const QPoint global = grid->viewport()->mapToGlobal(position);
    // Moving the offscreen platform cursor generates another event with no
    // buttons. For a simulated drag, deliver only the event carrying its buttons.
    if (buttons == Qt::NoButton) QCursor::setPos(global);
    QMouseEvent event(QEvent::MouseMove, QPointF(position), QPointF(global),
                      Qt::NoButton, buttons, Qt::NoModifier);
    QApplication::sendEvent(grid->viewport(), &event);
    // Drain the platform's own move while the tested popup/focus state is
    // still in effect, before the caller closes a dialog or changes windows.
    QCoreApplication::processEvents();
}
}

class UiTests : public QObject {
    Q_OBJECT
private slots:
    void folderTreeListsFilesystemRoots() {
        FolderTree tree;
        auto *files=tree.model(); QVERIFY(files);
        const auto root=files->index(0,0); QVERIFY(root.isValid());
#ifdef Q_OS_WIN
        QCOMPARE(root.data().toString(),QString("PC"));
        auto visibleRoots=[&] {
            QStringList names;
            for(int row=0;row<files->rowCount(root);++row)names.append(files->index(row,0,root).data(Qt::ToolTipRole).toString());
            return names;
        };
        for(const auto &drive:QDir::drives())
            QTRY_VERIFY(visibleRoots().contains(QDir::toNativeSeparators(drive.absoluteFilePath())));
#else
        QCOMPARE(root.data().toString(),QString("/"));
#endif
        QTemporaryDir directory; QVERIFY(directory.isValid());
        tree.selectPath(directory.path());
        QTRY_COMPARE(QDir::cleanPath(tree.selectedPath()),QDir::cleanPath(directory.path()));
    }
    void closeCancelsCollectionCover() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto scripts=directory.filePath("script"); QVERIFY(QDir().mkpath(scripts));
        QVERIFY(TestData::write(scripts+"/manifest.json",R"({"apiVersion":1,"kind":"reader","id":"close-test","extensions":["lag"],"magic":"4c414747","script":"decode.js"})"));
        QVERIFY(TestData::write(scripts+"/decode.js","function decode(){while(true){}}"));
        QVERIFY(configureScriptReaders({scripts}).isEmpty());
        const auto restore=qScopeGuard([] {configureScriptReaders({});});
        const auto path=directory.filePath("slow.lag"); QVERIFY(TestData::write(path,"LAGG input"));
        { LibraryStore library(directory.filePath("library.sqlite3"));
          QVERIFY(library.error().isEmpty()); QVERIFY(!library.add("playlist",path).isEmpty()); }
        MainWindow window(directory.filePath("settings.ini")); window.show();
        window.command("bookLists")->trigger();
        auto *list=window.findChild<QListWidget *>("collection_playlist"); QVERIFY(list);
        QTRY_VERIFY(list->isVisible());
        QTest::qWait(250);
        QVERIFY(window.backgroundWorkActive());
        QElapsedTimer elapsed;elapsed.start();window.close();
        QTRY_VERIFY_WITH_TIMEOUT(!window.backgroundWorkActive(),1500);
        qInfo()<<"Background work stopped after close:"<<elapsed.elapsed()<<"ms";
    }
    void visibleCachedCoverBypassesSlowGeneration() {
        QTemporaryDir d;const auto scripts=d.filePath("scripts"),books=d.filePath("books");
        QVERIFY(QDir().mkpath(scripts));QVERIFY(QDir().mkpath(books));
        QVERIFY(TestData::write(scripts+"/manifest.json",R"({"apiVersion":1,"kind":"reader","id":"slow-cover","extensions":["lag"],"magic":"4c414747","script":"decode.js"})"));
        QVERIFY(TestData::write(scripts+"/decode.js","function decode(){while(true){}}"));
        QVERIFY(configureScriptReaders({scripts}).isEmpty());
        const auto restore=qScopeGuard([]{configureScriptReaders({});});
        QVERIFY(TestData::write(books+"/01.lag","LAGG first"));QVERIFY(TestData::write(books+"/02.lag","LAGG second"));
        const auto png=books+"/03.png";QVERIFY(TestData::write(png,TestData::png(TestData::solid(Qt::blue))));
        auto cache=std::make_shared<ThumbnailDiskCache>();QVERIFY(!cachedCover(png,{},cache).image.isNull());
        MainWindow window(d.filePath("settings.ini"));window.show();window.openDirectory(books);QTRY_VERIFY(window.directoryReady());
        QTRY_VERIFY(window.directoryReady());
        auto *grid=window.findChild<ThumbnailGrid *>("zipLibrary");QVERIFY(grid);QCOMPARE(grid->count(),3);
        QTRY_VERIFY_WITH_TIMEOUT(grid->item(2)->data(ThumbnailRoleReady).toBool(),2000);
        QVERIFY(!grid->item(0)->data(ThumbnailRoleReady).toBool());
        window.openBook(png);QTRY_VERIFY_WITH_TIMEOUT(window.renderReady(),3000);
        window.close();QTRY_VERIFY_WITH_TIMEOUT(!window.backgroundWorkActive(),1500);
    }
    void explicitRefreshBypassesSavedCover() {
        QTemporaryDir d;const auto books=d.filePath("books");QVERIFY(QDir().mkpath(books));
        const auto source=books+"/page.png";QVERIFY(TestData::write(source,TestData::png(TestData::solid(Qt::red))));
        auto cache=std::make_shared<ThumbnailDiskCache>();
        const auto beforeList=QDir(cache->directory()).entryList({"*.png"},QDir::Files);
        const QSet<QString> before(beforeList.begin(),beforeList.end());
        QVERIFY(!cachedCover(source,{},cache).image.isNull());QString stored;
        for(const auto &name:QDir(cache->directory()).entryList({"*.png"},QDir::Files))if(!before.contains(name))stored=name;
        QVERIFY(!stored.isEmpty());QVERIFY(TestData::write(QDir(cache->directory()).filePath(stored),TestData::png(TestData::solid(Qt::blue))));
        MainWindow w(d.filePath("settings.ini"));w.show();w.openDirectory(books);QTRY_VERIFY(w.directoryReady());
        auto *grid=w.findChild<ThumbnailGrid *>("zipLibrary");QVERIFY(grid);QCOMPARE(grid->count(),1);
        const auto color=[&]{const auto image=grid->item(0)->icon().pixmap(80,120).toImage();return image.pixelColor(image.rect().center());};
        QTRY_COMPARE(color(),QColor(Qt::blue));
        w.command("thumbRefresh")->trigger();QTRY_VERIFY(w.directoryReady());
        QTRY_COMPARE(color(),QColor(Qt::red));w.close();
    }
    void pageModeDoesNotRenderHiddenReader() {
        QTemporaryDir d;const auto books=d.filePath("books");QVERIFY(QDir().mkpath(books));
        QVERIFY(TestData::write(books+"/pages.zip",TestData::zip({{"01.png",TestData::png(TestData::solid())},{"02.png",TestData::png(TestData::solid(Qt::blue))}})));
        MainWindow w(d.filePath("settings.ini"));w.show();w.openDirectory(books);QTRY_VERIFY(w.directoryReady());
        w.showPageThumbnails();QTRY_VERIFY(!w.isLoading() && w.pageCount()==2);
        QCOMPARE(w.viewMode(),MainWindow::ViewMode::Pages);QVERIFY(w.requestReady());
        auto *grid=w.findChild<ThumbnailGrid *>("pageThumbnails");QVERIFY(grid);
        QTRY_VERIFY(grid->item(0)->data(ThumbnailRoleReady).toBool());QVERIFY(!w.renderReady());
        grid->setCurrentRow(1);QApplication::clipboard()->clear();w.command("imageCopy")->trigger();
        QTRY_VERIFY(!QApplication::clipboard()->image().isNull());
        QCOMPARE(QApplication::clipboard()->image().pixelColor(0,0),QColor(Qt::blue));
        QCOMPARE(w.pageIndex(),0);QVERIFY(!w.renderReady());
        grid->setCurrentRow(0);QApplication::clipboard()->clear();
        w.runRequest({{"page",2},{"commands",QJsonArray{"imageCopy"}}});
        QTRY_VERIFY(!QApplication::clipboard()->image().isNull());
        QCOMPARE(grid->currentRow(),1);QCOMPARE(w.pageIndex(),1);
        QCOMPARE(QApplication::clipboard()->image().pixelColor(0,0),QColor(Qt::blue));QVERIFY(!w.renderReady());
        w.command("thumbAccept")->trigger();QTRY_VERIFY(w.renderReady());QCOMPARE(w.viewMode(),MainWindow::ViewMode::Reader);w.close();
    }
    void mangaDefaultsAndSavedSettings() {
        QTemporaryDir d;const auto config=d.filePath("new.ini");
        {
            MainWindow w(config);const auto keys=w.bindings();
            QVERIFY2(shortcutConflict(keys).isEmpty(),qPrintable(shortcutConflict(keys)));
            for(const auto &[id,key]:QList<QPair<QString,QString>>{{"scaleActual","C"},{"scaleFit","X"},
                {"autoSpread","Space"},{"reloadBook","F5"},{"thumbRefresh","F6"},{"toc","F7"},
                {"boundaryCycle","F8"},{"profile1","1"},{"profile10","0"},{"settingsLoad4","F4"},{"settingsSave4","Shift+F4"}})
                QCOMPARE(keys.value(id),QKeySequence(key));
            QSettings settings(config,QSettings::IniFormat);
            QCOMPARE(settings.value("cache/ahead").toInt(),1);QCOMPARE(settings.value("cache/behind").toInt(),0);
            QVERIFY(settings.value("General/watch").toBool());QVERIFY(settings.value("reading/leadingBlank").toBool());
            QCOMPARE(settings.value("navigation/count").toInt(),20);
            QVERIFY(!settings.value("processing/filterEnabled").toBool());
            QVERIFY(TestData::write(d.filePath("sample.png"),TestData::png(TestData::solid())));
            w.show();w.openBook(d.filePath("sample.png"));QTRY_VERIFY(w.renderReady());
            w.command("profile1")->trigger();QCOMPARE(w.imageOptions().cropPercent,5.0);
            const auto cropped=processImage(TestData::solid(Qt::red,{100,200}),w.imageOptions());QCOMPARE(cropped.size(),QSize(90,180));
            w.command("profile6")->trigger();QCOMPARE(w.imageOptions().resizeToHeight,1200);
            const auto resized=processImage(TestData::solid(Qt::red,{50,100}),w.imageOptions());QCOMPARE(resized.size(),QSize(600,1200));
            w.command("profile9")->trigger();QVERIFY(w.imageOptions().flipH);
            w.command("profile20")->trigger();QVERIFY(w.imageOptions().gray);
            w.command("saveProfile3")->trigger();w.command("profile3")->trigger();QVERIFY(w.imageOptions().gray);
            ShortcutDialog reset(keys);auto *editor=reset.findChild<QKeySequenceEdit *>("key_scaleActual");QVERIFY(editor);
            editor->setKeySequence(QKeySequence("Ctrl+Alt+C"));
            auto *resetButton=reset.findChild<QPushButton *>("resetVisibleShortcuts");QVERIFY(resetButton);resetButton->click();
            QCOMPARE(editor->keySequence(),QKeySequence("C"));
            w.close();
        }
        const auto old=d.filePath("saved.ini");
        {QSettings saved(old,QSettings::IniFormat);saved.setValue("keys/scaleActual","Ctrl+Alt+A");
            saved.setValue("keys/fullscreenGlobal","F11");saved.setValue("input/mouse/wheelUp","zoomIn");
            saved.setValue("cache/ahead",7);saved.setValue("General/watch",false);saved.setValue("display/background","#123456");}
        for(int restart=0;restart<2;++restart) {
            MainWindow w(old);QCOMPARE(w.bindings().value("scaleActual"),QKeySequence("Ctrl+Alt+A"));
            QCOMPARE(w.bindings().value("fullscreenGlobal"),QKeySequence("F11"));
            QSettings saved(old,QSettings::IniFormat);QCOMPARE(saved.value("cache/ahead").toInt(),7);
            QVERIFY(!saved.value("General/watch").toBool());QCOMPARE(saved.value("input/mouse/wheelUp").toString(),QString("zoomIn"));
            QCOMPARE(saved.value("display/background").toString(),QString("#123456"));w.close();
        }
    }
    void mangaMouseDefaultsAndScroll() {
        QTemporaryDir d;const auto config=d.filePath("settings.ini");
        QVERIFY(TestData::imageFolder(d.filePath("images"),3));
        MainWindow w(config);w.show();w.activateWindow();TestData::singlePage(w);
        w.openBook(d.filePath("images/1.png"));QTRY_VERIFY(w.renderReady());
        auto *view=w.findChild<PageView *>();QVERIFY(view);
        view->setScaleMode(ScaleMode::Actual);view->setPages({TestData::solid(Qt::red,{400,2000})},true,{});
        QTRY_VERIFY(view->scaleReady());const auto start=view->pan();
        QWheelEvent wheel(QPointF(100,100),QPointF(view->mapToGlobal(QPoint(100,100))),{},QPoint(0,-120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
        QApplication::sendEvent(view,&wheel);QTRY_VERIFY(view->pan().y()<start.y());
        QCOMPARE(w.pageIndex(),0);
        const auto defaults=w.bindings();
#ifndef Q_OS_MACOS
        view->setFocus();QTest::keyClick(view,Qt::Key_Return);QVERIFY(!w.isFullScreen());
        QTest::keyClick(view,Qt::Key_F11);QTRY_VERIFY(w.isFullScreen());
        QTest::keyClick(view,Qt::Key_F11);QTRY_VERIFY(!w.isFullScreen());
#endif
        w.showPageThumbnails();auto *pages=w.findChild<ThumbnailGrid *>("pageThumbnails");QVERIFY(pages);
        QTest::keyClick(pages,Qt::Key_Return);QCOMPARE(w.viewMode(),MainWindow::ViewMode::Reader);QVERIFY(!w.isFullScreen());
        QCOMPARE(w.bindings(),defaults);
        QTimer::singleShot(0,&w,[&] {
            auto *dialog=w.findChild<QDialog *>("mouseSettings");QVERIFY(dialog);QTimer::singleShot(2000,dialog,&QDialog::reject);
            auto *table=dialog->findChild<QTableWidget *>("inputBindings");QVERIFY(table);
            for(int row=0;row<table->rowCount();++row)if(table->item(row,0)->data(Qt::UserRole)=="wheelUp") {
                auto *choice=qobject_cast<QComboBox *>(table->cellWidget(row,1));QVERIFY(choice);
                QCOMPARE(choice->currentData().toString(),QString("wheelScrollUp"));
            }
            auto *reset=dialog->findChild<QPushButton *>("resetMouseBindings");QVERIFY(reset);reset->click();dialog->reject();
        });
        w.command("mouseSettings")->trigger();w.close();
    }
    void fullscreenDefaultDoesNotOverlapAccept() {
        QTemporaryDir d;const auto images=d.filePath("images");QVERIFY(TestData::imageFolder(images,2));
        MainWindow w(d.filePath("settings.ini"));w.show();w.activateWindow();w.openDirectory(images);QTRY_VERIFY(w.directoryReady());
        const auto keys=w.bindings();
        QCOMPARE(keys.value("thumbAccept"),QKeySequence("Return"));
        QCOMPARE(keys.value("treeAccept"),QKeySequence("Return"));
        QCOMPARE(keys.value("fullscreenGlobal"),defaultFullscreenShortcut());
        QVERIFY(keys.value("fullscreenGlobal")!=keys.value("thumbAccept"));
        QVERIFY(keys.value("fullscreenGlobal")!=keys.value("treeAccept"));
        auto *covers=w.findChild<ThumbnailGrid *>("zipLibrary");QVERIFY(covers);covers->setCurrentRow(0);covers->setFocus();
        // Scoped actions follow the actual focus, which can arrive after
        // show()/activateWindow() on macOS. Do not send Return before it is
        // bound to the thumbnail action, then mistake a missed key for a
        // rendering failure.
        QTRY_VERIFY(w.isActiveWindow() && covers->hasFocus());
        QCOMPARE(covers->count(),2);QCOMPARE(covers->currentRow(),0);
        QTRY_COMPARE(w.command("thumbAccept")->shortcut(),QKeySequence("Return"));
        QSignalSpy accepted(w.command("thumbAccept"),&QAction::triggered);
        QTest::keyClick(covers,Qt::Key_Return);QCOMPARE(accepted.count(),1);
        QTRY_VERIFY(w.renderReady());
        QCOMPARE(w.viewMode(),MainWindow::ViewMode::Reader);QVERIFY(!w.isFullScreen());
#ifndef Q_OS_MACOS
        auto *view=w.findChild<PageView *>();QVERIFY(view);view->setFocus();
        QTest::keyClick(view,Qt::Key_F11);QTRY_VERIFY(w.isFullScreen());
        QTest::keyClick(view,Qt::Key_F11);QTRY_VERIFY(!w.isFullScreen());
        w.showCovers();QTRY_VERIFY(w.requestReady());covers->setFocus();
        QTest::keyClick(covers,Qt::Key_F11);QTRY_VERIFY(w.isFullScreen());
        QTest::keyClick(covers,Qt::Key_F11);QTRY_VERIFY(!w.isFullScreen());
#endif
        QCOMPARE(w.bindings(),keys);w.close();
        MainWindow restored(d.filePath("settings.ini"));QCOMPARE(restored.bindings(),keys);restored.close();
    }
    void slideshowScrollDefault() {
        QTemporaryDir d;const auto path=d.filePath("tall.png");
        QVERIFY(TestData::write(path,TestData::png(TestData::solid(Qt::red,{400,3000}))));
        MainWindow w(d.filePath("settings.ini"));w.show();w.openBook(path);QTRY_VERIFY(w.renderReady());
        auto *view=w.findChild<PageView *>();QVERIFY(view);view->setScaleMode(ScaleMode::Actual);QTRY_VERIFY(view->scaleReady());
        const auto pan=view->pan();w.command("slideForward")->trigger();
        auto *timer=w.findChild<QTimer *>("slideshowTimer");QVERIFY(timer);QVERIFY(timer->isActive());
        QTRY_VERIFY(view->pan()!=pan);QCOMPARE(w.pageIndex(),0);QVERIFY(timer->isActive());
        w.command("slideForward")->trigger();QVERIFY(!timer->isActive());w.close();
    }
    void scriptPreferencesSaveCancelAndRestart() {
        QTemporaryDir d;const auto root=d.filePath("packages");
        QVERIFY(QDir().mkpath(root+"/reader"));QVERIFY(QDir().mkpath(root+"/filter"));
        QVERIFY(TestData::write(root+"/reader/manifest.json",R"({"id":"ui-reader","apiVersion":1,"extensions":["uiimg"],"magic":"554950","script":"decode.js","settings":{"red":{"type":"integer","minimum":0,"maximum":255,"default":10}}})"));
        QVERIFY(TestData::write(root+"/reader/decode.js","function decode(bytes,c){if(c.apiVersion!==1)throw 'API';return {width:1,height:1,pixels:new Uint8Array([c.settings.red,0,0,255]).buffer};}"));
        QVERIFY(TestData::write(root+"/filter/manifest.json",R"({"id":"ui-filter","kind":"filter","apiVersion":1,"script":"filter.js"})"));
        QVERIFY(TestData::write(root+"/filter/filter.js","function filter(image,c){if(c.apiVersion!==1)throw 'API';return [{op:'invert'}];}"));
        const auto config=d.filePath("settings.ini");const auto fingerprint=scriptReaderFingerprint();
        MainWindow w(config);w.show();
        int restartNotices=0;
        QTimer dismissNotice;
        connect(&dismissNotice,&QTimer::timeout,&w,[&] {
            if(auto *notice=qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                if(notice->objectName()=="scriptRestartNotice")++restartNotices;
                notice->accept();
            }
        });
        dismissNotice.start(10);
        const auto edit=[&](bool save) {
            QTimer::singleShot(0,&w,[&,save] {
                auto *dialog=w.findChild<QDialog *>("preferencesDialog");QVERIFY(dialog);QTimer::singleShot(2000,dialog,&QDialog::reject);
                dialog->findChild<QLineEdit *>("scriptPackagePath")->setText(root);
                dialog->findChild<QPushButton *>("addScriptDirectory")->click();
                QCOMPARE(dialog->findChild<QLabel *>("scriptRegistryError")->text(),QString{});
                auto *packages=dialog->findChild<QListWidget *>("scriptPackages");QVERIFY(packages);
                for(int row=0;row<packages->count();++row)
                    if(packages->item(row)->data(Qt::UserRole).toMap().value("id")=="ui-reader")packages->setCurrentRow(row);
                auto *red=dialog->findChild<QDoubleSpinBox *>("scripts/ui-reader/red");QVERIFY(red);red->setValue(70);
                auto *filter=dialog->findChild<QComboBox *>("scripts/filter");QVERIFY(filter);filter->setCurrentIndex(filter->findData("ui-filter"));
                const auto capture=qEnvironmentVariable("KOMAMIMIC_UI_CAPTURE");
                if(save && !capture.isEmpty()) {
                    auto *tabs=dialog->findChild<QTabWidget *>();
                    for(int i=0;i<tabs->count();++i)if(tabs->widget(i)->property("settingsPageId")=="scripts")tabs->setCurrentIndex(i);
                    QCoreApplication::processEvents();QVERIFY(dialog->grab().save(capture));
                }
                if(save)dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();else dialog->reject();
            });
            w.command("preferences")->trigger();
        };
        edit(false);
        QCOMPARE(restartNotices,0);
        {QSettings settings(config,QSettings::IniFormat);QVERIFY(!settings.contains("scripts/directories"));QVERIFY(!settings.contains("scripts/ui-reader/red"));}
        QCOMPARE(scriptReaderFingerprint(),fingerprint);
        edit(true);
        QCOMPARE(restartNotices,1);
        {QSettings settings(config,QSettings::IniFormat);QCOMPARE(settings.value("scripts/directories").toStringList(),QStringList{root});
            QCOMPARE(settings.value("scripts/ui-reader/red").toInt(),70);QCOMPARE(settings.value("scripts/filter").toString(),QString("ui-filter"));}
        QCOMPARE(scriptReaderFingerprint(),fingerprint);w.close();
        // A real new process must load saved registration before listing formats.
        QProcess process;process.start(QString::fromUtf8(KOMAMIMIC_EXECUTABLE),{"--config",config,"--formats"});
        QVERIFY(process.waitForStarted(5000));QVERIFY(process.waitForFinished(10000));QCOMPARE(process.exitCode(),0);
        QVERIFY2(process.readAllStandardOutput().contains("uiimg"),process.readAllStandardError().constData());
        const auto cleanup=qScopeGuard([]{configureScriptReaders({});});
        QVERIFY(configureScriptReaders({root}).isEmpty());bool recognized=false;
        const auto image=decodeScriptImage("UIP","test.uiimg",{},recognized,true,0,{{"ui-reader",QVariantMap{{"red",70}}}});
        QVERIFY(recognized);QVERIFY2(image.error.isEmpty(),qPrintable(image.error));QCOMPARE(image.image.pixelColor(0,0),QColor(70,0,0));
        const auto filtered=applyScriptFilter("ui-filter",image.image);QVERIFY(filtered.error.isEmpty());QCOMPARE(filtered.image.pixelColor(0,0),QColor(185,255,255));
        MainWindow remove(config);
        const auto removeRegistration=[&](bool save) {
            QTimer::singleShot(0,&remove,[&,save] {
                auto *dialog=remove.findChild<QDialog *>("preferencesDialog");QVERIFY(dialog);QTimer::singleShot(2000,dialog,&QDialog::reject);
                // Failed additions cannot alter the saved list or active registry.
                auto *directories=dialog->findChild<QListWidget *>("scriptDirectories");QVERIFY(directories);
                dialog->findChild<QLineEdit *>("scriptPackagePath")->setText(d.filePath("missing"));
                dialog->findChild<QPushButton *>("addScriptDirectory")->click();QCOMPARE(directories->count(),1);
                QVERIFY(!dialog->findChild<QLabel *>("scriptRegistryError")->text().isEmpty());
                directories->setCurrentRow(0);dialog->findChild<QPushButton *>("removeScriptDirectory")->click();
                QCOMPARE(directories->count(),0);
                if(save)dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();else dialog->reject();
            });
            remove.command("preferences")->trigger();
        };
        removeRegistration(false);
        QCOMPARE(restartNotices,1);
        {QSettings settings(config,QSettings::IniFormat);QCOMPARE(settings.value("scripts/directories").toStringList(),QStringList{root});}
        removeRegistration(true);remove.close();
        QCOMPARE(restartNotices,2);
        {QSettings settings(config,QSettings::IniFormat);QVERIFY(settings.value("scripts/directories").toStringList().isEmpty());}
        process.start(QString::fromUtf8(KOMAMIMIC_EXECUTABLE),{"--config",config,"--formats"});
        QVERIFY(process.waitForStarted(5000));QVERIFY(process.waitForFinished(10000));QCOMPARE(process.exitCode(),0);
        QVERIFY(!process.readAllStandardOutput().contains("uiimg"));
    }
    void scriptRegistrationRestartNotice_data() {
        QTest::addColumn<bool>("registered");
        QTest::addColumn<QString>("operation");
        QTest::addColumn<bool>("save");
        QTest::addColumn<int>("expectedNotices");
        QTest::addColumn<QString>("language");
        QTest::newRow("add") << false << QString("add") << true << 1 << QString("ja");
        QTest::newRow("remove") << true << QString("remove") << true << 1 << QString("ja");
        QTest::newRow("cancel-add") << false << QString("add") << false << 0 << QString("ja");
        QTest::newRow("cancel-remove") << true << QString("remove") << false << 0 << QString("ja");
        QTest::newRow("unchanged") << true << QString{} << true << 0 << QString("ja");
        QTest::newRow("parameters-only") << true << QString("parameters") << true << 0 << QString("ja");
        QTest::newRow("add-then-remove") << false << QString("add-remove") << true << 0 << QString("ja");
        QTest::newRow("remove-then-add") << true << QString("remove-add") << true << 0 << QString("ja");
        QTest::newRow("registration-and-language") << false << QString("add-language") << true << 1 << QString("ja");
        QTest::newRow("english") << false << QString("add") << true << 1 << QString("en");
    }
    void scriptRegistrationRestartNotice() {
        QFETCH(bool,registered);QFETCH(QString,operation);QFETCH(bool,save);
        QFETCH(int,expectedNotices);QFETCH(QString,language);
        const auto previousLanguage=I18n::currentLanguage();
        const auto restoreLanguage=qScopeGuard([&]{I18n::initialize(previousLanguage);});
        I18n::initialize(language);
        QTemporaryDir d;QVERIFY(d.isValid());
        const auto package=d.filePath("script"),config=d.filePath("settings.ini");
        QVERIFY(QDir().mkpath(package));
        QVERIFY(TestData::write(package+"/manifest.json",R"({"id":"notice-filter","kind":"filter","apiVersion":1,"script":"filter.js","settings":{"amount":{"type":"integer","minimum":0,"maximum":10,"default":1}}})"));
        QVERIFY(TestData::write(package+"/filter.js","function filter(image,c){return [];}"));
        {QSettings settings(config,QSettings::IniFormat);
            settings.setValue("ui/language",language);
            if(registered)settings.setValue("scripts/directories",QStringList{package});}
        MainWindow w(config);w.show();
        QStringList names,texts;bool savedBeforeNotice=true;
        QTimer dismissNotice;
        connect(&dismissNotice,&QTimer::timeout,&w,[&] {
            if(auto *notice=qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                names.append(notice->objectName());texts.append(notice->text());
                const auto saved=QSettings(config,QSettings::IniFormat).value("scripts/directories").toStringList();
                savedBeforeNotice &= saved==(operation=="remove"?QStringList{}:QStringList{package});
                notice->accept();
            }
        });
        dismissNotice.start(10);
        QTimer::singleShot(0,&w,[&] {
            auto *dialog=w.findChild<QDialog *>("preferencesDialog");QVERIFY(dialog);
            QTimer::singleShot(2000,dialog,&QDialog::reject);
            auto *directories=dialog->findChild<QListWidget *>("scriptDirectories");QVERIFY(directories);
            const auto add=[&] {
                dialog->findChild<QLineEdit *>("scriptPackagePath")->setText(package);
                dialog->findChild<QPushButton *>("addScriptDirectory")->click();
            };
            const auto remove=[&] {
                directories->setCurrentRow(0);
                dialog->findChild<QPushButton *>("removeScriptDirectory")->click();
            };
            if(operation.startsWith("add"))add();
            if(operation.startsWith("remove"))remove();
            if(operation=="add-remove")remove();
            if(operation=="remove-add")add();
            if(operation=="parameters") {
                auto *packages=dialog->findChild<QListWidget *>("scriptPackages");QVERIFY(packages);
                for(int i=0;i<packages->count();++i)
                    if(packages->item(i)->data(Qt::UserRole).toMap().value("id")=="notice-filter")packages->setCurrentRow(i);
                auto *amount=dialog->findChild<QDoubleSpinBox *>("scripts/notice-filter/amount");QVERIFY(amount);amount->setValue(2);
            }
            if(operation=="add-language") {
                auto *choice=dialog->findChild<QComboBox *>("ui/language");QVERIFY(choice);
                choice->setCurrentIndex(choice->findData("en"));
            }
            if(save)dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();else dialog->reject();
        });
        w.command("preferences")->trigger();
        QCOMPARE(names.size(),expectedNotices);
        if(expectedNotices) {
            QCOMPARE(names,QStringList{"scriptRestartNotice"});QVERIFY(savedBeforeNotice);
            QVERIFY(texts.first().contains(language=="en"?"restart KomaMimic":"起動し直してください"));
            if(operation=="add-language")QVERIFY(texts.first().contains("表示言語"));
        }
        if(operation=="parameters")QCOMPARE(QSettings(config,QSettings::IniFormat).value("scripts/notice-filter/amount").toInt(),2);
        w.close();
    }
    void adjacentArchivesKeepPageThumbnailMode() {
        for (bool playlist : {false,true}) {
            QTemporaryDir d; QStringList paths;
            for(int book=1;book<=3;++book) {
                const auto path=d.filePath(QString("%1.zip").arg(book));paths<<path;
                QVector<TestData::ZipEntry> entries;
                for(int page=0;page<book+1;++page) entries.append({QString("%1.png").arg(page).toUtf8(),TestData::png(TestData::solid(QColor(book*60,page*40,0)))});
                QVERIFY(TestData::write(path,TestData::zip(entries)));
            }
            QString first;
            if(playlist) {LibraryStore db(d.filePath("library.sqlite3"));first=db.add("playlist",paths[0]);QVERIFY(!db.add("playlist",paths[2]).isEmpty());}
            MainWindow w(d.filePath("settings.ini"));w.show();TestData::singlePage(w);
            if(playlist) {
                ReaderFeatures *features=nullptr;for(auto *child:w.children())if(auto *candidate=dynamic_cast<ReaderFeatures *>(child))features=candidate;
                QVERIFY(features);features->openCollection(first);
            } else w.openBook(paths[0]);
            QTRY_VERIFY(w.renderReady());w.showPageThumbnails();QCOMPARE(w.viewMode(),MainWindow::ViewMode::Pages);
            auto *grid=w.findChild<ThumbnailGrid *>("pageThumbnails");QVERIFY(grid);QCOMPARE(grid->count(),2);
            QTRY_VERIFY(w.command("nextBook")->isEnabled());w.command("nextBook")->trigger();QTRY_COMPARE(w.bookPath(),paths[playlist?2:1]);QTRY_VERIFY(w.requestReady());
            QCOMPARE(w.viewMode(),MainWindow::ViewMode::Pages);QCOMPARE(grid->count(),playlist?4:3);
            QTRY_VERIFY(w.command("previousBook")->isEnabled());w.command("previousBook")->trigger();QTRY_COMPARE(w.bookPath(),paths[0]);QTRY_VERIFY(w.requestReady());
            QCOMPARE(w.viewMode(),MainWindow::ViewMode::Pages);QCOMPARE(grid->count(),2);
            w.command("thumbCancel")->trigger();QCOMPARE(w.viewMode(),MainWindow::ViewMode::Reader);
            QTRY_VERIFY(w.command("nextBook")->isEnabled());w.command("nextBook")->trigger();QTRY_COMPARE(w.bookPath(),paths[playlist?2:1]);QTRY_VERIFY(w.requestReady());
            QCOMPARE(w.viewMode(),MainWindow::ViewMode::Reader);w.close();
        }
    }
    void toolbarDefaultsAndPersistence() {
        QTemporaryDir d;
        // Obsolete upper-bar keys remain harmless in an existing profile.
        { QSettings old(d.filePath("settings.ini"),QSettings::IniFormat);
          old.setValue("window/directoryToolbar",true);old.setValue("keys/focusPath","Ctrl+L"); }
        {
            MainWindow w(d.filePath("settings.ini")); w.show();
            auto *bottom=w.findChild<QToolBar *>("readerToolbar");QVERIFY(bottom);
            QCOMPARE(w.toolBarArea(bottom),Qt::BottomToolBarArea);
            QCOMPARE(w.findChildren<QToolBar *>().size(),1);
            QVERIFY(!w.findChild<QToolBar *>("directoryToolbar"));
            QVERIFY(!w.findChild<QLineEdit *>("directoryPath"));
            QVERIFY(!w.command("locationToolbarToggle"));QVERIFY(!w.command("focusPath"));
            QVERIFY(bottom->isHidden());w.command("toolbarToggle")->trigger();QVERIFY(bottom->isVisible());
            for(const auto *id:{"documentSettings","findDocument","folderBack","parentFolder"})QVERIFY(w.command(id));
            w.close();
        }
        MainWindow restored(d.filePath("settings.ini"));restored.show();
        QVERIFY(restored.findChild<QToolBar *>("readerToolbar")->isVisible());
        QVERIFY(restored.command("toolbarToggle")->isChecked());restored.close();
    }
    void toolbarPreferencesApplyAndCancel() {
        QTemporaryDir d; MainWindow w(d.filePath("settings.ini")); w.show();
        const auto edit = [&w](bool accept) {
            QTimer::singleShot(0, &w, [&w, accept] {
                auto *dialog = w.findChild<QDialog *>("preferencesDialog");
                if (!dialog) return;
                for (const auto &key : {"window/readerToolbar"})
                    dialog->findChild<QCheckBox *>(key)->setChecked(true);
                if (accept) dialog->accept(); else dialog->reject();
            });
            w.command("preferences")->trigger();
        };
        edit(false);
        for (const auto &id : {"readerToolbar"})
            QVERIFY(w.findChild<QToolBar *>(id)->isHidden());
        edit(true);
        for (const auto &id : {"readerToolbar"})
            QVERIFY(w.findChild<QToolBar *>(id)->isVisible());
        w.close();
    }
    void restoreAndActivateBringsWindowForward_data() {
        QTest::addColumn<bool>("minimized");
        QTest::newRow("background-visible") << false;
        QTest::newRow("background-minimized") << true;
    }

    void restoreAndActivateBringsWindowForward() {
        QFETCH(bool, minimized);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        MainWindow primary(directory.filePath("primary.ini"));
        primary.resize(640, 480);
        primary.show();
        primary.activateWindow();
        QTRY_VERIFY(primary.isActiveWindow());

        if (minimized) {
            primary.showMinimized();
            QTRY_VERIFY(primary.isMinimized());
        }

        QWidget foreground;
        foreground.resize(300, 200);
        foreground.show();
        foreground.activateWindow();
        QTRY_VERIFY(foreground.isActiveWindow());

        primary.restoreAndActivate();
        QTRY_VERIFY(primary.isVisible() && !primary.isMinimized());
        QTRY_VERIFY(primary.isActiveWindow());
        QVERIFY(!foreground.isActiveWindow());
        foreground.close();
        primary.close();
    }

    void restoreAndActivateKeepsWindowAndReadingState_data() {
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("minimized");
        for (int mode=0;mode<4;++mode) {
            const QString name=QStringList{"normal","maximized","fullscreen","fullscreen-from-maximized"}[mode];
            QTest::newRow(qPrintable(name+"-visible")) << mode << false;
            QTest::newRow(qPrintable(name+"-minimized")) << mode << true;
        }
    }
    void restoreAndActivateKeepsWindowAndReadingState() {
        QFETCH(int,mode); QFETCH(bool,minimized);
        QTemporaryDir d;const auto images=d.filePath("images");
        QVERIFY(TestData::imageFolder(images,3));
        MainWindow w(d.filePath("settings.ini"));w.resize(640,480);w.show();
        TestData::singlePage(w);w.openBook(images+"/1.png");QTRY_VERIFY(w.renderReady());
        w.nextPage();QTRY_COMPARE(w.pageIndex(),1);QTRY_VERIFY(w.renderReady());
        const auto book=w.bookPath();const auto normalSize=w.size();
        if(mode==1 || mode==3) {w.showMaximized();QTRY_VERIFY(w.isMaximized());}
        if(mode>=2) {w.setFullscreen(true);QTRY_VERIFY(w.isFullScreen());}
        if(minimized) {w.command("minimize")->trigger();QTRY_VERIFY(w.isMinimized());}
        w.restoreAndActivate();
        QTRY_VERIFY(w.isVisible() && !w.isMinimized());
        QCOMPARE(w.isFullScreen(),mode>=2);
        if(mode<2)QCOMPARE(w.isMaximized(),mode==1);
        if(mode==0)QCOMPARE(w.size(),normalSize);
        QCOMPARE(w.bookPath(),book);QCOMPARE(w.pageIndex(),1);
        QCOMPARE(w.viewMode(),MainWindow::ViewMode::Reader);
        if(mode>=2) {
            QSettings settings(d.filePath("settings.ini"),QSettings::IniFormat);
            QVERIFY(settings.value("window/fullscreen").toBool());
            w.setFullscreen(false);QTRY_VERIFY(!w.isFullScreen());
            QCOMPARE(w.isMaximized(),mode==3);
        }
        w.close();
    }
    void fullscreenPersistenceAndNormalWindow_data() {
        QTest::addColumn<bool>("native");
        QTest::addColumn<bool>("maximized");
        QTest::newRow("command-normal") << false << false;
        QTest::newRow("command-maximized") << false << true;
        QTest::newRow("native-normal") << true << false;
        QTest::newRow("native-maximized") << true << true;
    }
    void fullscreenPersistenceAndNormalWindow() {
        QFETCH(bool,native); QFETCH(bool,maximized);
        QTemporaryDir d; const auto config=d.filePath("settings.ini");
        { QSettings settings(config,QSettings::IniFormat);settings.setValue("window/readerToolbar",true); }
        QSize normalSize; QByteArray geometry;
        {
            MainWindow w(config);w.resize(640,480);w.show();
            QVERIFY(!w.isFullScreen());
            if(maximized)w.showMaximized();
            QCoreApplication::processEvents();normalSize=w.size();
            w.command("boundaryStop")->trigger();
            { QSettings settings(config,QSettings::IniFormat);geometry=settings.value("window/geometry").toByteArray(); }
            QVERIFY(!geometry.isEmpty());
            if(native)w.showFullScreen();else w.command("fullscreenGlobal")->trigger();
            QTRY_VERIFY(w.isFullScreen());
            { QSettings settings(config,QSettings::IniFormat);QVERIFY(settings.value("window/fullscreen").toBool()); }
            w.close();
            QSettings settings(config,QSettings::IniFormat);
            QVERIFY(settings.value("window/fullscreen").toBool());
            QCOMPARE(settings.value("window/geometry").toByteArray(),geometry);
        }
        {
            MainWindow restored(config);QVERIFY(!restored.isVisible());restored.show();
            QTRY_VERIFY(restored.isFullScreen());
            restored.setFullscreen(false);QTRY_VERIFY(!restored.isFullScreen());
            QCOMPARE(restored.isMaximized(),maximized);
            if(!maximized)QTRY_COMPARE(restored.size(),normalSize);
            QVERIFY(restored.findChild<QToolBar *>("readerToolbar")->isVisible());
            QVERIFY(restored.findChild<FolderBrowser *>()->isVisible());
            restored.close();
        }
        MainWindow normal(config);normal.show();QVERIFY(!normal.isFullScreen());normal.close();
        QSettings settings(config,QSettings::IniFormat);QVERIFY(!settings.value("window/fullscreen").toBool());
    }
    void fullscreenPreferencesApplyAndCancel() {
        QTemporaryDir d;const auto config=d.filePath("settings.ini");MainWindow w(config);w.show();
        const auto edit=[&](bool target,bool accept) {
            const bool before=w.isFullScreen();bool inspected=false;
            QTimer::singleShot(0,&w,[&] {
                auto *dialog=w.findChild<QDialog *>("preferencesDialog");QVERIFY(dialog);
                QTimer::singleShot(2000,dialog,&QDialog::reject);
                auto *box=dialog->findChild<QCheckBox *>("window/fullscreen");
                if(!box){dialog->reject();QFAIL("Missing fullscreen preference");}
                QCOMPARE(box->isChecked(),before);inspected=true;box->setChecked(target);
                if(accept)dialog->accept();else dialog->reject();
            });
            w.command("preferences")->trigger();QVERIFY(inspected);
            QCOMPARE(w.isFullScreen(),accept?target:before);
            QSettings settings(config,QSettings::IniFormat);
            QCOMPARE(settings.value("window/fullscreen",false).toBool(),accept?target:before);
        };
        edit(true,false);edit(true,true);edit(false,false);edit(false,true);
        w.command("fullscreenGlobal")->trigger();edit(false,false);
        w.close();
    }
    void fullscreenSurvivesProcessRestart() {
        QTemporaryDir d;const auto config=d.filePath("settings.ini");
        {
            MainWindow w(config);w.show();w.setFullscreen(true);w.close();
        }
        for(const auto &expected:{QByteArray("fullscreen"),QByteArray("normal")}) {
            QProcess child;
            child.start(QCoreApplication::applicationFilePath(),{"--probe-fullscreen",config});
            QVERIFY(child.waitForFinished(15000));QCOMPARE(child.exitStatus(),QProcess::NormalExit);QCOMPARE(child.exitCode(),0);
            QCOMPARE(child.readAllStandardOutput().trimmed(),expected);
        }
    }
    void fullscreenStartupRequestsKeepCurrentState() {
        QTemporaryDir d;const auto config=d.filePath("settings.ini");
        {QSettings settings(config,QSettings::IniFormat);settings.setValue("window/fullscreen",true);}
        {
            MainWindow w(config);
            // Saving another preference before show must retain startup restoration.
            w.command("boundaryStop")->trigger();w.show();QTRY_VERIFY(w.isFullScreen());
            w.runRequest({{"fullscreen",false},{"commands",QJsonArray{"toolbarToggle"}}});
            QTRY_VERIFY(w.command("toolbarToggle")->isChecked());QVERIFY(w.isFullScreen());
            w.setFullscreen(false);w.hide();w.show();QVERIFY(!w.isFullScreen());
            w.runRequest({{"fullscreen",true},{"commands",QJsonArray{"toolbarToggle"}}});
            QTRY_VERIFY(!w.command("toolbarToggle")->isChecked());QVERIFY(w.isFullScreen());w.close();
        }
        MainWindow overridden(config);overridden.setFullscreen(false);overridden.show();
        QVERIFY(!overridden.isFullScreen());overridden.close();
        QSettings settings(config,QSettings::IniFormat);QVERIFY(!settings.value("window/fullscreen").toBool());
    }
    void fullscreenChromeAtSeparateEdges() {
        QTemporaryDir d;
        { QSettings s(d.filePath("settings.ini"), QSettings::IniFormat); s.setValue("window/readerToolbar", true); }
        MainWindow w(d.filePath("settings.ini")); w.show(); w.activateWindow();
        auto *bottom = w.findChild<QToolBar *>("readerToolbar"); QVERIFY(bottom);
        const auto move = [&w](QPoint p) {
            QMouseEvent event(QEvent::MouseMove, QPointF(p), QPointF(w.mapToGlobal(p)),
                              Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(&w, &event);
        };
        w.setFullscreen(true); QTRY_VERIFY(w.isFullScreen());
        move(w.rect().center()); QTRY_VERIFY(w.menuBar()->isHidden() && bottom->isHidden());
        move({w.width()/2, 1}); QVERIFY(w.menuBar()->isVisible()); QVERIFY(bottom->isHidden());
        move(w.rect().center()); QTRY_VERIFY(w.menuBar()->isHidden());
        move({w.width()/2, w.height()-1}); QVERIFY(bottom->isVisible()); QVERIFY(w.menuBar()->isHidden());
        move(bottom->geometry().center()); QTest::qWait(400); QVERIFY(bottom->isVisible());
        // A menu must retain the controls even if the pointer has left them.
        auto *popup = w.findChild<QMenu *>("menuView"); popup->popup(w.mapToGlobal(w.rect().center()));
        move(w.rect().center()); QTest::qWait(400); QVERIFY(bottom->isVisible()); popup->hide();
        move(w.rect().center()); QTRY_VERIFY(bottom->isHidden());
        QVERIFY(w.command("toolbarToggle")->isChecked());
        w.setFullscreen(false); QVERIFY(bottom->isVisible());
        w.close();
    }
    void fullscreenMarginsWithoutUpperBar() {
        QTemporaryDir d;
        {QSettings settings(d.filePath("settings.ini"),QSettings::IniFormat);settings.setValue("window/readerToolbar",true);}
        MainWindow w(d.filePath("settings.ini"));const QMargins margins(2,3,4,5);w.setContentsMargins(margins);w.show();
        auto *bottom=w.findChild<QToolBar *>("readerToolbar");QVERIFY(bottom);
        for(bool native:{false,true}) {
            if(native)w.showFullScreen();else w.setFullscreen(true);
            QTRY_VERIFY(w.isFullScreen());QCOMPARE(w.contentsMargins(),margins);
            QCOMPARE(w.findChildren<QToolBar *>().size(),1);
            if(native)w.showNormal();else w.setFullscreen(false);
            QTRY_VERIFY(!w.isFullScreen());QCOMPARE(w.contentsMargins(),margins);QVERIFY(bottom->isVisible());
        }
        w.close();
    }
    void fullscreenToolbarPolicies() {
        for (int mode : {0, 1, 2}) {
            QTemporaryDir d;
            { QSettings s(d.filePath("settings.ini"), QSettings::IniFormat);
                for (const auto &key : {"window/readerToolbar"})
                    s.setValue(key, true);
                s.setValue("fullscreen/menu", mode); s.setValue("fullscreen/toolbar", mode); }
            MainWindow w(d.filePath("settings.ini")); w.show(); w.setFullscreen(true);
            auto *bottom = w.findChild<QToolBar *>("readerToolbar");
            QCOMPARE(bottom->isVisible(), mode == 1);
            const auto move = [&w](QPoint p) {
                QMouseEvent event(QEvent::MouseMove, QPointF(p), QPointF(w.mapToGlobal(p)),
                                  Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(&w, &event);
            };
            move({w.width()/2, 1});
            QCOMPARE(w.menuBar()->isVisible(), mode != 2);
            QCOMPARE(bottom->isVisible(), mode == 1);
            move({w.width()/2, w.height()-1}); QCOMPARE(bottom->isVisible(), mode != 2);
            for (const auto &id : {"toolbarToggle"})
                QVERIFY(w.command(id)->isChecked());
            w.setFullscreen(false);
            QVERIFY(bottom->isVisible());
            w.close();
        }
    }
    void nativeApplicationMenuRoles() {
        QTemporaryDir d; MainWindow w(d.filePath("settings.ini")); w.show();
        QCOMPARE(w.command("preferences")->menuRole(),QAction::PreferencesRole);
        QCOMPARE(w.command("about")->menuRole(),QAction::AboutRole);
        QCOMPARE(w.command("quit")->menuRole(),QAction::QuitRole);
        for (const auto &spec : shortcutSpecs())
            if (spec.id!="preferences" && spec.id!="about" && spec.id!="quit")
                QCOMPARE(w.command(spec.id)->menuRole(),QAction::NoRole);
        QVERIFY(!w.command("preferences")->menu());
        auto *file=w.findChild<QMenu *>("menuFile"); QVERIFY(file);
        const int importIndex=file->actions().indexOf(w.command("importMangaMeeyaSettings"));
        QVERIFY(importIndex>=0);
        QCOMPARE(file->actions().at(importIndex+1),w.command("importSettings"));
        QCOMPARE(file->actions().at(importIndex+2),w.command("exportSettings"));
        for (auto *menu : w.findChildren<QMenu *>()) {
            if (menu->objectName().startsWith("menu"))
                QCOMPARE(menu->menuAction()->menuRole(),QAction::NoRole);
        }
        auto *tools=w.findChild<QMenu *>("menuTools"); QVERIFY(tools);
        auto *slotMenu=w.findChild<QMenu *>("menuSettingsSlots"); QVERIFY(slotMenu);
        auto *loads=w.findChild<QMenu *>("menuSettingsLoad"); QVERIFY(loads);
        auto *saves=w.findChild<QMenu *>("menuSettingsSave"); QVERIFY(saves);
        QVERIFY(tools->actions().contains(slotMenu->menuAction()));
        QVERIFY(slotMenu->actions().contains(loads->menuAction()));
        QVERIFY(slotMenu->actions().contains(saves->menuAction()));
        QCOMPARE(loads->actions().size(),10); QCOMPARE(saves->actions().size(),10);
        bool opened=false;
        QTimer::singleShot(0,&w,[&] {
            auto *dialog=qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (dialog) { opened=true; dialog->reject(); }
        });
        w.command("preferences")->trigger(); QVERIFY(opened);
        w.close();
    }
    void mangaMeeyaMenuStructure() {
        QTemporaryDir d; MainWindow w(d.filePath("settings.ini"));
        QStringList titles;
        for (auto *action : w.menuBar()->actions()) titles << action->text();
        QCOMPARE(titles, QStringList({"ファイル(&F)", "表示(&V)", "スケールモード(&S)",
                                     "移動(&J)", "ブックリスト(&B)", "ツール(&T)", "ヘルプ(&H)"}));
        const auto commands = [](QMenu *menu) {
            QStringList ids;
            for (auto *action : menu->actions())
                if (action->objectName().startsWith("action_")) ids << action->objectName().mid(7);
            return ids;
        };
        auto *file = w.findChild<QMenu *>("menuFile"); QVERIFY(file);
        const auto fileIds = commands(file);
        QCOMPARE(fileIds.first(5), QStringList({"open", "folder", "openUrl", "external", "externalSettings"}));
        QCOMPARE(fileIds.last(), QString("quit"));
        QVERIFY(fileIds.indexOf("saveImage") < fileIds.indexOf("closeBook"));
        QVERIFY(fileIds.indexOf("pageSetup") < fileIds.indexOf("print"));
        auto *edit = w.findChild<QMenu *>("menuPageEdit"); QVERIFY(edit);
        QCOMPARE(commands(edit).first(2), QStringList({"editUndo", "editRedo"}));
        auto *move = w.findChild<QMenu *>("menuMove"); QVERIFY(move);
        const auto moveIds = commands(move);
        QCOMPARE(moveIds.first(8), QStringList({"next", "nextOne", "jumpNext10", "jumpNext20", "jumpNext50", "last", "nextFolder", "nextBook"}));
        QCOMPARE(moveIds.last(3), QStringList({"boundaryStop", "boundaryRepeat", "boundaryAdvance"}));
        auto *scale = w.findChild<QMenu *>("menuScale"); QVERIFY(scale);
        QCOMPARE(commands(scale).first(7), QStringList({"scaleFill", "scaleFit", "scaleActual", "fitWindow", "scaleEqualHeight", "scaleSpecified", "scaleFixed"}));
        auto *tools = w.findChild<QMenu *>("menuTools"); QVERIFY(tools);
        QVERIFY(tools->actions().contains(w.findChild<QMenu *>("menuImageProcessing")->menuAction()));
        auto *view = w.findChild<QMenu *>("menuView"); QVERIFY(view);
        QVERIFY(view->actions().contains(w.findChild<QMenu *>("menuDocument")->menuAction()));
        // Every previously exposed command remains reachable, including rarely
        // used profile/save slots. Tree key handling is local to its widget.
        QSet<QString> reachable;
        std::function<void(QMenu *)> collect = [&](QMenu *parent) {
            for (auto *action : parent->actions()) {
                if (action->menu()) collect(action->menu());
                else if (action->objectName().startsWith("action_")) reachable.insert(action->objectName().mid(7));
            }
        };
        for (auto *action : w.menuBar()->actions()) collect(action->menu());
        for (const auto &spec : shortcutSpecs()) {
            if (spec.scope == ShortcutScope::Tree || spec.id == "readerContextMenu") continue;
            QVERIFY2(reachable.contains(spec.id), qPrintable(spec.id));
        }
        w.close();
    }
    void menuChoicesShareReadingCommands() {
        QTemporaryDir d; MainWindow w(d.filePath("settings.ini"));
        auto *spread = w.findChild<QAction *>("action_spread"); QVERIFY(spread);
        auto *single = w.findChild<QAction *>("menu_spread_off"); QVERIFY(single);
        auto *doublePage = w.findChild<QAction *>("menu_spread_on"); QVERIFY(doublePage);
        QSignalSpy changed(spread, &QAction::toggled);
        single->trigger(); QVERIFY(!spread->isChecked()); QCOMPARE(changed.count(), 1);
        doublePage->trigger(); QVERIFY(spread->isChecked()); QCOMPARE(changed.count(), 2);
        spread->trigger(); QVERIFY(single->isChecked()); QVERIFY(!doublePage->isChecked());
        // Document synchronization blocks signals; opening the menu refreshes
        // the explicit choices from the same shared command state.
        { const QSignalBlocker blocker(spread); spread->setChecked(true); }
        QVERIFY(QMetaObject::invokeMethod(w.findChild<QMenu *>("menuView"), "aboutToShow"));
        QVERIFY(doublePage->isChecked()); QVERIFY(!single->isChecked());
        auto *rtl = w.findChild<QAction *>("action_rtl"); QVERIFY(rtl);
        w.findChild<QAction *>("menu_rtl_off")->trigger(); QVERIFY(!rtl->isChecked());
        rtl->trigger(); QVERIFY(w.findChild<QAction *>("menu_rtl_on")->isChecked());
        w.close();
    }
    void menuSettingsOpenRelevantTab() {
        QTemporaryDir d; MainWindow w(d.filePath("settings.ini"));
        for (const auto &[id, tab] : QList<QPair<QString, QString>>{
                 {"menuCacheSettings", "キャッシュ"}, {"menuAutoLayoutSettings", "自動組版"},
                 {"menuScriptSettings", "スクリプト"}, {"menuOriginSettings", "表示"}}) {
            auto *action = w.findChild<QAction *>(id); QVERIFY(action);
            QString selected;
            QTimer::singleShot(0, &w, [&] {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                if (!dialog) return;
                auto *tabs = dialog->findChild<QTabWidget *>();
                if (tabs) selected = tabs->tabText(tabs->currentIndex());
                dialog->reject();
            });
            action->trigger(); QCOMPARE(selected, tab);
        }
        w.close();
    }
    void optionalShortcutDefaultsAndSavedBindings() {
        const QHash<QString, QKeySequence> oldDefaults{
            {"archiveNames", QKeySequence("Ctrl+E")}, {"mediaPause", QKeySequence("P")},
            {"mediaStop", QKeySequence("Shift+P")}, {"focusTree", QKeySequence("Ctrl+Shift+T")},
            {"focusThumbnails", QKeySequence("Ctrl+Shift+L")}};
        QTemporaryDir d;
        {
            MainWindow w(d.filePath("settings.ini"));
            for (const auto &spec : shortcutSpecs()) if (oldDefaults.contains(spec.id)) {
                QVERIFY(spec.defaultKey.isEmpty()); QVERIFY(w.bindings().value(spec.id).isEmpty());
            }
            auto saved = w.bindings();
            for (auto it = oldDefaults.cbegin(); it != oldDefaults.cend(); ++it) saved[it.key()] = it.value();
            saved["thumbnails"]={}; // Ctrl+E is explicitly assigned to archiveNames in this saved profile.
            QVERIFY(w.applyBindings(saved)); w.close();
        }
        MainWindow restored(d.filePath("settings.ini"));
        for (auto it = oldDefaults.cbegin(); it != oldDefaults.cend(); ++it)
            QCOMPARE(restored.bindings().value(it.key()), it.value());
        restored.close();
    }
    void directoryChangesRefreshCovers() {
        QTemporaryDir d; const auto directory=d.filePath("images"), other=d.filePath("other");
        QVERIFY(TestData::imageFolder(directory)); QVERIFY(TestData::imageFolder(other,2));
        const auto archive=TestData::zip({{"1.png",TestData::png(TestData::solid())}});
        MainWindow w(d.filePath("settings.ini")); w.show(); w.activateWindow(); TestData::singlePage(w);
        w.openDirectory(directory);QTRY_VERIFY(w.directoryReady());
        auto *grid=w.findChild<ThumbnailGrid *>("zipLibrary"); QVERIFY(grid);
        grid->setLayoutMode(ThumbnailGrid::Layout::EightColumns);
        auto *watcher=w.findChild<QFileSystemWatcher *>("browserDirectoryWatcher"); QVERIFY(watcher);
        const auto paths=[grid] {
            QStringList result;
            for (int i=0;i<grid->count();++i) result.append(grid->item(i)->data(PathRole).toString());
            return result;
        };
        QCOMPARE(grid->count(),5); grid->setCurrentRow(3);
        const auto selected=grid->currentItem()->data(PathRole).toString();
        QVERIFY(TestData::write(directory+"/added.zip",archive));
        QTRY_COMPARE(grid->count(),6);
        QVERIFY(paths().contains(directory+"/added.zip"));
        QCOMPARE(grid->currentItem()->data(PathRole).toString(),selected);
        QCOMPARE(w.viewMode(),MainWindow::ViewMode::Covers); QVERIFY(w.bookPath().isEmpty());
        QVERIFY(QFile::rename(directory+"/added.zip",directory+"/renamed.zip"));
        QTRY_VERIFY(paths().contains(directory+"/renamed.zip"));
        QVERIFY(!paths().contains(directory+"/added.zip"));
        QVERIFY(QFile::remove(directory+"/renamed.zip")); QTRY_COMPARE(grid->count(),5);
        QCOMPARE(grid->currentItem()->data(PathRole).toString(),selected);
        // F6 must also rescan when a filesystem cannot deliver notifications.
        QVERIFY(watcher->removePath(directory));
        QVERIFY(TestData::write(directory+"/manual.zip",archive));
        grid->setFocus(); QTRY_VERIFY(grid->hasFocus());
        QTest::keyClick(grid,Qt::Key_F6); QTRY_VERIFY(w.directoryReady());QTRY_COMPARE(grid->count(),6);
        QVERIFY(paths().contains(directory+"/manual.zip"));
        QCOMPARE(grid->currentItem()->data(PathRole).toString(),selected);
        w.openBook(directory+"/1.png"); QTRY_VERIFY(w.requestReady());
        w.nextPage(); QTRY_VERIFY(w.renderReady());
        const auto book=w.bookPath(); const int page=w.pageIndex();
        QVERIFY(TestData::write(directory+"/reader.zip",archive)); QTRY_COMPARE(grid->count(),7);
        QCOMPARE(w.viewMode(),MainWindow::ViewMode::Reader);
        QCOMPARE(w.bookPath(),book); QCOMPARE(w.pageIndex(),page);
        auto *view=w.findChild<PageView *>(); QVERIFY(view); QVERIFY(view->hasFocus());
        w.showPageThumbnails(); QCOMPARE(w.viewMode(),MainWindow::ViewMode::Pages);
        auto *pages=w.findChild<ThumbnailGrid *>("pageThumbnails"); QVERIFY(pages);
        const int count=pages->count(), row=pages->currentRow();
        QVERIFY(TestData::write(directory+"/pages.zip",archive)); QTRY_COMPARE(grid->count(),8);
        QCOMPARE(w.viewMode(),MainWindow::ViewMode::Pages);
        QCOMPARE(pages->count(),count); QCOMPARE(pages->currentRow(),row); QVERIFY(pages->hasFocus());
        QCOMPARE(w.bookPath(),book); QCOMPARE(w.pageIndex(),page);
        // Discard pending notifications from the old folder when navigating.
        QVERIFY(TestData::write(directory+"/old.zip",archive));
        w.openDirectory(other);QTRY_VERIFY(w.directoryReady()); QCOMPARE(grid->count(),2);
        QCOMPARE(watcher->directories(),QStringList{other});
        QVERIFY(TestData::write(other+"/new.zip",archive)); QTRY_COMPARE(grid->count(),3);
        QVERIFY(paths().contains(other+"/new.zip"));
        QVERIFY(!paths().contains(directory+"/old.zip"));
        QCOMPARE(w.directory(),other); QCOMPARE(w.viewMode(),MainWindow::ViewMode::Covers);
        QVERIFY(TestData::imageFolder(other,40)); QTRY_COMPARE(grid->count(),41);
        grid->setCurrentRow(3);
        // A pending selection/layout update can scroll the list back to the
        // selected item after setValue() on macOS. Drain it before testing
        // whether a directory refresh preserves an explicitly set position.
        grid->doItemsLayout();
        QCoreApplication::processEvents();
        grid->doItemsLayout();
        const auto scrollSelection=grid->currentItem()->data(PathRole).toString();
        auto *bar=grid->verticalScrollBar(); QTRY_VERIFY(bar->maximum()>100);
        bar->setValue(bar->maximum()/2); const int scroll=bar->value();
        QVERIFY(scroll>0);
        QVERIFY(TestData::write(other+"/scroll.zip",archive)); QTRY_COMPARE(grid->count(),42);
        QCoreApplication::processEvents();
        // The scrollbar may be restored by a deferred zero-timeout timer.
        QTRY_COMPARE_WITH_TIMEOUT(bar->value(), scroll, 10000);
        QCOMPARE(grid->currentItem()->data(PathRole).toString(),scrollSelection);
        w.close();
    }
    void directoryRefreshPreservesEditingAndStopsAfterSettingsWrites() {
        QTemporaryDir d;
        const auto config=d.filePath("settings.ini");
        MainWindow w(config); w.show(); w.activateWindow(); w.openDirectory(d.path());QTRY_VERIFY(w.directoryReady());
        auto *grid=w.findChild<ThumbnailGrid *>("zipLibrary"); QVERIFY(grid);
        auto *field=new QLineEdit(&w); field->setGeometry(20,40,240,30); field->show(); field->setFocus();
        QTRY_VERIFY(field->hasFocus());
        field->setText("unfinished folder name"); field->setCursorPosition(4);
        auto *tree=w.findChild<FolderTree *>("folderTree"); QVERIFY(tree);
        auto *browser=w.findChild<FolderBrowser *>("folderTreePanel");QVERIFY(browser);
        browser->openLocation(QFileInfo(d.path()).absolutePath());
        QTRY_VERIFY(!tree->selectedPath().isEmpty());
        const auto treeSelection=tree->selectedPath();
        const auto archive=TestData::zip({{"1.png",TestData::png(TestData::solid())}});
        QVERIFY(TestData::write(d.filePath("new.zip"),archive)); QTRY_COMPARE(grid->count(),1);
        QCOMPARE(field->text(),QString("unfinished folder name")); QCOMPARE(field->cursorPosition(),4);
        QVERIFY(field->hasFocus()); QCOMPARE(w.viewMode(),MainWindow::ViewMode::Covers);
        QCOMPARE(tree->selectedPath(),treeSelection);
        // Refreshing a directory containing settings must not create an INI
        // write -> notification -> refresh -> INI write feedback loop.
        QSignalSpy resets(grid->model(),&QAbstractItemModel::modelReset);
        QTest::qWait(600); resets.clear(); QTest::qWait(600);
        QCOMPARE(resets.size(),0);
        w.close();
    }
    void thumbnailShortcutScopesFollowFocus() {
        QTemporaryDir d;
        const auto images=d.filePath("images"), books=d.filePath("books");
        QVERIFY(TestData::imageFolder(images));
        QVERIFY(QDir().mkpath(books));
        for (int i=0;i<2;++i)
            QVERIFY(TestData::write(books+QString("/%1.zip").arg(i),
                TestData::zip({{"1.png",TestData::png(TestData::solid())}})));
        const auto config=d.filePath("settings.ini");
        MainWindow w(config); w.show(); w.activateWindow(); TestData::singlePage(w);
        w.openBook(images+"/1.png"); QTRY_VERIFY(w.requestReady());
        QTRY_VERIFY(w.isActiveWindow());
        const auto originalPath=w.bookPath();
        for (const auto &directory : {images,books}) {
            w.openDirectory(directory);QTRY_VERIFY(w.directoryReady());
            auto *grid=w.findChild<ThumbnailGrid *>("zipLibrary"); QVERIFY(grid);
            QTRY_VERIFY(grid->hasFocus());
            QCOMPARE(w.viewMode(),MainWindow::ViewMode::Covers);
            // Native menu accelerators must not expose reader keys in covers.
            for (const auto *id : {"next","previous","nextOne","previousOne","first","last","treeRight"})
                QVERIFY2(w.command(id)->shortcut().isEmpty(),id);
            QCOMPARE(w.command("thumbRight")->shortcut(),QKeySequence("Right"));
            QTest::keyClick(grid,Qt::Key_Right); QCOMPARE(grid->currentRow(),1);
            QTest::keyClick(grid,Qt::Key_Left); QCOMPARE(grid->currentRow(),0);
            QTest::keyClick(grid,Qt::Key_End); QCOMPARE(grid->currentRow(),grid->count()-1);
            QTest::keyClick(grid,Qt::Key_Home); QCOMPARE(grid->currentRow(),0);
            if (directory==images) {
                grid->setLayoutMode(ThumbnailGrid::Layout::Custom,3);
                QTest::keyClick(grid,Qt::Key_Down); QCOMPARE(grid->currentRow(),3);
                QTest::keyClick(grid,Qt::Key_Up); QCOMPARE(grid->currentRow(),0);
            }
            QCOMPARE(w.viewMode(),MainWindow::ViewMode::Covers);
            QCOMPARE(w.bookPath(),originalPath); QCOMPARE(w.pageIndex(),0);
        }
        auto *field=new QLineEdit(&w); field->setGeometry(20,40,240,30); field->show(); field->setFocus(); QTRY_VERIFY(field->hasFocus());
        QVERIFY(w.command("thumbRight")->shortcut().isEmpty());
        QVERIFY(w.command("next")->shortcut().isEmpty());
        field->setText("abc"); field->setCursorPosition(1);
        QTest::keyClick(field,Qt::Key_Right); QCOMPARE(field->cursorPosition(),2);
        QCOMPARE(w.viewMode(),MainWindow::ViewMode::Covers);
        w.command("focusTree")->trigger();
        auto *tree=w.findChild<QWidget *>("folderTree"); QVERIFY(tree);
        QTRY_VERIFY(tree->hasFocus());
        QCOMPARE(w.command("treeRight")->shortcut(),QKeySequence("Right"));
        QVERIFY(w.command("thumbRight")->shortcut().isEmpty());
        QVERIFY(w.command("next")->shortcut().isEmpty());
        QTest::keyClick(tree,Qt::Key_Right);
        QCOMPARE(w.viewMode(),MainWindow::ViewMode::Covers);
        w.openBook(images+"/1.png"); QTRY_VERIFY(w.requestReady());
        auto *view=w.findChild<PageView *>(); QVERIFY(view); QTRY_VERIFY(view->hasFocus());
        QCOMPARE(w.command("next")->shortcut(),QKeySequence("Right"));
        QVERIFY(w.command("thumbRight")->shortcut().isEmpty());
        QTest::keyClick(view,Qt::Key_Right); QTRY_COMPARE(w.pageIndex(),1);
        w.showPageThumbnails();
        auto *pages=w.findChild<ThumbnailGrid *>("pageThumbnails"); QVERIFY(pages);
        QTRY_VERIFY(pages->hasFocus());
        QVERIFY(w.command("next")->shortcut().isEmpty());
        QCOMPARE(w.command("thumbRight")->shortcut(),QKeySequence("Right"));
        QTest::keyClick(pages,Qt::Key_Right); QCOMPARE(pages->currentRow(),2);
        QCOMPARE(w.viewMode(),MainWindow::ViewMode::Pages); QCOMPARE(w.pageIndex(),1);
        QTest::keyClick(pages,Qt::Key_Return);
        QCOMPARE(w.viewMode(),MainWindow::ViewMode::Reader); QCOMPARE(w.pageIndex(),2);
        QCOMPARE(w.command("next")->shortcut(),QKeySequence("Right"));
        auto bindings=w.bindings(); bindings["next"]=QKeySequence("Ctrl+Alt+N");
        QVERIFY(w.applyBindings(bindings));
        QCOMPARE(w.command("next")->shortcut(),bindings.value("next"));
        w.showCovers();QTRY_VERIFY(w.requestReady()); QVERIFY(w.command("next")->shortcut().isEmpty());
        QCOMPARE(w.bindings().value("next"),bindings.value("next"));
        w.close();
        MainWindow restored(config);
        QCOMPARE(restored.bindings().value("next"),bindings.value("next"));
        restored.close();
    }
    void thumbnailMouseKeyboardHandoff() {
        QTemporaryDir d; const auto path=d.filePath("pages"); QVERIFY(TestData::imageFolder(path));
        MainWindow w(d.filePath("settings.ini")); w.show(); w.activateWindow();
        w.openDirectory(path);QTRY_VERIFY(w.directoryReady());
        QTRY_VERIFY(w.isActiveWindow());
        for (const bool pages : {false, true}) {
            if (pages) {
                w.openBook(path+"/1.png");
                QTRY_VERIFY_WITH_TIMEOUT(!w.isLoading() && w.renderReady(),10000);
                w.showPageThumbnails();
            }
            auto *grid=w.findChild<ThumbnailGrid *>(pages ? "pageThumbnails" : "zipLibrary");
            QVERIFY(grid); QTRY_COMPARE(grid->count(),5); grid->setFocus(); QTRY_VERIFY(grid->hasFocus());
            QVERIFY(grid->mouseSelectionEnabled());
            const int anchor=w.pageIndex(); const auto mode=w.viewMode();
            const auto first=grid->visualItemRect(grid->item(0)).center();
            thumbnailPointer(grid,first);
            grid->selectFirst();
            QTest::keyClick(grid,Qt::Key_Right); QCOMPARE(grid->currentRow(),1);
            thumbnailPointer(grid,first); QCOMPARE(grid->currentRow(),1);
            // No item boundary is crossed: movement inside the same tile must
            // still hand control back to the pointer after keyboard navigation.
            thumbnailPointer(grid,first+QPoint(2,0)); QCOMPARE(grid->currentRow(),0);
            QTest::keyClick(grid,Qt::Key_Right); QCOMPARE(grid->currentRow(),1);
            QCOMPARE(grid->selectedItems().size(),1);
            QCOMPARE(w.viewMode(),mode); QCOMPARE(w.pageIndex(),anchor);
        }
        w.close();
    }
    void thumbnailStationaryPointerAndScrolling() {
        ThumbnailGrid grid; grid.resize(490,350); grid.setLayoutMode(ThumbnailGrid::Layout::Custom,3);
        for(int i=0;i<100;++i) grid.addItem(QString::number(i));
        grid.show(); grid.activateWindow(); grid.setFocus();
        QTRY_VERIFY(grid.hasFocus() && grid.isActiveWindow());
        QTRY_VERIFY(grid.verticalScrollBar()->maximum()>0);
        const auto position=grid.visualItemRect(grid.item(1)).center();
        thumbnailPointer(&grid,position); QCOMPARE(grid.currentRow(),1);
        grid.moveSelection(1,0); QCOMPARE(grid.currentRow(),2);
        thumbnailPointer(&grid,position); QCOMPARE(grid.currentRow(),2);
        grid.verticalScrollBar()->setValue(grid.gridSize().height());
        const int value=grid.verticalScrollBar()->value(); QVERIFY(value>0);
        thumbnailPointer(&grid,position); QCOMPARE(grid.currentRow(),2);
        const auto moved=position+QPoint(2,0);
        auto *under=grid.itemAt(moved); QVERIFY(under); const int row=grid.row(under); QVERIFY(row!=2);
        thumbnailPointer(&grid,moved); QCOMPARE(grid.currentRow(),row);
        QCOMPARE(grid.verticalScrollBar()->value(),value);
        thumbnailPointer(&grid,{-2,-2}); QCOMPARE(grid.currentRow(),row);
        grid.resize(510,360); QCoreApplication::processEvents();
        thumbnailPointer(&grid,grid.viewport()->mapFromGlobal(QCursor::pos()));
        QCOMPARE(grid.currentRow(),row);
        // Select a partly visible bottom row without auto-scrolling it into view.
        const QPoint bottom(20,grid.viewport()->height()-2);
        QVERIFY(grid.itemAt(bottom));
        const int before=grid.verticalScrollBar()->value();
        thumbnailPointer(&grid,bottom); QCOMPARE(grid.currentItem(),grid.itemAt(bottom));
        QCOMPARE(grid.verticalScrollBar()->value(),before);
    }
    void thumbnailFocusAndPopupGuards() {
        QWidget host; QVBoxLayout layout(&host); QLineEdit field; ThumbnailGrid grid;
        layout.addWidget(&field); layout.addWidget(&grid);
        for(int i=0;i<10;++i) grid.addItem(QString::number(i));
        host.resize(600,450); host.show(); host.activateWindow(); grid.setFocus();
        QTRY_VERIFY(grid.hasFocus() && host.isActiveWindow());
        grid.selectFirst();
        const auto point=grid.visualItemRect(grid.item(1)).center();
        field.setFocus(); QTRY_VERIFY(field.hasFocus());
        thumbnailPointer(&grid,point); QCOMPARE(grid.currentRow(),0); QVERIFY(field.hasFocus());
        grid.setFocus(); QTRY_VERIFY(grid.hasFocus());
        thumbnailPointer(&grid,point); QCOMPARE(grid.currentRow(),0);
        thumbnailPointer(&grid,point+QPoint(2,0)); QCOMPARE(grid.currentRow(),1);
        grid.setMouseSelectionEnabled(false); grid.selectFirst();
        thumbnailPointer(&grid,point+QPoint(4,0)); QCOMPARE(grid.currentRow(),0);
        grid.setMouseSelectionEnabled(true);
        QMenu menu; menu.addAction("test"); menu.popup(host.mapToGlobal(QPoint(10,10)));
        QTRY_VERIFY(QApplication::activePopupWidget());
        thumbnailPointer(&grid,point+QPoint(6,0)); QCOMPARE(grid.currentRow(),0);
        menu.close(); grid.setFocus();
        QDialog dialog(&host); dialog.setModal(true); dialog.show();
        QTRY_VERIFY(QApplication::activeModalWidget());
        thumbnailPointer(&grid,point+QPoint(8,0)); QCOMPARE(grid.currentRow(),0);
        dialog.close(); host.activateWindow(); grid.setFocus();
        QTRY_VERIFY(grid.hasFocus() && host.isActiveWindow());
        thumbnailPointer(&grid,point+QPoint(10,0),Qt::MiddleButton); QCOMPARE(grid.currentRow(),0);
        QWidget other; other.show(); other.activateWindow(); QTRY_VERIFY(!host.isActiveWindow());
        QCOMPARE(grid.currentRow(),0);
        thumbnailPointer(&grid,point+QPoint(12,0)); QCOMPARE(grid.currentRow(),0);
    }
    void thumbnailPreferencePersistence() {
        QTemporaryDir d; const auto settings=d.filePath("settings.ini");
        {
            MainWindow w(settings); w.show();
            for(const auto *name:{"zipLibrary","pageThumbnails"})
                QVERIFY(w.findChild<ThumbnailGrid *>(name)->mouseSelectionEnabled());
            bool edited=false;
            QTimer::singleShot(0,&w,[&]{
                auto *dialog=w.findChild<QDialog *>("preferencesDialog");
                if(!dialog)return;
                if(auto *box=dialog->findChild<QCheckBox *>("thumbnails/mouseSelection")) {
                    box->setChecked(false); edited=true;
                }
                dialog->accept();
            });
            w.command("preferences")->trigger(); QVERIFY(edited);
            for(const auto *name:{"zipLibrary","pageThumbnails"})
                QVERIFY(!w.findChild<ThumbnailGrid *>(name)->mouseSelectionEnabled());
            QSettings saved(settings,QSettings::IniFormat);
            QCOMPARE(saved.value("thumbnails/mouseSelection").toBool(),false);
            QTimer::singleShot(0,&w,[&]{
                auto *dialog=w.findChild<QDialog *>("preferencesDialog");
                if(!dialog)return;
                if(auto *box=dialog->findChild<QCheckBox *>("thumbnails/mouseSelection")) box->setChecked(true);
                dialog->reject();
            });
            w.command("preferences")->trigger();
            QVERIFY(!w.findChild<ThumbnailGrid *>("zipLibrary")->mouseSelectionEnabled());
            w.close();
        }
        MainWindow restored(settings);
        for(const auto *name:{"zipLibrary","pageThumbnails"})
            QVERIFY(!restored.findChild<ThumbnailGrid *>(name)->mouseSelectionEnabled());
        restored.close();
    }
    void loadNavigateAndViews() {
        QTemporaryDir d; const auto path=d.filePath("pages"); QVERIFY(TestData::imageFolder(path));
        MainWindow w(d.filePath("settings.ini")); w.show();
        w.openBook(path+"/1.png"); QTRY_VERIFY_WITH_TIMEOUT(!w.isLoading() && w.renderReady(),10000);
        QCOMPARE(w.pageCount(),5); QCOMPARE(w.currentPageName(),QString("1.png"));
        TestData::singlePage(w); QTRY_VERIFY(w.renderReady());
        w.nextPage(); QTRY_VERIFY(w.renderReady()); QCOMPARE(w.pageIndex(),1);
        w.previousPage(); QTRY_VERIFY(w.renderReady()); QCOMPARE(w.pageIndex(),0);
        w.command("spread")->trigger(); QTRY_VERIFY(w.renderReady());
        w.nextPage(); QTRY_VERIFY(w.renderReady()); QCOMPARE(w.pageIndex(),1);
        w.nextPage(); QTRY_VERIFY(w.renderReady()); QCOMPARE(w.pageIndex(),3);
        w.showPageThumbnails(); QCOMPARE(w.viewMode(),MainWindow::ViewMode::Pages);
        w.parentFolder();QTRY_VERIFY(w.viewMode()!=MainWindow::ViewMode::Covers || w.requestReady()); QCOMPARE(w.viewMode(),MainWindow::ViewMode::Covers);
        w.close();
    }
    void newestLoadWins() {
        QTemporaryDir d; QVERIFY(TestData::imageFolder(d.filePath("a"),8)); QVERIFY(TestData::imageFolder(d.filePath("b"),2));
        MainWindow w(d.filePath("settings.ini")); w.show();
        w.openBook(d.filePath("a/1.png")); w.openBook(d.filePath("b/1.png"));
        QTRY_VERIFY_WITH_TIMEOUT(!w.isLoading() && w.renderReady(),10000);
        QCOMPARE(w.pageCount(),2); QCOMPARE(w.bookPath(),d.filePath("b/1.png"));
        w.close();
    }
    void shortcutScopesAndPersistence() {
        QHash<QString,QKeySequence> defaults; QSet<QString> ids;
        for(const auto &s:shortcutSpecs()) {
            QVERIFY(!ids.contains(s.id)); ids.insert(s.id); defaults[s.id]=s.defaultKey;
            if(!s.defaultKey.isEmpty()) QVERIFY2(s.defaultKey[0].key()!=Qt::Key_unknown,qPrintable(s.id));
        }
        QCOMPARE(defaults.value("previousFolder"),QKeySequence(Qt::CTRL | Qt::Key_PageUp));
        QCOMPARE(defaults.value("nextFolder"),QKeySequence(Qt::CTRL | Qt::Key_PageDown));
        QCOMPARE(defaults.value("scrollNext"),QKeySequence(Qt::Key_PageDown));
        QCOMPARE(defaults.value("scrollPrevious"),QKeySequence(Qt::Key_PageUp));
#ifdef Q_OS_MACOS
        QCOMPARE(defaults.value("fullscreenGlobal"),QKeySequence("Ctrl+Meta+F"));
#else
        QCOMPARE(defaults.value("fullscreenGlobal"),QKeySequence("F11"));
#endif
        QVERIFY2(shortcutConflict(defaults).isEmpty(),qPrintable(shortcutConflict(defaults)));
        auto conflict=defaults; conflict["open"]=defaults["next"]; QVERIFY(!shortcutConflict(conflict).isEmpty());
        conflict=defaults; conflict["next"]=QKeySequence("Ctrl+K, Ctrl+N"); QVERIFY(!shortcutConflict(conflict).isEmpty());
        QTemporaryDir d;
        int index=0;
        for (const auto &saved : {QKeySequence("V"),QKeySequence("F11"),QKeySequence{}}) {
            const auto config=d.filePath(QString("settings-%1.ini").arg(index++));
            {
                MainWindow w(config);
                QCOMPARE(w.bindings().value("fullscreenGlobal"),defaults.value("fullscreenGlobal"));
                auto custom=w.bindings(); custom["fullscreenGlobal"]=saved; if(saved==QKeySequence("V"))custom["scaleFixed"]={};
                QVERIFY(w.applyBindings(custom)); w.close();
            }
            MainWindow restored(config);
            QCOMPARE(restored.bindings().value("fullscreenGlobal"),saved); restored.close();
        }
        const auto legacy=d.filePath("legacy.ini");
        { QSettings settings(legacy,QSettings::IniFormat); settings.setValue("keys/fullscreen","Return"); }
        MainWindow migrated(legacy);
        QCOMPARE(migrated.bindings().value("fullscreenGlobal"),defaults.value("fullscreenGlobal"));
        migrated.close();
    }
    void spreadGeometryAndZoom() {
        PageView v; v.resize(640,480); v.show();
        const auto image=TestData::solid(); v.setPages({image,image},true,{});
        QTRY_VERIFY(v.scaleReady()); auto rects=v.pageRects(); QCOMPARE(rects.size(),2); QVERIFY(rects[0].left()>rects[1].left());
        v.setPages({image,image},false,{}); QTRY_VERIFY(v.scaleReady()); rects=v.pageRects(); QVERIFY(rects[0].left()<rects[1].left());
        v.setNoUpscale(true); QTRY_VERIFY(v.scaleReady()); QVERIFY(v.pageRects()[0].width()<=32);
        v.setNoUpscale(false); v.setPages({TestData::solid(Qt::red,{1000,1200})},false,{});
        v.setScaleMode(ScaleMode::Actual); QTRY_VERIFY(v.scaleReady()); v.setPan({100000,-100000});
        QVERIFY(v.pan().x()<100000); QVERIFY(v.pan().y()>-100000);
        v.setScaleMode(ScaleMode::Fit); QTRY_VERIFY(v.scaleReady()); v.setPan({100,100}); QCOMPARE(v.pan(),QPointF());
    }
    void pointerAndAnimation() {
        PageView v; v.resize(400,300); v.show();
        QSignalSpy next(&v,&PageView::nextRequested), previous(&v,&PageView::previousRequested);
        v.setPages({TestData::solid()},true,{}); QTRY_VERIFY(v.scaleReady());
        QTest::mouseClick(&v,Qt::LeftButton); QCOMPARE(next.count(),1);
        QTest::mouseClick(&v,Qt::RightButton); QCOMPARE(previous.count(),1);
        ImageResult animation(TestData::solid(Qt::red)); animation.frames={animation.image,TestData::solid(Qt::blue)};
        animation.delays={100,100}; animation.loopCount=-1; v.setMedia({animation},true,{});
        v.setAnimationPaused(true); QCOMPARE(v.animationFrame(),0); QTest::qWait(130); QCOMPARE(v.animationFrame(),0);
        v.setAnimationPaused(false); QTRY_COMPARE_WITH_TIMEOUT(v.animationFrame(),1,3000);
        v.stopAnimations(); QCOMPARE(v.animationFrame(),0);
    }
};
int main(int argc,char **argv) {
    if(argc==3 && QByteArray(argv[1])=="--probe-fullscreen") {
        qputenv("QT_QPA_PLATFORM","offscreen");
        QApplication app(argc,argv);
        QCoreApplication::setOrganizationName("KomaMimicTests");
        QCoreApplication::setApplicationName("Regression");
        {
            MainWindow w(QString::fromLocal8Bit(argv[2]));w.show();
            QCoreApplication::processEvents();
            QTextStream(stdout) << (w.isFullScreen()?"fullscreen":"normal") << '\n';
            // Save normal mode so the next, separate process checks both ways.
            w.setFullscreen(false);w.close();
        }
        QThreadPool::globalInstance()->waitForDone();
        return 0;
    }
    return isolatedTestMain<UiTests>(argc,argv);
}
#include "ui_tests.moc"
