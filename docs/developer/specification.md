# Specification and implementation boundaries

## Controls and persistence

Action IDs and storage keys are independent of display names and translations.
Preserve common, reading, thumbnail, and tree assignment scopes without conflicting
with text-field editing. Return confirms a list selection. The default full-screen
shortcut is F11 on Windows/Linux and Control+Command+F on macOS.

Apply the manga-oriented defaults only to empty settings: spreads, right-to-left
opening, and a separate cover page are enabled; automatic spreads and image
processing are off; preloading is one display unit ahead and zero behind. Existing
settings retain their saved values and compatible defaults. There is no custom top
bar; use the standard menu and bottom reading toolbar. See `defaultpreferences.*`,
`shortcuts.*`, and the [basic user guide](../site/en/user/basic.md).

## Images, archives, and output

Images use installed Qt plugins and bundled script readers; archives use libarchive;
PDF uses Poppler GLib/Cairo. Loading is not delegated to external 7z/lha/unrar or
pdfimages commands. Unicode names take priority. ZIP/LHA entries without Unicode
information default to CP932. See [supported formats](../site/en/user/formats.md).

Logical edits and display processing normally leave original inputs unchanged;
saving and exporting produce the results. Renaming real files and moving them to
the trash are separate actions. PDF rendering and image extraction are serialized,
releasing documents and pages after each operation. Raster PDF output does not
preserve original text or links.

PDF limits are 100,000 pages and 48 megapixels of output. The original-size baseline
has a maximum edge of 2400 px; rerendering for zoom or DPI has a maximum edge of
6144 px. These do not limit memory used by Poppler's internal parsing.

## 表示・サムネイルキャッシュ（v1.5.0）

現在の表示を最優先する。表紙・ページ一覧・本棚等は、保存済み画像があれば元データを
検査せず表示する。missだけを生成し、表示を保存完了待ちにしない。利用ごとの検証・周期検証は
行わず、取得済み一覧の差分や関連する変更通知がある対象だけを後続の更新確認へ送る。

- 一覧で得た属性は再利用し、未変更の画像・ZIPを確認のためだけに再openしない。
- ファイルのシンボリックリンクも、一覧とBookでリンク先の実パスを同じ規則で記録し、表示モードの切替だけで再生成しない。旧リンク表紙の判定値も照合し、未変更の保存PNGは書き換えない。ページの更新確認で開き直した後も観測情報と一致しない場合は、その要求をエラーで終了し、既存PNGを保持する。
- フォルダへの追加後も採用元と属性が同じなら、再生成・PNG再保存・保存日時更新をしない。
- フォルダ表紙の追加探索深度は0～8、既定0。A内のBを表示する場合、0でもB直下を探し、B/Cへ進まない。候補はファイル優先・自然順で決める。打ち切りや深度縮小で既存画像を失効させない。
- 移動・モード切替・画面外化で不要になった未完了要求は取消し、旧要求を自動再開しない。完成済みの保存待ち画像は保持する。
- `thumbnails/preloadAll`は既定false。明示有効時だけ、現在一覧の可視処理完了後に別のBackground要求で残りを1件ずつ取得する。スクロールでは先読み要求を取消し、可視優先でidle後に未完了位置から新しく要求する。フォルダ・Book・モード移動でも取り消し、モードに入り直すと現在一覧を新しく走査する。容量で解放した項目をidleごとに再取得しない。
- 有効時は表示アイコンも画面外で保持する。`thumbnails/memoryMegabytes`は既定128、16～2048 MiB。現在一覧の非表示元サムネイルとアイコンの画素量を計上し、古い非表示分から解放する。表示中の画像・他用途cache・Qt overheadは別。無効化と非表示モードでは通常の件数制限へ戻す。既存のPNGキー・namespace・永続形式は変更しない。
- 読書は現在表示、次に先読み、最後に完成済みサムネイルの保存。余計なサムネイル生成・検証・隠れた一覧走査を開始しない。
- 保存は元ソースへ戻らず完成画像を原子的に確定する。旧PNGを先に削除せず、失敗・競合時は保存を再試行する。通常終了時の未保存画像は既存pending形式へ退避する。
- 保存PNGの読込失敗はcache missとして扱う。pendingが読めなければ確定済みPNGを使い、どちらも読めなければ必要な画像を生成する。読込のために保存lockを待たない。
- 容量超過時と上限引下げ時は保存日時が古い順に整理する。読込で日時を変更せず、期限・未使用期間・件数だけでは削除しない。
- 既存`thumbnails-v2`とキー読込の互換性を保持し、新しい永続依存情報・処理済み管理ファイルを追加しない。深部の履歴が不明な場合に無制限の探索で補わず、必要時はF6を使う。

詳細は[利用者向けキャッシュ仕様](../site/user/settings.md)と
[実装構造](architecture.md)を参照。

## Documents

TXT, Aozora Bunko ruby annotations, Markdown, and EPUB share a document view.
Display settings take precedence in this order: per book, per format, global. Text
positions are saved and restored after reflow. TXT/Markdown documents inside an
archive have separate identities for listing and reading-position storage.

Resources are served through a temporary WebEngine profile with a per-book
`koma-document://` origin. External communication, file URLs, and downloads are
blocked. Book scripts are removed; iframe sandboxing and CSP are used. Distinguish
the application's layout JavaScript from the book's content. Markdown images are
restricted to the document directory and checked using canonical paths. DRM-encrypted
text and images are diagnosed as unsupported rather than decrypted.

Document page thumbnails, image processing, page editing, raster export, and printing
are outside this view's scope. Font-size changes do not affect fixed layouts or text
embedded in images.

## Scripts and OS integration

The public script specification is [API 1](../site/en/scripts/api.md). Each worker
uses a fresh QJSEngine, with checks on deadlines, inputs, and outputs. This is not a
general sandbox that protects the entire operating system.

The Windows application uses the GUI subsystem and does not create a console.
When started from a terminal, it preserves inherited output handles and attaches
to the parent console when needed. The worker uses pipes and `CREATE_NO_WINDOW`.
Connecting to an existing window uses bounded retries, including waiting for the
server to become available.

Automatic display-sleep inhibition is off by default. It is acquired only while
reading in the foreground, and released on minimizing, switching to another app,
returning to a list, or exiting. It uses Windows power APIs, IOKit on macOS, and an
available D-Bus method on Linux. On macOS it targets automatic display sleep. See
[native platform validation](platform-validation.md) for checking actual behavior.

## 読書開始と一括ロード

Readerのopenはページ順・編集・再開位置を確定して現在画像を1回だけ要求する。
非表示ページ項目やJSON snapshotはサムネイル表示まで準備しない。ファイル列挙時の
属性は保持し、後から新しい属性を古いBookのsnapshotへ混ぜない。
Coversの走査・GUI分割配送も離脱で取り消し、Reader開始のために親一覧を走査しない。

`preloadAll`は保存オプション`cache/bundleLoad`を切り替えるGlobal action。
既定offで、MangaMeeyaの`Config/BundleLoad`を取り込む。Readerの最初の描画後、
通常先読みが終わってからrawファイルを一件ずつ保持する。Pages/Coversでは読まない。
`cache/bundleMegabytes`は16～2048 MiB、既定128 MiB。現在表示はこの保持の完了を待たない。
取消でも完成済みデータは同Bookで再利用し、オフ・キャッシュ解放で破棄する。
通常ファイルキャッシュ・復号画素キャッシュとサムネイルPNGの形式は別。

nested Bookは展開した内部書庫の一時ファイル寿命を保持し、個々の画像は参照で遅延読込する。
フォルダ内書庫もsource Book参照を使う。ZIPは構造的な中央情報の検査を保ち、
局所local headerエラーを対象へ分離する。TIFF/scriptのページ数確定に必要な読込は残す。
アニメーションは現在ファイルのフレーム列を上限内で準備し、途中取消を確認する。

履歴writeはRAMのpending revisionを可視読取へ重ね、描画後のStore laneで独立DB接続へ保存する。
取消・失敗は未保存として保持し、ackは同revisionだけを消す。明示DB操作はepochを更新し、
旧writeの再適用を防いでからflushする。通常終了時にも残りをflushする。

通常のページ・表紙PNGのキーは変更しない。旧nested/folderContainersのページPNGは
inlineデータのhashをキーに含むため、現在キーがmissした場合だけ、その表示対象のデータから
旧キーを復元してlookupする。cache-only lookupは元データを読まない。旧PNGが見つかれば
再復号・再保存せず利用する。旧PNGの全削除・一括変換や新しい永続対応表は導入しない。


## 文書の現在表示・組方向・挿絵

ReaderのOpenOptions.deferDocumentsは有効。検査・独立preview向けの既定APIは従来どおり
全文検査を行い、ReaderはEPUBのZIP inventory／OPFを先に解析し、現在sectionの本文と
リソースを要求時に取得する。元ファイルの属性と名前・重複・形式・容量・暗号化の検査は維持。
raw resource cacheは32 MiB、解析済みsectionは16 MiBに制限する。取消はinventory、
resourceのchunk読込、XML走査へ伝播する。WebEngineのresource供給もGUI threadで
読込・変換せず、jobとviewの寿命に従う取消付きworkerへ渡す。

文書controllerは未組版sectionを仮のlocationとして保持し、現在sectionと現在の見開きに
必要な相方だけを組版する。ページ表はrevisionで同期し、未確定の総数は「…」。表示に
不要なiframeは外し、現在表示と直近のsectionだけを残す。固定・可変の属性を区別し、
有効な組方向・見開き順序・左右指定・wide/soloを同じページ表で扱う。可変本文への
縦／横指定は読む順序にも適用し、文書に従う場合は明示的なEPUB指定、なければ本文組方向。
固定layoutの組方向は上書きしない。

EPUBのcoverAloneは既定true。表紙文書・表紙画像と本文sectionの参照から識別し、
識別された表紙を単ページ枠で中央にfitする。出版社のsoloは別属性。本文順序にない表紙は
挿入しない。EPUBもメタデータのopen後に文書viewへ直接入り、最初の対象sectionを要求する。
再開位置が本文なら表紙を挟まない。表紙の識別・単ページfitは文書view内で維持する。

青空のWindows相対区切りを正規化して既存の範囲検査に通す。変換したTXT/Markdownの
画像だけのblockは別sectionとし、captionを保持する。canonical locationはtext.htmlと
元のoffsetを維持するため、画像sectionの追加で履歴の本文位置を別の意味へ変えない。
standalone画像は実寸で横長かを判定し、wideImages有効なら単独全幅。混在画像は本文内。
Readerの画像は要求時にdecode／PNG化し、合計容量と32 MiB cacheを制限する。

前後の本への操作は読込中も既知の隣接情報または明示移動時の走査で受理する。直前の
文書view／Book／locationを1冊だけ保持し、元ファイルのサイズ・時刻と文書設定が同じ場合
に復帰する。古いviewの通知は現在viewへ適用しない。強制再読込は現在viewを再生成する。
