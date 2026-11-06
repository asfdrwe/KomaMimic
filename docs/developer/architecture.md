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
image processing and EPUB cover extraction. 保存済みサムネイルは仮表示できるが、元データを使う処理は変更検証を省略しない。

## Boundaries to preserve

- Keep Book's logical page sequence, display names, original bytes, and archive entries distinct.
- Separate document text positions from image page numbers and spread settings.
- Keep text-field editing separate from the scope of reading shortcuts.
- Do not expose QObject or arbitrary filesystem, network, or process capabilities to JavaScript.
- Settings and history are runtime data. Tests use temporary profiles.

See the [specification](specification.md) for limits and [automated tests](testing.md)
for verifying changes.

## 表示要求とキャッシュ保存

`displayjobs.*`はGUIスレッドの要求キューと、各1workerの7系統を所有する。

| 系統 | 役割 | 進行条件 |
|---|---|---|
| Cache | 可視の保存PNG／登録画像の読込 | 最優先。結果をGUIへ渡すまで他系統の新規実行を保留 |
| Render | 読書モードの現在画像の描画 | 移動前のサムネイル生成workerから独立 |
| Navigation | 現在フォルダの一覧取得 | 移動前の生成workerから独立 |
| Display | 可視missの生成、Book open | 画面外生成を降格 |
| Update | 可視画像の検証・差替え、兄弟一覧、使用量表示 | 現在の一覧取得と可視missが終わり、静穏になってから開始 |
| Store | PNG保存、明示削除 | 専用worker。キャッシュ表示・現在の一覧取得・可視missを優先 |
| Background | 画面外になった要求済み生成、先読み、整理 | 現在の一覧取得と可視missが終わり、静穏になってから開始 |

Update／Store／Backgroundのthread priorityは低くする。待機時間の長さで現在表示の要求を
追い越すことはない。OS I/Oやcodecを含む表示時間の上限は保証しない。native codec／Poppler
呼出中は直ちに取消できないことがある。

画面外になった主一覧・本棚等の要求は、`visible(scope, ids)`でBackgroundへ降格し、
再可視化で元の優先度へ戻す。実行中に移動した仕事は協調取消後に再実行する。
画面世代はGUIへの適用を制御し、有効なキャッシュ生成の寿命とは分ける。
明示取消には`retain`、終了には`stop`を使う。取得エラーは1秒後に再試行し、
その間は同じlaneの別要求を進める。未閲覧の全ファイルを巡回する仕組みではない。

フォルダ一覧は列挙と必要なソートの後、200件単位でGUIへ追加する。名前順でも全対象の
size/mtimeを取得していた前処理を廃止し、兄弟一覧は独立したUpdate要求にした。
主グリッドと本棚等は可視の全対象をCacheへ登録し、missだけDisplayへ送る。
主グリッドで生成・検証が実行中でも、未表示の項目のCache要求を抑止しない。
文字コード設定のsnapshotから既存キャッシュの候補キーを構築し、Cache投入前に
元ファイルのsize／mtimeを照会しない。元ソースに対応する設定の確定は生成・検証workerへ送る。
正常な既存画像は変更検証より先に表示し、表示中は2秒間隔を目安に再検証する。
エラー時は既存画像を保ち、表紙なしと読取失敗を区別する。

ページPNGは安定したパス・ページ識別子と描画条件で検索し、鮮度stampはPNG内に持つ。
従来のfingerprintキーはBookを開いた時点のsnapshotで検索し、利用可能な旧PNGを移行する。
Cache workerでは元ファイルのstatや書庫走査を行わない。検証時に変更を検出したらBookを
開き直し、名前／重複entry識別子と文書ページ等を照合してから生成する。同じ古いBookからの
再検証では開き直したBookを共有し、各ページごとの再openを避ける。

フォルダ表紙の検証は候補を自然順に選び直す。保持候補は各フォルダ16件で、列挙件数の
上限ではない。元画像のPNGが整理済みでも鮮度の一致するフォルダPNGを再利用する。
表紙がなくなった場合や保存対象外のテキスト表紙へ変わった場合は旧フォルダPNGを無効化する。
再帰要求全体は同じキャッシュ削除世代を使い、削除前の結果を新世代へ保存しない。

遅延保存の画像RAMと直近画像RAMは各32 MiB。QImageの共有領域は重複保持し得る。
使用時刻更新は省略可能だが、画像はRAM上限で捨てず、`thumbnails-v2/pending/`の
原子的に保存したPNGへ退避する。保存中の画像にもRAMの予約枠を残し、再enqueueで上限を
超えたり仕事を失ったりしない。通常lookupはinventoryや保存mutexを待たない。
保存前検証・commit・容量整理はStoreで実行する。書込失敗／取消は再試行対象に残し、
ソース失効による破棄とは別の結果として扱う。容量変更や無効化でpendingを消さない。

通常終了ではworkerの取消完了後に未保存画像をpendingへ退避し、次回起動のStoreで再開する。
pendingには再開用キーと画像があり、元データのコピーやユーザー文書は保存しない。
保存前の検証関数は再起動を跨げないため、復元画像も仮表示として再検証する。
明示clearはpendingも削除する。容量上限は完成済みPNGの合計に適用し、pendingと
QSaveFileの一時ファイルは追加領域を使用する。ディスク満杯や書込権限エラーはログへ記録する。
突然の電源断までRAM上の全生成結果が永続化済みになる保証ではない。

`komamimic.display.debug=true`はlane・scope・要求ID・待機／実行時間、
`komamimic.cache.debug=true`はlookup・保存等を記録する。格納先は引き続き
`thumbnails-v2`。キー変更はページと登録ページ表紙にあり、通常表紙のキーは維持する。
