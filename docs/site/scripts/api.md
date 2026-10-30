# JavaScript API 1

日本語 | [English](../en/scripts/api.md)

アプリの版とは独立したAPI番号で、manifestの `apiVersion` は `1` です。
ES module、Node.jsのrequire、Promiseを返す非同期関数は使いません。
通常の関数として `decode`、必要な場合は `inspect` または `filter` を定義します。

## manifest

```json
{
  "apiVersion": 1,
  "id": "org.example.reader.toy",
  "name": "Toy image",
  "extensions": ["toy"],
  "magic": "544f59",
  "script": "decode.js",
  "license": "GPL-3.0-only"
}
```

readerはextensionsとmagicを持ち、filterは `kind: "filter"` を指定します。
magicは先頭1〜32 byteの16進表現で、一致は拡張子より優先します。
一覧の列挙は拡張子を使うため、未知拡張子の自動発見を保証しません。
extensionsは小文字英数字1〜12文字、scriptはpackage直下の通常の.jsファイル名です。
外への相対パスやsymlinkは拒否します。異なるID間の形式重複は登録時にエラーにします。
IDは英数字・下線・ピリオド・ハイフンの1〜64文字、script名は英数字・下線・ハイフンと
末尾の `.js` です。magicの先頭部分だけが他のreaderと重なる定義も避けてください。
nameとlicenseは説明用で、権利・ライセンスの適合性を自動判定しません。

## 単画像reader

`decode(input, context)` は `{width, height, pixels}` を返します。
inputは符号化されたファイル全体のArrayBufferです。書庫の展開やファイル取得は本体が担当します。
width/heightは正の整数、pixelsは `width * height * 4` byteちょうどのArrayBufferです。
RGBA8、上から下・左から右、非premultiplied、行paddingなしで返します。
Uint8Arrayそのものではなく `.buffer` を返してください。subarrayは元配列全体を参照するため、
必要なら `slice().buffer` を使います。サムネイルでも全画像を復号して本体で縮小します。

## 複数ページreader

manifestに `multipage: true` を追加します。
`inspect(input, context)` は `{pages: ページ数}`（1〜10000）を返します。
decodeは `context.pageIndex` の0始まりページを返します。同じ入力と設定に対して
inspectとdecodeの結果を一貫させます。アニメーションのframe APIではありません。

## context

| 項目 | 意味 |
|---|---|
| apiVersion | 1 |
| maxBytes | 符号化入力上限、64 MiB |
| maxPixels | 出力画素上限、48 × 1024 × 1024 |
| maxPages | ページ数上限、10000 |
| pageIndex | 0始まりの対象ページ |
| settings | schemaの既定値・型・範囲を適用した設定 |

## filter

`filter(image, context)` は最大32個の操作を配列で返します。
imageはwidth/heightだけの記述子です。JSへ全画素を渡さず、本体が検証してnative加工を実行します。

| op | パラメータ |
|---|---|
| grayscale / invert / flipH / flipV | なし |
| gamma | value: 0.1〜10 |
| brightness / contrast | value: -100〜100（整数として使用） |
| rotate | value: -270〜270の90度単位 |
| resize | width/height: 1〜16384、積はmaxPixels以内 |

filterは通常のnative加工の後、表示と加工保存へ適用します。原本byte保存、文書ビュー、
表紙プレビューには適用しません。アニメーションはframeごとに適用してdelayを保持します。
失敗を黙って無加工へ置き換えません。filterを使用するには画像加工を有効にします。

## 設定schema

```json
"settings": {
  "grayscale": {"type": "boolean", "default": true},
  "gamma": {"type": "number", "minimum": 0.1, "maximum": 5, "default": 1},
  "contrast": {"type": "integer", "minimum": -100, "maximum": 100, "default": 10}
}
```

キーは英字で始まる英数字・下線32文字以内、最大32項目です。
boolean/integer/numberを扱い、環境設定に入力欄を生成します。readerも同じschemaを使えます。
数値のminimum/maximumは-1000000〜1000000で、minimum ≤ default ≤ maximumとします。
integerは範囲と既定値も整数です。数値の範囲を省略すると±1000000、defaultを省略するとminimumです。
`scripts/<id>/<key>` に保存し、schemaにない値は渡しません。設定snapshotは本を開くときに取得します。
文字列・パス権限・本別script設定は提供しません。
filterはIDを `scripts/filter` に保存します。ID `0` と `1` は予約されているので使用しません。

## 実行制限と診断

| 対象 | 上限 |
|---|---|
| manifest / JS source | 16 KiB / 256 KiB |
| 入力 / 出力 | 64 MiB / 48 Mi画素 |
| request header / filter操作列JSON | 1 MiB / 16 KiB |
| 通常の1要求 | 親の起動・転送を含め10秒 |

要求ごとに新しいworkerのQJSEngineを使用します。例外、不正寸法、byte数、期限超過は診断になります。
取消・期限でworkerを停止し、古い世代の結果をGUIへ反映しません。
Linuxの仮想アドレス空間とWindowsのJob Objectでは2 GiB上限を設定します。
macOSに同等の信頼できる上限はなく、入力・出力・時間制限を使います。
これはアプリ全体のRSS制限やOS全体を保護する汎用sandboxではありません。

filesystem/network/process/QObject/QML importのnative能力は公開しません。
JavaScript標準の日時・乱数等を完全に禁止しているわけではありません。
CLIの `--formats` / `--commands` で保存登録に問題があればstderrへ出し終了コード2を返します。
