# Automated tests

Qt Test and Python 3.9+ are required. The Windows SDK setup helper needs Python
3.10+. The examples below are for Linux/macOS. Keep test builds separate from the
application-only builds in the README.

```sh
python3 tools/check_source.py
cmake -S . -B build-test -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DKOMAMIMIC_TEST_SCOPE=full
cmake --build build-test --parallel 4
ctest --test-dir build-test --output-on-failure --parallel 2 --no-tests=error
```

On Windows, prepare the README environment and use a separate build:

```powershell
cmake --preset windows-fast -B build-windows-test -DBUILD_TESTING=ON
cmake --build build-windows-test --config Release --parallel 4
ctest --test-dir build-windows-test -C Release --output-on-failure --parallel 2 --no-tests=error
```

Set PATH for the Qt and vcpkg DLLs and configure Qt plugin discovery as in the build
instructions.

The Windows offscreen Qt Test entry point sets `QT_QPA_FONTDIR` from
`%WINDIR%\Fonts` if it is unset, and defaults `QTWEBENGINE_CHROMIUM_FLAGS`
to `--disable-gpu` if it is unset. These settings affect tests, not the
installed application. On Windows, `ui_tests` has a 600-second CTest timeout
and `cli_tests` has
180 seconds, and `document_tests` has 300 seconds. CLI subprocess helpers wait
up to 60 seconds for completion on Windows, including GUI startup and settings
writes; other OSes use 15 seconds. A subprocess timeout logs its arguments,
elapsed time, stdout and stderr before failing the test. Cache checks run in
`cache_tests` and `cache_capacity_tests`, each with a 300-second limit on every OS.
The capacity suite contains the 20,001-file check, disk eviction with pending
replacement pixels, and creation-order capacity management. Both use the same
filesystem resource lock. Each case logs its start and elapsed milliseconds so a
timeout retains the active case and completed costs. For local full runs on
Windows, use `--parallel 1` to reduce contention with filesystem-heavy suites.
Rerun timed-out suites individually to distinguish contention from a genuine hang.

## Test scopes

`full` registers 26 CTest entries (27 when Node.js is available): 23 Qt suites (compatibility, reader, UI,
feature, format, PDF, CLI, script, PIC, document, power, evolution, cache, cache_capacity, i18n,
archive_text, legacy_settings, thumbnail, thumbnail_scroll, thumbnail_preload, reader_startup, document_behavior and document_surface), the C-locale format check, and the Python
single-instance IPC and distribution checks.
`platform` registers 19 CTest entries (20 when Node.js is available): compatibility, CLI, UI, PDF, script,
document, power, cache, cache_capacity, i18n, legacy_settings, thumbnail, thumbnail_scroll, thumbnail_preload, reader_startup, document_behavior, document_surface, single-instance IPC, and
distribution. Some Qt suites run only selected platform integration functions.
This is the Windows-fast CI default. Its cache suite selects cancellation,
disk-persistence and worker-scheduling regressions. Its capacity suite selects
eviction and creation-order checks, without the 20,001-file case. These are not
the full cache suites.
Do not report passing `platform` checks as the full test scope.

Both scopes check fallback from unreadable cache PNGs to another saved image or
generation. These permission-based rows skip when the OS/account can still read
the restricted fixture. Concurrent-writer failures log the worker, key, null-image
state and error after all workers have joined.

`thumbnail_tests` checks source-access counts, visible-image eviction, folder and ZIP
reuse in fresh processes, nested source changes, explicit refresh, and cached display
while generation is busy. `thumbnail_scroll_tests` separately runs the 220-folder/
220-page ZIP scroll-and-persistence cases in both scopes. Each has its own 180-second
timeout; the cache suite has a 300-second budget. Scroll cases log elapsed
time at fixture, book-load, outward-scroll, persistence and return-check milestones.
Each suite uses a distinct application name for native Windows/macOS cache isolation.
The platform cache selection verifies that 300 cache hits preserve unsaved images.

シンボリックリンクの回帰条件は両scopeの`thumbnail_tests`で実行する。ファイルリンクでの
表紙・ページ往復、旧表紙PNGの内容・保存日時の維持、ファイル／ディレクトリリンクの
画像・ZIPページの未変更時再利用と変更時更新、古い観測情報による再openの反復防止を
検査する。実際のsymlink作成がOS／権限で利用できない場合は、その条件だけ理由付きでskipする。

```sh
ctest --test-dir build-test -R '^(ui|script)_tests$' --output-on-failure
```

`compatibility_tests` checks the reading filename area, custom thumbnail layout,
independent browser navigation, settings persistence, and the direct toolbar toggle.
It runs in both scopes, including native Windows/macOS jobs.

## Execution conditions and interpreting results

CTest runs Qt tests offscreen with temporary settings and original synthetic inputs.
If real WebEngine, local IPC, or D-Bus is unavailable, tests may skip with a reason.
Record CTest successes, Qt pass/fail/skip counts, and Python test counts separately.
A Qt skip does not complete a GUI check. The dedicated Linux D-Bus test requires
`dbus-daemon`. The offscreen single-instance Python test checks IPC forwarding
and `--new-window`, not a real compositor's foreground-activation policy.
WebEngine GPU-context warnings alone are not test failures. If a Windows
document view fails to render, a temporary launch with
`QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu` can isolate a GPU/driver issue,
potentially at lower performance. Do not disable the WebEngine sandbox as
a routine graphics workaround.

`distribution_tests` checks public source re-export, rejection of unclassified data,
reproducible source archives, synthetic fixture loading and RGBA comparisons, and
validation/deployment helpers. Git tracking tests create small repositories only
inside temporary directories; they do not initialize the active public source tree.

## Windows CIキャッシュの回帰試験

`test_windows_cache.py`は版番号だけの変更・JSON整形・改行差で依存キーが変わらないことと、
依存・baseline・triplet・registry等の変更で変わることを確認する。
完成ZIPだけの保存判定、途中cacheの継続、package削除の反映、旧キー移行、
変更なし時の転送省略、CLIのGitHub出力、workflowの失敗時保存条件も検査する。
source-checksで先行実行し、distribution群にも含める。実際のGitHub cache転送や
Windowsでの時間短縮を模擬試験だけで確認済みとは扱わない。

## サムネイルの優先順位・取消・永続化

cache群は、保存待ち80枚の全件再利用、32 MiB上限、書込失敗とcommit直前取消の再試行、
pendingの再起動復元とclear、FIFO容量整理と読込時の日時不変を検査する。
保存PNGの反復取得と保存完了後のRAM再利用、作成時刻の保持、32 MiBの上限と無効化も検査する。
取消済みnative処理を保留したまま新しいCache／Display／Navigation／Renderが進むこと、
旧要求を降格・再開しないこと、遅れて終わった旧生成が新画像を上書きしないことも検査する。

thumbnail群は実際の属性検査・走査・生成・Book open／画像読込の回数を観測する。
画像ZIP、TXT入りZIP、EPUBのフォルダ往復では、保存PNGの復号回数とRAM hitを区別して確認する。
文書のBook状態を保持したまま表紙一覧へ戻るケースと、native文書描画から画像一覧へ戻るケースを分ける。
未変更の再表示・待機・再訪で回数が増えないこと、変更がある対象だけ更新し、通知を
処理後に反復しないことを確認する。採用元が変わらないファイル追加ではPNGのバイト列と
日時も不変とする。既存v2のメタデータ省略PNG、探索深度0／1／2と設定の保存、
深度縮小後の既存画像保持、読書中のサムネイル処理停止、失敗時の旧画像保持を検査する。
ページの元ZIP削除を使う試験では、生成用Bookを解放してから検証用Bookを開く。
生成用Bookは連続読込用cursorを保持するため、そのまま元ZIPを削除する前提を置かない。
検証用のmetadata snapshotでキャッシュhit後もZIPを削除できること、削除後の表示、
変更後の旧画像先行表示と明示検証による更新を確認し、hit中の追加アクセス回数も検査する。
旧PNGから分からない深部変更のテストには実際の対象ディレクトリ変更通知を供給し、
周期検証や余計な本体読込をテストの前提にしない。

独立したthumbnail_preload群は全件先読みの既定OFF、設定の保存・取消・再起動、表紙とページのcold/warm取得、
ディスク保存OFFでも表示アイコンを保持すること、スクロール復帰時の追加元画像読込なし、
メモリ上限とOFF時の解放、スクロール・モード・フォルダ・Book・設定OFF・前景要求による
先読み取消も検査する。合成データを使い、私有作品をfixtureへ含めない。

UI群は親の多数項目から子のwarm hitへの移動、ページ表示、明示次巻操作、
モードを離れた一覧が勝手に更新されないことを検査する。
220項目の往復ではfixture側から保存を補わず、アプリ自身の保存済み画像をlookupする。
大量の永続書込を行うUI／cache／thumbnail／thumbnail_scroll群はRESOURCE_LOCKで直列化し、
UIはRUN_SERIALで単独実行する。

## Validation with logs

Thumbnail-cache diagnostics are disabled by default. On Linux, enable them for
one run with `QT_LOGGING_RULES='komamimic.cache.debug=true' ./komamimic ...`.
The category records hashed cache keys, hits, generation, unavailable locks,
inventory counts and evictions. `cover-key`/`page-key` log the source
fingerprint and relevant source/name-encoding details; application, Qt,
script-version and script-settings fingerprints no longer enter these keys.
Cache-directory paths appear in these local
logs; review diagnostics before sharing them. Folder cache-only lookup does not
scan the source. Change-triggered folder updates scan bounded candidates; a normal cache hit does not.
The activity counters distinguish directory enumeration, metadata probes, generation,
and Book opens/image requests. Opening a book to read it is separate from thumbnail reuse.

`document_probe --parse-only path...` checks document parsing without Chromium.
Without `--parse-only`, it waits for actual WebEngine layout and fails on a
renderer error or timeout. Passing a generated fixture root selects the TXT,
Markdown, reflow EPUB and fixed EPUB samples. Run deployment checks with the
package's libraries/plugins; a development Qt installation can hide missing or
incompatible runtime dependencies.
Use `--output report.json` for automated checks: WebEngine/Chromium subprocesses
can write diagnostics to stdout, so stdout must not be used as the report channel.
The probe writes the report atomically and fails if it cannot save it. The AppImage
check retains stdout and stderr separately and requires all four rendered fixtures.

```sh
python3 tools/prepare_validation.py --output validation-kit
python3 tools/prepare_validation.py --verify validation-kit
python3 tools/run_validation.py run --build-dir build-test \
  --fixtures validation-kit/fixtures --output results/run-01 \
  --jobs 4 --test-scope full
```

Use a new output name for each run. Keep result JSON, CTest logs, dependency versions,
and source hashes; do not mix them with old failure logs or successes from another
build. Native manual checks have a [separate procedure](platform-validation.md).

親に300フォルダがある状態からキャッシュ済みの子へ移動し、移動前の生成・検証・保存・
画面外workerを停止したまま一覧と青い表紙画像が表示されることを確認する。
また更新中の項目を未表示に戻すと旧要求が取り消され、再可視化ではキャッシュから復帰できることを検査する。


`document_behavior_tests` exercises deferred EPUB resources, document cancellation,
legacy relative illustration paths, caption/location preservation, and book navigation
without cover-list items. Its native cases verify writing-mode overrides, cover sizing,
landscape Aozora pages and retained-view restoration; they require working WebEngine IPC.
`document_surface_tests`はChromiumを起動せず、RHIを明示した独立プロセスで
通常のQuick子では親の描画面が変わることと、文書専用surfaceでは親と一覧の
描画面を維持することを比較する。初回生成・モード切替・保持文書・破棄を含む。
Quick部品は非表示のため、GPU描画や実WebEngineの復帰を確認する試験とは別である。
When Node.js is available, `document_layout_tests` also executes the production reader
controller against a deterministic DOM adapter to test lazy scheduling, spread order,
resume offsets, request replacement and recovery from a later chapter error. This
adapter does not replace native rendering checks. Synthetic fixtures contain no real
book or author names. Private sample diagnostics must use anonymous identifiers in logs.
