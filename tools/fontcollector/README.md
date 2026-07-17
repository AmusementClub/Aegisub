# Font Collector Tool

This directory builds the standalone font collector shared library and CLI.

Preferred standalone build, from the repository root:

```powershell
cmake -S tools/fontcollector -B build-fontcollector-static `
  -G "Visual Studio 18 2026" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=$resolvedVcpkgRoot/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DVCPKG_APPLOCAL_DEPS=OFF
cmake --build build-fontcollector-static --config Release --target fontcollector_cli
cmake --build build-fontcollector-static --config Release --target fontcollector_cli_standalone
```

Run the standalone smoke test after building:

```powershell
ctest --test-dir build-fontcollector-static -C Release -R fontcollector_cli.*smoke --output-on-failure
```

The DLL wrapper output is written to `build-fontcollector-static/fontcollector` and should contain:

- `aegisub_fontcollector.dll`
- `fontcollector.exe`

The independent CLI output is written to `build-fontcollector-static/fontcollector-standalone` and should contain:

- `fontcollector.exe`

The `aegisub_fontcollector` shared library exposes the C API declared in `include/aegisub/fontcollector/fontcollector.h`. `fontcollector_cli` links to that DLL as a thin CLI wrapper. `fontcollector_cli_standalone` links the static core into the executable for redistribution as a single binary.

The tool manifest intentionally excludes ICU, uchardet and Boost.Locale. The normal GUI build leaves `AEGISUB_BUILD_FONT_COLLECTOR_TOOL` off, so it does not find or link CLI11.

When `--encoding` is omitted, the tool uses Aegisub's core charset detector without uchardet: BOMs are honored, otherwise non-binary text defaults to UTF-8.

## CLI

The preferred command form is:

```powershell
fontcollector list <file-or-dir> [more...] [--recursive] [--details] [--json]
fontcollector check <file-or-dir> [more...] [--recursive] [--details] [--json] [--strict]
fontcollector validate <file-or-dir> [more...] [--recursive] [--details] [--json]
fontcollector normalize <file-or-dir> [more...] [--target localized|english] [--recursive] [--details] [--json]
fontcollector collect <file-or-dir> [more...] --to <dir> [--recursive] [--details] [--json] [--strict]
fontcollector collect <file-or-dir> [more...] --to-script-dir [--recursive] [--details] [--json] [--strict]
```

Legacy mode flags are still accepted:

```powershell
fontcollector <file-or-dir> [more...] --check
fontcollector <file-or-dir> [more...] --copy <dir>
fontcollector <file-or-dir> [more...] --copy-to-script-dir
```

Directory inputs scan immediate `.ass` and `.ssa` children; add `--recursive` to include subdirectories. `validate` is strict by default and exits with `20` when fonts, styles, glyphs, or copies are missing.

`normalize` is read-only. It scans style font names and non-empty explicit `\fn` tags, then reports safe canonical-name changes and unsafe findings without writing the input file. `--target` defaults to `localized`; use `--target english` for verified English Win32 family names. Full, PostScript, and typographic names remain informational and are never treated as safe ASS family aliases.

Output defaults to concise human-readable text. It reports actionable issues with input file paths and source line numbers, then prints one summary line per input file. Use `--details` to include backend/cache/search events plus full ASS font usage and matched font details. Use `--json` for automation.

`list` is the font inventory mode, shaped after ACGrip's ListAssFonts output. Installed fonts are printed as `localized display name <ASS font name>` using the matched names reported by the native API; unresolved fonts are printed as their ASS font name and colored red when stdout is an interactive Windows console. Fonts with missing glyphs are colored blue. Missing glyph reports are written to stderr as `AssFontInfo 'Font,bold,italic' Dialogue #... is missing characters: ...`. `list --details` adds ASS weight/italic, install status, styles, source lines, matched family/full names, backend source, and matched font paths under each font.

## Batch Resolution

Multi-file CLI runs use the native session batch API. The collector first analyzes every ASS file, merges font requests by `(facename, bold, italic)`, unions their codepoints, resolves each merged request once, and then maps the result back to each input file. This keeps font backend/cache work low while preserving per-file diagnostics.

Text output is grouped by input file. JSON output is always a schema-versioned object:

```json
{
  "schema_version": 1,
  "ok": false,
  "result": 20,
  "summary": {},
  "backend": { "requested": "auto", "resolved": "gdi-dwrite" },
  "files": [],
  "diagnostics": []
}
```

Each `files[]` entry contains `input`, `ok`, CLI `result`, native `operation_result`, `error`, `summary`, `backend`, `diagnostics`, `events`, and `font_usage`. Top-level `diagnostics[]` is the flattened list from every file, suitable for CI/reporting. In each file report:

- `font_usage[].lines` contains source file line numbers using that ASS font request.
- `font_usage[].matched_font.missing_lines` contains source file line numbers containing missing glyphs.
- `diagnostics[].file` and `diagnostics[].lines` identify the exact ASS file and source lines for missing styles, missing fonts, missing glyphs, and collection failures.
- For in-memory `AssFile` callers without source line metadata, these fields fall back to dialogue row numbers.

The C ABI keeps the original single-file functions and adds:

```c
int aegisub_fontcollector_session_collect_batch(
    AegisubFontCollectorSession *session,
    AegisubFontCollectorBatchItem *items,
    size_t item_count,
    char *error_buffer,
    size_t error_buffer_size);
```

Each `AegisubFontCollectorBatchItem` owns its callbacks, summary pointer, error buffer, and result code. Invalid items report their own errors and do not prevent valid items in the same batch from being resolved.

Font-name normalization uses a separate versioned, read-only C API. Every new request, result, summary, and batch item begins with `struct_size`. Initialize it to `sizeof(struct)` for current headers; the public `*_V1_SIZE` constants describe the stable v1 prefixes accepted by newer libraries:

```c
int aegisub_fontcollector_build_normalization_plan(
    AegisubFontNameNormalizationRequest const *request,
    AegisubFontNameNormalizationCallback callback,
    void *user_data,
    AegisubFontNameNormalizationSummary *summary,
    char *error_buffer,
    size_t error_buffer_size);

int aegisub_fontcollector_build_normalization_plan_batch(
    AegisubFontNameNormalizationBatchItem *const *items,
    size_t item_count,
    char *error_buffer,
    size_t error_buffer_size);
```

The batch API takes an array of item pointers, constructs one immutable family catalog, and uses it for every input plan. Each item's request pointer must remain valid for the call. Callback strings are valid only for the callback duration. Neither function writes subtitle files.

Callers must inspect each `AegisubFontNameNormalizationBatchItem::result` (and that item's error buffer). The batch function return value is only for argument/catalog-level failures; individual input read or analysis failures do not make the batch return non-zero by themselves.

JSON output for `fontcollector normalize --json` uses `schema_version: 2`. The root `summary` aggregates counts across files and includes `catalog_available` (true only when every successfully analyzed file had a usable family catalog).
