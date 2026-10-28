# 製品アイコン

SVG原本は `assets/icons/komamimic.svg`、symbol版は `komamimic-symbol.svg` です。
PNG各サイズ・Windows ICO・macOS ICNSを `assets/icons/`、desktop定義を `assets/` に置きます。
これらはKomaMimic独自のGPL-3.0-onlyのassetです。

再生成にはCairoSVGとPillowを使います。`python3 tools/generate_icons.py` はリポジトリ内の
既存画像を更新し、`--check` は変更せずに比較します。作業コピーで実行してください。
原本と生成物を目視で確認し、`tools/public_source.json` のreviewed_assetsにあるSHA-256を
更新します。hashを変えただけで視覚・出所の確認を省略しません。
WindowsとmacOSでは最終packageのExplorer/Finder・Dock等で実際の表示を確認してください。
