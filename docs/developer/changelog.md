# Changelog

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
