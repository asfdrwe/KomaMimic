# Changelog

## v1.1.0

- Make reading filenames and thumbnail page numbers optional and hidden by default; reclaim the filename band for image display.
- Add an independent two-pane folder browser as the default, retaining an optional single tree.
- Add custom thumbnail dimensions, columns, spacing, filename lines, starting corner and decorative frames. Defaults: 64×64, automatic columns, zero spacing, one filename line, upper left, no frame.
- Move the toolbar visibility toggle directly into the View menu.

## v1.0.3

- Fix Linux AppImage document startup aborts caused by bundled graphics/C++ libraries conflicting with newer host Mesa drivers.
- Open EPUB files through Unicode-safe file descriptors for both body and cover loading on Windows.
- Add deployed WebEngine rendering checks, interrupted thumbnail-cache writer tests, and opt-in cache diagnostics.

## v1.0.0 — Initial release
