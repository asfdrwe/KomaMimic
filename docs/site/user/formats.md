# 対応形式と制限

| 種類 | 主な対応 |
|---|---|
| 画像 | JPEG、PNG、BMP、GIF、WebP、TIFFなど、導入済みQt画像pluginが扱う形式 |
| アニメーション・複数ページ | GIF、WebP、APNG、複数ページTIFF |
| 書庫 | ZIP/CBZ、RAR/CBR、7z/CB7、LHA/LZH、tar系 |
| PDF | ページ描画、内部画像単位の読込 |
| 文書 | TXT・青空文庫ルビ・Markdown・EPUB |
| 同梱script reader | MAG/MAX、旧PIC、PIC2、連結PPM |

AVIF・JPEG XL・HEIFなどは実際に導入・同梱されたpluginに依存します。
同じ拡張子のすべての派生形式を保証するものではありません。
書庫の分割・自己解凍・すべての暗号化方式には対応していません。

ZIP/LHAのUnicode情報を優先し、情報がない場合の初期文字コードはCP932です。
文字化けする場合は「ツール → 書庫内の名前の文字コード」でプレビューして選びます。
パスワードは環境設定で入力して本を開き直します。その起動中だけ保持します。

KomaMimicはMangaMeeyaの独立した実装で、完全互換ではありません。
Susie/AviSynthのDLL、旧独自スクリプト、GCA、バイナリMMTの互換実行は提供しません。
画像読込・filterの拡張は[JavaScript API 1](../scripts/api.md)を使います。
