# Working minimal examples

[日本語](../../scripts/examples.md) | English

Example source is in `examples/scripts/`. Each uses a small synthetic input and
avoids conflicts with existing image formats. The source is GPL-3.0-only.

## A one-pixel reader

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

Create a sample with Python:

```sh
python3 -c "from pathlib import Path; Path('../tmp/sample.toy').write_bytes(b'TOY'+bytes([255,64,0]))"
```

Register toy-reader in Preferences, restart, and open sample.toy to see one orange
pixel. A development build can also be launched with:

```sh
../tmp/build-linux/komamimic --script-readers examples/scripts/toy-reader ../tmp/sample.toy
```

## A two-page reader

`examples/scripts/toy-pages/` accepts only the four bytes `TPG2`. Inspect returns
two pages: pageIndex=0 is one red pixel; pageIndex=1 is one blue pixel. Check the
manifest's `multipage: true` and decode's page-index validation.

```sh
python3 -c "from pathlib import Path; Path('../tmp/sample.toyp').write_bytes(b'TPG2')"
../tmp/build-linux/komamimic --script-readers examples/scripts/toy-pages ../tmp/sample.toyp
```

## A configurable filter

`examples/scripts/toy-filter/` declares `invert` (boolean) and returns an inversion
operation only when enabled.

```javascript
function filter(image, context) {
    return context.settings.invert ? [{op: 'invert'}] : [];
}
```

Register it in Preferences, restart, and select “Toy invert” from the filter list.
Enable image processing and open an image you created. Reopen the book after
changing settings. Registration, saving, and cancellation follow the
[installation procedure](index.md); return values follow the [API specification](api.md).
