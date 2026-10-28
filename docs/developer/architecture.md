# 基本構造

KomaMimicはQt WidgetsのGUI、読込・編集を担うコア、JavaScriptを処理する
別実行ファイルから構成されます。文書表示にはQt WebEngineを使用します。

| 領域 | 入口 | 役割 |
|---|---|---|
| 起動・CLI・IPC | `src/main.cpp`、`startup.cpp` | 引数、設定、既存ウィンドウへの転送 |
| 本・書庫 | `book.*`、`archivepath.*`、`archivenames.*` | 入力の識別、ページ列、名前・文字コード |
| GUI | `mainwindow*`、`pageview.*`、`thumbnailgrid.*` | 一覧、読書、メニュー、全画面 |
| 操作 | `shortcuts.*`、`readerfeatures.*`、`readerinput.*` | 操作ID、キー・マウス・ゲームパッド |
| 文書 | `documentsource.*`、`publication.*`、`documentview.*`、`documentsettings.*` | TXT/Markdown/EPUB、資源、組版、読書位置 |
| 画像・出力 | `imageprocessing.*`、`bookexport.*`、`media.*`、`pdfsource.*` | 加工、アニメーション、保存、PDF |
| スクリプト | `scriptreader.*`、`scriptworker.cpp`、`scripts/readers/` | 登録、検査、workerとの通信、API 1 |
| 永続化 | `librarystore.*`、`thumbnailcache.*` | 本棚・履歴、サムネイルのディスク保存 |
| OS連携 | `idleinhibitor.*`、`power_*`、`inhibitprotocol.*` | 読書中の自動消灯抑制 |
| 移行 | `legacysettings.*`、`legacysettingsdialog.cpp` | INIの変換計画、確認、バックアップ |
| 配布 | `tools/`、`cmake/` | ソース選別、検証セット、runtime配置 |

## 入力から表示まで

GUIからの要求は本の識別とページ選択を経て、画像読込または文書解析へ進みます。
画像はnative codec、PDF、またはscript workerで復号し、表示用加工とキャッシュへ渡します。
TXT/Markdown/EPUBはPublicationと文書設定を使ってWebEngineへ表示します。

非同期処理は世代番号と取消を確認し、古い本の結果で現在の表示を更新しません。
サムネイルは本体の画像処理やEPUB表紙抽出と共通の識別を使います。
キャッシュhitでも原本の変更検査を迂回させないことが必要です。

## 変更時に維持する境界

- Bookの論理ページ列・表示名・原本byte列・書庫entryを区別します。
- 文書の本文位置と画像のページ番号・見開き設定を分離します。
- 入力欄の操作と読書用ショートカットの有効範囲を分離します。
- JavaScriptへQObject・任意のfilesystem/network/process能力を公開しません。
- 設定・履歴は実行時データです。テストは一時プロファイルを使用します。

具体的な制限は[仕様](specification.md)、変更結果の確認は[自動テスト](testing.md)を参照してください。
