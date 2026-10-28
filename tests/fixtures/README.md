# 合成fixture

画像サンプル集ではなく、CI・回帰検証に必要な入力と期待結果を管理します。旧配布物の画像・実際の漫画や書籍を置きません。生成器・自作の期待値はGPL-3.0-onlyです。

`generate_script_samples.py`はPython 3の標準ライブラリだけでMAG／旧PIC／PIC2の12ケースと基準PNG、同内容のZIPを生成します。workerへ渡してRGBA全画素を照合できます。

```sh
python3 tests/fixtures/generate_script_samples.py --verify-worker ./build/komamimic-script-worker
```

生成先の既定は`tests/fixtures/generated/script-readers`と隣のZIPです。出力先が存在する場合は上書きしません。生成済みデータはGit・公開ソース・バイナリ配布へ含めません。CI試験は新しい一時フォルダを使います。ZIPの時刻・modeを固定し、同じ生成環境で同じbyte列になることを試験します。

他の画像・書庫・文書fixtureは`tests/test_support.*`と各試験内で必要時に生成します。公開できる固定fixtureが将来必要になったら、最小入力・出所・ライセンス・期待結果・公開選別規則を追加してレビューします。内部実例の変名・縮小だけで公開fixtureにしません。

## 実機確認用の合成入力

`generate_validation_samples.py --output <新しいフォルダ>`で番号付きPNG、UTF-8/CP932 ZIP、隣接書庫、tar/tar.gz、TXT/Markdown混在ZIP、GIF/APNG/TIFF、PDF、TXT/Markdown、reflow/fixed EPUB、自作INI、壊れた入力と上記script-reader入力を生成します。Python標準ライブラリだけを使います。実画面の操作と期待結果は[実機検証手順](../../docs/developer/platform-validation.md)にあります。

通常は`tools/prepare_validation.py`から呼び、選別済みソース・入力・hashをまとめた持出しZIPにします。生成データはテスト用で、公開リポジトリ／利用者向け配布の画像サンプル集にはしません。
