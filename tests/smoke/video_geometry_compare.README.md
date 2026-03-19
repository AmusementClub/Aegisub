# video-geometry-compare

`video-geometry-compare` now supports two input modes:

1. Built-in generated sample mode
   - `video-geometry-compare --samples-dir <dir> --report <json>`
   - Uses the repository's built-in synthetic sample set and pinned expectations.
2. Local manifest mode
   - `video-geometry-compare --manifest <txt> --report <json>`
   - Uses a local text manifest of real sample files.

## Manifest format

- One sample per line.
- Format: `sample_name|path_to_video_file`
- Empty lines and `#` comments are ignored.
- Relative paths are resolved against the manifest file location.

Use [video_geometry_compare_manifest.example.txt](video_geometry_compare_manifest.example.txt) as a template, but keep your real manifest outside git-tracked paths if it contains local or sensitive locations.

## Behavior

- Built-in mode asserts pinned geometry expectations for the synthetic sample set.
- Manifest mode does not assume theoretical geometry expectations.
- Manifest mode still validates:
  - provider width/height/DAR against computed display output
  - BGRA vs native video parity
  - `SourceStorage` vs `SourceVisible` overlay parity

That makes manifest mode suitable for collecting and freezing real-world FFMS/provider behavior before changing geometry consumption logic.
