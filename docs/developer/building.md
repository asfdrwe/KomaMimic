# Development environment and builds

Use the OS-specific [README](../../README.md) instructions for an application-only
CMake build. This page adds details about dependency discovery and development
output directories. Use C++20, CMake 3.22+, and Qt 6.5+. Qt Qml and WebEngineWidgets
are required; there is no separate image-only build.

## Output directories

Use `../tmp/` next to the public repository. Keep builds, fixtures, results, and
stages separate. Do not reuse a CMakeCache across generators or toolchains, or copy
it to another source tree. Configure a fresh build after moving the source. An
incremental build can be reused with the same source location and dependencies.

## Windows

Use Visual Studio 2022/MSVC x64 and the official Qt SDK. Normally, use the
`windows-fast` preset. Only non-Qt dependencies are built with the baseline in
`cmake/windows-fast/vcpkg.json` and the Release-only triplet. Set `VCPKG_ROOT` and
`KOMAMIMIC_QT_ROOT`. Do not switch to building all of Qt from source.
`tools/windows-fast.json` configures SDK acquisition; `tools/setup_windows.py`
downloads and verifies it.

Use `VCPKG_BINARY_SOURCES` to select a dedicated binary cache. Before reusing a Qt
SDK, vcpkg checkout, or build tree, check the baseline, triplet, compiler, and SDK.
Release builds generate PDBs for the application's own code. Keep each PDB with the
executable from the same build.

At runtime, Qt, `vcpkg_installed/x64-windows-release/bin`, and Qt plugins must be
discoverable. A normal build does not copy official Qt SDK DLLs, plugins, or
WebEngine resources next to the executable. Set the search paths in PowerShell as
shown in the README, or use the executable in a `stage_validation.py` validation
folder when launching from Explorer. See the README for examples.

## macOS

The initial target is Apple Silicon/arm64. Match the architectures of Homebrew,
the compiler, and all dependencies. `cmake/Homebrew.cmake` discovers the prefixes of
relevant formulae, including split Qt modules, while respecting explicit settings
such as `CMAKE_PREFIX_PATH`. No `brew link --force` is needed. To disable automatic
discovery, pass `-DKOMAMIMIC_USE_HOMEBREW=OFF` and provide the required prefixes.

## Linux

The README lists Fedora 44 dependencies. Other distributions require equivalent
development packages. Qt DBus provides display-sleep inhibition; without it, only
that feature is unavailable. To select another Qt installation, configure a new
build with `-DCMAKE_PREFIX_PATH=/path/to/Qt/6.x/gcc_64`.

## Development builds and distribution packages

Keep the application and `komamimic-script-worker` in the same output location.
CMake install rules alone do not assemble Qt, WebEngine, codecs, and other runtime
dependencies for distribution. See [automated tests](testing.md) and
[packaging](packaging.md).
