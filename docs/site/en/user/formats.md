# Supported formats and limitations

[日本語](../../user/formats.md) | English

| Type | Main support |
|---|---|
| Images | Formats handled by installed Qt image plugins, including JPEG, PNG, BMP, GIF, WebP, and TIFF |
| Animated / multipage images | GIF, WebP, APNG, multipage TIFF |
| Archives | ZIP/CBZ, RAR/CBR, 7z/CB7, LHA/LZH, tar variants |
| PDF | Page rendering and loading individual embedded images |
| Documents | TXT, Aozora Bunko ruby annotations and illustrations, Markdown, EPUB |
| Bundled script readers | MAG/MAX, legacy PIC, PIC2, concatenated PPM |

AVIF, JPEG XL, HEIF, and similar formats depend on the plugins actually installed or
bundled. Support does not guarantee every variant sharing the same extension.
Split archives, self-extracting archives, and all encryption methods are not supported.

Unicode information in ZIP/LHA takes priority; entries without it default to CP932.
For garbled names, preview and choose an encoding under Tools → Archive name
encoding. Enter passwords in Preferences and reopen the book. Passwords are kept
only for the current application session.

KomaMimic is an independent implementation inspired by MangaMeeya and is not fully
compatible. It does not provide compatible execution of Susie/AviSynth DLLs, legacy
proprietary scripts, GCA, or binary MMT. Use [JavaScript API 1](../scripts/api.md)
to extend image readers and filters.


See [Differences from MangaMeeya](compatibility.md) for compatibility and sorting persistence.

## Illustrations in Aozora Bunko TXT

The [Aozora image annotation](https://www.aozora.gr.jp/annotation/graphics.html)
`［＃Description（image.png、横320×縦240）入る］` and its older form without dimensions are supported,
as are caption annotations. Images fit the reading area while preserving their aspect ratio.

Standalone TXT resolves images beneath its containing directory; archived TXT resolves relative paths
within the same archive. Keep the TXT and image directory structure together. Japanese filenames work too.
Legacy illustrated TXT using `<img src="image.jpg">` is also supported as an image reference.
Other attributes such as event handlers are discarded; arbitrary HTML is not executed.
External URLs and images outside the allowed root are blocked. Unresolved references remain visible as annotations.
Limits are 256 distinct image references, 16 MiB and 16 million pixels per image, and 64 MiB of converted images in total.
Not every Aozora annotation or HTML tag is interpreted.
