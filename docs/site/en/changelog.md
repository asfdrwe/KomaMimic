# Changelog

[日本語](../changelog.md) | English

## v1.4.3

- Fix the automated test for returning from an archive listing to filesystem entries on macOS to wait for the expected list contents. Report expected and actual paths on failure.
- Register the document URL scheme before Qt application startup in archive-document tests.
- Version 1.4.2 is skipped.

## v1.4.1

- Show a restart notice after saving additions or removals of script registrations in Preferences. Combine it with the language restart notice when both change.
- Fix visible thumbnails disappearing during display-cache eviction after fast scrolling through many folders or ZIP pages.
- Improve automated-test waits and time budgets across platforms, including Windows and macOS, and record diagnostics when Windows CLI test subprocesses time out.

## v1.4.0
- Display saved cover thumbnails before checking source ZIP metadata; validate in low-priority background work and regenerate/replace covers only when a source change is detected.
- Start a separate `thumbnails-v2` disk cache; no migration from `thumbnails-v1`. To reclaim the old cache space, clear it in the previous version before upgrading.
- Document F6 thumbnail refresh and cache clearing as recovery steps for incorrect covers. Explain that thumbnail dimensions use Qt logical pixels, not DPI.
- Add cache fast-display, validation and forced-regeneration regression tests, and stabilize the macOS directory-refresh scroll-position UI test.

## v1.3.0

- Remove the 20,000-entry disk thumbnail cache limit and use only the configured capacity to decide retention. Preserve existing committed PNGs if a replacement write fails.
- Avoid unnecessary cache regeneration when application, Qt, or script versions change, or unrelated script settings are edited. Older cache-key files are not reused; clear the thumbnail cache in Preferences to reclaim disk space if needed. Original images and reading history are unaffected.
- Fix MangaMeeya settings import on Windows failing to persist settings while the original INI file was held open.
- Expand cross-platform regression coverage for large caches, writer/cancellation races, settings import, and single-instance activation; stabilize Windows GUI tests.

## v1.2.1

- Restore a minimized existing window before requesting activation on a second launch in single-instance mode, preserving maximized/fullscreen state and reading position.

## v1.2.0

- Prioritize visible cover and page thumbnails, separating saved-cache lookup from expensive image generation so cached images can appear while other thumbnails are being generated.
- Display generated images before saving them. Run cache writes, maintenance, and reading prefetch as background work, and cancel obsolete work when the display target changes.
- Stop hidden reader rendering and prefetch in thumbnail mode. Decode a selected page for image copying only when explicitly requested.
- Load and sort folder lists asynchronously while preserving selection and scroll position during updates.

## v1.1.1

- Remember fullscreen state across restarts. Quitting in fullscreen restores fullscreen; leaving fullscreen before quitting restores a normal window.
- Add Preferences → Display → Fullscreen to change the current window mode.
- Include fullscreen state in settings slots and settings-file export/import while preserving the normal window layout when leaving fullscreen.

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
- Fix settings-file export failing on Windows.

## v1.0.3

- Fix Linux AppImage document startup aborts caused by bundled libraries conflicting with the host Mesa drivers.
- Fix opening EPUB files whose paths contain Japanese or other Unicode characters on Windows.
- Add deployed document-rendering checks, interrupted thumbnail-cache writer tests, and opt-in cache diagnostics.

## v1.0.0 — Initial release
