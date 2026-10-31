# 設定・読書データ・キャッシュ

日本語 | [English](../en/user/settings.md)

## 設定と読書データ

通常起動ではOS標準の設定場所に `settings.ini` を保存します。
同じフォルダの `library.sqlite3` に本棚・履歴・読書状態などを保存します。

| OS | 通常の設定ファイル |
|---|---|
| Windows | `%LOCALAPPDATA%\KomaMimic\KomaMimic\settings.ini` |
| macOS | `~/Library/Preferences/KomaMimic/KomaMimic/settings.ini` |
| Linux | `~/.config/KomaMimic/KomaMimic/settings.ini` |

Linuxの `XDG_CONFIG_HOME` を指定している場合は `~/.config` の部分が変わります。
コマンドラインの `--config /path/to/settings.ini` で別の設定を指定できます。
旧配置を使っていた場合は、起動時の移行によって場所が変わる場合があります。

バックアップはアプリを終了して設定フォルダごとコピーします。

![settings-settings-ja.png](../assets/screenshots/settings-settings-ja.png)

## キャッシュ

「環境設定 → キャッシュ」でメモリの先読みとサムネイルのディスク保存を設定します。
サムネイルのディスク保存は既定で有効、上限512 MiBです。設定画面から削除できます。
サムネイルは再生成できますが、設定や読書履歴の代わりにはなりません。

| OS | 通常のサムネイルキャッシュ |
|---|---|
| Windows | `%LOCALAPPDATA%\KomaMimic\KomaMimic\cache\thumbnails-v1` |
| macOS | `~/Library/Caches/KomaMimic/KomaMimic/thumbnails-v1` |
| Linux | `~/.cache/KomaMimic/KomaMimic/thumbnails-v1` |

Linuxの `XDG_CACHE_HOME` を指定した場合は `~/.cache` の部分が変わります。
これらはQtの[OS別保存場所](https://doc.qt.io/qt-6/qstandardpaths.html#StandardLocation-enum)を使います。
別の `--config` を指定しても、OS標準のキャッシュ場所は独立しています。
メモリキャッシュの値はアプリ全体の使用メモリ上限ではありません。

![settings-cache-ja.png](../assets/screenshots/settings-cache-ja.png)

### 再利用と再生成

保存済みの正常なサムネイルPNGは、別の処理による保存・整理ロックやキャッシュ全件の一覧走査を待たずに読み込みます。
使用時刻の更新は可能なときに行います。保存・削除・容量整理には引き続きロックを使用し、PNGと管理情報は一時ファイルから置き換えます。
生成した画像を保存するロックが取れない場合は、画像を表示してディスク保存を省略することがあります。
この場合、次回は再生成する場合がありますが、他の保存済みPNGを無効にする操作ではありません。

容量上限に加えて最大20,000枚まで保持し、古いものから整理します。
アプリ・Qt・画像readerの変更、元ファイルのパス・サイズ・更新日時、名前の文字コード等が変わると、対応するキャッシュは再生成します。
表示するサムネイルの寸法・間隔等を変えるだけでは全キャッシュを削除しません。
使用量表示には以前の版等のPNGも含まれるため、「ファイルが残っている」ことと「現在の条件で再利用できる」ことは同じではありません。

異常終了時の未完成の一時ファイルは、完成済みPNGとして読みません。残ったロックはQtによる所有プロセス等の判定で回収します。
v1.1.0ではロック競合による不要な再生成を修正していますが、過去に報告された「異常終了後に全体が再生成された」現象の原因すべてを特定したものではありません。
フォルダ表紙の候補探索や書庫を開く際のページ索引の読込は、サムネイルの再生成とは別の処理です。
TXT・Markdownやテキストを含む書庫の表紙等は、フォントや外部画像への依存を避けるためディスク保存の対象外になる場合があります。

## 配色と言語

環境設定の一般でシステム追従・ライト・ダークと、日本語・English・システム設定を選べます。
UI言語は保存後の再起動で反映します。配色モードは本文や画像の設定色を置き換えません。

詳細は[MangaMeeyaとの違い](compatibility.md)を参照してください。
