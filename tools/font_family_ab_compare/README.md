# font-family-ab-compare

A/B equivalence tool for the Windows font family catalog.

Compares:

1. **Legacy** — snapshot of the pre-observation single-pass builder
   (`legacy/font_family_catalog_win_legacy.cpp`, extracted from git HEAD when
   the observation split landed).
2. **Modern** — `ObserveWindowsFontFamilies` + `DeriveWindowsFontFamilyCatalog`
   (no on-disk observation cache).

This is **not** a CI regression test. Local font sets change; results are
evidence of equivalence on a given machine, not a permanent golden master.

## Build

From the repository root (Windows, existing `build-dir` / RelWithDebInfo):

```powershell
cmake --build build-dir --config RelWithDebInfo --target font-family-ab-compare --parallel
```

Requires `WITH_SMOKE=ON` (or reconfigure with it). Target is `EXCLUDE_FROM_ALL`.

## Run

```powershell
# Default report directory: ./font-family-ab/ (cwd)
& "build-dir\RelWithDebInfo\font-family-ab-compare.exe"

# Explicit report directory under the build tree (recommended)
& "build-dir\RelWithDebInfo\font-family-ab-compare.exe" --out-dir build-dir/font-family-ab
```

Exit codes:

| Code | Meaning |
|------|---------|
| 0 | Structural match |
| 1 | Differences (see `diff.txt`) |
| 2 | Hard failure (args, I/O, incomplete observe) |

## Report artifacts

| File | Content |
|------|---------|
| `summary.txt` | `MATCH`/`DIFF` and counts |
| `diff.txt` | Per-family structural differences |
| `meta.txt` | Locale, `provider_fingerprint`, `os_build_fingerprint`, timings |

**Authoritative status:** read `build-dir/font-family-ab/summary.txt` (or the
`--out-dir` you chose) after each run. Do not treat historical family counts as
a permanent golden master — installed fonts differ by machine and over time.

## Comparison rules

Compared (per family, matched by sorted Win32 family name set):

- Localized / English presentation names
- Name list: value, locale, kind, variant_role (not absolute `entity_token`)
- RBIZ outcomes: requested/realized weight & italic, role, status
- Relative token partition (which RBIZ slots share a non-zero token)
- `BuildVariantChoices` / `FindImplicitVariantSelection` (role/weight/italic/status + relative token part)
- `automatic_pinning_reliable`

**Not** compared:

- Absolute `entity_token` numeric values
- Ephemeral `FontFamilyId` values
- Probe counts / wall time

## Expected edge differences (documented)

After H1/H2 work, ordinary fonts should still match. Documented edge cases that
may differ:

- Hard-linked font files (path identity vs file identity)
- Faces with no recoverable path/identity

Re-run after any change that can affect Observe merge/alias reuse or Derive
(including parallel Observe, `find_seed_for_alias_candidate` win32-name reuse,
and PhysicalVariantKey interning). gtest serial↔parallel equality does **not**
replace this legacy↔modern check.

### Latest local verification

| Field | Value |
|-------|--------|
| Date | 2026-07-25 |
| Result | **MATCH** (`EXIT=0`) |
| Report | `build-dir/font-family-ab/summary.txt` |
| Locale | zh-cn |
| Families matched | 570 (legacy=570, modern=570, differing=0) |
| Modern wall | ~1.7 s (`modern_physical_probes=2384`) |
| Notes | Includes parallel Observe workers + win32 alias seed-probe reuse |

If your machine differs, overwrite that report directory and trust the new
`summary.txt` / `meta.txt`.

## Phase F store profile

Each run also times `EncodeFontFamilyObsStore` + `SaveFontFamilyObsStoreAtomic`
on the live observation snapshot and records:

- `store_encode_us` / `store_save_us` in `meta.txt`
- `phase_f_stringpool_action=close_no_change` when save &lt; 50 ms

Same local run as above: encode ≈13 ms, save ≈17 ms → **StringPool O(n²) left
unchanged** until profile exceeds the threshold or enterprise font sets
(&gt;5k families) show pain. Prefer `meta.txt` over numbers copied here.
