# CI configuration and operation

## Documentation and GitHub Pages

`.github/workflows/pages.yml` runs on changes to `docs/site/**` on main, MkDocs
configuration and dependencies, the documentation workflow, and public-source
selection tools. Pull requests run checks and builds only. Pushes to main and manual
runs selecting main publish to GitHub Pages after a successful build.
`docs/site/` contains the Japanese guide and its English translation under `en/`;
`docs/developer/` is not included in the site. One MkDocs build produces both
languages, with matching-page language links and shared screenshot assets.

For initial setup, select **GitHub Actions** under repository Settings → Pages →
Build and deployment → Source. Commit and push the workflow manually, then check
both build and deploy in **Documentation and Pages**. For a manual run, select main
under Run workflow. The public URL is https://asfdrwe.github.io/KomaMimic/ ; the
English guide is at https://asfdrwe.github.io/KomaMimic/en/ .

Actions are pinned to commit SHAs. The build has read-only contents permission;
only deploy receives pages and id-token write permissions. No PAT needs to be added
as a workflow secret. Local checks, from the repository root:

```sh
python3 tools/check_source.py --tracked
python3 -m mkdocs build --strict --site-dir ../tmp/site-preview
```

See GitHub's [custom Pages workflow documentation](https://docs.github.com/en/pages/getting-started-with-github-pages/using-custom-workflows-with-github-pages).

## Application build CI

The application build and automated-test workflow is
`.github/workflows/build.yml`. Distinguish local workflow definitions from actual
successful jobs on GitHub. It accepts main pushes, pull requests, and manual runs,
with read-only contents permission.

| Job | Work | Saved output |
|---|---|---|
| source-checks | Tracked files versus public selection, translations, links, action pins, actionlint, source archive reproducibility | Source distribution candidate |
| linux | Fedora 44 container, Release build, CLI, 17 full CTest groups | Configuration/test logs, JUnit XML |
| macos | macOS 15 arm64, Homebrew dependencies, Release build, CLI, 17 full CTest groups, signing/deployment/DMG checks | .app ZIP, unnotarized DMG candidate, source, checksums, configuration/test/deployment logs, JUnit XML |
| windows | Windows 2022/MSVC, official Qt SDK, Release-only vcpkg, nine platform groups, runtime deployment and distribution ZIP checks | Distribution ZIP candidate, source, checksums, separate artifact of application PDBs, configuration/test/deployment logs, JUnit XML, result JSON |

After common checks pass, the three OS jobs run in parallel. A failure on one OS
does not cancel another OS's job. A new commit on the same branch or PR cancels
older runs.

Both Build and the AppImage workflow also accept `workflow_call` from the
[Release workflow](releasing.md). Their normal triggers remain unchanged. Release
tag runs are not cancelled, and their concurrency groups differ from the caller's.

Automatic push/PR runs are limited to changes in build or validation inputs:
`src/`, `cmake/`, `scripts/`, `translations/`, `assets/`, `examples/`, `tests/`,
`tools/`, `CMakeLists.txt`, `CMakePresets.json`, `vcpkg*.json`, `.gitattributes`,
`.github/actions/`, and the Build workflow itself. `tests/fixtures/README.md` and
`tools/public_source.json` are excluded. Changes only to documents, MkDocs settings,
or documentation source registration do not run the application workflow. Public
selection is checked by Documentation and Pages. A simultaneous code change also
runs Build. Manual runs are available regardless of changed paths. Update Build's
`paths` when adding build inputs outside these locations.

| Change | Application build/tests | MkDocs/Pages |
|---|---|---|
| Only `docs/developer/**` | Skipped | Skipped |
| Only `docs/site/**`, including images | Skipped | Runs; PRs do not publish |
| Only `mkdocs.yml`, `requirements-docs.txt`, or `tools/public_source.json` | Skipped | Runs; PRs do not publish |
| Only `README.md` | Skipped | Skipped |
| Build workflow itself | Runs | Skipped |
| Only Rocky AppImage workflow | Skipped | Skipped; AppImage is manual |
| Only application code | Runs | Skipped |
| Application code and `docs/site/**` | Runs | Runs |

The site workflow also watches its own definition, documentation dependencies, and
public-selection tools. Application Build runs for selection-tool code changes;
documentation configuration alone follows the table above.

Failures remain job failures. Diagnostics are saved for 14 days on both success and
failure. Distinguish overall CTest success from Qt pass/fail/skip counts and Python
test counts. On Linux/macOS, inspect LastTest.log and ctest.xml. On Windows, inspect
ctest.log, LastTest.log, ctest.xml, and automatic.json in diagnostics. Windows
platform success is not full-scope success.

Windows vcpkg logs under the short path `D:/kfb` are copied into
`results/vcpkg-build-logs/`, retaining each port's directory layout. Uploaded paths
are kept inside the repository. Logs use CI paths and synthetic fixtures, not
personal reading data.

Source, Windows ZIP, and macOS candidates are Actions artifacts. The Windows ZIP
contains the application, worker, dependency DLLs, Qt plugins, WebEngine resources,
translations, and collected notices; application PDBs are a separate artifact.
The Linux job in Build saves logs only. The separate workflow below creates the
Linux AppImage. Build does not create tags, GitHub Releases, or automatic releases.

For initial setup, the administrator commits and pushes the workflow and documents,
then checks jobs under **Build**. Run workflow can also run main manually. No PAT
is required. Actions are pinned to commit SHAs. Windows keeps separate Qt SDK and
vcpkg binary caches, keyed by configuration, manifests, runner image, and other
inputs. Qt is not rebuilt from source. Disabling WebEngine's sandbox in the root
Fedora container applies only to test processes; it is not a setting recommended
for normal users.

## Windows x86_64 distribution ZIP candidates

The Windows job retains the official Qt 6.11.2 SDK and Release-only vcpkg caches.
After platform validation, `stage_validation.py` deploys the runtime and
`package_windows.py pack` assembles the distribution layout. It checks AMD64 PE
architecture and normal/delay imports for the application, worker, plugins, and
all dependency DLLs. Unresolved dependencies are rejected except Windows system
DLLs and explicit MSVC prerequisites. Unused positioning, QML debugging, and
non-SQLite database plugins are excluded. Platform UI tests also check that the F3
tree enumerates all filesystem roots before selecting a folder on a specific drive.

The packager does not copy additional MSVC runtime DLLs from the SDK. Microsoft's
[official Visual C++ v14 x64 Redistributable](https://aka.ms/vs/17/release/vc_redist.x64.exe)
is a prerequisite, also noted in the ZIP's `配布情報.txt`. The CI runner already has
the runtime; these checks alone do not test a machine where it is absent.

The generated ZIP is re-extracted elsewhere and all payload hashes and PE files
are checked again. With Qt/vcpkg removed from PATH and Qt/SDL overrides removed,
the application prints its version and formats, the worker decodes 12 samples with
full RGBA comparisons, and a native reader probe runs. The probe is added only for
validation and is absent from the ZIP. Failure prevents distribution artifact upload.
The deployed `platforms/qwindows.dll` is used with `QT_QPA_PLATFORM=windows`; this
does not rely on the SDK's offscreen test plugin. The probe streams Qt initialization
and per-case progress to stderr, retaining partial diagnostics in
`native-readers-stderr.log` even if the 120-second timeout is reached.

Successful artifacts, retained for 14 days:

- **komamimic-windows-x86_64-zip**: `KomaMimic-1.0.0-windows-x86_64.zip`,
  `BUILD_MANIFEST.json`, `SHA256SUMS`, and application source tar.gz, manifest, and
  checksums in `sources/`.
- **komamimic-windows-x86_64-symbols**: application PDBs, matching executable hashes,
  version and commit, and symbols ZIP checksums.
- **diagnostics-windows-msvc-x64**: existing diagnostics plus CLI/worker/reader logs
  from the re-extracted ZIP.

The ZIP records version, commit, source contents, dirty status, dependency imports,
and bundled file hashes. Installed vcpkg copyright files and legal documents,
attribution, and SPDX data present in the Qt SDK are copied in their original form.
The manifest records whether Qt notices were found; finding some documents does not
establish that all notices are complete. **Final third-party-notice and required
corresponding-source review, and GUI checks of downloaded artifacts, are separate.**
The [Windows installation guide](../site/en/user/install.md#windows-zip) describes
the reviewed artifact layout. This job does not create tags or Releases.

## macOS arm64 app and unnotarized DMG candidates

The macOS job retains Homebrew dependencies, a Release build, and 17 full CTest
groups. `stage_validation.py` deploys the app; `package_macos.py` creates its
distribution app ZIP and DMG. Shared triggers, read permissions, concurrency,
Windows dependency caches, and other OS jobs are preserved. The macOS timeout is
60 minutes, covering deployment, compression, and re-extraction checks.

Deployment uses `macdeployqt` and includes Qt/non-Qt dependencies, WebEngine helpers
and resources, image/SQLite plugins, Japanese catalogs, and runtime-loaded SDL3
with its internal symlink. The validation probe passes through the same deployment
process to repair Homebrew load paths, then is moved outside the app. Rpaths that
resolve outside the bundle, including relative ones, are removed and internal
rpaths added. Even for a library used only by runtime loading, a deleted SDK runpath
is replaced with one pointing to the bundled Frameworks. Deployment and auditing
share path interpretation. Modified code is signed from the inside out, preserving
and checking WebEngine helper and other entitlements.

Packaging checks stage hashes, symlinks, and current source contents, then collects
original notices, SBOMs, and receipts/formulae for Homebrew dependencies and their
transitive dependencies. After notices and build information are added, the outer
app is ad-hoc signed again. Every Mach-O must include arm64; the application and
worker must be arm64-only. All load-command and rpath targets, deep/strict codesign
verification, and symlink confinement within the bundle are checked.

`ditto` wraps the app in a ZIP; re-extraction checks hashes, executable modes,
symlinks, and signatures. The DMG uses HFS+ and UDZO, and contains KomaMimic.app, an
Applications link, distribution information, and the manifest. After `hdiutil verify`,
it is mounted read-only and its payload and signatures are checked again. The app
is copied to a working folder and launched with Homebrew removed from PATH and
DYLD/Qt/SDL overrides cleared. Checks cover version, formats, 12 worker samples with
full RGBA comparisons, and a native reader probe using the bundled cocoa QPA plugin.
The probe is temporary; payload and signatures are checked after its removal.
Diagnostic logs also check that dyld did not load Homebrew libraries. The DMG is
detached on failure as well as success.

The successful **komamimic-macos-arm64-dmg** artifact retains these for 14 days:

- `KomaMimic-1.0.0-macos-arm64.dmg`
- `KomaMimic-1.0.0-macos-arm64.app.zip`, containing the complete KomaMimic.app
- `BUILD_MANIFEST.json`, `HOMEBREW.json`, `SHA256SUMS`
- Application source tar.gz, manifest, and checksums under `sources/`

Uploading an app directly would lose executable modes and symlinks, so it is stored
inside a DMG/ZIP. Failure prevents distribution artifact upload. Deployment,
signing, otool, compression, mounting, CLI/worker/reader logs and JSON, and preserved
entitlements are saved in **diagnostics-macos15-arm64**.

**No Developer ID signing, Apple notarization, notarytool, or stapler is used.**
Apple secrets are unnecessary. The manifest records ad-hoc signing, no notarization,
and pending manual verification at generation time. Codesign success is not
Gatekeeper approval. The administrator separately checks Finder startup of a
browser-downloaded artifact; the verified steps are in the
[macOS installation guide](../site/en/user/install.md#macos-dmg).
The final audit of all dependency notices and required corresponding source remains
separate. This job does not publish tags or Releases.

Linux can run simulated checks of consistency, dependency resolution, detach on
failure, and artifact gating. Actual macdeployqt/codesign/lipo/ditto/hdiutil/cocoa
behavior must be checked in macOS Actions.

## Linux AppImage generation on Rocky 8

`.github/workflows/rocky8-probe.yml` is a manually dispatched and reusable AppImage workflow.
Start it with **Linux AppImage (Rocky 8) → Run workflow → main**. On an Ubuntu 24.04
runner, it uses a Rocky Linux 8.10 x86_64 container (`rockylinux/rockylinux:8.10`),
installs Rocky, EPEL, and Raven RPMs, and runs a GCC Toolset 12 Release build, CLI
checks, and 17 full CTest groups. Qt and WebEngine use Raven RPMs, not source builds.
Raven base, extras, rx-devel, and multimedia are enabled; multimedia resolves
WebEngine dependencies such as FFmpeg, OpenH264, and libvpx.

GLib and Cairo use `rx-glib2-devel` and `rx-cairo-devel`. Their pkg-config directories
are discovered and preferred over system versions, with selected versions and
locations logged during configuration. Their `libdir` values also enter
`CMAKE_LIBRARY_PATH` so libraries such as Cairo can be found at link time.
Runtime RPMs alone do not satisfy transitive `.pc` requirements, so UUID, mount,
blkid, SELinux, PCRE, and libffi development RPMs are installed too.

The Poppler helper's `--install-missing-pc-deps` option is for CI containers only.
Before downloading source, it checks `--cflags --libs` for every required module,
uses DNF's `pkgconfig(name)` provider lookup for missing `.pc` dependencies, and
rechecks until transitive dependencies are resolved. Unresolved dependencies,
version mismatches, or the 16-iteration limit stop the process; flags are not
omitted and warnings are not suppressed. Without this option, no packages are
added. The post-install RPM list is retained. After installing Poppler, a small
C++ program is compiled, linked, and run with the application's pkg-config data for
Poppler, Cairo, and GLib before configuring the application.

Runtime discovery also prefers RX GLib/Cairo over `/usr/lib64`, preventing version
changes between configuration and execution. The small program prints loaded
library versions. Cairo is selected from actual shared libraries in `rx-cairo` and
`rx-cairo-devel`, with an absolute `pkgcfg_lib_POPPLER_cairo` value for FindPkgConfig.
A versioned RPM file works even if the development `libcairo.so` link is broken.
The selected target is logged, and loading and the actual Cairo version are checked
before configuration.

The real JPEG ABI 62 library is located through its RPM, and its directory is added
to runtime discovery and transitive link-time discovery (`-rpath-link`). ICU matches
Qt's version 78, explicitly selecting headers and shared libraries from
`libicu78-devel`. Major shared libraries are checked for unresolved dependencies
with `ldd` before the application build. Release optimization is retained with
`-g1` diagnostic information. Only if CTest fails, synthetic fixtures are regenerated
and `validation_probe` runs under GDB, recording backtraces and loaded libraries in
`probe-debug.log`. Diagnostics never convert the original CTest failure into success.

Qt WebEngine and the Qt TIFF plugin require TIFF ABI 6. Poppler is also built from
source against `libtiff6-devel`, avoiding simultaneous loading of ABI 5 and 6.
`tools/setup_linux_poppler.py` obtains
[Debian's Poppler source](https://sources.debian.org/src/poppler/22.12.0-2%2Bdeb12u2/)
22.12.0-2+deb12u2, checks sizes and SHA-256 values against the source description,
and applies Debian patches in order. Qt plugins remain RPM-provided. The build
enables the GLib API and image decoders used by the app, but excludes unused Qt
wrappers, utilities, signature verification, HTTP fetching, and other features.
Broken Cairo development links are supplemented inside the working prefix.
Poppler CMaps and other data are bundled; a small patch lets AppRun select their
location with `POPPLER_DATADIR`. Source, patches, original author/license notices,
origins, checksums, and configuration are retained. AppDir generation starts only
after all 17 groups pass with these dependencies. Python 3.11 is selected explicitly.

Rocky baseos, appstream, extras, and powertools use `dl.rockylinux.org` directly to
avoid long mirror-switching delays. DNF connection/low-speed timeouts are 20 seconds
with two retries; the Bootstrap step has a ten-minute limit. Bootstrap and detailed
DNF logs are saved in diagnostics.

Rocky's system libarchive lacks the RAR5 API, so libarchive 3.8.10 is built in the
same container after verifying the official source SHA-256. It is installed into
a working prefix, and application headers, link targets, and runtime libraries all
use that version. Compilation, linking, and invocation of the RAR5 API are checked
before application configuration. Downloaded source, original copyright/license
notices, and build logs are retained in diagnostics.

Dependency installation, configuration, build/test logs, and installed RPM versions
are kept in **diagnostics-rocky8-raven-x64** for 14 days. This workflow is independent
of the three-OS Build workflow. It has no direct push trigger and creates no Releases
or tags; the Release workflow calls it automatically for a version-tag build.

After tests pass, the Rocky job runs `tools/build_appimage.py stage`, deploying the
application, worker, Qt plugins, WebEngine helpers/resources/locales, Japanese
translations, and dependency shared libraries into AppDir. It makes RPATHs relative,
excludes glibc, the loader, the host graphics stack (OpenGL/EGL, GBM, DRM, glapi),
and the GNU C++/unwind runtimes. These must stay compatible with the host's Mesa
drivers; an older bundled libstdc++ can make WebEngine abort during GLX startup.
Dependency traversal stops at host-library boundaries instead of collecting their
private Gallium/LLVM dependencies. Undefined C++ ABI symbols are checked against
the Ubuntu 22.04 baseline (GLIBCXX 3.4.29, CXXABI 1.3.13, GCC 11.0.0).
The audit rejects references to build-environment libraries and TIFF ABI 5/6 mixing, and checks that all
payload ELF files require at most glibc 2.35, the Ubuntu 22.04 baseline. Normal
AppRun does not disable the sandbox.

RPATH rewriting uses patchelf 0.19.2 built from checksum-pinned official source.
Version 0.18.0 also has an [initialization-code relocation issue](https://github.com/NixOS/patchelf/issues/639),
so the builder rejects versions below 0.19.2 and checks sections, LOAD segments, and
initialization functions in every ELF. An ELF specifying custom functions within
`.text` through `-Wl,-init/-fini` is valid. On hosts with both 32-bit and 64-bit NSS,
only x86_64 libraries are selected. glibc's libmvec is not bundled. Deployed AppRun
must pass `--version` and `--formats` before compression.

The following Ubuntu 22.04 container job rechecks Rocky AppDir hashes and packages
it with appimagetool and a static runtime. It does not rebuild the application or
dependencies. appimagetool uses the [official 1.9.1 release URL](https://github.com/AppImage/appimagetool/releases/tag/1.9.1)
and the SHA-256 in `tools/appimage-tools.json`. The runtime also has a pinned SHA-256;
if its upstream continuous asset changes, a mismatch stops packaging. Updates require
checking the official release asset and testing actual compression/startup before
changing the configuration. Mismatch errors show expected and actual hashes.

In an environment without Qt or Raven installed, checks cover AppImage `--version`
and `--formats`, the extracted worker, and the native reader probe on generated
fixtures. A separate `document_probe` renders TXT, Markdown and reflow/fixed EPUB
with the deployed WebEngine and the xcb QPA plugin under Xvfb. This also creates
a GL context; successful parsing alone cannot pass the document deployment check.
The Ubuntu job supplies its Mesa/GBM and C++ runtimes, without Qt or Raven.
The probe is an intermediate test artifact and is not shipped in the AppImage.
Its `--output` option writes `document-render.json` separately from Chromium's
stdout/stderr. Both diagnostic streams and the report are retained in the
diagnostics artifact on success or failure. Missing, malformed, parsing-only or
incomplete reports fail verification even if the process exits with code 0.
CI uses extract-and-run without FUSE. Only the root test container disables the
Chromium sandbox externally; AppRun does not change the user's sandbox setting.

The successful **komamimic-linux-x86_64-appimage** artifact contains:

- `KomaMimic-1.0.0-linux-x86_64.AppImage` and `SHA256SUMS`
- `komamimic.desktop` for `~/.local/share/applications/`, and a 256×256
  `komamimic.png` for `~/.local/share/icons/hicolor/256x256/apps/`
- `BUILD_MANIFEST.json` with bundled hashes, required glibc, dependency RPM versions,
  licenses, Source RPMs, and related data
- Application source/manifest and built libarchive and patched Poppler source in `sources/`

The external desktop entry launches that version's AppImage from `~/.local/bin/`.
The internal entry is retained. Both external files are hashed in the manifest and
included in `SHA256SUMS` with the AppImage and manifest. Ubuntu runs
`desktop-file-validate` and checksum checks before upload. See the
[installation guide](../site/en/user/install.md) for placement, updates, and removal.

Intermediate compression artifacts are retained for seven days; candidates and
diagnostics for 14 days. Ubuntu logs are in **diagnostics-appimage-ubuntu22-x64**.
RPM license documents are copied into the AppImage, but a public release also needs
an audit of dependency corresponding source, notice completeness, and notices for
the AppImage runtime itself. Actions success does not replace manual GUI checks.
Native downloaded-artifact checks and installation instructions are completed
separately from final Release publication.

## Release preparation sequence

1. The administrator reviews public contents and manually creates the repository, commits, and pushes.
2. Inspect job results, dependency versions, skips, and logs through GitHub and gh; resolve failures.
3. Establish the required artifacts, including macOS app and Linux packages.
4. Assemble dependency notices and required corresponding source, and test downloaded Actions artifacts on real systems.
5. Maintain installation/removal instructions based on those artifacts; verify Release and Pages publication separately.

The application build definition does not create Releases. The separate
[Release workflow](releasing.md) publishes only after all required jobs succeed for
a matching stable tag. Its manual mode only assembles artifacts. Write permission
is restricted to its final publish job. Pages uses the documentation workflow
above. Headless CI success does not substitute for manual checks.
