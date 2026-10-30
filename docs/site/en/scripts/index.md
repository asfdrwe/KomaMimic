# Installing and registering scripts

[日本語](../../scripts/index.md) | English

Screenshots on this page show the Japanese UI.

API 1 JavaScript can add image readers, multipage readers, and image filters.
It does not provide a general document-reader, archive-reader, or writer API, or
compatible execution of Susie/AviSynth plugins.

## Registration in Preferences

1. Put `manifest.json` and the JavaScript in the same package folder.
2. Under Preferences → Scripts, select that package or a folder containing packages directly beneath it.
3. Use the inspect-and-add action to validate the contents, then choose settings and a filter.
4. Save with OK. Restart after adding/removing folders. Reopen the book after changing values.

![Script preferences](../../assets/screenshots/script-preferences-ja.png)

Registration checks do not execute JavaScript. Cancel leaves registrations,
settings, and the running package list unchanged. Removing a registration does
not delete files. A later registration replaces an earlier one with the same ID.
At most 32 packages, including bundled packages, and 32 saved registration folders
are allowed.

If a registered location cannot be read at startup, the app starts with bundled
readers and shows the cause in Preferences. Fix or unregister the folder and restart.
The app does not automatically discover and execute JavaScript inside images or
archives.

## Development overrides

```sh
komamimic --script-readers /path/to/package /path/to/sample.toy
```

This option can be repeated. It overrides saved registrations for that session only
and is not saved automatically. It starts an independent instance instead of
forwarding to an existing window. Restart after editing source.

Continue with the [API specification](api.md) and [minimal examples](examples.md).
