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
Writing, deletion and trimming still use a lock, and PNGs and inventory revisions use temporary-file replacement.
PNG lookup for the current viewport runs separately from source generation; hits appear as they become available.
Generated images are returned for display before PNG persistence. Visible updates and saving have separate workers and can progress during continued interaction. Unfinished requests are cancelled after leaving the viewport.
Page-thumbnail mode does not render the hidden reading image or prefetch reading pages.
Pending images spill to disk at the memory limit, and write failures or contention are retried. Normal shutdown journals unfinished images for recovery on the next launch. The journal and atomic-write temporary files need extra disk space beyond the completed-cache limit. A full or unwritable disk, or an abrupt shutdown, can still require regeneration.

Disk capacity is the only retention limit. When exceeded, the oldest saved images are removed first. Reading a PNG never changes its save time. Neither age alone nor file count causes deletion.
Saved images are displayed without checking the original source. There is no per-use or periodic validation. Only relevant change notifications or differences in directory-entry information trigger a lower-priority update check. Unchanged source images and ZIPs are not reopened.
Adding files without changing the selected source or its attributes does not regenerate or rewrite the PNG. Deep changes that the existing format cannot identify are not exhaustively tracked; use F6 when an explicit refresh is needed.
Preferences → Thumbnails → additional folder-cover search levels defaults to 0. When viewing A containing B, 0 searches B's direct files, including images and archives, without entering B/C. A value of 1 also searches C's direct files. A folder icon is shown if no candidate is found within the limit. Reducing the depth does not delete an existing cover.
Leaving a folder or view cancels its unfinished work; completed images still get saved. Reading prioritizes the current page, then prefetch, without thumbnail generation or update checks.

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
