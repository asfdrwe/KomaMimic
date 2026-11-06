# Changelog

## 未リリース

- 多数の親フォルダから子へ移動した際の表紙表示を優先。現在の一覧取得を旧生成から分離し、キャッシュ要求前の元ソース属性照会と検証中のキャッシュ抑止を除去。検証・保存・画面外処理は現在の表示要求後に実行。

- Cache／Display／Update／Store／Backgroundの5系統へ分離。可視要求の降格・再開、エラーの遅延再試行、表示済み画像の定期検証を追加。ページの安定キーと旧キー読込、変更時のBook再open・entry照合を実装。
- 保存待ちをpending PNGへ退避し、通常終了・次回起動を跨いで再開。書込失敗／取消／ソース失効を区別し、80枚全件保存・故障注入・前景負荷・本棚hit・ページ差替え等の自動試験を追加。

- フォルダ表紙の安定パスキー保存とCache laneでの先行表示を追加。Update laneで既存の候補選択と元ファイル検証を行い、内部画像PNGが失われても一致するフォルダ表紙は再生成しない。
- 遅延保存キューでは使用時刻更新より画像を優先。使用時刻更新が未保存画像を押し出す問題を修正。再帰するフォルダ要求全体でキャッシュ削除の世代を共有する。
- 別プロセス再利用・階層内変更・保存前変更・F6・競合・高速スクロールの試験を追加／維持。

## v1.4.3

- 書庫一覧から実ファイルを開いた後の試験で、読込・描画完了とbookPathを確認し、表紙一覧のPathRole集合が期待する物理ファイルへ置換されるまで待つ。件数を正確に2件とし、書庫内パスが残らない厳密な比較を維持。
- 一覧のreadyフラグ・期待パス・実際の全パスを失敗診断へ追加。
- ArchiveTextTestsのmainで文書resourceとURLスキームをQApplication生成前に初期化。他の文書試験と同じ起動条件へ揃える。
- v1.4.2は欠番とし、版番号と両manifestを1.4.3へ更新。既存タグは変更しない。

## v1.4.1

- スクリプト登録先一覧の保存前後を比較し、追加・削除を保存した場合だけ再起動案内を表示。言語変更の案内と統合し、取消・変更なし・パラメータのみの変更では表示しない。
- ThumbnailGridの表示用キャッシュ整理で可視行を保護し、画面外の古い画像から解放。保持上限を維持し、フォルダ表紙・ZIPページの高速スクロール後の欠落を修正。
- 対応する明示言語の初期化で不要なOS UI言語照会を省き、設定復元の互換試験では非同期読込・描画完了を待つ。
- サムネイル回帰を独立CTestへ分離し、Windows/macOSのキャッシュ先と時間枠を分離。保存fixtureの競合待機と段階ごとの所要時間ログを追加。fullは20群、platformは13群。
- WindowsのCLI子プロセス終了待機を60秒、CLI群を180秒、UI群を600秒へ調整。起動失敗の診断と、Timeout時の引数・経過時間・標準出力・標準エラーを記録。

## v1.4.0
- Cover thumbnails: use a path/settings-based lookup key that does not require source ZIP metadata; store the source stamp with the cached image, show cached covers first, and validate in low-priority work before regenerating changed sources.
- Use the separate `thumbnails-v2` disk cache without migrating v1 PNGs. Preserve atomic image writes and existing page-thumbnail behavior.
- Test cached-cover responsiveness while source validation is blocked, changed-source replacement, and explicit refresh recovery from incorrect cache pixels.
- Make the macOS directory-change scroll-position test establish its Qt item layout before setting the test scroll position; retain the actual scroll-position assertion.
- Document F6 and cache-clear recovery, upgrade caveats, and Qt logical-pixel thumbnail sizing in both languages.

## v1.3.0

- ThumbnailDiskCache: remove the 20,000-entry eviction rule; enforce the configured byte capacity only. Save before trimming and retain committed pixels on replacement failure.
- Reduce cache-key dependencies on application/Qt/script versions and unrelated script settings. Remove obsolete migration handling and add large-count, cancellation, and concurrency tests.
- Windows: close the backup source `QFile` before updating `QSettings` to avoid an atomic-save `AccessError` in the MangaMeeya importer.
- Expand regression coverage and stabilize Windows offscreen/WebEngine testing with explicit font and GPU test configuration and appropriate timeouts.

## v1.2.1

- 単一起動IPCの受信時に最小化状態だけを解除して表示・前面化を要求する共通処理を追加。最大化・全画面状態と読書位置を保持。
- 通常・最大化・全画面・最大化からの全画面について、最小化あり／なしの8ケースを自動試験に追加。

## v1.2.0

- キャッシュ読取・現在表示・後続処理の専用実行枠と取消可能な要求キューを追加。可視行の変更時は旧要求を取り消し、古い結果の反映を防止。
- サムネイルのlookup・生成・保存を分離。生成画像を先に返し、上限付き保存待ちキューとRAMキャッシュを追加。保存世代でclear・設定変更前の要求を無効化。
- フォルダ列挙・ソートをworkerへ移し、一覧を分割登録。非表示Readerの描画・先読みを停止し、一覧準備完了とReader描画完了を分離。
- 読書先読み、キャッシュ整理・使用量調査等を後続枠へ移動。書庫metadata走査とデータ取得mutex待ちにも取消確認を追加。
- キャッシュhitの独立進行、不要要求の取消、保存待ちの上限とclear、F6再生成、非表示Reader停止、画像コピーと起動コマンドの回帰テストを追加。

## v1.1.1

- Persist fullscreen state across restarts, including native window-state changes, and add a fullscreen checkbox to Preferences.
- Restore fullscreen after the normal window and controls are ready, retaining the normal geometry, splitter, and maximized state.
- Preserve fullscreen state through settings slots and settings-file import/export without overwriting the imported value during application.
- Add restart, preferences, native-state, settings-file, and CLI regression tests.

## v1.1.0

- Make reading filenames and thumbnail page numbers optional and hidden by default; reclaim the filename band for image display.
- Add an independent two-pane folder browser as the default, retaining an optional single tree.
- Add custom thumbnail dimensions, columns, spacing, filename lines, starting corner and decorative frames. Defaults: 64×64, automatic columns, zero spacing, one filename line, upper left, no frame.
- Move the toolbar visibility toggle directly into the View menu.

## v1.0.3

- Fix Linux AppImage document startup aborts caused by bundled graphics/C++ libraries conflicting with newer host Mesa drivers.
- Open EPUB files through Unicode-safe file descriptors for both body and cover loading on Windows.
- Add deployed WebEngine rendering checks, interrupted thumbnail-cache writer tests, and opt-in cache diagnostics.

## v1.0.0 — Initial release
