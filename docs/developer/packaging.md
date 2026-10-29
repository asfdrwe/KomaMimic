# パッケージング

この文書は開発者向けの配置手順と残る配布工程です。利用者向けのインストール・
アンインストール手順は、GitHub Actionsで生成した成果物を確認してから作成します。

## ソース

```sh
python3 tools/check_source.py
python3 tools/prepare_release.py --output ../tmp/source-release-01
```

公開選択だけを含むtar.gz、SOURCE_MANIFEST.json、SHA256SUMSを新規出力します。
同じsource・epoch・生成環境では時刻・所有者・modeを正規化して再現可能にします。
この処理はGitのcommit・tag・pushやGitHub Releaseの公開を行いません。

## 検証用runtime

```sh
python3 tools/stage_validation.py --build-dir ../tmp/build-native --output ../tmp/stage-native-01
```

| OS | 現在の配置処理 | 確認するもの |
|---|---|---|
| Windows x64 | Qt deploymentと非Qt DLL、配布ZIP候補、別ZIPの自作PDB | 全PEの通常/遅延import、再展開後のCLI/worker/reader、WebEngine資源、画像/SQLite plugin、翻訳 |
| macOS arm64 | .app、macdeployqt、依存再配置、ad-hoc署名 | helper、dylib、Qt resources、entitlements、codesign/otool/architecture、Finder起動 |
| Linux x64 | Rocky 8でbuild/test後、AppDirの依存を再配置しAppImage化 | 本体/worker、desktop/icon、Qt plugin、WebEngine資源、glibc要件、別環境での読込 |

sdl2-compatがSDL3を実行時ロードする構成では、静的な依存検査だけでは不足します。
macOSのhelperはSDL3を明示配置し、実行時に要求される名前も用意します。
macOSのad-hoc署名とAppleの公証は別です。初回配布の公証は行わない方針です。
LinuxのActions生成は[CIのAppImage workflow](ci.md#rocky-8でのlinux-appimage生成)を参照してください。
`tools/build_appimage.py`がCMake install後の依存・資源配置、相対RPATH、glibc要求の検査、
hash固定したツールによる圧縮、取得候補の検査を行います。AppImage内は検証待ちの候補と表示します。
Qt WebEngineとQt TIFF pluginはRPMを使用し、PopplerをTIFF ABI 6に対してビルドします。
Popplerのデータ、適用patchと対応ソースも同梱します。TIFF ABI 5/6の混在は生成時に拒否します。

## Windows配布ZIP候補

Windows上で、同じソースをビルド・platform検証した後に実行します。出力先は新規フォルダーにします。

```powershell
python tools/stage_validation.py --build-dir ../tmp/build-windows-fast --output ../tmp/stage-windows-01
python tools/package_windows.py pack --build-dir ../tmp/build-windows-fast --stage ../tmp/stage-windows-01 --output ../tmp/windows-zip-01
```

`KomaMimic-1.0.0-windows-x86_64.zip`は、単一フォルダー内の`komamimic.exe`から起動する構成です。
worker、依存DLL、Qt plugin・WebEngine資源・翻訳・読込スクリプト・通知原文・build情報を含みます。
自作PDBは`symbols/`の別ZIPです。Windows標準DLLと、Microsoft公式Visual C++ v14 x64
再頒布可能パッケージを前提条件として扱い、SDK内の任意のMSVC DLLを追加同梱しません。

stageの全hashと公開ソース内容を照合してから圧縮し、再展開した実物をSDK/vcpkgのPATHなしで検査します。
本体ソースとchecksumも新規出力します。手順・artifactの構成と未確認範囲は
[WindowsのCI](ci.md#windows-x86_64の配布zip候補)を参照してください。
本体のソース出力と、同梱した第三者依存の必要な対応ソースの収集は別工程です。

## 正式配布への条件

CIの各OSビルド・試験・配置・artifactを確立し、実際の取得物で起動と主要操作を確認します。
同梱した依存の正確な版、ライセンスと著作権表示、必要な対応ソース、patch、ビルド情報、
checksumを揃えます。詳細はREADMEの[配布条件](../../README.md#distribution)と
[第三者通知](../../README.md#third-party-libraries-and-code)です。
検証ZIPができたことだけで一般向け配布の準備完了とはしません。

この結果をもとに、OS別インストール・更新・アンインストールと初回起動の文書を書きます。
