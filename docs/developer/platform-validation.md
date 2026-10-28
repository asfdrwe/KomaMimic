# 実機検証

自動試験の後、実際に配置した成果物を対象OSで確認します。
試験入力は `tests/fixtures/generate_validation_samples.py` が生成する自作データです。
原作ファイルや持込みの漫画・書籍は公開検証セットへ含めません。

```sh
python3 tools/prepare_validation.py --output ../tmp/validation-kit
python3 tools/prepare_validation.py --verify ../tmp/validation-kit
```

セットの `source/` を対象OSでビルドします。Windowsの開発環境は
[ビルド手順](building.md)と[README](../../README.md)を参照してください。
sourceを移動した場合は以前のCMakeCacheをコピーせず、新しいbuildを作ります。

```sh
python3 tools/run_validation.py run --build-dir ../tmp/build-native \
  --fixtures ../tmp/validation-kit/fixtures --output ../tmp/results/native-01 \
  --jobs 4 --test-scope full
python3 tools/stage_validation.py --build-dir ../tmp/build-native --output ../tmp/stage-native-01
```

WindowsはQt/vcpkgの環境を設定して `--windows-mode prebuilt --test-scope platform` を使用します。
staging後のCLIとGUIで同じexe・workerを使い、配置manifestのhashで識別します。

## 手動確認

`run_validation.py` が作る `manual-guide.md` と `manual.csv` に、実行した範囲・観察を記録します。
以下を中心に、変更内容に応じて対象を選びます。

- 開発用PATHなしの起動、メニュー、日本語、一覧とReturn決定、全画面、下部バー。
- 画像・書庫・PDF・TXT/Markdown/EPUBの表示、移動、書庫内文書、再開。
- 新規設定と既存設定、スクリプト登録・設定・取消・再起動、filter。
- キャッシュ、保存・書出し、更新監視、OS固有の消灯抑制・DPI・印刷・入力機器。

確認していない項目はNOT_RUNまたは理由付きNAにし、既存の成功から推定しません。
自動試験のskipと手動の未実行を混同しません。変更が小さい場合は必要な回帰だけを選びます。

## macOSの取得経路

最終配布物ではブラウザで取得した.appをFinderから起動し、Gatekeeperの警告と、
必要な場合のアプリ個別許可後の起動を確認します。
