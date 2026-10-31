# 対応形式と制限

日本語 | [English](../en/user/formats.md)

| 種類 | 主な対応 |
|---|---|
| 画像 | JPEG、PNG、BMP、GIF、WebP、TIFFなど、導入済みQt画像pluginが扱う形式 |
| アニメーション・複数ページ | GIF、WebP、APNG、複数ページTIFF |
| 書庫 | ZIP/CBZ、RAR/CBR、7z/CB7、LHA/LZH、tar系 |
| PDF | ページ描画、内部画像単位の読込 |
| 文書 | TXT・青空文庫ルビ/挿絵・Markdown・EPUB |
| 同梱script reader | MAG/MAX、旧PIC、PIC2、連結PPM |

AVIF・JPEG XL・HEIFなどは実際に導入・同梱されたpluginに依存します。
同じ拡張子のすべての派生形式を保証するものではありません。
書庫の分割・自己解凍・すべての暗号化方式には対応していません。

ZIP/LHAのUnicode情報を優先し、情報がない場合の初期文字コードはCP932です。
文字化けする場合は「ツール → 書庫内の名前の文字コード」でプレビューして選びます。
パスワードは環境設定で入力して本を開き直します。その起動中だけ保持します。

## 青空文庫TXTの挿絵

[青空文庫の画像注記](https://www.aozora.gr.jp/annotation/graphics.html)の
`［＃説明（画像.png、横320×縦240）入る］`と、寸法を省いた旧記法に対応します。
キャプション注記にも対応します。本文中の画像は縦横比を保って表示範囲に収めます。

単体TXTではTXTのあるフォルダ以下、書庫内TXTでは同じ書庫内の相対パスから画像を読みます。
TXTと画像のフォルダ構成を維持してください。日本語の画像名も使用できます。
従来の挿絵付きTXTにある`<img src="画像.jpg">`も画像参照として扱います。
これはHTML文書の実行機能ではなく、イベント属性等は取り込みません。
外部URLや許可範囲外の画像は読み込まず、解決できない参照は注記を残します。
画像は最大256種類の参照、1画像16MiB・1600万画素、変換後の画像合計64MiBまでです。
すべての青空文庫注記やHTMLタグの解釈を保証するものではありません。

KomaMimicはMangaMeeyaの独立した実装で、完全互換ではありません。
Susie/AviSynthのDLL、旧独自スクリプト、GCA、バイナリMMTの互換実行は提供しません。
画像読込・filterの拡張は[JavaScript API 1](../scripts/api.md)を使います。


詳細は[MangaMeeyaとの違い](compatibility.md)を参照してください。
