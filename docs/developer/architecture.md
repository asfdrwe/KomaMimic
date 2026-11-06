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
Cacheの結果配送が終わるまで後続仕事の新規開始を保留し、現在表示の要求が残る間は
更新検査・保存を開始しない。読書はRenderによる現在画像、Backgroundによる先読み、
Storeによる完成済みサムネイルの保存の順とする。

`visible(scope, ids)`と`retain`は不要になった要求を取り消す。旧生成を画面外へ降格して
再開しない。協調取消が返らないnative処理は旧executorに残し、現在対象は新しいexecutorで
開始する。旧結果のGUI適用を世代と要求IDで防ぎ、終了時には残るworkerも待ち合わせる。
元媒体・codec自体の所要時間に上限を保証する仕組みではない。

現在一覧は列挙・ソート後に200件ずつGUIへ反映し、そのとき取得したファイル情報を再利用する。
文字コード設定のsnapshotと従来キーから保存PNGを検索し、Cache投入前に元ソースの
size／mtime照会・書庫open・候補探索を挟まない。ページは安定キーと旧fingerprintキーの
両方を読める。通常表紙を含む既存`thumbnails-v2`を維持し、一括削除・形式移行を行わない。

### 更新の契機と探索範囲

`ThumbnailChanges`が取得済み一覧の差分と対象別の変更通知をRAMで管理する。
キャッシュhit、可視化、再訪、再起動、時間経過だけでは検査しない。周期検証は行わない。
既存PNGのstamp／保存日時と一覧の情報を比較できる範囲で変更を判断し、具体的な兆候がある
対象だけUpdateで確認する。同じ兆候で無期限に再試行しない。既存形式から分からない深部の
履歴は追加照会で補わず、保存画像を有効とする。F6は明示再取得の例外である。

フォルダ表紙はファイル優先・自然順で候補を選ぶ。保持候補は各階層16件で、列挙件数の
上限ではない。`thumbnails/folderDepth`は0～8、既定0。Aを開いてBの表紙を出す場合、
0でもB直下を列挙し、B/Cには進まない。1ならC直下まで進む。深度打ち切りでは既存PNGを
無効化しない。空結果や読めない候補はRAMに保持し、新しい兆候・設定変更・F6まで反復しない。

Bへ候補を追加しても採用元のstampが既存表紙と一致すれば、元ファイルの再照会・本体読込・
生成・PNG再保存を行わない。処理済み通知と探索中の一覧はRAMだけに置き、新しい管理ファイルや
PNGメタデータを追加しない。採用元が変わる場合は新候補のキャッシュ、なければ生成へ進む。
設定範囲内で表紙対象の消滅を確認した場合だけ空表示へ反映する。一時的な失敗では旧画像を保つ。

読書中はサムネイル生成・検証・隠れた表紙一覧の走査・容量集計を開始しない。
次巻操作で必要になった近隣一覧は、既にあるRAM情報を使い、不足時に明示操作のNavigationで取得する。

### 完成画像の保存と保持

`ThumbnailDiskCache`は画像を表示へ返し、保存担当へ完成画像を渡す。保存時の元ソース再検査は
行わない。画像RAMと直近画像RAMは各32 MiB。保存待ち画像は上限で捨てず、既存の
`thumbnails-v2/pending/`へ原子的に退避する。通常lookupはinventory／保存mutex待ちから独立する。
保存担当はキー別の世代を確認し、遅れて終わった旧生成・旧保存による上書きを防ぐ。
QSaveFileで新PNGを確定するまで既存PNGを保ち、保存失敗・取消は再試行する。

容量上限は完成済みPNGの合計に適用する。超過時だけ保存日時が古い順（FIFO）に削除し、
読込ではPNG日時を更新しない。既存PNGのmtimeは作成日時の近似として引き継ぐ。
未変更画像を期限・未使用期間・件数・表示条件変更だけで消さない。
通常終了時はworker停止後に未保存画像をpendingへ退避し、次回Storeで再開する。
pendingと保存中の一時ファイルには完成済み容量とは別の領域が必要であり、書込不能や
退避前の異常終了まで永続化を保証しない。明示clearはpendingも削除する。

`komamimic.display.debug=true`でlane・scope・要求ID・待機／実行時間、
`komamimic.cache.debug=true`でlookup・保存・容量整理等を診断する。
