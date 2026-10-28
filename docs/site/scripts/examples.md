# 動く最小例

例のソースは `examples/scripts/` にあります。いずれも自作の小さな入力を使い、
既存の画像形式との重複を避けています。ソースはGPL-3.0-onlyです。

## 1画素のreader

`toy-reader/manifest.json`:

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

`toy-reader/decode.js`:

```javascript
function decode(input, context) {
    const b = new Uint8Array(input);
    if (b.length !== 6 || b[0] !== 84 || b[1] !== 79 || b[2] !== 89)
        throw new Error("expected TOY and three RGB bytes");
    const rgba = new Uint8Array([b[3], b[4], b[5], 255]);
    return {width: 1, height: 1, pixels: rgba.buffer};
}
```

サンプルをPythonで作ります。

```sh
python3 -c "from pathlib import Path; Path('../tmp/sample.toy').write_bytes(b'TOY'+bytes([255,64,0]))"
```

環境設定でtoy-readerを登録して再起動し、sample.toyを開くと橙色の1画素になります。
開発ビルドなら次の起動もできます。

```sh
../tmp/build-linux/komamimic --script-readers examples/scripts/toy-reader ../tmp/sample.toy
```

## 2ページのreader

`examples/scripts/toy-pages/` は `TPG2` の4byteだけを入力とし、
inspectで2ページを返します。pageIndex=0は赤、1は青の1画素です。
manifestの `multipage: true` と、ページ番号を検査するdecodeを確認してください。

```sh
python3 -c "from pathlib import Path; Path('../tmp/sample.toyp').write_bytes(b'TPG2')"
../tmp/build-linux/komamimic --script-readers examples/scripts/toy-pages ../tmp/sample.toyp
```

## 設定付きfilter

`examples/scripts/toy-filter/` は `invert`（boolean）を宣言し、有効なときだけ色反転を返します。

```javascript
function filter(image, context) {
    return context.settings.invert ? [{op: 'invert'}] : [];
}
```

環境設定で登録して再起動し、filter一覧から「Toy invert」を選びます。
画像加工を有効にして自作画像を開きます。設定値を変えたら本を開き直してください。
登録・保存・取消の扱いは[導入手順](index.md)、返り値は[API仕様](api.md)に従います。
