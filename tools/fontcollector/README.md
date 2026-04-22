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
```

The runtime output is written to `build-fontcollector-static/fontcollector` and should contain only:

- `aegisub_fontcollector.dll`
- `fontcollector.exe`

The tool manifest intentionally excludes ICU, uchardet and Boost.Locale. The normal GUI build leaves `AEGISUB_BUILD_FONT_COLLECTOR_TOOL` off, so it does not find or link CLI11.

When `--encoding` is omitted, the tool uses Aegisub's core charset detector without uchardet: BOMs are honored, otherwise non-binary text defaults to UTF-8.
