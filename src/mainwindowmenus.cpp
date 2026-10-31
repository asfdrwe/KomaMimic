// SPDX-License-Identifier: GPL-3.0-only
#include "mainwindow.h"
#include <QCoreApplication>
#include "readerfeatures.h"
#include <QAction>
#include <QActionGroup>
#include <QMenu>
#include <QMenuBar>
#include <QSignalBlocker>

QMenu *MainWindow::createPopupMenu() {
    auto *menu = new QMenu(this);
    for (const auto &id : {"toolbarToggle", "tree"})
        menu->addAction(m_actions.value(id));
    return menu;
}

// Keep presentation here; command connections and shortcut IDs stay in their
// respective feature modules. Menus, toolbars and custom bindings share actions.
void MainWindow::buildMenus() {
    const auto menu = [this](const char *id, const QString &title) {
        auto *result = new QMenu(title, menuBar());
        result->menuAction()->setMenuRole(QAction::NoRole);
        result->setObjectName(id);
        menuBar()->addMenu(result);
        return result;
    };
    const auto sub = [](QMenu *parent, const char *id, const QString &title) {
        auto *result = new QMenu(title, parent);
        // Names such as "Settings slots" must never replace Preferences on Mac.
        result->menuAction()->setMenuRole(QAction::NoRole);
        result->setObjectName(id);
        parent->addMenu(result);
        return result;
    };
    const auto add = [this](QMenu *parent, const QStringList &ids) {
        for (const auto &id : ids) parent->addAction(m_actions.value(id));
    };
    const auto settings = [this](QMenu *parent, const char *id,
                                  const QString &title, const QString &page) {
        auto *action = new QAction(title, parent);
        action->setMenuRole(QAction::NoRole);
        action->setObjectName(id);
        parent->addAction(action);
        connect(action, &QAction::triggered, this, [this, page] {
            m_features->preferences(page);
        });
    };
    // Use the familiar names where behavior agrees; retain explicit wording
    // such as non-destructive deletion and script processing where it differs.
    const QHash<QString, QString> labels{
        {"open", QCoreApplication::translate("KomaMimic", "開く(&O)…")}, {"folder", QCoreApplication::translate("KomaMimic", "フォルダを開く(&D)…")},
        {"openUrl", QCoreApplication::translate("KomaMimic", "URLを開く(&H)…")}, {"external", QCoreApplication::translate("KomaMimic", "外部プログラムで開く(&J)")},
        {"externalSettings", QCoreApplication::translate("KomaMimic", "外部プログラム起動設定(&K)…")},
        {"saveImage", QCoreApplication::translate("KomaMimic", "保存(&S)…")},
        {"closeBook", QCoreApplication::translate("KomaMimic", "閉じる(&C)")}, {"reloadBook", QCoreApplication::translate("KomaMimic", "更新(&R)")},
        {"importSettings", QCoreApplication::translate("KomaMimic", "設定ファイルのインポート(&I)…")},
        {"exportSettings", QCoreApplication::translate("KomaMimic", "設定ファイルのエクスポート(&E)…")},
        {"imagePaste", QCoreApplication::translate("KomaMimic", "クリップボードから画像を貼り付け(&A)")},
        {"imageCopy", QCoreApplication::translate("KomaMimic", "クリップボードへ画像をコピー(&B)")},
        {"pageSetup", QCoreApplication::translate("KomaMimic", "ページ設定(&U)…")}, {"print", QCoreApplication::translate("KomaMimic", "印刷(&P)…")}, {"quit", QCoreApplication::translate("KomaMimic", "終了(&X)")},
        {"autoSpread", QCoreApplication::translate("KomaMimic", "自動見開き表示")}, {"splitVertical", QCoreApplication::translate("KomaMimic", "左右分割")},
        {"splitHorizontal", QCoreApplication::translate("KomaMimic", "上下分割")}, {"library", QCoreApplication::translate("KomaMimic", "表紙サムネイル／読書画面")},
        {"scaleFill", QCoreApplication::translate("KomaMimic", "縦横どちらかに合わせる")}, {"scaleFit", QCoreApplication::translate("KomaMimic", "ウィンドウサイズに合わせる")},
        {"scaleActual", QCoreApplication::translate("KomaMimic", "原寸サイズ（100%）")}, {"fitWindow", QCoreApplication::translate("KomaMimic", "画像サイズにウィンドウを合わせる")},
        {"scaleEqualHeight", QCoreApplication::translate("KomaMimic", "縦サイズを揃える（2ページ）")},
        {"scaleSpecified", QCoreApplication::translate("KomaMimic", "指定スケールサイズ…")}, {"scaleFixed", QCoreApplication::translate("KomaMimic", "倍率固定")},
        {"next", QCoreApplication::translate("KomaMimic", "次に進む")}, {"previous", QCoreApplication::translate("KomaMimic", "前に戻る")}, {"first", QCoreApplication::translate("KomaMimic", "最初のページへ")},
        {"last", QCoreApplication::translate("KomaMimic", "最終ページへ")}, {"nextBook", QCoreApplication::translate("KomaMimic", "次の書庫へ")}, {"previousBook", QCoreApplication::translate("KomaMimic", "前の書庫へ")},
        {"preloadAll", QCoreApplication::translate("KomaMimic", "全ページ一括ロード（メモリ上限内）")},
        {"shortcuts", QCoreApplication::translate("KomaMimic", "キーボード(&K)…")}, {"mouseSettings", QCoreApplication::translate("KomaMimic", "マウス・ジェスチャ(&M)…")},
        {"gamepadSettings", QCoreApplication::translate("KomaMimic", "ゲームパッド(&P)…")}, {"toolbarSettings", QCoreApplication::translate("KomaMimic", "ツールバー(&T)…")},
        {"preferences", QCoreApplication::translate("KomaMimic", "環境設定(&C)…")}, {"imageInfo", QCoreApplication::translate("KomaMimic", "画像情報(&I)…")},
        {"cacheInfo", QCoreApplication::translate("KomaMimic", "キャッシュ情報(&C)…")}, {"about", QCoreApplication::translate("KomaMimic", "KomaMimicについて(&V)…")}
    };
    for (auto it = labels.cbegin(); it != labels.cend(); ++it) m_actions.value(it.key())->setText(it.value());

    auto *file = menu("menuFile", QCoreApplication::translate("KomaMimic", "ファイル(&F)"));
    add(file, {"open", "folder", "openUrl", "external", "externalSettings"});
    file->addSeparator(); add(file, {"saveImage"});
    auto *output = sub(file, "menuExport", QCoreApplication::translate("KomaMimic", "その他の保存・書き出し"));
    add(output, {"saveSpread", "saveOriginal", "saveAll", "saveZip", "savePdf", "saveGif", "saveThumbnail"});
    auto *quick = sub(file, "menuQuickSave", QCoreApplication::translate("KomaMimic", "クイックセーブ"));
    add(quick, {"quickSave1", "quickSave2", "quickSave3", "quickSave4"});
    add(file, {"quickSaveSettings"});
    file->addSeparator(); add(file, {"closeBook", "reloadBook"});
    auto *edit = sub(file, "menuPageEdit", QCoreApplication::translate("KomaMimic", "ページ編集(&E)"));
    add(edit, {"editUndo", "editRedo"}); edit->addSeparator();
    add(edit, {"selectRange", "selectStart", "selectEnd"}); edit->addSeparator();
    add(edit, {"editCut", "editCopy", "editPaste", "editDelete", "editClear"}); edit->addSeparator();
    add(edit, {"insertBlank"}); edit->addSeparator(); add(edit, {"findPages", "addPages"});
    edit->addSeparator(); add(edit, {"trashPages"});
    add(file, {"renameFile"});
    file->addSeparator(); add(file, {"importMangaMeeyaSettings", "importSettings", "exportSettings"});
    file->addSeparator(); add(file, {"imagePaste", "imageCopy"});
    file->addSeparator(); add(file, {"pageSetup", "print"});
    file->addSeparator();
    auto *history = file->addAction(QCoreApplication::translate("KomaMimic", "アクセス履歴…")); history->setObjectName("menuAccessHistory");
    connect(history, &QAction::triggered, this, [this] { m_features->showCollection("history", 2); });
    file->addSeparator(); add(file, {"quit"});

    auto *view = menu("menuView", QCoreApplication::translate("KomaMimic", "表示(&V)"));
    // Explicit choices use the existing toggle command, including its document
    // layout override. They do not add competing keyboard shortcuts.
    const auto choices = [this](QMenu *parent, const QString &id,
                                 const QString &off, const QString &on) {
        auto *group = new QActionGroup(parent);
        auto *source = m_actions.value(id);
        auto *a = parent->addAction(off), *b = parent->addAction(on);
        a->setObjectName("menu_" + id + "_off"); b->setObjectName("menu_" + id + "_on");
        for (auto *action : {a, b}) { action->setCheckable(true); group->addAction(action); }
        const auto sync = [source, a, b] {
            const QSignalBlocker first(a), second(b);
            a->setChecked(!source->isChecked()); b->setChecked(source->isChecked());
            a->setEnabled(source->isEnabled()); b->setEnabled(source->isEnabled());
        };
        connect(source, &QAction::changed, parent, sync);
        // State updates made with QSignalBlocker must also appear when opened.
        connect(parent, &QMenu::aboutToShow, parent, sync);
        connect(a, &QAction::triggered, this, [source] { source->setChecked(false); });
        connect(b, &QAction::triggered, this, [source] { source->setChecked(true); });
        sync();
    };
    choices(view, "spread", QCoreApplication::translate("KomaMimic", "1ページ表示"), QCoreApplication::translate("KomaMimic", "2ページ表示"));
    choices(sub(view, "menuDirection", QCoreApplication::translate("KomaMimic", "見開き方向")), "rtl", QCoreApplication::translate("KomaMimic", "左開き"), QCoreApplication::translate("KomaMimic", "右開き"));
    settings(view, "menuOriginSettings", QCoreApplication::translate("KomaMimic", "原点（初期表示位置）の設定…"), "display");
    add(sub(view, "menuPageLayout", QCoreApplication::translate("KomaMimic", "ページ配置・切り替え")), {"spread", "rtl", "cover", "coverBlank"});
    view->addSeparator(); add(view, {"autoSpread", "splitVertical", "splitHorizontal", "splitReset"});
    settings(view, "menuAutoLayoutSettings", QCoreApplication::translate("KomaMimic", "自動表示・分割の設定…"), "automatic");
    view->addSeparator(); add(view, {"thumbnails"});
    add(sub(view, "menuThumbnailLayout", QCoreApplication::translate("KomaMimic", "サムネイルの配置")), {"thumbFit", "thumbEight", "thumbCustom"});
    add(view, {"library", "thumbRefresh"});
    settings(view, "menuThumbnailSettings", QCoreApplication::translate("KomaMimic", "サムネイルの設定…"), "thumbnails");
    add(sub(view, "menuThumbnailSelection", QCoreApplication::translate("KomaMimic", "サムネイルの選択")),
        {"thumbAccept", "thumbCancel", "thumbLeft", "thumbRight", "thumbUp", "thumbDown", "thumbFirst", "thumbLast", "thumbPageUp", "thumbPageDown"});
    view->addSeparator(); add(view, {"fullscreenGlobal", "exitFullscreen"});
    view->addSeparator(); add(view, {"loupe"});
    add(sub(view, "menuLoupe", QCoreApplication::translate("KomaMimic", "ルーペの調整")), {"loupeLarger", "loupeSmaller", "loupeZoomIn", "loupeZoomOut"});
    add(view, {"toolbarToggle"});
    add(view, {"tree", "bookLists", "bookshelf", "toc"});
    settings(view, "menuFullscreenSettings", QCoreApplication::translate("KomaMimic", "フルスクリーン時の設定…"), "display");
    view->addSeparator();
    auto *document = sub(view, "menuDocument", QCoreApplication::translate("KomaMimic", "文書（TXT・Markdown・EPUB）"));
    add(document, {"documentSettings", "textSizeIncrease", "textSizeDecrease", "textSizeReset", "documentWritingMode", "documentPublisherStyles"});
    document->addSeparator(); add(document, {"documentBack", "findDocument"});
    view->addSeparator(); add(view, {"newWindow"});
    add(sub(view, "menuWindow", QCoreApplication::translate("KomaMimic", "ウィンドウ操作")), {"minimize", "maximize", "tray"});

    auto *scale = menu("menuScale", QCoreApplication::translate("KomaMimic", "スケールモード(&S)"));
    add(scale, {"scaleFill", "scaleFit", "scaleActual", "fitWindow", "scaleEqualHeight", "scaleSpecified", "scaleFixed"});
    add(sub(scale, "menuZoom", QCoreApplication::translate("KomaMimic", "幅・高さ・拡大率の操作")), {"scaleWidth", "scaleHeight", "zoomIn", "zoomOut", "scaleCycle"});
    scale->addSeparator(); add(scale, {"noUpscale", "scaleEqualHeightToggle", "scaleFitToggle"});
    scale->addSeparator(); add(scale, {"resizeSmooth", "resizeArea", "resizeLanczos", "resizeBicubic", "resizeBilinear", "resizeAreaWeak", "resizeAreaStrong", "resizeNearest"});

    auto *move = menu("menuMove", QCoreApplication::translate("KomaMimic", "移動(&J)"));
    add(move, {"next", "nextOne", "jumpNext10", "jumpNext20", "jumpNext50", "last", "nextFolder", "nextBook"});
    move->addSeparator(); add(move, {"previous", "previousOne", "jumpPrevious10", "jumpPrevious20", "jumpPrevious50", "first", "previousFolder", "previousBook"});
    move->addSeparator(); add(move, {"parentFolder"});
    move->addSeparator(); add(move, {"goPage", "thumbnails"});
    add(sub(move, "menuJump", QCoreApplication::translate("KomaMimic", "指定量・目次・マーカーへの移動")), {"jumpFixedNext", "jumpFixedPrevious", "jumpPercentNext", "jumpPercentPrevious", "nextToc", "previousToc", "gotoMarker"});
    add(sub(move, "menuScroll", QCoreApplication::translate("KomaMimic", "スクロール")), {"scrollLeft", "scrollRight", "scrollUp", "scrollDown", "scrollNext", "scrollPrevious", "wheelScrollUp", "wheelScrollDown"});
    add(sub(move, "menuFolderNavigation", QCoreApplication::translate("KomaMimic", "フォルダ履歴・現在のフォルダ")), {"folderBack", "folderForward", "readFolder"});
    add(sub(move, "menuFocus", QCoreApplication::translate("KomaMimic", "フォーカス移動")), {"focusTree", "focusThumbnails"});
    move->addSeparator(); add(move, {"boundaryCycle", "boundaryStop", "boundaryRepeat", "boundaryAdvance"});

    auto *lists = menu("menuBookList", QCoreApplication::translate("KomaMimic", "ブックリスト(&B)"));
    add(lists, {"bookLists"}); lists->addSeparator();
    add(lists, {"marker", "gotoMarker", "addToc", "addFavorite", "registerCover", "registerSpine"});
    lists->addSeparator();
    add(sub(lists, "menuSlideshow", QCoreApplication::translate("KomaMimic", "ページ・プレイリストの再生")), {"slideForward", "slideReverse", "slidePause", "slideStop", "timedList"});
    lists->addSeparator(); auto *covers = sub(lists, "menuListThumbnails", QCoreApplication::translate("KomaMimic", "リストサムネイル表示"));
    for (const QString kind : {"playlist", "favorite", "history"}) {
        if (kind != "playlist") covers->addSeparator();
        for (int i = 0; i < 3; ++i) add(covers, {kind + QString::number(i)});
    }
    lists->addSeparator(); add(lists, {"addPlaylist", "addShelf", "bookshelf", "toc", "listDetails"});
    lists->addSeparator(); add(lists, {"listSave", "listOpen"});
    add(sub(lists, "menuBookMetadata", QCoreApplication::translate("KomaMimic", "ZIPの書誌情報")), {"zipMetadataImport", "zipMetadataExport"});
    settings(lists, "menuHistorySettings", QCoreApplication::translate("KomaMimic", "履歴・再開位置の設定…"), "general");

    auto *tools = menu("menuTools", QCoreApplication::translate("KomaMimic", "ツール(&T)"));
    settings(tools, "menuCacheSettings", QCoreApplication::translate("KomaMimic", "ファイル先読み・キャッシュの設定(&A)…"), "cache");
    add(tools, {"preloadAll", "clearCache"});
    settings(tools, "menuEffectSettings", QCoreApplication::translate("KomaMimic", "ページエフェクトの設定…"), "display");
    tools->addSeparator();
    auto *sorting = sub(tools, "menuFileSort", QCoreApplication::translate("KomaMimic", "ファイルソート(&S)"));
    add(sorting, {"sortNatural", "sortName", "sortType", "sortSize", "sortDate", "sortEntry", "sortUnicode"});
    sorting->addSeparator(); add(sorting, {"sortReverse", "sortRandom"});
    settings(tools, "menuFolderSortSettings", QCoreApplication::translate("KomaMimic", "フォルダ・表紙一覧の並び順…"), "display");
    add(tools, {"archiveNames"}); tools->addSeparator();
    auto *processing = sub(tools, "menuImageProcessing", QCoreApplication::translate("KomaMimic", "画像加工"));
    add(processing, {"filterEnabled", "filterSettings", "filterReset"}); processing->addSeparator();
    add(processing, {"rotateRight", "rotateLeft", "rotateHalf", "filterFlipH", "filterFlipV", "filterAutoCrop", "filterGray", "filterInvert", "filterBinary"});
    add(sub(processing, "menuSharpen", QCoreApplication::translate("KomaMimic", "シャープ化")), {"sharpenNone", "sharpenWeak", "sharpenStrong"});
    settings(tools, "menuScriptSettings", QCoreApplication::translate("KomaMimic", "スクリプト読み込み・フィルターの設定…"), "scripts");
    auto *profiles = sub(tools, "menuProfiles", QCoreApplication::translate("KomaMimic", "画像加工プロファイル"));
    for (int i = 1; i <= 20; ++i) add(profiles, {QString("profile%1").arg(i)});
    profiles->addSeparator(); auto *saveProfiles = sub(profiles, "menuSaveProfiles", QCoreApplication::translate("KomaMimic", "現在の設定を保存"));
    for (int i = 1; i <= 20; ++i) add(saveProfiles, {QString("saveProfile%1").arg(i)});
    add(sub(tools, "menuAnimation", QCoreApplication::translate("KomaMimic", "アニメーション画像の再生")), {"mediaPause", "mediaStop", "mediaRestart", "mediaRepeat"});
    tools->addSeparator();
    add(sub(tools, "menuInputSettings", QCoreApplication::translate("KomaMimic", "操作設定(&M)")), {"shortcuts", "mouseSettings", "gamepadSettings", "toolbarSettings", "swapMouseButtons"});
    add(tools, {"preferences"});
    tools->addSeparator(); auto *slotMenu = sub(tools, "menuSettingsSlots", QCoreApplication::translate("KomaMimic", "設定スロット"));
    auto *loads = sub(slotMenu, "menuSettingsLoad", QCoreApplication::translate("KomaMimic", "読み込む")), *saves = sub(slotMenu, "menuSettingsSave", QCoreApplication::translate("KomaMimic", "保存"));
    for (int i = 1; i <= 10; ++i) {
        add(loads, {QString("settingsLoad%1").arg(i)}); add(saves, {QString("settingsSave%1").arg(i)});
    }
    auto *users = sub(tools, "menuUserCommands", QCoreApplication::translate("KomaMimic", "ユーザコマンド"));
    for (int i = 1; i <= 8; ++i) add(users, {QString("userCommand%1").arg(i)});

    auto *help = menu("menuHelp", QCoreApplication::translate("KomaMimic", "ヘルプ(&H)"));
    add(help, {"imageInfo", "cacheInfo"}); help->addSeparator(); add(help, {"help"});
    help->addSeparator(); add(help, {"about"});
}
