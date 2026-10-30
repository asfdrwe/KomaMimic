# JavaScript API 1

[日本語](../../scripts/api.md) | English

The API number is independent of the application version; manifest `apiVersion`
is `1`. Do not use ES modules, Node.js require, or asynchronous functions returning
Promises. Define `decode` as an ordinary function, with `inspect` or `filter` when
needed.

## Manifest

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

Readers declare extensions and magic; filters specify `kind: "filter"`. Magic is
1–32 leading bytes expressed as hexadecimal and takes precedence over extension
matching. List enumeration uses extensions, so automatic discovery of unknown
extensions is not guaranteed. Extensions contain 1–12 lowercase alphanumeric
characters; script is an ordinary `.js` file directly inside the package. Paths
outside the package and symlinks are rejected. Conflicting formats across different
IDs cause registration errors.

IDs contain 1–64 alphanumeric characters, underscores, periods, or hyphens. Script
names use alphanumeric characters, underscores, or hyphens followed by `.js`.
Avoid magic definitions whose prefixes overlap those of other readers. Name and
license are descriptive fields; the app does not automatically determine ownership
or license compatibility.

## Single-image readers

`decode(input, context)` returns `{width, height, pixels}`. Input is an ArrayBuffer
containing the entire encoded file; the application handles file access and archive
extraction. Width/height must be positive integers. Pixels must be an ArrayBuffer
of exactly `width * height * 4` bytes: RGBA8, top-to-bottom and left-to-right,
non-premultiplied, without row padding.

Return `.buffer`, not the Uint8Array itself. A subarray still references the whole
underlying buffer; use `slice().buffer` when needed. Thumbnails also decode the
entire image before the application scales it down.

## Multipage readers

Add `multipage: true` to the manifest. `inspect(input, context)` returns
`{pages: pageCount}`, with 1–10000 pages. Decode returns the zero-based page selected
by `context.pageIndex`. Inspect and decode must be consistent for the same input
and settings. This is not an animation-frame API.

## Context

| Field | Meaning |
|---|---|
| apiVersion | 1 |
| maxBytes | Encoded input limit: 64 MiB |
| maxPixels | Output pixel limit: 48 × 1024 × 1024 |
| maxPages | Page limit: 10000 |
| pageIndex | Zero-based target page |
| settings | Settings after applying schema defaults, types, and ranges |

## Filters

`filter(image, context)` returns an array of at most 32 operations. Image is a
descriptor containing only width/height. Full pixel data is not passed to JavaScript;
the application validates operations and performs native processing.

| op | Parameters |
|---|---|
| grayscale / invert / flipH / flipV | None |
| gamma | value: 0.1–10 |
| brightness / contrast | value: -100–100, used as an integer |
| rotate | value: multiples of 90 from -270 to 270 |
| resize | width/height: 1–16384; their product must not exceed maxPixels |

Filters run after normal native processing and apply to display and processed-image
saving. They do not apply to original-byte saving, the document view, or cover
previews. Animation is filtered frame by frame, preserving delays. Failure is not
silently replaced with unprocessed output. Enable image processing to use filters.

## Settings schema

```json
"settings": {
  "grayscale": {"type": "boolean", "default": true},
  "gamma": {"type": "number", "minimum": 0.1, "maximum": 5, "default": 1},
  "contrast": {"type": "integer", "minimum": -100, "maximum": 100, "default": 10}
}
```

Keys start with a letter, contain only alphanumeric characters or underscores, and
have at most 32 characters. Up to 32 items are allowed. Boolean/integer/number
settings generate fields in Preferences. Readers can use the same schema. Numeric
minimum/maximum values are within -1000000–1000000, with
`minimum ≤ default ≤ maximum`. Integer bounds and defaults must also be integers.
Omitting numeric bounds uses ±1000000; omitting default uses minimum.

Values are stored under `scripts/<id>/<key>`; values outside the schema are not
passed. A settings snapshot is taken when opening the book. Strings, path
permissions, and per-book script settings are not provided. The selected filter ID
is stored under `scripts/filter`. IDs `0` and `1` are reserved and must not be used.

## Execution limits and diagnostics

| Target | Limit |
|---|---|
| Manifest / JS source | 16 KiB / 256 KiB |
| Input / output | 64 MiB / 48 Mi pixels |
| Request header / filter operation-list JSON | 1 MiB / 16 KiB |
| Normal request | 10 seconds, including parent-side startup and transfer |

Each request uses a fresh worker QJSEngine. Exceptions, invalid dimensions or byte
counts, and timeouts produce diagnostics. Cancellation or deadlines stop the
worker; old-generation results cannot update the GUI. Linux virtual address space
and Windows Job Objects impose a 2 GiB limit. macOS has no equally reliable limit;
input, output, and time limits apply instead. This is neither an application-wide
RSS limit nor a general sandbox protecting the whole OS.

Native filesystem/network/process/QObject/QML import capabilities are not exposed.
Standard JavaScript date/time, random numbers, and similar facilities are not
completely prohibited. CLI `--formats` / `--commands` reports invalid saved
registrations to stderr and returns exit code 2.
