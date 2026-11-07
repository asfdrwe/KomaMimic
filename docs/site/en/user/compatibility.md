# Differences from MangaMeeya

[日本語](../../user/compatibility.md) | English

KomaMimic is an independent implementation inspired by MangaMeeya, not a fully
compatible replacement. These are known differences, not promises of future support.

| Feature | Support and differences | Alternative or explanation |
|---|---|---|
| Plugins and scripts | No compatible execution of Susie/AviSynth DLLs or original proprietary scripts | Use [JavaScript API 1](../scripts/api.md) for image readers and filters |
| Unsupported formats | No compatible reading of GCA or binary MMT | See [formats and limitations](formats.md) |
| Custom thumbnails | Columns, dimensions, spacing, filename lines, starting corner and decorative frames are supported. Not all original resamplers, JPEG subsampling controls, cache formats, extensions or locations are reproduced; original caches are not imported | See [display settings](views.md). Cache formats and pixels need not match the original |
| Sorting menu | Uses the existing natural, name, type, size, date, entry, Unicode and random modes, with a shared reverse toggle, rather than separate entries for every original path-order variant | See persistence below |
| Folder browser | Two panes start at virtual Desktop on Windows or Places on macOS/Linux. This does not reproduce every Shell or Finder feature | Optional single tree starts at PC on Windows or `/` on macOS/Linux. See [controls](views.md) |
| Autoplay timing settings | MangaMeeya uses a dedicated playback-settings dialog; KomaMimic uses Preferences → Display | See the locations below and [playback controls](library.md) |
| Settings migration | Converts only recognized INI settings. Unknown values are not guessed. Original mouse/gamepad, plugin, external-program, path and history settings are not migrated by this operation | Review the actual scope in the [import preview](migration.md) |


### Thumbnail size units

KomaMimic's custom width, height and spacing use **logical pixels**, not DPI. With Qt high-DPI scaling, the physical pixel count may differ from the configured number. MangaMeeya also offers numeric width and height controls, but equal values do not guarantee identical onscreen size or cached image dimensions. This guide does not assert the original program's internal DPI behavior. See [display settings](views.md).

## Autoplay settings and page navigation

| Application | Timing setting |
|---|---|
| MangaMeeya | Book list → Playlist playback settings… (「ブックリスト → プレイリスト再生設定…」), then Display time (「表示時間」) in the playback-settings dialog |
| KomaMimic | Preferences → Display → Automatic playback interval (ms) |

KomaMimic's Book list → Page and playlist playback provides forward/reverse playback,
pause, and stop. Set the interval separately in Preferences as listed above.

When ordinary autoplay advances or reverses pages, both MangaMeeya and KomaMimic
use the same navigation behavior as their manual next/previous commands.
KomaMimic follows the normal page-turning settings, including spreads and behavior
at the end of a book. The different settings location does not change that navigation behavior.

## File sorting and persistence

Tools → File sorting and Preferences → Display edit the same default for covers
and image-reading pages. The mode and reverse flag survive restart, settings slots
and KomaMimic settings export/import. Sorting does not modify source files or
archives, and preserves the selected file or reading page by identity. Cancelling
settings import does not change the current settings.

Defaults are natural order and reverse off. Explicit existing cover-sort settings
are retained. The default applies to subsequently opened image books and archives;
it does not reorder the chapters or text pages of TXT, Markdown or EPUB documents.
Automatic defaults preserve structurally edited books (inserted blanks, deleted or split pages); explicit sorting remains available.
An imported list’s explicit per-book order takes precedence over the shared default when opening it. You can still change its order explicitly after opening.
Undoing a reading-page reorder does not reset the saved default preference.

Folders precede files in ordinary cover listings. Equal types, sizes or dates use
natural order as a tie-breaker. Entry order means archive entry order inside an
archive, but OS enumeration order in a normal folder; it need not match across OSes.
Random mode saves the mode, not a fixed permutation or seed. Opening again or
explicitly selecting random creates a new order; repainting alone does not reshuffle.
Reverse flips the current random order while retaining folders first.

Reading filenames and added thumbnail page numbers are now hidden by default and
can be enabled. These implemented options are not listed as unsupported features.
See [settings and caches](settings.md) for storage and backup.
