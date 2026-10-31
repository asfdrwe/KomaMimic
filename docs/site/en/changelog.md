# Changelog

[日本語](../changelog.md) | English

## v1.1.0

- Make reading filenames and thumbnail page numbers optional and hidden by default; reclaim the filename band for image display.
- Add an independent two-pane folder browser as the default, retaining an optional single tree.
- Add custom thumbnail dimensions, columns, spacing, filename lines, starting corner and decorative frames. Defaults: 64×64, automatic columns, zero spacing, one filename line, upper left, no frame.
- Move the toolbar visibility toggle directly into the View menu.

- Share file-sort mode and reverse defaults between cover lists and image reading, including settings slots and settings-file import/export. Preserve an imported list’s explicit book order on opening.
- Fix folder-tree name-column sizing and selection through hidden ancestor directories.
- Support Aozora TXT image/caption annotations and limited img references in illustrated TXT. Prevent image-reader history from overriding a document’s single-page layout.
- Reuse committed thumbnail PNGs without waiting for writer/maintenance locks or a full inventory scan; avoid unnecessary source regeneration under contention.
- Add Differences from MangaMeeya with supported alternatives and settings-persistence details.

## v1.0.3

- Fix Linux AppImage document startup aborts caused by bundled libraries conflicting with the host Mesa drivers.
- Fix opening EPUB files whose paths contain Japanese or other Unicode characters on Windows.
- Add deployed document-rendering checks, interrupted thumbnail-cache writer tests, and opt-in cache diagnostics.

## v1.0.0 — Initial release
