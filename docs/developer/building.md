# 開発環境とビルド

アプリだけのCMakeビルドは[README](../../README.md)のOS別手順を使います。
ここでは開発時の探索・出力先を補足します。C++20、CMake 3.22以上、Qt 6.5以上を使用し、
Qt QmlとWebEngineWidgetsは必須です。画像表示だけの別ビルドは提供しません。

## 出力先

公開リポジトリの隣にある `../tmp/` を使います。ビルド・fixture・結果・stageを分け、
異なるgeneratorやtoolchainのCMakeCacheを流用しません。CMakeCacheを別のソースへコピーせず、
ソースを移動したら新しいbuildを構成してください。同じソースと依存での増分ビルドは再利用できます。

## Windows

Visual Studio 2022/MSVC x64と公式Qt SDKを使います。通常は `windows-fast` presetで、
非Qt依存のみを `cmake/windows-fast/vcpkg.json` のbaselineとRelease専用tripletで構築します。
`VCPKG_ROOT` と `KOMAMIMIC_QT_ROOT` を設定してください。Qt全体のソースビルドへ切り替えません。
Qtの取得設定は `tools/windows-fast.json`、取得・検査は `tools/setup_windows.py` です。

`VCPKG_BINARY_SOURCES` で専用のbinary cacheを指定できます。既存のQt SDK・vcpkg checkout・
buildtreeを再利用するときは、baseline・triplet・compiler・SDKの一致を確認します。
Releaseでも自コードのPDBを生成します。PDBとexeは同じbuildの組を保持してください。
起動時にはQt、`vcpkg_installed/x64-windows-release/bin`、Qt pluginsを探索できる必要があります。
通常のbuildだけでは公式Qt SDKのDLL・plugin・WebEngine資源をexeの隣へコピーしません。
READMEのPowerShellで探索先を設定して起動するか、Explorerから起動する場合は
`stage_validation.py`で作った検証用フォルダのexeを使用します。具体例はREADMEを参照してください。

## macOS

初回対象はApple Silicon/arm64です。Homebrew、compiler、依存のアーキテクチャを揃えます。
CMakeの `cmake/Homebrew.cmake` が関連formulaのprefixを調べます。明示した
`CMAKE_PREFIX_PATH` 等を優先し、分割したQt moduleの探索にも使います。
`brew link --force` は不要です。自動探索を使わない場合は
`-DKOMAMIMIC_USE_HOMEBREW=OFF` と必要なprefixを指定します。

## Linux

Fedora 44の依存例はREADMEにあります。他のdistributionでは同等の開発パッケージが必要です。
Qt DBusは消灯抑制に使います。ない場合はその機能だけ利用不可になります。
別Qtを使う場合は新しいbuildで `-DCMAKE_PREFIX_PATH=/path/to/Qt/6.x/gcc_64` を指定します。

## 開発ビルドと配布物

本体と `komamimic-script-worker` を同じ出力先で保持します。
CMakeのinstall ruleだけではQt/WebEngine/codec等の依存を配布用に揃えられません。
テストは[自動テスト](testing.md)、配置は[パッケージング](packaging.md)を参照してください。
