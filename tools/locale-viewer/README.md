# Aegisub Localization Viewer

Small Avalonia desktop helper for inspecting translation coverage in `po/*.po`.

The viewer reads `po/LINGUAS`, parses each `.po` file, and shows:

- summary counts for languages, average coverage, fuzzy entries, and missing entries
- one row per language with translated, fuzzy, missing, and coverage progress
- source strings with the selected language's current `msgstr` / `msgstr[n]`
- editable translation fields with save back to the selected `.po` file
- comparison rows for the same source string in the other languages from `LINGUAS`
- PO parser diagnostics and header-language mismatches
- manual refresh without modifying translation files

Refresh runs on a background task and parses languages in parallel, then replaces the
UI snapshot in one update. Entry search is debounced and uses a precomputed search index
per source string. Translation saves reload the selected `.po` file, update the matching
entries, and regenerate the file with Karambolo.PO. The window also has Avalonia
headless tests for the refresh command, summary binding, language list, entry list,
preview/edit/save flow, editor panel, save button, and multi-language comparison.
