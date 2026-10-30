# Packaging

This page describes developer deployment procedures and distribution requirements.
User installation, update, and removal instructions are in the
[installation guide](../site/en/user/install.md), based on reviewed Actions artifacts.

## Source

```sh
python3 tools/check_source.py
python3 tools/prepare_release.py --output ../tmp/source-release-01
```

This creates a new tar.gz, SOURCE_MANIFEST.json, and SHA256SUMS containing only the
public selection. Timestamps, owners, and modes are normalized for reproducibility
with the same source, epoch, and generation environment. It does not commit, tag,
push, or publish a GitHub Release.

## Validation runtimes

```sh
python3 tools/stage_validation.py --build-dir ../tmp/build-native --output ../tmp/stage-native-01
```

| OS | Current deployment | Checks |
|---|---|---|
| Windows x64 | Qt deployment and non-Qt DLLs, distribution ZIP candidate, separate ZIP of the application's own PDBs | Normal/delay imports of all PE files, CLI/worker/readers after re-extraction, WebEngine resources, image/SQLite plugins, translations |
| macOS arm64 | .app ZIP and unnotarized DMG, macdeployqt, dependency relocation, ad-hoc signing | Helpers, dylibs, Qt resources, entitlements, codesign/otool/architectures, reading after re-extraction and mounting, Finder startup |
| Linux x64 | Build/test on Rocky 8, relocate AppDir dependencies, create AppImage | Application/worker, desktop entry/icon, Qt plugins, WebEngine resources, glibc requirements, reading in another environment |

Static dependency checks alone are insufficient when sdl2-compat loads SDL3 at
runtime. The macOS helper deploys SDL3 explicitly and supplies the names requested
at runtime. Ad-hoc signing and Apple notarization are separate; the initial macOS
distribution is not notarized.

See the [AppImage workflow](ci.md#linux-appimage-generation-on-rocky-8).
`tools/build_appimage.py` deploys dependencies and resources after CMake install,
checks relative RPATHs and glibc requirements, compresses with checksum-pinned tools,
and checks the candidate. The AppImage identifies itself as a candidate awaiting
verification. Qt WebEngine and the Qt TIFF plugin use RPMs; Poppler is built against
TIFF ABI 6. Poppler data, applied patches, and corresponding source are bundled.
Packaging rejects a mixture of TIFF ABI 5 and 6.

## Windows distribution ZIP candidates

Run on Windows after building and platform-testing the same source. Use new output
directories.

```powershell
python tools/stage_validation.py --build-dir ../tmp/build-windows-fast --output ../tmp/stage-windows-01
python tools/package_windows.py pack --build-dir ../tmp/build-windows-fast --stage ../tmp/stage-windows-01 --output ../tmp/windows-zip-01
```

`KomaMimic-1.0.0-windows-x86_64.zip` launches through `komamimic.exe` in a single
folder. It includes the worker, dependency DLLs, Qt plugins, WebEngine resources,
translations, reader scripts, original notices, and build information. The
application's own PDBs are in a separate ZIP under `symbols/`. Windows system DLLs
and Microsoft's official Visual C++ v14 x64 Redistributable are prerequisites;
arbitrary MSVC DLLs from the SDK are not added to the package.

All stage hashes and public source contents are checked before compression. The
actual re-extracted package is tested without Qt/vcpkg PATH entries. Application
source and checksums are also generated. See [Windows CI](ci.md#windows-x86_64-distribution-zip-candidates)
for the procedure, artifact layout, and checks that need separate confirmation.
Exporting application source and collecting required corresponding source for
bundled third-party dependencies are separate tasks.

## macOS arm64 distribution candidates

Create a native arm64 Release build with `BUILD_TESTING=ON` and
`CMAKE_OSX_ARCHITECTURES=arm64`. After full validation, run with the same source,
using new output directories.

```sh
python3 tools/stage_validation.py --build-dir ../tmp/build-macos --output ../tmp/stage-macos-01
python3 tools/package_macos.py --build-dir ../tmp/build-macos --stage ../tmp/stage-macos-01 --output ../tmp/macos-dmg-01
```

The stage also retains a validation probe whose load paths are repaired by the same
deployment pass as the application and worker. The distribution app excludes that
probe. After verifying stage hashes, symlinks, and source contents, the packager adds
notices and build information, then reseals the outer app. It creates an `.app.zip`
that preserves executable modes and internal framework symlinks, and a `.dmg` with
KomaMimic.app and an Applications link. Checksums and application source are emitted
only after DMG verification, read-only mounting, and CLI/worker/native-reader checks
on a copied installation all pass.

Signing is ad-hoc; no notarization is performed. No Developer ID or notarization
credentials are required. See [macOS CI](ci.md#macos-arm64-app-and-unnotarized-dmg-candidates)
for the process, artifacts, and native checks. Browser-downloaded Finder/Gatekeeper
checks and the final audit of dependency notices and corresponding source are
separate. The user guide documents the verified
[macOS installation procedure](../site/en/user/install.md#macos-dmg).

## Requirements for public distribution

Verify each OS's CI build, tests, deployment, and artifacts, then confirm startup
and key operations using the downloaded artifacts. Assemble exact bundled dependency
versions, license and copyright notices, required corresponding source, patches,
build information, and checksums. See the README's
[distribution requirements](../../README.md#distribution) and
[third-party notices](../../README.md#third-party-libraries-and-code).
A working validation ZIP alone does not complete general distribution preparation.

Keep OS-specific installation, update, removal, and first-launch instructions aligned
with the reviewed artifacts. Tag-triggered Release publication and the final
dependency/source audit remain separate from candidate packaging.
