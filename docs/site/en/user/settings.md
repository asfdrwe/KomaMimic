# Settings, reading data, and caches

[日本語](../../user/settings.md) | English

Screenshots on this page show the Japanese UI.

## Settings and reading data

Normal startup stores `settings.ini` in the OS-standard settings location.
`library.sqlite3` in the same folder stores bookshelves, history, reading positions,
and related data.

| OS | Default settings file |
|---|---|
| Windows | `%LOCALAPPDATA%\KomaMimic\KomaMimic\settings.ini` |
| macOS | `~/Library/Preferences/KomaMimic/KomaMimic/settings.ini` |
| Linux | `~/.config/KomaMimic/KomaMimic/settings.ini` |

On Linux, `XDG_CONFIG_HOME` overrides the `~/.config` portion.
Use `--config /path/to/settings.ini` to specify another settings file.
Older layouts may be migrated to a different location at startup.

To back up, quit the app and copy the entire settings folder.

![General settings](../../assets/screenshots/settings-settings-ja.png)

## Caches

Preferences → Cache controls in-memory preloading and thumbnail disk storage.
Thumbnail disk storage is enabled by default with a 512 MiB limit and can be cleared
from this dialog. Thumbnails can be regenerated; they do not replace settings or
reading-history backups.

| OS | Default thumbnail cache |
|---|---|
| Windows | `%LOCALAPPDATA%\KomaMimic\KomaMimic\cache\thumbnails-v1` |
| macOS | `~/Library/Caches/KomaMimic/KomaMimic/thumbnails-v1` |
| Linux | `~/.cache/KomaMimic/KomaMimic/thumbnails-v1` |

On Linux, `XDG_CACHE_HOME` overrides `~/.cache`. These paths use
[Qt's OS-specific storage locations](https://doc.qt.io/qt-6/qstandardpaths.html#StandardLocation-enum).
A different `--config` does not change the independent OS-standard cache location.
The memory-cache setting is not a limit on total application memory use.

![Cache settings](../../assets/screenshots/settings-cache-ja.png)

## Colors and language

Preferences → General lets you choose system/light/dark colors and Japanese,
English, or the system language. Save and restart to apply the UI language.
The color mode does not replace configured document or image colors.

See [Differences from MangaMeeya](compatibility.md) for compatibility and sorting persistence.

## Reuse and regeneration

Committed, valid thumbnail PNGs can be read without waiting for writer/maintenance locks or a full cache inventory scan.
Last-use updates are best-effort. Writing, deletion and trimming still use a lock, and PNGs and inventory revisions use temporary-file replacement.
If a generated image cannot obtain the save lock, it may be displayed without being saved. It may need regeneration next time;
this does not invalidate other committed PNGs.

The cache retains at most 20,000 thumbnails as well as respecting the configured byte limit, trimming older entries first.
Changes to the application, Qt, image readers, source path/size/modification time or filename encoding can require regeneration.
Changing display thumbnail dimensions or spacing alone does not clear all cached images.
Usage totals can include PNGs from earlier versions, so a file remaining on disk does not prove it matches the current cache key.

Incomplete temporary files left by an abnormal exit are not read as committed PNGs. Qt checks lock ownership and other stale-lock conditions before reclaiming a leftover lock.
v1.1.0 fixes unnecessary regeneration caused by lock contention; it does not establish every cause of the previously reported full-regeneration symptom.
Folder-cover candidate searches and archive page-index loading are separate from thumbnail regeneration.
TXT, Markdown and covers of archives containing text may be excluded from disk persistence because they depend on fonts or external images.
