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
(a full local run took 274 seconds without parallel suites), `cli_tests` has
180 seconds, and `document_tests` has 300 seconds. CLI subprocess helpers wait
up to 60 seconds for completion on Windows, including GUI startup and settings
writes; other OSes use 15 seconds. A subprocess timeout logs its arguments,
elapsed time, stdout and stderr before failing the test. The cache suite uses
300 seconds on every OS for its 20,001-file regression. For local full runs on
Windows, use `--parallel 1` to reduce contention with filesystem-heavy suites.
Rerun timed-out suites individually to distinguish contention from a genuine hang.

## Test scopes

`full` registers 20 CTest entries: 17 Qt suites (compatibility, reader, UI,
feature, format, PDF, CLI, script, PIC, document, power, evolution, cache, i18n,
archive_text, legacy_settings and thumbnail), the C-locale format check, and the Python
single-instance IPC and distribution checks.
`platform` registers 13 CTest entries: compatibility, CLI, UI, PDF, script,
document, power, cache, i18n, legacy_settings, thumbnail, single-instance IPC, and
distribution. Some Qt suites run only selected platform integration functions.
This is the Windows-fast CI default. Its cache suite selects cancellation,
disk-persistence and worker-scheduling regressions; it is not the full cache suite.
Do not report passing `platform` checks as the full test scope.

`thumbnail_tests` runs visible-image eviction and the 220-folder/220-page ZIP
scroll regressions in both scopes. It also checks folder and ZIP reuse in fresh
processes with marked cached pixels, nested source changes, leaf-cache eviction,
explicit refresh, invalidation during recursive lookup, and cached folder display
while the generation worker is busy. The platform cache selection includes the
regression where 300 cache hits used to discard an unsaved image. It has its own 180-second timeout, separate
from the cache suite's 300-second budget. Scroll cases log elapsed time at fixture,
book-load, outward-scroll, disk-cache preparation and return-check milestones.
This preserves the assertions while making slow filesystem stages visible in CI.
Its application name also isolates the native Windows/macOS cache directory
from `cache_tests`, which includes explicit cache-clearing tests.

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

## サムネイルの優先順位・再試行・永続化

cache群では、保存待ち80枚の全件再利用、32 MiB上限、書込失敗とcommit直前取消の再試行、
pendingの再起動復元と明示clear、キャッシュ表示および前景の完了前にUpdate／Store／Backgroundを開始しないこと、
画面外要求の降格・再開・エラー再試行を検査する。上限確認だけで未保存画像の脱落を許容しない。
これらの追加cache試験はfullとplatformの両方に含める。大量の永続書込を行う
UI／cache／thumbnail群はCTestのRESOURCE_LOCKで相互に直列化する。UI群は新規プロセスの
起動と設定保存も多いためRUN_SERIALで単独実行し、既存の時間枠を維持する。

thumbnail群では、本棚の遅いmissに続くwarm hitの先行表示、ページ一覧で旧PNGを表示してから
入力操作なしに新画像へ差し替えること、検証済み子フォルダの変更／一時エラーからの回復、
空フォルダの旧永続PNG削除、保存lock競合中の終了と別プロセス再利用を確認する。
220項目の往復ではfixture側から保存を補わず、アプリ自身の保存済み画像をlookupする。

## Validation with logs

Thumbnail-cache diagnostics are disabled by default. On Linux, enable them for
one run with `QT_LOGGING_RULES='komamimic.cache.debug=true' ./komamimic ...`.
The category records hashed cache keys, hits, generation, unavailable locks,
inventory counts and evictions. `cover-key`/`page-key` log the source
fingerprint and relevant source/name-encoding details; application, Qt,
script-version and script-settings fingerprints no longer enter these keys.
Cache-directory paths appear in these local
logs; review diagnostics before sharing them. Folder cache-only lookup does not
scan the source. Background validation still scans candidates, and opening an
archive still indexes its pages; that source I/O alone does not prove thumbnail
regeneration. `cover-regenerate` identifies actual entry into source decoding,
whereas a folder PNG can be saved from existing cached pixels.

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
また検証中の項目を未表示に戻し、同じ検証要求を残したままキャッシュから復帰できることを検査する。
