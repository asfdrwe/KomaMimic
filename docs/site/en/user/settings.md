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

## Fullscreen

Use Preferences → Display → Fullscreen and press OK to change the current window.
Cancel leaves it unchanged. Changes made through menus, shortcuts, or OS window controls are also remembered.
Quitting in fullscreen restores fullscreen on the next startup; leaving fullscreen before quitting restores a normal window instead.
Leaving fullscreen restores the normal window's position, size, and maximized state.

New settings and older settings without a saved fullscreen state start in a normal window.
`--fullscreen` (`-f`) starts in fullscreen regardless of the saved state.
Settings slots and KomaMimic settings export/import include the fullscreen state.
Importing an older file without this setting keeps the current fullscreen state.

## Caches

Preferences → Cache controls in-memory preloading and thumbnail disk storage.
Thumbnail disk storage is enabled by default with a 512 MiB limit and can be cleared
from this dialog. Thumbnails can be regenerated; they do not replace settings or
reading-history backups.

| OS | Default thumbnail cache |
|---|---|
| Windows | `%LOCALAPPDATA%\KomaMimic\KomaMimic\cache\thumbnails-v2` |
| macOS | `~/Library/Caches/KomaMimic/KomaMimic/thumbnails-v2` |
| Linux | `~/.cache/KomaMimic/KomaMimic/thumbnails-v2` |

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
PNG lookup for the current viewport runs separately from source generation; hits appear as they become available.
Generated images are returned for display before PNG persistence. Saving, last-use updates, and maintenance run after current display work settles.
Page-thumbnail mode does not render the hidden reading image or prefetch reading pages.
Pending writes may be omitted because of their memory limit, writer contention, or shutdown; those images may need regeneration next time.
This does not invalidate other committed PNGs.

The configured disk capacity is the only retention limit; there is no file-count limit. Oldest-used thumbnails are removed when capacity is exceeded.
Cached covers are shown before accessing the original archive's metadata. A low-priority check then compares source size, modification time, resolved path and related information. Changed covers are regenerated and replaced. Full content hashes are not calculated. Other cache types, including page thumbnails, can use different validation rules.
Changing KomaMimic, Qt, scripts or script settings alone does not invalidate thumbnails. If such a change affects the rendered image, explicitly refresh thumbnails or clear the cache.
Changing display thumbnail dimensions or spacing alone does not clear all cached images.
`thumbnails-v2` does not read or migrate the earlier `thumbnails-v1` cache. We recommend clearing the old cache in the **old** app under Preferences → Cache **before updating**. Clearing the cache in the new app normally affects only `thumbnails-v2`; it does not automatically remove the old `thumbnails-v1` directory.
Clearing thumbnails does not remove source images, archives, the library or reading history.

Incomplete temporary files left by an abnormal exit are not read as committed PNGs. Qt checks lock ownership and other stale-lock conditions before reclaiming a leftover lock.
TXT, Markdown and covers of archives containing text may be excluded from disk persistence because they depend on fonts or external images.

### Workaround for incorrect thumbnails

1. In the cover or page-thumbnail view, press `F6` (**Refresh thumbnails** / **サムネイルを再取得**) to regenerate images from their sources. Try this first for stale, incorrect or missing thumbnails.
2. If the problem persists, clear the current thumbnail cache from Preferences → Cache, then reopen the affected folder or archive to regenerate it.
3. If the problem remains, check that the original image or ZIP can be read, disk space and file permissions. Regenerating thumbnails cannot repair a damaged original file.

Clearing the cache does not erase original files, bookshelves or reading history. Regeneration requires time to read the sources.
