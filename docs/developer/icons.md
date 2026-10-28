# アイコン

SVG原本は `assets/icons/komamimic.svg`、symbol版は `komamimic-symbol.svg` です。
PNG各サイズ・Windows ICO・macOS ICNSを `assets/icons/`、desktop定義を `assets/` に置きます。
これらはKomaMimic独自のGPL-3.0-onlyのassetです。

再生成にはCairoSVGとPillowを使います。`python3 tools/generate_icons.py` はリポジトリ内の
既存画像を更新し、`--check` は変更せずに比較します。作業コピーで実行してください。
