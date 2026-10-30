# CIの構成と確立手順

## 文書サイトとGitHub Pages

`.github/workflows/pages.yml`は、mainへの`docs/site/**`の変更、MkDocs設定・依存指定・
文書workflow・公開選別関連ツールの変更で起動します。PRでは検査とbuildだけを実行し、
mainへのpushとmainを選んだ手動実行では成功後にGitHub Pagesへ公開します。
生成対象は`docs/site/`で、`docs/developer/`はサイトへ含めません。

初回はリポジトリのSettings → Pages → Build and deployment → Sourceで
**GitHub Actions**を選びます。その後workflowを手動commit・pushし、Actionsの
**Documentation and Pages**でbuild・deployの成功を確認します。
手動起動する場合はRun workflowでmainを選びます。
公開先は https://asfdrwe.github.io/KomaMimic/ です。

各Actionはcommit SHAで固定しています。buildはcontentsのread権限だけを使い、
deployだけにpagesとid-tokenのwrite権限を与えます。PATをworkflowのsecretとして追加する必要はありません。
ローカル確認例（リポジトリ直下）：

```sh
python3 tools/check_source.py --tracked
python3 -m mkdocs build --strict --site-dir ../tmp/site-preview
```

GitHub設定の参考：[カスタムworkflowでのPages公開](https://docs.github.com/en/pages/getting-started-with-github-pages/using-custom-workflows-with-github-pages)。

## 本体ビルドCI

以下が本体ビルド・自動テスト用workflowの構成です。ローカルに定義を用意した段階と、
GitHubで実際に各jobが成功した段階を区別して確認します。

定義は `.github/workflows/build.yml` です。mainへのpush、pull request、
手動実行を入口に、contentsの権限をreadへ限定しています。
現在の定義と、GitHub上で実行済みの結果を区別してください。

| job | 内容 | 保存するもの |
|---|---|---|
| source-checks | Git追跡ファイルと公開選択、翻訳、リンク、action pin、actionlint、ソースarchiveの再現性 | ソース配布候補 |
| linux | Fedora 44 container、Release build、CLI、full CTest 17群 | 構成・試験ログ、JUnit XML |
| macos | macOS 15 arm64、Homebrew依存、Release build、CLI、full CTest 17群、署名・配置・DMG検査 | .app ZIP・公証なしDMG候補・ソース・checksum、構成・試験・配置ログ、JUnit XML |
| windows | Windows 2022/MSVC、公式Qt SDK、Release専用vcpkg、platform検証9群、runtime配置・配布ZIP検査 | 配布ZIP候補・ソース・checksum、別artifactの自作PDB、構成・試験・配置ログ、JUnit XML、結果JSON |

共通検査の成功後、3OSのjobを並列実行します。OSごとは独立したjobなので、
あるOSの失敗を理由に他のOSのjobをキャンセルしません。同じブランチ・PRに新しいcommitが
来た場合は古い実行を取り消します。

push・PRの自動起動は、ビルド・検証の入力に変更がある場合に限定します。
対象は`src/`・`cmake/`・`scripts/`・`translations/`・`assets/`・`examples/`・`tests/`・`tools/`と、
`CMakeLists.txt`・`CMakePresets.json`・`vcpkg*.json`・`.gitattributes`・
`.github/actions/`・Build自身のworkflowです。
`tests/fixtures/README.md`と`tools/public_source.json`は対象から除きます。
文書・MkDocs設定・文書の公開対象登録だけの変更では本体workflowを起動しません。
公開対象登録の検査はDocumentation and Pagesで実行します。
コード等を同時に変更した場合は本体workflowも起動します。手動実行は変更パスに関係なく使用できます。
新しいビルド入力を上記以外の場所へ追加する場合は、Buildの`paths`も更新してください。

| 変更内容 | 本体ビルド・テスト | MkDocs・Pages |
|---|---|---|
| `docs/developer/**`だけ | 省略 | 省略 |
| `docs/site/**`だけ（画像を含む） | 省略 | 実行（PRでは公開しない） |
| `mkdocs.yml`・`requirements-docs.txt`・`tools/public_source.json`だけ | 省略 | 実行（PRでは公開しない） |
| `README.md`だけ | 省略 | 省略 |
| Build自身のworkflow | 実行 | 省略 |
| Rocky AppImageのworkflowだけ | 省略 | 省略（AppImageは手動実行） |
| 本体コードだけ | 実行 | 省略 |
| 本体コードと`docs/site/**` | 実行 | 実行 |

MkDocs設定・依存指定・文書workflow・公開選別関連ツールの変更もサイトworkflowの対象です。
これらは`docs/**`外なので本体workflowも起動します。

失敗はjobの失敗として扱い、診断artifactは成功・失敗にかかわらず14日間保存します。
CTest全体の成功と、Qtのpass/fail/skip、Pythonの件数を区別して確認してください。
Linux/macOSはCTestのLastTest.logとctest.xml、Windowsはdiagnostics内のctest.log・
LastTest.log・ctest.xml・automatic.jsonを確認します。Windowsのplatform成功をfull成功と扱いません。
Windowsの短い作業パス`D:/kfb`にあるvcpkgログは、`results/vcpkg-build-logs/`へportごとの
構成を保ってコピーしてから保存します。artifactへ渡すパスはリポジトリ内へ揃えます。
ログにはCI上のパスや自作fixtureを使用し、個人の読書データは使用しません。

ソース配布候補・Windows配布ZIP候補・macOS配布候補はActionsのartifactとして保存します。WindowsのZIPには本体とworker、
依存DLL、Qt plugin・WebEngine資源・翻訳、収集した通知文書を含めます。自作PDBは別artifactです。
このBuild内のLinux jobはログ保存までです。Linux AppImageは下記の独立workflowで生成します。
このworkflowはtag・GitHub Releaseの作成や自動公開を行いません。

初回は管理者がworkflowと本文を手動commit・pushし、Actionsの**Build**で各jobを確認します。
同じ画面のRun workflowからmainを選んで手動実行もできます。PATの追加は不要です。

GitHub Actionsはcommit SHAで固定しています。WindowsはQt SDKとvcpkgのbinary cacheを分け、
設定・manifest・runner image等をkeyへ含めます。Qtをソースから再構築しません。
Fedoraのroot containerでのWebEngine sandbox無効化は試験プロセスに限った設定です。
通常の利用環境への設定方法として案内しません。

## Windows x86_64の配布ZIP候補

BuildのWindows jobは、既存の公式Qt 6.11.2 SDKとRelease専用vcpkgのcacheをそのまま使います。
platform検証後に`stage_validation.py`で配置し、`package_windows.py pack`で配布用の構成へまとめます。
本体・worker・全plugin/依存DLLのAMD64 PEと通常・遅延importを検査し、Windows標準DLLと
明示したMSVC前提条件以外の未解決依存を拒否します。アプリで使わない位置情報・QML debugging・
SQLite以外のDB pluginは候補から除きます。F3のツリーでは、特定ドライブのフォルダーを選ぶ前に
全ファイルシステムrootが列挙されることもplatformのUI試験で確認します。

MSVCランタイムDLLをSDKから追加コピーすることはせず、Microsoft公式の
[Visual C++ v14 x64再頒布可能パッケージ](https://aka.ms/vs/17/release/vc_redist.x64.exe)を前提条件とします。
ZIP内の`配布情報.txt`にも記載します。CI runnerにはランタイムがあるため、
この検査だけでランタイム未導入環境への対応を確認したとは扱いません。

作成したZIPを別フォルダーへ再展開し、全payload hashとPEを再検査します。
Qt/vcpkgをPATHから除き、Qt/SDL等のoverrideを取り除いた環境で本体の版・形式一覧、
workerによる12サンプルの全RGBA比較とnative reader probeを実行します。
probeは検査時だけ展開先へ追加し、配布ZIPには含めません。失敗時は配布artifactを保存しません。
この検査では同梱した`platforms/qwindows.dll`を使い、`QT_QPA_PLATFORM=windows`を指定します。
SDKの試験用offscreen pluginには依存しません。probeのQt初期化と各caseの進捗はstderrへ逐次出力し、
120秒の上限に達した場合も`native-readers-stderr.log`へ途中の診断を残します。

成功時のartifactは以下です（14日間保存）。

- **komamimic-windows-x86_64-zip**：`KomaMimic-1.0.0-windows-x86_64.zip`、
  `BUILD_MANIFEST.json`、`SHA256SUMS`と`sources/`内の本体ソースtar.gz・manifest・checksum。
- **komamimic-windows-x86_64-symbols**：自作exeのPDBと対応するexeのhash/版/commit、symbols ZIPのchecksum。
- **diagnostics-windows-msvc-x64**：従来の診断に加え、ZIP再展開後のCLI・worker・reader検査ログ。

ZIPには版・commit・ソース内容・dirty状態・依存import・同梱ファイルhashを記録します。
インストール済みvcpkgのcopyright等と、Qt SDK内に存在する法的文書・attribution/SPDXを原文で収集します。
Qt通知を発見できたかもmanifestに記録しますが、発見した文書だけで全通知が揃ったとは判断しません。
**最終的な第三者通知と必要な対応ソースの照合、取得物のGUI確認は別途必要です。**
利用者向けWindows導入文書は、この構成のActions取得物を確認してから追加します。
このjobはタグ・Releaseを作成しません。

## macOS arm64の.appと公証なしDMG候補

BuildのmacOS jobは、従来のHomebrew依存とRelease build・full CTest 17群を使います。
`stage_validation.py`で.appを配置し、`package_macos.py`で配布用の.app ZIPとDMGへまとめます。
共通の起動条件・read権限・concurrency、Windowsの依存cacheと他OSのjobは維持します。
macOS jobの上限は配置・圧縮・再展開検査を含め60分です。

配置には`macdeployqt`を使い、Qt・非Qt依存、WebEngine helper・資源、画像/SQLite plugin、
日本語catalog、実行時ロードするSDL3とその内部symlinkを含めます。
検査用probeも同じdeployment処理を通してHomebrewへのload pathを修正し、.appの外へ退避します。
束外へ解決されるrpath（相対指定も含む）を削除し、束内rpathを追加します。
実行時ロード専用のライブラリでも、削除したSDK向けrunpathを束内Frameworks向けへ置き換えます。
配置・検査のパス解釈は共通です。変更したcodeを内側から署名し直す際、
WebEngine helper等のentitlementsを保持して一致を検査します。

packはstageのhash・symlinkと現在のソース内容を照合し、Homebrew依存とその推移的依存の
元の通知文書・SBOM・receipt/formulaを収集します。appへ通知とbuild情報を加えた後、
外側をad-hoc署名し直します。全Mach-Oにarm64を要求し、本体/workerはarm64単独とします。
全load commandとrpathの解決先、codesignのdeep/strict検証、symlinkの束内完結も検査します。

配布用.appは`ditto`でZIPに包み、再展開した実物のhash・実行mode・symlink・署名を確認します。
DMGはHFS+・UDZOで作り、KomaMimic.appとApplicationsリンク、配布情報、manifestを入れます。
`hdiutil verify`後にread-onlyでmountして全payloadと署名を再検査し、作業用フォルダーへコピーします。
そのコピーをHomebrewをPATHから除き、DYLD/Qt/SDL overrideを除いた環境で起動し、版・形式一覧、workerの
12サンプル全RGBAとnative reader probeを検査します。QPAは同梱するcocoaを使います。
probeは検査時だけ追加し、除去後にpayloadと署名が元に戻っていることを確認します。
dyldがHomebrewのライブラリをロードしていないことも診断logで確認し、失敗時もDMGをdetachします。

成功時のartifact **komamimic-macos-arm64-dmg** に次を14日間保存します。

- `KomaMimic-1.0.0-macos-arm64.dmg`
- `KomaMimic-1.0.0-macos-arm64.app.zip`（中はKomaMimic.app一式）
- `BUILD_MANIFEST.json`・`HOMEBREW.json`・`SHA256SUMS`
- `sources/`内の本体ソースtar.gz・manifest・checksum

.appを直接artifactへ渡すと実行modeやsymlinkを保持できないため、DMG/ZIPの内側へ収めます。
失敗時は配布artifactを保存せず、**diagnostics-macos15-arm64**へ配置・署名・otool・
圧縮・mount・CLI/worker/readerのlog/JSONと、保持したentitlementsを保存します。

**Developer ID署名・Appleの公証・notarytool/staplerの実行は行いません。** Apple用secretは不要です。
manifestにもad-hoc/公証なし/手動未確認を記録します。codesign成功をGatekeeper通過と扱いません。
ブラウザーで取得した実物のFinder起動は管理者が最小限確認し、その後にmacOS導入文書を追加します。
最終的な全依存通知・必要な対応ソースの照合は別工程です。タグ・Releaseの公開は行いません。

Linuxでは整合性・依存解決・失敗時のdetach/成果物gate等を模擬試験できますが、
macdeployqt/codesign/lipo/ditto/hdiutil/cocoaでの実動作はmacOS Actionsでの確認範囲です。

## Rocky 8でのLinux AppImage生成

`.github/workflows/rocky8-probe.yml`は、手動実行専用のAppImage workflowです。
Actionsの **Linux AppImage (Rocky 8) → Run workflow → main** から起動します。
Ubuntu 24.04 runner上のRocky Linux 8.10 x86_64コンテナ
（`rockylinux/rockylinux:8.10`）内で、Rocky・EPEL・Ravenの
RPMを導入し、GCC Toolset 12によるRelease build、CLI確認、full CTest 17群を実行します。
QtとQt WebEngineはRavenのRPMを使い、ソースからのビルドは行いません。
Ravenはbase・extras・rx-devel・multimediaを有効にします。multimediaは
Qt WebEngineが要求するFFmpeg・OpenH264・libvpx等の依存を解決するために必要です。
GLibとCairoは`rx-glib2-devel`・`rx-cairo-devel`を導入します。これらのRPMに含まれるpkg-configファイルの
配置先を取得して標準版より優先し、選択された版・配置先を構成ログへ記録します。
これらの`libdir`も`CMAKE_LIBRARY_PATH`へ渡し、Cairo等のライブラリをリンク時に探索できるようにします。
実行用RPMだけでは`.pc`の間接依存が揃わないため、UUID・mount・blkid・SELinux・PCRE・libffiの
開発用RPMも導入します。Poppler helperの`--install-missing-pc-deps`はCIコンテナ専用です。
ソース取得前に使用する全モジュールの`--cflags --libs`を検査し、不足する`.pc`について
DNFの`pkgconfig(名前)`提供機能でパッケージを導入して再検査します。間接依存を含めて解決するまで
繰り返しますが、解消しない依存・版不一致・16回の上限では停止します。フラグの省略や警告の抑制はしません。
このオプションなしではパッケージを追加しません。導入後のRPM一覧も保存します。
Poppler install後は本体と同じpkg-config情報でPoppler・Cairo・GLibを使う小さなC++プログラムを
コンパイル・リンク・実行してから本体の構成へ進みます。
実行時の探索順もRXのGLib・Cairoをシステムの`/usr/lib64`より先にし、構成時と実行時で版が
入れ替わらないようにします。上の小さなプログラムではロードしたライブラリの版も表示します。
Cairoは`rx-cairo`と`rx-cairo-devel`から実在する共有ライブラリを取得し、FindPkgConfigの
`pkgcfg_lib_POPPLER_cairo`へ絶対パスを指定します。開発用`libcairo.so`がリンク切れの場合も、
RPM内のバージョン付き実ファイルを使用できます。リンクの参照先をログに記録し、構成前に
選択したライブラリのロードと実際のCairoバージョンを確認します。
JPEG ABI 62の実ライブラリも提供RPMから取得し、その配置先を実行時とリンク時の
間接依存探索（`-rpath-link`）に追加します。ICUはQtが使う78に揃え、`libicu78-devel`の
ヘッダーと共有ライブラリを明示指定します。主要な共有ライブラリは本体build前に
`ldd`で未解決依存がないことも確認します。
Releaseの最適化を保持して`-g1`の診断情報を付けます。CTestが失敗した場合だけ、
自作fixtureを生成し直して`validation_probe`をGDBで実行し、呼び出し履歴と読込ライブラリを
`probe-debug.log`へ保存します。この診断の結果で元のCTest失敗を成功扱いにはしません。

Qt WebEngine本体とQt TIFF pluginはTIFF ABI 6を要求します。同一プロセスへABI 5も
読み込まないよう、Popplerも`libtiff6-devel`に対してソースからビルドします。
`tools/setup_linux_poppler.py`は[DebianのPopplerソース](https://sources.debian.org/src/poppler/22.12.0-2%2Bdeb12u2/)
22.12.0-2+deb12u2を取得し、ソース記述ファイルのSHA-256・サイズと照合してから
Debianのpatchを順に適用します。Qt pluginはRPMのまま使用します。
使用するGLib APIと画像デコーダーを有効にし、Qtラッパー・ユーティリティ・署名検証・HTTP取得など
本体で使わないPopplerの機能はビルドしません。リンク切れのCairo開発用リンクは作業用prefix内で補います。
PopplerのCMap等も同梱し、AppRunから`POPPLER_DATADIR`で配置先を渡す小さなpatchを適用します。
使用ソース、patch、著作者・ライセンス原文、取得元・checksum・構成情報を保存します。
この依存構成での17群成功を確認してからAppDirを生成します。
Pythonは3.11を明示します。

Rockyのbaseos・appstream・extras・powertoolsは公式配布先`dl.rockylinux.org`を直接使用し、
ミラー切替による長時間待機を避けます。DNFの接続・低速通信のtimeoutは20秒、retryは2回、
Bootstrap step全体の上限は10分です。BootstrapとDNFの詳細ログも診断artifactへ保存します。

Rocky標準のlibarchiveはRAR5対応APIを提供しないため、libarchive 3.8.10も
公式ソースのSHA-256を検証して同じコンテナ内でビルドします。作業用prefixへ配置し、
本体のヘッダー・リンク先・実行時ライブラリをその版へ揃えます。
本体の構成前にRAR5 APIのコンパイル・リンク・呼出しを確認します。
取得したソースと著作権・ライセンス原文、ビルドログも診断artifactに保存します。

依存導入・構成・ビルド・試験のログと導入RPM一覧を
`diagnostics-rocky8-raven-x64` artifactへ14日間保存します。
このworkflowは既存3OSのBuildとは独立し、pushでは起動しません。Releaseやtagは作成しません。

Rocky jobではテスト成功後に`tools/build_appimage.py stage`を実行し、本体とworker、
Qt plugin・WebEngine helper/resources/locales・日本語翻訳・依存共有ライブラリをAppDirへ配置します。
RPATHを相対化し、非同梱とするglibc・ローダー・ホストのOpenGL/EGLライブラリを除き、
ビルド環境のライブラリを参照していないことを確認します。TIFF ABI 5/6の混在を拒否し、
全payload ELFのglibc要求がUbuntu 22.04相当の2.35以下であることを検査します。
通常のAppRunにはsandbox無効化を設定しません。
RPATHの書換えにはSHA-256固定の公式ソースから構築したpatchelf 0.19.2を使います。
0.18.0にも[初期化コードの移動に関する問題](https://github.com/NixOS/patchelf/issues/639)があるため、
builderは0.19.2未満を拒否し、全ELFのsection・LOAD segment・初期化関数の対応を検査します。
`-Wl,-init/-fini`で`.text`内の独自関数を指定したELFも有効です。
NSSは32bit/64bitが共存するホストでもx86_64の実体だけを選択し、glibcのlibmvecは同梱しません。
配置後のAppRunで`--version`と`--formats`を実行してから圧縮へ進みます。

続くUbuntu 22.04コンテナのjobは、Rockyで作ったAppDirのhashを再確認し、
appimagetoolと静的runtimeで1ファイルへまとめます。アプリや依存の再コンパイルはしません。
appimagetoolは[公式リリース1.9.1](https://github.com/AppImage/appimagetool/releases/tag/1.9.1)のURLへ固定し、
`tools/appimage-tools.json`のSHA-256で検証します。runtimeも固定SHA-256で検証し、
その上流continuous assetが更新されてhashが変わった場合は停止します。
更新時は公式release assetと照合し、実物で圧縮・起動検査を済ませてから設定を更新します。
不一致のエラーには期待値と実際のhashを表示します。
Qt・Ravenをインストールしない環境で、AppImageの`--version`・`--formats`、展開後のworker、
生成fixtureのnative reader probeを検査します。CIではFUSEを必要としないextract-and-runを使います。

成功後のartifact **komamimic-linux-x86_64-appimage** に以下を保存します。

- `KomaMimic-1.0.0-linux-x86_64.AppImage`と`SHA256SUMS`
- `~/.local/share/applications/`用の`komamimic.desktop`と、
  `~/.local/share/icons/hicolor/256x256/apps/`用の`komamimic.png`（256×256）
- 同梱ファイルのhash・必要glibc・依存RPM版/ライセンス/Source RPM等の`BUILD_MANIFEST.json`
- `sources/`内の本体ソース・manifest、構築したlibarchiveとpatch適用済みPopplerのソース

外付けのdesktop entryは、その版のAppImageを`~/.local/bin/`から起動します。
AppImage内のdesktop entryは従来のまま保持します。外付け2ファイルのhashをmanifestへ記録し、
AppImage・manifestと合わせて`SHA256SUMS`へ含めます。Ubuntu側でdesktop-file-validateと
checksum検査を通してからartifactへ保存します。利用者の配置・更新・削除は
[インストール文書](../site/user/install.md)を参照してください。

圧縮用の中間artifactは7日間、取得候補と診断artifactは14日間保存します。
Ubuntu側のログは**diagnostics-appimage-ubuntu22-x64**へ保存します。
RPMに含まれるライセンス文書もAppImage内へコピーしますが、正式公開前には依存の対応ソース・
通知の網羅性とAppImage runtime自体の通知も確認します。Actionsの成功はGUI手動確認の代替ではありません。
取得物のFedora等での確認、導入・削除文書と正式Releaseは後続です。

## GitHub上で確立する順序

1. 利用者が公開フォルダを確認し、手動でrepository作成・commit・pushを行います。
2. Webとghで各jobの結果・依存版・skip・ログを確認し、失敗を解消します。
3. macOSの.appとLinuxの配布形式を含め、必要なartifactの生成を整えます。
4. 配布対象の依存通知・対応ソースを揃え、Actionsの取得物を実機で確認します。
5. 取得物に基づく導入・削除手順を作成し、ReleaseとPagesの公開を別に確認します。

本体ビルド定義にはRelease作成はありません。Pages公開は上記の文書専用workflowで行います。
将来の公開jobはPR検証から分離し、必要なjobにだけ書込み権限を与えます。
手動テストをheadless CIの成功から代替したことにはしません。
