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
installed application. On Windows, `ui_tests` and `document_tests` have
300-second CTest timeouts for parallel runs; the cache suite uses 300
seconds on every OS for its 20,001-file regression. Rerun timed-out
suites individually to distinguish contention from a genuine hang.

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
scroll regressions in both scopes. It has its own 180-second timeout, separate
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

## Validation with logs

Thumbnail-cache diagnostics are disabled by default. On Linux, enable them for
one run with `QT_LOGGING_RULES='komamimic.cache.debug=true' ./komamimic ...`.
The category records hashed cache keys, hits, generation, unavailable locks,
inventory counts and evictions. `cover-key`/`page-key` log the source
fingerprint and relevant source/name-encoding details; application, Qt,
script-version and script-settings fingerprints no longer enter these keys.
Cache-directory paths appear in these local
logs; review diagnostics before sharing them. A folder-cover lookup still scans
candidate files before a PNG hit, and opening an archive still indexes its pages;
that source I/O alone does not prove thumbnail regeneration.

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
