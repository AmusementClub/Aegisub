[![Build Status](https://github.com/wangqr/Aegisub/actions/workflows/gha-ci.yml/badge.svg)](https://github.com/wangqr/Aegisub/actions/workflows/gha-ci.yml)

# Aegisub

For binaries and general information see [the homepage](http://www.aegisub.org) and [release page](https://github.com/wangqr/Aegisub/releases).

The bug tracker can be found at https://github.com/wangqr/Aegisub/issues .

If you want to test the upstream version, r8942 [can be downloaded here](http://www.plorkyeran.com/aegisub/). If both r8942 and this fork have some common issue, report at [upstream](https://github.com/Aegisub/Aegisub/issues) may let more people see your issue, and I am also watching the upstream for issues. If it is a wangqr fork specific issue, report it here.

Support is available on IRC ( irc://irc.rizon.net/aegisub , for upstream version) or via issues.

## Building Aegisub

### CMake

CMake is the supported build system for this fork. The repository now assumes bundled LuaJIT; the old Autotools entry points are no longer maintained.

Aegisub has some required dependencies:
* `libass`
* `Boost` (with ICU support)
* `OpenGL`
* `libicu`
* `wxWidgets`
* `zlib`
* `fontconfig` (not needed on Windows)

and optional dependencies:
* `ALSA`
* `FFMS2`
* `FFTW`
* `Hunspell`
* `OpenAL`
* `uchardet`
* `AviSynth+`

You can use the package manager provided by your distro to install these dependencies. Package names vary by distro. Some useful references are:

* For ArchLinux, refer to [AUR](https://aur.archlinux.org/cgit/aur.git/tree/PKGBUILD?h=aegisub-git).
* For macOS, see [Special notice for macOS](https://github.com/wangqr/Aegisub/wiki/Special-notice-for-macOS) on project Wiki.

You still need to install the dependencies above. To enable AviSynth+ support, it is also needed. Installing dependencies on Windows can be tricky, as Windows doesn't have a good package manager. Refer to [the Wiki page](https://github.com/wangqr/Aegisub/wiki/Compile-guide-for-Windows-(CMake,-MSVC)) on how to get all dependencies on Windows.

After installing the dependencies, you can clone and build Aegisub with:

```sh
git clone https://github.com/wangqr/Aegisub.git
cd Aegisub
mkdir build-dir
cmake -S . -B build-dir  # Or use cmake-gui / ccmake
cmake --build build-dir
```

On Windows, generate version metadata before configuring:

```powershell
bash.exe build/version.sh .
cmake -S . -B build-dir
cmake --build build-dir --config RelWithDebInfo
```

This repository also includes Visual Studio presets in `CMakePresets.json`. For example:

```powershell
cmake --preset relwithdebinfo-x64
cmake --build --preset relwithdebinfo-x64
```

Features can be turned on/off in CMake by toggling the `WITH_*` switches.
On Windows, `-DAEGISUB_LUAJIT_SHARED=ON` builds the bundled LuaJIT as `lua51.dll` instead of statically linking it into the executable.

For Archlinux users, you can also try the [PKGBUILD in project wiki](https://github.com/wangqr/Aegisub/wiki/PKGBUILD-for-Arch).

## Updating Moonscript

From within the Moonscript repository, run `bin/moon bin/splat.moon -l moonscript moonscript/ > bin/moonscript.lua`.
Open the newly created `bin/moonscript.lua`, and within it make the following changes:

1. Prepend the final line of the file, `package.preload["moonscript"]()`, with a `return`, producing `return package.preload["moonscript"]()`.
2. Within the function at `package.preload['moonscript.base']`, remove references to `moon_loader`, `insert_loader`, and `remove_loader`. This means removing their declarations, definitions, and entries in the returned table.
3. Within the function at `package.preload['moonscript']`, remove the line `_with_0.insert_loader()`.

The file is now ready for use, to be placed in `automation/include` within the Aegisub repo.

## License

All files in this repository are licensed under various GPL-compatible BSD-style licenses; see LICENCE and the individual source files for more information.
The official Windows build is GPLv2 due to including fftw3.
