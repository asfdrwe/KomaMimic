# CIの構成と確立手順

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

既存定義にはRelease作成やPagesへのdeployはありません。MkDocsは現在ローカルbuildのみです。
将来の公開jobはPR検証から分離し、必要なjobにだけ書込み権限を与えます。
手動テストをheadless CIの成功から代替したことにはしません。
