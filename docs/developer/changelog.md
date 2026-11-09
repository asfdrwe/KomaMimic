# Changelog

## 未リリース

- 文書の可変本文で組方向と見開き順序を連動。EPUBの表紙を中央単ページにし、現在章・必要リソースから準備する。読込取消と直前1冊の表示復帰、読込中の前後移動を改善。
- 青空文庫のWindows形式の相対画像参照に対応。画像だけの区画とキャプションを分離し、横長挿絵を全幅表示。元の本文位置を保持して必要時に画像を読む。

- Readerのページ項目・サムネイルsnapshot整形を遅延し、Coversの走査・分割配送もモード寿命へ合わせた。位置・編集・ソート復元中のrenderを抑制し、確定した対象を1回だけ要求。
- 一括ロードは`cache/bundleLoad`の保存オプションへ変更。`Config/BundleLoad`を取り込み、読書中の最初の描画・通常先読み後にrawファイルを1件ずつ保持。旧一括画素復号・末尾一括配送とeagerなサムネイル情報収集を削除。
- ZIPの中央情報読込をまとめ、Book列挙で再利用。局所local headerエラーを対象ページへ分離。nested/folderContainersはsource Bookを保持し画像の全件inline化を廃止。TIFF/script inspectとGIF/APNGへ取消を伝播。
- 履歴のpending snapshotとrevision確認を導入し、現在画像の描画後に別connectionで保存。旧writeはepochで無効化し、終了時・明示DB操作では残りをflush。非表示の履歴・本棚UIの再構築も表示時へ移動。
- `reader_startup_tests`をfull/platformへ追加（full22群・platform15群）。既存群の時間枠は維持。

- Windows依存cacheのキーからアプリ版番号等を除外し、baseline・依存設定・triplet等に基づくidentityへ変更。既存キーもfallback復元し、不要なcold buildを避ける。
- cache復元と保存を分離。Qt SDKは検証直後、vcpkgの完成packageは後続test失敗時も保存し、immutableな部分cacheを次回更新できる世代キーを使用。変更なしなら再転送しない。
- cache復元キー・保存判断の診断JSONと回帰試験を追加。platform試験・時間枠・配布検証は維持。

## v1.5.0

- Windows CIのページcache試験で、画像生成用Bookが保持する書庫cursorを解放してからZIPを削除するようfixtureを修正。元ZIP不在時のcache表示と変更後の更新を維持し、cache hit中の追加アクセスなしも検査。

- CMake・通常/Windows-fast manifest・READMEの版番号を1.5.0へ更新。v2キャッシュ互換性を保ち、日英利用ガイド・開発者仕様・Releaseノートを新仕様へ統一。
- 220件の往復スクロールをthumbnail_scroll_testsへ分離。各群の時間枠と検証内容を維持し、fullは21群、platformは14群。

- サムネイルは元ソースの検査なしで保存画像を最優先表示。周期検証を廃止し、取得済み一覧の差分・対象別変更通知がある場合だけ更新する。採用元未変更の追加では属性再照会・本体読込・生成・PNG再保存をしない。
- フォルダ表紙の探索を設定0～8階層へ制限。既定0は表紙対象フォルダ直下のみ。深度縮小・打ち切りでは既存画像を失効させない。thumbnails-v2と従来キー読込を維持し、新しい永続依存記録を追加しない。
- 不要要求は破棄し、降格・再開を廃止。取消済みnative処理から現在要求のexecutorを分離し、旧生成が新画像を上書きする競合を防ぐ。読書中の自動サムネイル処理と隠れた一覧走査を停止。
- 完成画像の保存は現在表示・読書先読みより後に実行。保存時の元ソース再照会を廃止し、既存pending退避と失敗再試行を維持。容量整理は保存日時のFIFOとし、読込でPNG日時を更新しない。
- 未変更時のアクセス回数、採用元不変時のPNG同一性、探索深度、読書中の処理停止、取消・新旧競合、保存復元の回帰試験を追加。

- TXT・Markdown・書庫内テキストの完成表紙も保存。文書本文と画像ページサムネイルの対象は区別する。

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
