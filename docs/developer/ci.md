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
| macos | macOS 15 arm64、Homebrew依存、Release build、CLI、full CTest 17群 | 構成・試験ログ、JUnit XML |
| windows | Windows 2022/MSVC、公式Qt SDK、Release専用vcpkg、platform検証8群、runtime配置 | 検証ZIP・PDB、構成・試験・配置ログ、JUnit XML、結果JSON |

共通検査の成功後、3OSのjobを並列実行します。OSごとは独立したjobなので、
あるOSの失敗を理由に他のOSのjobをキャンセルしません。同じブランチ・PRに新しいcommitが
来た場合は古い実行を取り消します。push・PRとも変更が`docs/**`だけなら本体workflowを起動しません。
コード等を同時に変更した場合は本体workflowも起動します。手動実行は変更パスに関係なく使用できます。

| 変更内容 | 本体ビルド・テスト | MkDocs・Pages |
|---|---|---|
| `docs/developer/**`だけ | 省略 | 省略 |
| `docs/site/**`だけ（画像を含む） | 省略 | 実行（PRでは公開しない） |
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

ソース配布候補とWindows検証ZIPはActionsのartifactとして保存します。WindowsのZIPには本体とworker、
依存DLL、Qt plugin・WebEngine資源・翻訳、自コードのPDBと通知文書を含めます。
Linux/macOSは今回ログ保存までで、配布用バイナリartifactは後続で整えます。
このworkflowはtag・GitHub Releaseの作成や自動公開を行いません。

初回は管理者がworkflowと本文を手動commit・pushし、Actionsの**Build**で各jobを確認します。
同じ画面のRun workflowからmainを選んで手動実行もできます。PATの追加は不要です。

GitHub Actionsはcommit SHAで固定しています。WindowsはQt SDKとvcpkgのbinary cacheを分け、
設定・manifest・runner image等をkeyへ含めます。Qtをソースから再構築しません。
Fedoraのroot containerでのWebEngine sandbox無効化は試験プロセスに限った設定です。
通常の利用環境への設定方法として案内しません。

## Rocky 8での試験ビルド

`.github/workflows/rocky8-probe.yml`は、AppImageのビルド基準環境を検討するための
手動実行専用workflowです。Actionsの **Rocky 8 build probe → Run workflow** から起動します。
Ubuntu 24.04 runner上のRocky Linux 8.10 x86_64コンテナ
（`rockylinux/rockylinux:8.10`）内で、Rocky・EPEL・Ravenの
RPMを導入し、GCC Toolset 12によるRelease build、CLI確認、full CTest 17群を実行します。
QtとQt WebEngineはRavenのRPMを使い、ソースからのビルドは行いません。
Ravenはbase・extras・rx-devel・multimediaを有効にします。multimediaは
Qt WebEngineが要求するFFmpeg・OpenH264・libvpx等の依存を解決するために必要です。
Popplerは必要な22.0以上を満たすため`rx-poppler-glib-devel`を使い、その依存にも
`rx-glib2-devel`・`rx-cairo-devel`を導入します。これらのRPMに含まれるpkg-configファイルの
配置先を取得して標準版より優先し、選択された版・配置先を構成ログへ記録します。
Pythonは3.11を明示します。

Rocky標準のlibarchiveはRAR5対応APIを提供しないため、libarchive 3.8.10だけは
公式ソースのSHA-256を検証して同じコンテナ内でビルドします。作業用prefixへ配置し、
本体のヘッダー・リンク先・実行時ライブラリをその版へ揃えます。
本体の構成前にRAR5 APIのコンパイル・リンク・呼出しを確認します。
取得したソースと著作権・ライセンス原文、ビルドログも診断artifactに保存します。

依存導入・構成・ビルド・試験のログと導入RPM一覧を
`diagnostics-rocky8-raven-x64` artifactへ14日間保存します。
この試験は既存3OSのBuildとは独立し、pushでは起動しません。AppImageの生成や公開は行いません。
依存の同時導入と全テストの成功はGitHub上の実行結果で確認します。
Rocky上でのビルド成功だけでは、同梱依存を含むAppImageの他OSでの互換性は確定しません。

## GitHub上で確立する順序

1. 利用者が公開フォルダを確認し、手動でrepository作成・commit・pushを行います。
2. Webとghで各jobの結果・依存版・skip・ログを確認し、失敗を解消します。
3. macOSの.appとLinuxの配布形式を含め、必要なartifactの生成を整えます。
4. 配布対象の依存通知・対応ソースを揃え、Actionsの取得物を実機で確認します。
5. 取得物に基づく導入・削除手順を作成し、ReleaseとPagesの公開を別に確認します。

本体ビルド定義にはRelease作成はありません。Pages公開は上記の文書専用workflowで行います。
将来の公開jobはPR検証から分離し、必要なjobにだけ書込み権限を与えます。
手動テストをheadless CIの成功から代替したことにはしません。
