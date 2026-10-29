# 自動テスト

Qt TestとPython 3.9以上が必要です。WindowsのSDK取得helperにはPython 3.10以上を使用します。
以下はLinux/macOSの例です。テスト用のbuildはREADMEのアプリ専用buildとは分けます。

```sh
python3 tools/check_source.py
cmake -S . -B build-test -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DKOMAMIMIC_TEST_SCOPE=full
cmake --build build-test --parallel 4
ctest --test-dir build-test --output-on-failure --parallel 2 --no-tests=error
```

WindowsはREADMEの環境を準備し、別のbuildで実行します。

```powershell
cmake --preset windows-fast -B build-windows-test -DBUILD_TESTING=ON
cmake --build build-windows-test --config Release --parallel 4
ctest --test-dir build-windows-test -C Release --output-on-failure --parallel 2 --no-tests=error
```

Qtとvcpkg DLLのPATH、Qt pluginの探索先もビルド手順と同じように設定してください。

## 試験範囲

`full` は17群です。reader、UI、feature、format、PDF、CLI、script、PIC、document、
power、evolution、cache、i18n、archive_text、legacy_settingsに、C localeのformatと
Pythonのdistribution検査を加えます。

`platform` は9群です。CLI・UI・PDF・script・document・power・cache・i18n・distributionを選び、
一部のQt群はOS連携に必要な関数に限定します。Windows-fastの既定はこちらです。
cache群は、ZIP読込・画像取得の48箇所の中止タイミングでプロセスが終了する回帰試験に限定します。
9群の成功を全機能の17群成功と記録しないでください。

```sh
ctest --test-dir build-test -R '^(ui|script)_tests$' --output-on-failure
```

## 実行条件と結果の読み方

CTestのQt試験はoffscreenを設定し、一時設定と自作入力を使います。
実WebEngineやローカルIPC、D-Busが使えない実行環境では、理由付きskipになる場合があります。
CTestの成功件数、Qtのpass/fail/skip、Pythonの実行件数を区別して記録してください。
Qtがskipした場合に実画面確認を完了したことにはなりません。
Linuxの専用D-Bus試験には `dbus-daemon` が必要です。

`distribution_tests` は公開ソースの再出力、未分類データの拒否、再現可能なソースarchive、
合成fixtureの読込・RGBA照合、検証・配置helperを検査します。
Git追跡検査の試験では一時ディレクトリ内だけに小さなGit repositoryを作ります。
作業中の公開フォルダを初期化する処理ではありません。

## ログ付き検証

```sh
python3 tools/prepare_validation.py --output validation-kit
python3 tools/prepare_validation.py --verify validation-kit
python3 tools/run_validation.py run --build-dir build-test \
  --fixtures validation-kit/fixtures --output results/run-01 \
  --jobs 4 --test-scope full
```

出力先は毎回新しい名前を使います。結果JSON・CTestログ・依存版・source hashを保持し、
古い失敗ログや別のbuildの成功と混ぜません。実機試験は[別手順](platform-validation.md)です。
