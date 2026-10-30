# UI translations

The UI supports Japanese and English, selectable in Preferences. Catalogs are in
`translations/`; `src/i18n.*` loads them. Qt Linguist Tools compiles and embeds them
in the application at build time. Qt's qttranslations package is also needed for
standard dialogs.

Translate display strings, not action IDs, settings keys, API field names, or file
names. Preserve placeholders, HTML, newlines, and numeric meaning, and check menu
shortcut labels. Run `python3 tools/check_translations.py` and `i18n_tests`, then
restart the GUI in the selected language to check menus, Preferences, and errors.

Core developer documentation is maintained in English. The user and script
developer guide keeps Japanese source pages and English translations under
`docs/site/en/`. Update the matching translation when changing the Japanese guide.
English pages reuse the Japanese screenshots and identify them as Japanese UI.
