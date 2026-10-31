# Supported formats and limitations

[日本語](../../user/formats.md) | English

| Type | Main support |
|---|---|
| Images | Formats handled by installed Qt image plugins, including JPEG, PNG, BMP, GIF, WebP, and TIFF |
| Animated / multipage images | GIF, WebP, APNG, multipage TIFF |
| Archives | ZIP/CBZ, RAR/CBR, 7z/CB7, LHA/LZH, tar variants |
| PDF | Page rendering and loading individual embedded images |
| Documents | TXT, Aozora Bunko ruby annotations, Markdown, EPUB |
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

Custom thumbnails support columns, dimensions, spacing, filename lines, starting
corner and decorative frames. They do not reproduce all original downsampling
algorithms, JPEG subsampling controls, BMP/JPEG/PNG cache formats, custom cache
extensions or custom storage locations. Original thumbnail caches are not imported.
