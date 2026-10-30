# Bundled readers and filters

[日本語](../../scripts/bundled.md) | English

Bundled packages are embedded in the app and work without extra registration.
Editable source is in the repository's `scripts/readers/`. The same readers handle
standalone files, folders, archives, and thumbnails.

| Package | Supported input | Main limitations |
|---|---|---|
| mag | MAKI02 16/256-color MAG, MAG-family MAX | No MAKI01 or MSX screen 8/YJK |
| pic | Supported machine profiles/modes of legacy Yanagisawa PIC | No PC-88VA tile or generic 32-bit modes, among others; unrelated to other PIC formats |
| pic2 | P2DT, 15/24-bit, P2BM/P2BI/P2SF/P2SS | No palettes, other depths, or unknown blocks; multiple blocks are composed into a still image |
| pnm | Single/concatenated P6 PPM, RGB, maxval 1–65535 | This reader does not implement P3/P5 or other variants |
| reading-filter | Grayscale, gamma, contrast | Saves settings; applied when image processing is enabled |

Parts of PIC/PIC2 are ported from wuimg's 0BSD implementation. Original author,
permission, and disclaimer notices remain in the source; upstream.json records
provenance and content snapshots. MAG/PPM are independent implementations.

To experiment with an edited bundled reader, copy the package and explicitly
register it with the same ID. Restart to reload it rather than modifying the
embedded package directly.
