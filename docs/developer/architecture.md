# Architecture

KomaMimic consists of a Qt Widgets GUI, a core that loads and edits books, and a
separate executable that runs JavaScript. Document rendering uses Qt WebEngine.

| Area | Entry points | Responsibility |
|---|---|---|
| Startup, CLI, IPC | `src/main.cpp`, `startup.cpp` | Arguments, settings, forwarding to an existing window |
| Books and archives | `book.*`, `archivepath.*`, `archivenames.*` | Input identification, page sequences, names and encodings |
| GUI | `mainwindow*`, `pageview.*`, `thumbnailgrid.*` | Lists, reading, menus, full screen |
| Controls | `shortcuts.*`, `readerfeatures.*`, `readerinput.*` | Action IDs, keyboard, mouse, gamepad |
| Documents | `documentsource.*`, `publication.*`, `documentview.*`, `documentsettings.*` | TXT/Markdown/EPUB, resources, layout, reading position |
| Images and output | `imageprocessing.*`, `bookexport.*`, `media.*`, `pdfsource.*` | Processing, animation, saving, PDF |
| Scripts | `scriptreader.*`, `scriptworker.cpp`, `scripts/readers/` | Registration, validation, worker communication, API 1 |
| Persistence | `librarystore.*`, `thumbnailcache.*` | Bookshelves and history, thumbnail disk storage |
| OS integration | `idleinhibitor.*`, `power_*`, `inhibitprotocol.*` | Preventing automatic display sleep while reading |
| Migration | `legacysettings.*`, `legacysettingsdialog.cpp` | INI conversion plans, review, backup |
| Distribution | `tools/`, `cmake/` | Public source selection, validation kits, runtime deployment |

## From input to display

A GUI request identifies the book and selects pages, then loads images or parses a
document. Images are decoded by native codecs, the PDF backend, or a script worker,
then passed to display processing and caching. TXT/Markdown/EPUB use Publication and
document settings to render in WebEngine.

文書ビュー内の`DocumentSurface`は専用のnative子ウィンドウを持ち、WebEngineの
RHI合成をその領域に収める。外側のDocumentViewとモード切替領域は通常のwidgetを
維持し、表紙・ページ一覧と画像読書ビューの描画面を変更しない。現在の文書と
直前の保持文書の両方に同じ境界を設ける。Qt 6.8以降を必要とする。

Asynchronous work checks generation numbers and cancellation so results for an old
book cannot update the current display. Thumbnails share identification with core
image processing and EPUB cover extraction. 保存済みサムネイルは原則そのまま有効とし、利用ごとの元データ検証は行わない。

## Boundaries to preserve

- Keep Book's logical page sequence, display names, original bytes, and archive entries distinct.
- Separate document text positions from image page numbers and spread settings.
- Keep text-field editing separate from the scope of reading shortcuts.
- Do not expose QObject or arbitrary filesystem, network, or process capabilities to JavaScript.
- Settings and history are runtime data. Tests use temporary profiles.

See the [specification](specification.md) for limits and [automated tests](testing.md)
for verifying changes.

## 表示要求とキャッシュ保存

`displayjobs.*`がGUI要求とCache／Navigation／Render／Display／Update／Background／Storeの
7系統を管理する。現在の可視項目はCacheから読み、missだけDisplayで生成する。
Cacheの結果配送が終わるまで不足画像の生成を保留する。生成済み画像は保存前から
メモリで表示でき、生成workerが通常のPNGへ直接保存する。読書はRenderによる現在画像、
Backgroundによる設定範囲の先読みを優先し、非表示一覧の生成を開始しない。

`visible(scope, ids)`と`retain`は不要になった要求を取り消す。旧生成を画面外へ降格して
再開しない。協調取消が返らないnative処理は旧executorに残し、現在対象は新しいexecutorで
開始する。旧結果のGUI適用を世代と要求IDで防ぎ、終了時には残るworkerも待ち合わせる。
元媒体・codec自体の所要時間に上限を保証する仕組みではない。

現在一覧は列挙・ソート後に200件ずつGUIへ反映し、そのとき取得したファイル情報を再利用する。
文字コード設定のsnapshotと従来キーから保存PNGを検索し、Cache投入前に元ソースの
size／mtime照会・書庫open・候補探索を挟まない。ページは安定キーと旧fingerprintキーの
両方を読める。通常表紙を含む既存`thumbnails-v2`を維持し、一括削除・形式移行を行わない。

### 更新の契機と探索範囲

`ThumbnailChanges`は初回一覧と、所属フォルダの変更通知で更新したファイル属性を保持する。
画像は画像ファイル、ZIPはZIPファイル自体の更新日時がキャッシュ作成日時より新しい場合だけ
自動再生成する。フォルダ自身のmtimeや内容の照合は使わない。確認用の元画像読込・ZIP再open・
周期検査・再帰走査は行わない。判定情報がない場合は既存画像を使い、必要時はF6で再取得する。
フォルダ表紙は自然順・ファイル優先で候補を選び、設定された追加深度0～8の範囲だけを探索する。
画像がない対象は所属フォルダの変更通知や設定変更後に再探索できる。

### 完成画像の保存と保持

`ThumbnailDiskCache`は完成画像をメモリへ公開してから、生成worker内で通常のPNGへ直接保存する。
表示workerは`thumbnails/concurrency`（1～4件、既定1件）に従って可視サムネイルを生成する。設定変更後の開始件数を制限し、実行中の処理や既存キャッシュは破棄しない。読書画像・先読みの並列数は変更しない。キャッシュ読出しには独立したworkerがあり、
メモリ読出しは保存mutex・容量集計・元データ確認を待たない。書込中の画像もメモリから使え、
通常の復号済みLRUは32 MiBに制限する。遅れて完成した旧生成で新画像を上書きしない。
QSaveFileが新PNGを確定するまで旧PNGを保持する。別の保存退避・復旧・終了時退避・再試行queueはない。
フォルダ表紙では採用元のパスをPNGに保持し、フォルダ通知時の一覧属性と対応付ける。
旧PNGに採用元情報がなくても画像をそのまま使い、変更確認のために元データを開かない。
容量を超えた場合は保存日時順に整理し、読出しでPNG日時を変更しない。

`komamimic.display.debug=true`でlane・scope・要求ID・待機／実行時間、
`komamimic.cache.debug=true`でlookup・保存・容量整理等を診断する。
