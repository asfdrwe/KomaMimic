# Architecture

KomaMimic consists of a Qt Widgets GUI, a core that loads and edits books, and a
separate executable that runs JavaScript. Document rendering uses Qt WebEngine.

| Area | Entry points | Responsibility |
|---|---|---|
| Startup, CLI, IPC | `src/main.cpp`, `startup.cpp` | Arguments, settings, forwarding to an existing window |
| Books and archives | `book.*`, `archivepath.*`, `archivenames.*` | Input identification, page sequences, names and encodings |
| GUI | `mainwindow*`, `pageview.*`, `thumbnailgrid.*` | Lists, reading, menus, full screen |
| Controls | `shortcuts.*`, `readerfeatures.*`, `readerinput.*` | Action IDs, keyboard, mouse, gamepad |
| Documents | `documentsource.*`, `publication.*`, `documentview.*`, `documentsettings.*` | TXT/Markdown/EPUB, resources, layout, reading position |
| Images and output | `imageprocessing.*`, `bookexport.*`, `media.*`, `pdfsource.*` | Processing, animation, saving, PDF |
| Scripts | `scriptreader.*`, `scriptworker.cpp`, `scripts/readers/` | Registration, validation, worker communication, API 1 |
| Persistence | `librarystore.*`, `thumbnailcache.*` | Bookshelves and history, thumbnail disk storage |
| OS integration | `idleinhibitor.*`, `power_*`, `inhibitprotocol.*` | Preventing automatic display sleep while reading |
| Migration | `legacysettings.*`, `legacysettingsdialog.cpp` | INI conversion plans, review, backup |
| Distribution | `tools/`, `cmake/` | Public source selection, validation kits, runtime deployment |

## From input to display

A GUI request identifies the book and selects pages, then loads images or parses a
document. Images are decoded by native codecs, the PDF backend, or a script worker,
then passed to display processing and caching. TXT/Markdown/EPUB use Publication and
document settings to render in WebEngine.

Asynchronous work checks generation numbers and cancellation so results for an old
book cannot update the current display. Thumbnails share identification with core
image processing and EPUB cover extraction. A cache hit must not bypass checks for
changes to the original input.

## Boundaries to preserve

- Keep Book's logical page sequence, display names, original bytes, and archive entries distinct.
- Separate document text positions from image page numbers and spread settings.
- Keep text-field editing separate from the scope of reading shortcuts.
- Do not expose QObject or arbitrary filesystem, network, or process capabilities to JavaScript.
- Settings and history are runtime data. Tests use temporary profiles.

See the [specification](specification.md) for limits and [automated tests](testing.md)
for verifying changes.
