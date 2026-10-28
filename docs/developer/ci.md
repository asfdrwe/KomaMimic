# CIの構成と確立手順

## 文書サイトとGitHub Pages

`.github/workflows/pages.yml`は、mainへの`docs/**`の変更、MkDocs設定・依存指定・
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

## 本体ビルドCI（後続）

以下は次段階で導入・確認する本体ビルドCIの構成です。現在は文書用workflowだけを追加しています。

定義は `.github/workflows/build.yml` です。push（main/master・vタグ）、pull request、
手動実行を入口に、contentsの権限をreadへ限定しています。
現在の定義と、GitHub上で実行済みの結果を区別してください。

| job | 内容 | 保存するもの |
|---|---|---|
| source-checks | Git追跡ファイルと公開選択、翻訳、リンク、action pin、actionlint、ソースarchiveの再現性 | ソース配布候補 |
| linux | Fedora 44 container、Release build、CLI、full CTest | 構成・試験ログ |
| macos | macOS 15 arm64、Homebrew依存、Release build、CLI、full CTest | 構成・試験ログ |
| windows | Windows 2022/MSVC、公式Qt SDK、Release専用vcpkg、platform検証、runtime配置 | 検証ZIP・PDB、構成・試験・配置ログ |

GitHub Actionsはcommit SHAで固定しています。WindowsはQt SDKとvcpkgのbinary cacheを分け、
設定・manifest・runner image等をkeyへ含めます。Qtをソースから再構築しません。
Fedoraのroot containerでのWebEngine sandbox無効化は試験プロセスに限った設定です。
通常の利用環境への設定方法として案内しません。

## GitHub上で確立する順序

1. 利用者が公開フォルダを確認し、手動でrepository作成・commit・pushを行います。
2. Webとghで各jobの結果・依存版・skip・ログを確認し、失敗を解消します。
3. macOSの.appとLinuxの配布形式を含め、必要なartifactの生成を整えます。
4. 配布対象の依存通知・対応ソースを揃え、Actionsの取得物を実機で確認します。
5. 取得物に基づく導入・削除手順を作成し、ReleaseとPagesの公開を別に確認します。

本体ビルド定義にはRelease作成はありません。Pages公開は上記の文書専用workflowで行います。
将来の公開jobはPR検証から分離し、必要なjobにだけ書込み権限を与えます。
手動テストをheadless CIの成功から代替したことにはしません。
