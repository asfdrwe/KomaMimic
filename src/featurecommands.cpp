// SPDX-License-Identifier: GPL-3.0-only
#include "readerfeatures.h"
#include <QCoreApplication>
QVector<ShortcutSpec> featureShortcutSpecs() {
  QVector<ShortcutSpec> specs;
  const auto add = [&](const char *id, const QString &label, const char *key = "",
                       ShortcutScope scope = ShortcutScope::Reader) {
    specs.append({id, label, QKeySequence(key), scope});
  };
  add("documentSettings",QCoreApplication::translate("KomaMimic", "文書の表示設定…"),"",ShortcutScope::Global);
  add("documentBack",QCoreApplication::translate("KomaMimic", "文書のリンク元へ戻る"));
  add("findDocument",QCoreApplication::translate("KomaMimic", "本文を検索…"));
  add("textSizeIncrease", QCoreApplication::translate("KomaMimic", "文書の文字を大きくする"));
  add("textSizeDecrease", QCoreApplication::translate("KomaMimic", "文書の文字を小さくする"));
  add("textSizeReset", QCoreApplication::translate("KomaMimic", "文書の文字サイズを既定に戻す"));
  add("documentWritingMode", QCoreApplication::translate("KomaMimic", "文書の縦書き／横書き切り替え"));
  {
    add("documentPublisherStyles", QCoreApplication::translate("KomaMimic", "出版社CSSの使用を切り替え"));
  }
  add("closeBook", QCoreApplication::translate("KomaMimic", "本を閉じる"), "Ctrl+W", ShortcutScope::Global);
  add("reloadBook", QCoreApplication::translate("KomaMimic", "本を開き直す"), "F5", ShortcutScope::Global);
  add("jumpFixedNext", QCoreApplication::translate("KomaMimic", "指定枚数進む"), "Alt+Right");
  add("jumpFixedPrevious", QCoreApplication::translate("KomaMimic", "指定枚数戻る"), "Alt+Left");
  add("jumpPercentNext", QCoreApplication::translate("KomaMimic", "指定割合進む"), "<");
  add("jumpPercentPrevious", QCoreApplication::translate("KomaMimic", "指定割合戻る"), ">");
  add("scaleFill", QCoreApplication::translate("KomaMimic", "縦横どちらかいっぱいに合わせる"), "Z");
  add("boundaryCycle", QCoreApplication::translate("KomaMimic", "リピート状態を切り替える"), "F8");
  add("wheelScrollUp", QCoreApplication::translate("KomaMimic", "ホイールで上へスクロール"));
  add("wheelScrollDown", QCoreApplication::translate("KomaMimic", "ホイールで下へスクロール"));
  add("scaleEqualHeightToggle", QCoreApplication::translate("KomaMimic", "縦サイズ揃え／100%倍率固定"));
  add("scaleFitToggle", QCoreApplication::translate("KomaMimic", "ウィンドウに合わせる／100%倍率固定"));
  add("scaleEqualHeight", QCoreApplication::translate("KomaMimic", "縦サイズを揃える"));
  add("scaleFixed", QCoreApplication::translate("KomaMimic", "現在の倍率を固定する"), "V");
  add("swapMouseButtons", QCoreApplication::translate("KomaMimic", "マウスの左右ボタンを切り替える"), "",
      ShortcutScope::Global);
  add("readerContextMenu", QCoreApplication::translate("KomaMimic", "読書メニュー"), "Menu");
  add("timedList", QCoreApplication::translate("KomaMimic", "時間付きページリストを再生"));
  for (const QString kind : {"playlist", "favorite", "history"})
    for (int layout = 0; layout < 3; ++layout) {
      const QString title = kind == "playlist"   ? QCoreApplication::translate("KomaMimic", "プレイリスト")
                            : kind == "favorite" ? QCoreApplication::translate("KomaMimic", "お気に入り")
                                                 : QCoreApplication::translate("KomaMimic", "履歴");
      specs.append(
          {kind + QString::number(layout),
           title + QCoreApplication::translate("KomaMimic", "の表紙（%1）")
                       .arg(QStringList{QCoreApplication::translate("KomaMimic", "1画面"), QCoreApplication::translate("KomaMimic", "8列"), QCoreApplication::translate("KomaMimic", "カスタム")}[layout]),
           {},
           ShortcutScope::Global});
    }
  add("goPage", QCoreApplication::translate("KomaMimic", "指定ページへ"), "Ctrl+N");
  for (int count : {10, 20, 50}) {
    specs.append({"jumpNext" + QString::number(count),
                  QCoreApplication::translate("KomaMimic", "%1ページ進む").arg(count),
                  {},
                  ShortcutScope::Reader});
    specs.append({"jumpPrevious" + QString::number(count),
                  QCoreApplication::translate("KomaMimic", "%1ページ戻る").arg(count),
                  {},
                  ShortcutScope::Reader});
  }
  add("autoSpread", QCoreApplication::translate("KomaMimic", "自動見開き"), "Space");
  add("splitVertical", QCoreApplication::translate("KomaMimic", "左右分割"));
  add("splitHorizontal", QCoreApplication::translate("KomaMimic", "上下分割"));
  add("splitReset", QCoreApplication::translate("KomaMimic", "分割を解除"));
  add("insertBlank", QCoreApplication::translate("KomaMimic", "白紙ページを挿入"), "Insert");
  add("coverBlank", QCoreApplication::translate("KomaMimic", "先頭の空きページを切り替える"), "Num+0");
  add("scrollLeft", QCoreApplication::translate("KomaMimic", "左へスクロール"), "Ctrl+Left");
  add("scrollRight", QCoreApplication::translate("KomaMimic", "右へスクロール"), "Ctrl+Right");
  add("scrollUp", QCoreApplication::translate("KomaMimic", "上へスクロール"), "Ctrl+Up");
  add("scrollDown", QCoreApplication::translate("KomaMimic", "下へスクロール"), "Ctrl+Down");
  add("scrollNext", QCoreApplication::translate("KomaMimic", "スクロール兼ページ送り"), "PgDown");
  add("scrollPrevious", QCoreApplication::translate("KomaMimic", "スクロール兼ページ戻り"), "PgUp");
  add("loupe", QCoreApplication::translate("KomaMimic", "ルーペ"), "L");
  add("loupeLarger", QCoreApplication::translate("KomaMimic", "ルーペを大きく"));
  add("loupeSmaller", QCoreApplication::translate("KomaMimic", "ルーペを小さく"));
  add("loupeZoomIn", QCoreApplication::translate("KomaMimic", "ルーペを拡大"));
  add("loupeZoomOut", QCoreApplication::translate("KomaMimic", "ルーペを縮小"));
  add("scaleSpecified", QCoreApplication::translate("KomaMimic", "倍率を指定"), "B");
  add("scaleCycle", QCoreApplication::translate("KomaMimic", "スケールモードを切り替える"));
  add("fitWindow", QCoreApplication::translate("KomaMimic", "画像にウィンドウを合わせる"), "N");
  add("bookLists", QCoreApplication::translate("KomaMimic", "ブックリストを表示"), "Ctrl+B", ShortcutScope::Global);
  add("bookshelf", QCoreApplication::translate("KomaMimic", "本棚を表示"), "", ShortcutScope::Global);
  add("toc", QCoreApplication::translate("KomaMimic", "目次を表示"), "F7", ShortcutScope::Global);
  add("addToc", QCoreApplication::translate("KomaMimic", "目次に追加／削除"));
  add("nextToc", QCoreApplication::translate("KomaMimic", "次の目次項目"));
  add("previousToc", QCoreApplication::translate("KomaMimic", "前の目次項目"));
  add("addFavorite", QCoreApplication::translate("KomaMimic", "お気に入りへ登録"), "Ctrl+F");
  add("addPlaylist", QCoreApplication::translate("KomaMimic", "プレイリストへ登録"));
  add("addShelf", QCoreApplication::translate("KomaMimic", "本棚へ登録"));
  add("marker", QCoreApplication::translate("KomaMimic", "現在位置をマーク"), "Ctrl+M");
#ifdef Q_OS_MACOS
  add("gotoMarker", QCoreApplication::translate("KomaMimic", "マーカーへ移動"), "Meta+,");
#else
  add("gotoMarker", QCoreApplication::translate("KomaMimic", "マーカーへ移動"), "Ctrl+,");
#endif
  add("registerCover", QCoreApplication::translate("KomaMimic", "現在のページを表紙に登録"));
  add("registerSpine", QCoreApplication::translate("KomaMimic", "現在のページを背表紙に登録"));
  add("saveThumbnail", QCoreApplication::translate("KomaMimic", "表紙サムネイルを保存"));
  add("listDetails", QCoreApplication::translate("KomaMimic", "ブックリストの詳細・表示列"), "", ShortcutScope::Global);
  add("zipMetadataImport", QCoreApplication::translate("KomaMimic", "ZIPコメントから書誌情報を取り込む"));
  add("zipMetadataExport", QCoreApplication::translate("KomaMimic", "書誌情報付きZIPを保存"));
  add("listOpen", QCoreApplication::translate("KomaMimic", "ブックリストを読み込む"), "", ShortcutScope::Global);
  add("listSave", QCoreApplication::translate("KomaMimic", "ブックリストを保存"), "", ShortcutScope::Global);
  add("selectStart", QCoreApplication::translate("KomaMimic", "ページ選択の始点"));
  add("selectEnd", QCoreApplication::translate("KomaMimic", "ページ選択の終点"));
  add("selectRange", QCoreApplication::translate("KomaMimic", "ページ範囲を選択"));
  add("editCopy", QCoreApplication::translate("KomaMimic", "ページをコピー"), "");
  add("editCut", QCoreApplication::translate("KomaMimic", "ページを切り取り"), "");
  add("editPaste", QCoreApplication::translate("KomaMimic", "ページを貼り付け"), "");
  add("editDelete", QCoreApplication::translate("KomaMimic", "ページを削除（非破壊）"), "Delete");
  add("trashPages", QCoreApplication::translate("KomaMimic", "元画像をごみ箱へ移動"));
  add("editClear", QCoreApplication::translate("KomaMimic", "全ページを削除（非破壊）"));
  add("editUndo", QCoreApplication::translate("KomaMimic", "元に戻す"), "Ctrl+Z");
  add("editRedo", QCoreApplication::translate("KomaMimic", "やり直す"), "Ctrl+Shift+Z");
  add("addPages", QCoreApplication::translate("KomaMimic", "画像ファイルを追加"));
  add("imageCopy", QCoreApplication::translate("KomaMimic", "画像をクリップボードへ"), "Ctrl+C");
  add("imagePaste", QCoreApplication::translate("KomaMimic", "クリップボード画像を追加"), "Ctrl+V",
      ShortcutScope::Global);
  add("saveSpread", QCoreApplication::translate("KomaMimic", "現在の見開きを画像として保存"));
  add("saveImage", QCoreApplication::translate("KomaMimic", "加工した画像を保存"), "Ctrl+S");
  add("saveOriginal", QCoreApplication::translate("KomaMimic", "元の画像データを保存"));
  add("saveAll", QCoreApplication::translate("KomaMimic", "ページを一括保存"));
  add("saveZip", QCoreApplication::translate("KomaMimic", "ZIPに書き出す"));
  add("savePdf", QCoreApplication::translate("KomaMimic", "PDFに書き出す"));
  add("saveGif", QCoreApplication::translate("KomaMimic", "アニメーションGIFに書き出す"));
  add("quickSaveSettings", QCoreApplication::translate("KomaMimic", "クイックセーブの設定"), "", ShortcutScope::Global);
  add("print", QCoreApplication::translate("KomaMimic", "印刷"), "Ctrl+P");
  add("pageSetup", QCoreApplication::translate("KomaMimic", "ページ設定"), "Ctrl+U");
  add("slideForward", QCoreApplication::translate("KomaMimic", "ページを自動再生"), "F12");
  add("slideReverse", QCoreApplication::translate("KomaMimic", "ページを逆自動再生"));
  add("slidePause", QCoreApplication::translate("KomaMimic", "自動再生を一時停止"));
  add("slideStop", QCoreApplication::translate("KomaMimic", "自動再生を停止"));
  for (const auto &[id, label] :
       QVector<QPair<QString, QString>>{{"sortNatural", QCoreApplication::translate("KomaMimic", "自然順")},
                                        {"sortName", QCoreApplication::translate("KomaMimic", "名前順")},
                                        {"sortType", QCoreApplication::translate("KomaMimic", "種類順")},
                                        {"sortSize", QCoreApplication::translate("KomaMimic", "サイズ順")},
                                        {"sortDate", QCoreApplication::translate("KomaMimic", "更新日時順")},
                                        {"sortEntry", QCoreApplication::translate("KomaMimic", "格納順")},
                                        {"sortUnicode", QCoreApplication::translate("KomaMimic", "Unicode順")},
                                        {"sortReverse", QCoreApplication::translate("KomaMimic", "逆順")},
                                        {"sortRandom", QCoreApplication::translate("KomaMimic", "ランダム")}})
    specs.append({id, label, {}, ShortcutScope::Reader});
  add("preloadAll", QCoreApplication::translate("KomaMimic", "全ページをキャッシュ"));
  add("clearCache", QCoreApplication::translate("KomaMimic", "画像キャッシュを解放"));
  add("importMangaMeeyaSettings", QCoreApplication::translate("KomaMimic", "MangaMeeyaの設定を取り込む…"), "", ShortcutScope::Global);
  add("importSettings", QCoreApplication::translate("KomaMimic", "設定をインポート"), "", ShortcutScope::Global);
  add("exportSettings", QCoreApplication::translate("KomaMimic", "設定をエクスポート"), "", ShortcutScope::Global);
#ifdef Q_OS_MACOS
  add("preferences", QCoreApplication::translate("KomaMimic", "環境設定"), "Ctrl+,", ShortcutScope::Global);
#else
  add("preferences", QCoreApplication::translate("KomaMimic", "環境設定"), "Ctrl+Alt+,", ShortcutScope::Global);
#endif
  add("toolbarSettings", QCoreApplication::translate("KomaMimic", "ツールバーの設定"), "", ShortcutScope::Global);
  add("mouseSettings", QCoreApplication::translate("KomaMimic", "マウス・ジェスチャの設定"), "", ShortcutScope::Global);
  add("gamepadSettings", QCoreApplication::translate("KomaMimic", "ゲームパッドの設定"), "", ShortcutScope::Global);
  add("toolbarToggle", QCoreApplication::translate("KomaMimic", "ツールバー表示"), "", ShortcutScope::Global);
  add("openUrl", QCoreApplication::translate("KomaMimic", "URLを開く"), "", ShortcutScope::Global);
  add("external", QCoreApplication::translate("KomaMimic", "外部プログラムで開く"));
  add("externalSettings", QCoreApplication::translate("KomaMimic", "外部プログラムの設定"), "", ShortcutScope::Global);
  add("findPages", QCoreApplication::translate("KomaMimic", "ファイル名・ページ名の検索"), "Ctrl+Shift+F",
      ShortcutScope::Global);
  add("renameFile", QCoreApplication::translate("KomaMimic", "現在のファイル名を変更"));
  add("newWindow", QCoreApplication::translate("KomaMimic", "別ウィンドウを開く"), "Ctrl+Shift+N", ShortcutScope::Global);
  add("minimize", QCoreApplication::translate("KomaMimic", "ウィンドウを最小化"), "", ShortcutScope::Global);
  add("maximize", QCoreApplication::translate("KomaMimic", "ウィンドウを最大化"), "", ShortcutScope::Global);
  add("tray", QCoreApplication::translate("KomaMimic", "トレイに格納"), "Num+.", ShortcutScope::Global);
  add("imageInfo", QCoreApplication::translate("KomaMimic", "画像情報"), "Ctrl+I");
  add("cacheInfo", QCoreApplication::translate("KomaMimic", "キャッシュ情報"), "", ShortcutScope::Global);
  add("help", QCoreApplication::translate("KomaMimic", "操作説明"), "", ShortcutScope::Global);
  add("about", QCoreApplication::translate("KomaMimic", "KomaMimicについて"), "", ShortcutScope::Global);
  for (int i = 1; i <= 20; ++i) {
    specs.append({QString("profile%1").arg(i),
                  QCoreApplication::translate("KomaMimic", "画像加工プロファイル%1を読み込む").arg(i),
                  i<=10 ? QKeySequence(QString::number(i%10)) : QKeySequence{},
                  ShortcutScope::Reader});
    specs.append({QString("saveProfile%1").arg(i),
                  QCoreApplication::translate("KomaMimic", "画像加工プロファイル%1を保存").arg(i),
                  {},
                  ShortcutScope::Reader});
  }
  for (int i = 1; i <= 10; ++i) {
    specs.append({QString("settingsLoad%1").arg(i),
                  QCoreApplication::translate("KomaMimic", "設定スロット%1を読み込む").arg(i),
                  i<=4 ? QKeySequence(QString("F%1").arg(i)) : QKeySequence{},
                  ShortcutScope::Global});
    specs.append({QString("settingsSave%1").arg(i),
                  QCoreApplication::translate("KomaMimic", "設定スロット%1を保存").arg(i),
                  i<=4 ? QKeySequence(QString("Shift+F%1").arg(i)) : QKeySequence{},
                  ShortcutScope::Global});
  }
  for (int i = 1; i <= 8; ++i)
    specs.append({QString("userCommand%1").arg(i),
                  QCoreApplication::translate("KomaMimic", "ユーザコマンド%1").arg(i),
                  {},
                  ShortcutScope::Reader});
  for (int i = 1; i <= 4; ++i)
    specs.append({QString("quickSave%1").arg(i),
                  QCoreApplication::translate("KomaMimic", "クイックセーブ%1").arg(i),
                  {},
                  ShortcutScope::Reader});
  return specs;
}
