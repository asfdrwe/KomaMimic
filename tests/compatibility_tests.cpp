// SPDX-License-Identifier: GPL-3.0-only
#include "test_support.h"
#include "foldertree.h"
#include "pageview.h"
#include <QAction>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QMenuBar>
#include <QMenu>
#include <QSettings>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QTreeWidget>

class CompatibilityTests : public QObject {
    Q_OBJECT
private slots:
    void filenameBandChangesImageGeometry() {
        PageView view; view.resize(500,600); view.show();
        view.setPages({TestData::solid(Qt::red,{100,1000})},false,"001.jpg");
        QTRY_VERIFY(view.scaleReady());
        QVERIFY(!view.filenameVisible());
        const auto hidden=view.pageRects().first();
        view.setFilenameVisible(true); QTRY_VERIFY(view.scaleReady());
        const auto shown=view.pageRects().first();
        QCOMPARE(hidden.height()-shown.height(),26.0);
        view.setScaleMode(ScaleMode::Actual); QTRY_VERIFY(view.scaleReady());
        view.setPan({0,100000}); const auto withBand=view.pan();
        view.setFilenameVisible(false);QTRY_VERIFY(view.scaleReady());
        QCOMPARE(withBand.y()-view.pan().y(),13.0);
        QVERIFY(view.imageArea().contains(view.pageRects().first().center()));
    }
    void customLayoutRetainsImagesAndLogicalOrder() {
        ThumbnailGrid grid;grid.resize(420,300);grid.show();
        for(int i=0;i<24;++i) {
            auto *item=new QListWidgetItem(QString("%1.jpg").arg(i));item->setToolTip(item->text());grid.addItem(item);
            grid.setThumbnail(i,TestData::solid(Qt::red,{100,150}),{});
        }
        grid.requestVisible();QTRY_VERIFY(grid.gridSize().height()>64);
        QCOMPARE(grid.customColumns(),0);QCOMPARE(grid.iconSize(),QSize(64,64));
        CustomThumbnailSettings custom;custom.filenameLines=0;custom.rightOrigin=true;
        grid.setCustomSettings(custom);QTRY_COMPARE(grid.gridSize().height(),66);
        for(int i=0;i<grid.count();++i)QVERIFY(grid.item(i)->data(ThumbnailRoleReady).toBool());
        QTRY_VERIFY(grid.visualItemRect(grid.item(0)).left()>grid.visualItemRect(grid.item(1)).left());
        grid.setCurrentRow(1);grid.moveSelection(-1,0);QCOMPARE(grid.currentRow(),2);
        grid.moveSelection(1,0);QCOMPARE(grid.currentRow(),1);
        custom.columns=4;custom.width=200;custom.height=100;custom.horizontalGap=5;custom.verticalGap=7;
        grid.setCustomSettings(custom);QTRY_VERIFY(grid.iconSize().width()<200);
        QCOMPARE(grid.item(0)->text(),QString("0.jpg"));QVERIFY(grid.item(0)->data(ThumbnailRoleReady).toBool());
    }
    void browserKeepsLeftAndRightIndependent() {
        QTemporaryDir d;QVERIFY(d.isValid());
        const auto a=d.filePath("A"), b=d.filePath("A/B");QVERIFY(QDir().mkpath(b));
        QVERIFY(TestData::write(a+"/001.png",TestData::png(TestData::solid())));
        const auto config=d.filePath("settings.ini");
        MainWindow window(config);window.show();window.activateWindow();
        auto *browser=window.findChild<FolderBrowser *>();QVERIFY(browser);QVERIFY(browser->twoPane());
        auto *upper=window.findChild<QTreeWidget *>("folderBrowserList");QVERIFY(upper);
        auto *tree=browser->tree();
        browser->openLocation(d.path());QTRY_COMPARE(browser->location(),d.path());
        window.openBook(a+"/001.png");QTRY_VERIFY(window.renderReady());
        upper->setFocus();QTRY_VERIFY(upper->hasFocus());
        QTreeWidgetItem *folder=nullptr;
        for(int i=0;i<upper->topLevelItemCount();++i)if(upper->topLevelItem(i)->text(0)=="A")folder=upper->topLevelItem(i);
        QVERIFY(folder);upper->setCurrentItem(folder);QTest::keyClick(upper,Qt::Key_Return);
        QTRY_COMPARE(browser->location(),a);QCOMPARE(tree->browseRoot(),a);
        QCOMPARE(window.viewMode(),MainWindow::ViewMode::Reader);QCOMPARE(window.bookPath(),a+"/001.png");
        const auto root=tree->model()->index(0,0);
        QTRY_VERIFY(tree->model()->rowCount(root)>0);
        const auto child=tree->model()->index(0,0,root);QCOMPARE(child.data().toString(),QString("B"));
        tree->setCurrentIndex(child);tree->setFocus();QTRY_VERIFY(tree->hasFocus());
        QCOMPARE(browser->location(),a);QCOMPARE(window.viewMode(),MainWindow::ViewMode::Reader);
        QTest::mouseClick(tree->viewport(),Qt::LeftButton,Qt::NoModifier,tree->visualRect(child).center());
        QCOMPARE(window.viewMode(),MainWindow::ViewMode::Reader);
        QTest::mouseDClick(tree->viewport(),Qt::LeftButton,Qt::NoModifier,tree->visualRect(child).center());
        QTRY_COMPARE(window.directory(),b);
        window.openBook(a+"/001.png");QTRY_VERIFY(window.renderReady());
        tree->setFocus();QTRY_VERIFY(tree->hasFocus());QTest::keyClick(tree,Qt::Key_Return);
        QTRY_COMPARE(window.directory(),b);
        QCOMPARE(window.viewMode(),MainWindow::ViewMode::Covers);QCOMPARE(browser->location(),a);QCOMPARE(tree->browseRoot(),a);
        const auto selection=tree->selectedPath();window.openDirectory(d.path());
        QCOMPARE(browser->location(),a);QCOMPARE(tree->browseRoot(),a);QCOMPARE(tree->selectedPath(),selection);
        QVERIFY(QDir().mkpath(a+"/C"));
        QTRY_COMPARE(tree->model()->rowCount(root),2);
        QTRY_VERIFY([&] {for(int i=0;i<upper->topLevelItemCount();++i)if(upper->topLevelItem(i)->text(0)=="C")return true;return false;}());
        QCOMPARE(tree->selectedPath(),selection);QCOMPARE(window.directory(),d.path());
        window.close();
        MainWindow restored(config);auto *again=restored.findChild<FolderBrowser *>();QVERIFY(again);
        QTRY_COMPARE(again->location(),a);QTRY_COMPARE(again->tree()->browseRoot(),a);
        again->setTwoPane(false);again->saveSettings();restored.close();
        MainWindow single(config);auto *one=single.findChild<FolderBrowser *>();QVERIFY(one);QVERIFY(!one->twoPane());
#ifdef Q_OS_WIN
        QCOMPARE(one->tree()->browseRoot(),QString("km:computer"));
#else
        QCOMPARE(one->tree()->browseRoot(),QString("/"));
#endif
        single.close();
    }
    void displayPreferencesAndPersistence() {
        QTemporaryDir d;const auto config=d.filePath("settings.ini");
        QVERIFY(TestData::imageFolder(d.filePath("pages"),3));
        QVERIFY(QFile::rename(d.filePath("pages/1.png"),d.filePath("pages/001.png")));
        {
            MainWindow window(config);window.show();window.openDirectory(d.filePath("pages"));window.readFolder();QTRY_VERIFY(window.renderReady());
            window.showPageThumbnails();auto *grid=window.findChild<ThumbnailGrid *>("pageThumbnails");QVERIFY(grid);
            QCOMPARE(grid->item(0)->text(),QString("001.png"));const int page=window.pageIndex();
            bool edited=false;
            QTimer timer;QObject::connect(&timer,&QTimer::timeout,&window,[&] {
                auto *dialog=qobject_cast<QDialog *>(QApplication::activeModalWidget());if(!dialog)return;
                auto *filename=dialog->findChild<QCheckBox *>("display/filename");if(!filename)return;
                filename->setChecked(true);dialog->findChild<QCheckBox *>("thumbnails/numbers")->setChecked(true);
                dialog->findChild<QCheckBox *>("thumbnails/frames")->setChecked(true);
                dialog->findChild<QCheckBox *>("folders/twoPane")->setChecked(false);
                dialog->findChild<QSpinBox *>("thumbnails/width")->setValue(96);
                edited=true;dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            });timer.start(10);window.command("preferences")->trigger();timer.stop();QVERIFY(edited);
            QCOMPARE(grid->item(0)->text(),QString("1  001.png"));QCOMPARE(window.pageIndex(),page);
            QVERIFY(window.findChild<PageView *>()->filenameVisible());QCOMPARE(grid->customSettings().width,96);QVERIFY(grid->customSettings().frames);
            QVERIFY(!window.findChild<FolderBrowser *>()->twoPane());
            bool direct=false;
            for(auto *menu:window.menuBar()->findChildren<QMenu *>())
                if(menu->objectName()=="menuView")direct=menu->actions().contains(window.command("toolbarToggle"));
            QVERIFY(direct);QVERIFY(!window.findChild<QMenu *>("menuToolbars"));window.close();
        }
        MainWindow restored(config);QVERIFY(restored.findChild<PageView *>()->filenameVisible());
        QVERIFY(!restored.findChild<FolderBrowser *>()->twoPane());
        QCOMPARE(restored.findChild<ThumbnailGrid *>("pageThumbnails")->customSettings().width,96);restored.close();
    }
};
ISOLATED_TEST_MAIN(CompatibilityTests)
#include "compatibility_tests.moc"
