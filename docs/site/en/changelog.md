# Changelog

[日本語](../changelog.md) | English

## v1.7.0

- Fix cover regeneration when switching between covers and page thumbnails through a file symlink, preserving existing saved images.
- Stop repeated file reopening when page-thumbnail verification encounters inconsistent resolved paths or source observations.
- Select the cover image before loading a ZIP/CBZ entry, reducing repeated archive reads and avoiding page-count scans of unrelated TIFF images. Existing saved thumbnails remain reusable.
- Reduce cover-selection overhead for ZIP archives with many entries: reuse extension and text-decoding setup and the size captured for each open file, and choose the candidate during name parsing. Preserve filename interpretation and selection order.

## v1.6.0

- Keep thumbnail display working when a cache PNG cannot be read: use another saved copy when available, otherwise generate the image from its source.
- Wait for image-page presentation after command-line page selection or navigation before executing following commands, preserving the last book and reading position on immediate exit.
- Isolate document rendering so WebEngine does not switch the cover and page galleries to RHI composition. Building now requires Qt 6.8 or later.
- Reuse decoded cover and page thumbnails in memory to reduce saved-PNG reads when revisiting folders or switching views.
- Added optional full-list thumbnail preloading, off by default. Visible covers and pages take priority; remaining images are loaded progressively and retained within an offscreen memory limit, with interruption and cancellation on navigation.
- Synchronize reflow writing mode and spread order. Support the EPUB single-page cover setting with centered covers, and open directly in the document view using the current sections and required resources. Improve load cancellation, navigation between books during loading, and reuse of the previous document view.
- Fix EPUB display failures after repeated forward and backward page navigation.
- Resolve Windows-style relative Aozora image paths. Load illustration sections on demand, preserve captions/text locations, and display standalone landscape illustrations at full spread width.
- Defer hidden thumbnail preparation and directory listing until the relevant view is active. Cancel old GUI deliveries and avoid duplicate image requests during opening.
- Make Load all files a saved loading option. Retain file data within its memory budget only while reading, after current display and nearby preloading.
- Read nested and folder-container images on demand. Isolate local errors in unshown entries and improve animation cancellation.
- Reuse Windows dependency caches across application versions, preserve completed dependencies after test failures, and avoid re-uploading unchanged caches.

## v1.5.0

- Show saved thumbnails first without checking their sources. Remove periodic validation and update only targets with concrete change evidence. Adding files without changing the selected source or its attributes does not regenerate or rewrite the thumbnail. Unchanged images are retained.
- Add a folder-cover search depth setting. The setting accepts 0–8; the default 0 searches the cover target folder's direct files without entering deeper folders. Reducing depth preserves existing images.
- Cancel obsolete work on folder or view changes and prioritize the current display. Reading runs the current display and prefetch without thumbnail work.
- Preserve pending images through disk journaling, retry writes, and resume after normal shutdown. When capacity is exceeded, remove the oldest saved images first (FIFO); reads never change their timestamps. Keep thumbnails-v2 compatibility.
- Generate missing visible thumbnails before background work, display them before saving, and preserve old images on replacement failures. Completed TXT, Markdown, and archive-text cover previews are also saved.

## v1.4.3

- No user-facing behavior changes. Version 1.4.2 is skipped.

## v1.4.1

- Show a restart notice after saving additions or removals of script registrations in Preferences. Combine it with the language restart notice when both change.
- Protect visible thumbnails during display-cache eviction, releasing older offscreen images first.

## v1.4.0
- Display saved cover thumbnails before checking source ZIP metadata; validate in low-priority background work and regenerate/replace covers only when a source change is detected.
- Start a separate `thumbnails-v2` disk cache; no migration from `thumbnails-v1`. To reclaim the old cache space, clear it in the previous version before upgrading.
- Refresh covers with F6 and clear saved thumbnails in Preferences. Thumbnail dimensions use Qt logical pixels.

## v1.3.0

- Remove the 20,000-entry disk thumbnail cache limit and use only the configured capacity to decide retention. Preserve existing committed PNGs if a replacement write fails.
- Avoid unnecessary cache regeneration when application, Qt, or script versions change, or unrelated script settings are edited. Older cache-key files are not reused; clear the thumbnail cache in Preferences to reclaim disk space if needed. Original images and reading history are unaffected.
- Fix MangaMeeya settings import on Windows failing to persist settings while the original INI file was held open.

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

## v1.0.0 — Initial release
