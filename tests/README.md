# Tests

- `tests/` contains the GoogleTest suite built as `gtest-run` and executed by
  `test-aegisub`.
- `smoke/` contains native process and integration checks. The available
  targets are conditional on platform and optional backends; graphical checks
  require a usable Windows desktop and are intentionally separate from the
  fast unit-test gate.
- `plugin-bridge-smoke/` contains versioned automation scenarios and their
  native process runners.
- `gui-automation/` contains the Windows UIA correctness and specialized Skia
  drivers, plus their shared black-box driver support.
- `fixtures/` contains source fixtures. `setup.ps1`, `setup.bat`, and
  `setup.sh` copy them into the build-tree test runtime before unit tests run.
- `support/` contains shared GoogleTest fixtures and test-only stubs.

Generated runtime data belongs under `build-dir/tests-runtime`; the source
tree should contain only test sources and reusable fixtures.

Run the Windows UIA correctness driver directly after building Aegisub:

```powershell
dotnet run --file tests/gui-automation/gui-automation-uia-correctness.cs `
  --artifacts-path build-dir/dotnet-artifacts/gui-automation-uia/RelWithDebInfo `
  -- `
  --exe build-dir/RelWithDebInfo/Aegisub.exe `
  --open tests/plugin-bridge-smoke/trim-selected-input.ass `
  --artifacts build-dir/gui-automation-uia-correctness/RelWithDebInfo
```
