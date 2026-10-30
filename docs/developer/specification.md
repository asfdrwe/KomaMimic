# Specification and implementation boundaries

## Controls and persistence

Action IDs and storage keys are independent of display names and translations.
Preserve common, reading, thumbnail, and tree assignment scopes without conflicting
with text-field editing. Return confirms a list selection. The default full-screen
shortcut is F11 on Windows/Linux and Control+Command+F on macOS.

Apply the manga-oriented defaults only to empty settings: spreads, right-to-left
opening, and a separate cover page are enabled; automatic spreads and image
processing are off; preloading is one display unit ahead and zero behind. Existing
settings retain their saved values and compatible defaults. There is no custom top
bar; use the standard menu and bottom reading toolbar. See `defaultpreferences.*`,
`shortcuts.*`, and the [basic user guide](../site/en/user/basic.md).

## Images, archives, and output

Images use installed Qt plugins and bundled script readers; archives use libarchive;
PDF uses Poppler GLib/Cairo. Loading is not delegated to external 7z/lha/unrar or
pdfimages commands. Unicode names take priority. ZIP/LHA entries without Unicode
information default to CP932. See [supported formats](../site/en/user/formats.md).

Logical edits and display processing normally leave original inputs unchanged;
saving and exporting produce the results. Renaming real files and moving them to
the trash are separate actions. PDF rendering and image extraction are serialized,
releasing documents and pages after each operation. Raster PDF output does not
preserve original text or links.

PDF limits are 100,000 pages and 48 megapixels of output. The original-size baseline
has a maximum edge of 2400 px; rerendering for zoom or DPI has a maximum edge of
6144 px. These do not limit memory used by Poppler's internal parsing.

## Documents

TXT, Aozora Bunko ruby annotations, Markdown, and EPUB share a document view.
Display settings take precedence in this order: per book, per format, global. Text
positions are saved and restored after reflow. TXT/Markdown documents inside an
archive have separate identities for listing and reading-position storage.

Resources are served through a temporary WebEngine profile with a per-book
`koma-document://` origin. External communication, file URLs, and downloads are
blocked. Book scripts are removed; iframe sandboxing and CSP are used. Distinguish
the application's layout JavaScript from the book's content. Markdown images are
restricted to the document directory and checked using canonical paths. DRM-encrypted
text and images are diagnosed as unsupported rather than decrypted.

Document page thumbnails, image processing, page editing, raster export, and printing
are outside this view's scope. Font-size changes do not affect fixed layouts or text
embedded in images.

## Scripts and OS integration

The public script specification is [API 1](../site/en/scripts/api.md). Each worker
uses a fresh QJSEngine, with checks on deadlines, inputs, and outputs. This is not a
general sandbox that protects the entire operating system.

The Windows application uses the GUI subsystem and does not create a console.
When started from a terminal, it preserves inherited output handles and attaches
to the parent console when needed. The worker uses pipes and `CREATE_NO_WINDOW`.
Connecting to an existing window uses bounded retries, including waiting for the
server to become available.

Automatic display-sleep inhibition is off by default. It is acquired only while
reading in the foreground, and released on minimizing, switching to another app,
returning to a list, or exiting. It uses Windows power APIs, IOKit on macOS, and an
available D-Bus method on Linux. On macOS it targets automatic display sleep. See
[native platform validation](platform-validation.md) for checking actual behavior.
