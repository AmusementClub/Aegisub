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

All collection commands accept `--matcher platform|libass`. `platform` is the
default and uses the native provider:

| OS | platform matcher | libass matcher provider | Fontconfig linked? |
|----|------------------|-------------------------|--------------------|
| Windows | GDI/DirectWrite | DirectWrite | No |
| macOS | CoreText | Fontconfig (optional) | Optional - only for libass |
| Linux/Unix | Fontconfig | Fontconfig | Yes (required) |

`libass` uses the ported libass selector (family substitutions, attribute
scoring, and per-codepoint glyph checks). On Windows the catalog is DirectWrite
(not Fontconfig). On macOS Fontconfig is optional: without it, platform/CoreText
still works and `--matcher libass` fails with a clear error. On Linux both
matchers share the Fontconfig catalog but use different selection algorithms.
JSON reports include the selected `matcher` and resolved `provider`.
The provider is selected by the build platform; there is no user-selectable
provider cross-product. On macOS, Fontconfig remains an optional dependency used
only by the libass matcher.

The libass matcher also accepts `--additional-fonts` and
`--additional-fonts-recursive` to inspect font files which are not installed.
Use `--exclude-system-fonts` for a deterministic private-font catalog (requires
at least one additional font file). These options are intentionally restricted
to `--matcher libass`.

On Windows, a selected font can occasionally expose readable data through
DirectWrite/GDI without exposing a local file path. Such a face is reported as
`memory_only`, not missing. Collection commands dump its bytes using a
readable `<full-face-name>.<ext>` filename and an extension derived from the
font signature. A numeric suffix is added only when different font data would
otherwise use the same name; duplicate byte streams are collected only once.

JSON and `--details` output include the candidates considered by the libass
selector. Scores use libass semantics: lower is better. Candidate evidence
includes the name-match stage, provider order, glyph coverage, and codepoints
actually selected. An equal-score tie is reported as an ambiguous match; a
score is a ranking heuristic and is not proof that the renderer chose the same
face.

Directory inputs scan immediate `.ass` and `.ssa` children; add `--recursive` to include subdirectories. `validate` is strict by default and exits with `20` when fonts, styles, glyphs, or copies are missing.

`normalize` is read-only. It scans style font names and non-empty explicit `\fn` tags, then reports safe canonical-name changes and unsafe findings without writing the input file. `--target` defaults to `localized`; use `--target english` for verified English Win32 family names. Full, PostScript, and typographic names remain informational and are never treated as safe ASS family aliases.

Output defaults to concise human-readable text. It reports actionable issues with input file paths and source line numbers, then prints one summary line per input file. Use `--details` to include provider/cache/search events plus full ASS font usage and matched font details. Use `--json` for automation.

`list` is the font inventory mode, shaped after ACGrip's ListAssFonts output. Installed fonts are printed as `localized display name <ASS font name>` using the matched names reported by the native API; unresolved fonts are printed as their ASS font name and colored red when stdout is an interactive Windows console. Fonts with missing glyphs are colored blue. Missing glyph reports are written to stderr as `AssFontInfo 'Font,bold,italic' Dialogue #... is missing characters: ...`. `list --details` adds ASS weight/italic, install status, styles, source lines, matched family/full names, provider source, and matched font paths under each font.

## Batch Resolution

Multi-file CLI runs use the native session batch API. The collector first analyzes every ASS file, merges font requests by `(facename, bold, italic)`, unions their codepoints, resolves each merged request once, and then maps the result back to each input file. This keeps provider/cache work low while preserving per-file diagnostics.

Text output is grouped by input file. JSON output is always a schema-versioned object:

```json
{
  "schema_version": 2,
  "ok": false,
  "result": 20,
  "summary": {},
  "matcher": "platform",
  "provider": "gdi-dwrite",
  "files": [],
  "diagnostics": []
}
```

Each `files[]` entry contains `input`, `ok`, CLI `result`, native `operation_result`, `error`, `summary`, `matcher`, `provider`, `diagnostics`, `events`, and `font_usage`. Top-level `diagnostics[]` is the flattened list from every file, suitable for CI/reporting. In each file report:

- `font_usage[].lines` contains source file line numbers using that ASS font request.
- `font_usage[].matched_font.missing_lines` contains source file line numbers containing missing glyphs.
- `diagnostics[].file` and `diagnostics[].lines` identify the exact ASS file and source lines for missing styles, missing fonts, missing glyphs, and collection failures.
- For in-memory `AssFile` callers without source line metadata, these fields fall back to dialogue row numbers.

The collection C ABI exposes one session/batch path:

```c
int aegisub_fontcollector_session_create_with_options(
    AegisubFontCollectorSessionOptions const *options,
    ...);

int aegisub_fontcollector_session_collect_batch(
    AegisubFontCollectorSession *session,
    AegisubFontCollectorBatchItem *items,
    size_t item_count,
    char *error_buffer,
    size_t error_buffer_size);
```

Each `AegisubFontCollectorBatchItem` owns its callbacks, summary pointer, error buffer, and result code. Invalid items report their own errors and do not prevent valid items in the same batch from being resolved.

`AegisubFontCollectorSessionOptions` is versioned with `struct_size`; its
additional font paths are UTF-8 files supplied to the libass provider.
`include_system_fonts = 0` creates a private-font-only catalog and requires
`additional_font_file_count > 0` (zero-init leaves this field at 0 — set it to
`1` unless you intentionally build a private catalog).
`collect_match_candidates = 1` enables the extra candidate scan. The CLI sets
it only for `--details` or `--json`; the GUI leaves it off.
Usage callbacks expose callback-scoped `match_candidates` evidence appended to
`AegisubFontCollectorFontUsage` when enabled.

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
