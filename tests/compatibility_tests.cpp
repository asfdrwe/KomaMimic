// SPDX-License-Identifier: GPL-3.0-only
#include "test_support.h"
#include "foldertree.h"
#include "pageview.h"
#include "readerfeatures.h"
#include <QMessageBox>
#include <QAction>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QMenuBar>
#include <QHeaderView>
#include <QScrollBar>
#include <QMenu>
#include <QSettings>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QTreeWidget>

class CompatibilityTests : public QObject {
    Q_OBJECT
private slots:
    void folderTreeNamesRemainReadable_data() {
        QTest::addColumn<bool>("twoPane");
        QTest::newRow("two-pane") << true;
        QTest::newRow("single-pane") << false;
    }
    void folderTreeNamesRemainReadable() {
        QFETCH(bool,twoPane);
        QTemporaryDir d;QVERIFY(d.isValid());
        QString selected=d.path();
        for(int depth=0;depth<5;++depth)selected+=QString("/階層%1").arg(depth);
        const QString name=QStringLiteral("スマホデータ_日本語と長いフォルダ名_abcdefghi");
        selected+='/'+name;QVERIFY(QDir().mkpath(selected));
        FolderTree tree;auto font=tree.font();font.setPointSize(18);tree.setFont(font);
        tree.resize(320,600);tree.show();tree.setTwoPane(twoPane);
        if(twoPane)tree.setBrowseRoot(d.path(),QStringLiteral("画像"));
        qInfo() << "tree column mode/width/viewport" << tree.header()->sectionResizeMode(0)
                << tree.columnWidth(0) << tree.viewport()->width();
        QCOMPARE(tree.header()->sectionResizeMode(0),QHeaderView::ResizeToContents);
        if(twoPane) {
            auto index=tree.model()->index(0,0);
            for(int depth=0;depth<6;++depth) {
                tree.expand(index);
                QTRY_COMPARE(tree.model()->rowCount(index),1);
                index=tree.model()->index(0,0,index);
            }
            QCOMPARE(index.data().toString(),name);tree.setCurrentIndex(index);tree.scrollTo(index);
        } else {
            tree.selectPath(selected);QTRY_COMPARE(tree.selectedPath(),selected);
        }
        const auto index=tree.currentIndex();QVERIFY(index.isValid());
        QTRY_VERIFY(tree.visualRect(index).width()>=tree.sizeHintForIndex(index).width());
        QTRY_VERIFY(tree.horizontalScrollBar()->maximum()>0);
        tree.horizontalScrollBar()->setValue(tree.horizontalScrollBar()->maximum());
        QVERIFY(tree.visualRect(index).right()<=tree.viewport()->rect().right());
        // Changing the root recreates the model's columns. Their sizing policy
        // must also survive repeated one/two-pane switches and later inserts.
        tree.setTwoPane(!twoPane);tree.setTwoPane(twoPane);
        tree.setBrowseRoot(d.path(),QStringLiteral("画像"));
        QCOMPARE(tree.header()->sectionResizeMode(0),QHeaderView::ResizeToContents);
        qInfo() << "tree column after root reset" << tree.columnWidth(0);
    }
    void singlePaneSelectsHiddenAncestors() {
        QTemporaryDir d;QVERIFY(d.isValid());
        const auto selected=d.filePath(".hidden-parent/日本語/leaf");QVERIFY(QDir().mkpath(selected));
        FolderTree tree;tree.selectPath(selected);
        QTRY_COMPARE(tree.selectedPath(),selected);
        tree.setTwoPane(true);tree.setTwoPane(false);tree.selectPath(selected);
        QTRY_COMPARE(tree.selectedPath(),selected);
    }
    void coverSortCommandsApplyWithoutAnOpenBook() {
        QTemporaryDir d;const auto config=d.filePath("settings.ini");
        for(const auto &name:{"10.png","2.png","1.png"})QVERIFY(TestData::write(d.filePath(name),TestData::png(TestData::solid())));
        MainWindow window(config);window.show();window.openDirectory(d.path());
        auto *grid=window.findChild<ThumbnailGrid *>("zipLibrary");QVERIFY(grid);QCOMPARE(window.pageCount(),0);
        window.command("sortNatural")->trigger();QCOMPARE(grid->item(0)->text(),QString("1.png"));QCOMPARE(grid->item(1)->text(),QString("2.png"));
        grid->setCurrentRow(1);const auto selected=grid->currentItem()->data(PathRole);
        window.command("sortUnicode")->trigger();QCOMPARE(grid->item(1)->text(),QString("10.png"));QCOMPARE(grid->currentItem()->data(PathRole),selected);
        window.command("sortReverse")->trigger();QCOMPARE(grid->item(0)->text(),QString("2.png"));
        QCOMPARE(grid->currentItem()->data(PathRole),selected);window.close();
        MainWindow restored(config);restored.openDirectory(d.path());
        QCOMPARE(restored.findChild<ThumbnailGrid *>("zipLibrary")->item(0)->text(),QString("2.png"));restored.close();
    }
    void sharedSortRestoresForReadingAndSettingsSlots() {
        QTemporaryDir d;const auto config=d.filePath("settings.ini");
        for(const auto &name:{"10.png","2.png","1.png"})QVERIFY(TestData::write(d.filePath(name),TestData::png(TestData::solid())));
        MainWindow window(config);window.show();window.openDirectory(d.path());
        window.command("sortUnicode")->trigger();window.readFolder();QTRY_VERIFY(window.renderReady());
        auto *pages=window.findChild<ThumbnailGrid *>("pageThumbnails");QVERIFY(pages);
        QCOMPARE(pages->item(1)->text(),QString("10.png"));
        const auto current=window.currentPageName();window.command("sortReverse")->trigger();
        QCOMPARE(window.currentPageName(),current);QCOMPARE(pages->item(0)->text(),QString("2.png"));
        QVERIFY(window.command("sortUnicode")->isChecked());QVERIFY(window.command("sortReverse")->isChecked());
        QTimer dismiss;connect(&dismiss,&QTimer::timeout,&window,[]{if(auto *box=qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))box->accept();});dismiss.start(10);
        ReaderFeatures *features=nullptr;for(auto *child:window.children())if(auto *candidate=dynamic_cast<ReaderFeatures *>(child))features=candidate;QVERIFY(features);
        const auto exported=d.filePath("export.ini");features->settingsFile(false,exported);QVERIFY(QFileInfo::exists(exported));
        window.command("settingsSave4")->trigger();
        window.command("sortNatural")->trigger();window.command("settingsLoad4")->trigger();
        QVERIFY(window.command("sortUnicode")->isChecked());QCOMPARE(pages->item(0)->text(),QString("2.png"));
        window.command("sortNatural")->trigger();features->settingsFile(true,exported);
        QVERIFY(window.command("sortUnicode")->isChecked());QCOMPARE(window.currentPageName(),current);
        window.showCovers();window.command("sortRandom")->trigger();
        auto *covers=window.findChild<ThumbnailGrid *>("zipLibrary");QStringList before;
        for(int i=0;i<covers->count();++i)before.append(covers->item(i)->text());
        window.command("sortReverse")->trigger();std::reverse(before.begin(),before.end());
        for(int i=0;i<covers->count();++i)QCOMPARE(covers->item(i)->text(),before[i]);
        dismiss.stop();window.close();
        MainWindow restarted(config);restarted.openDirectory(d.path());restarted.readFolder();QTRY_VERIFY(restarted.renderReady());
        QVERIFY(restarted.command("sortRandom")->isChecked());restarted.close();
    }
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
