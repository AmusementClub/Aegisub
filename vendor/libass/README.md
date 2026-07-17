# libass runtime API list

`libass_functions.inc` is the generated list of libass entry points that
Aegisub resolves at runtime. It is generated from the external package's
`ass/ass.h` and the actual `api.ass_*`/`loaded.ass_*` uses in the libass
provider and runtime loader.

## External package layout (Windows)

```text
include/ass/ass.h
include/ass/ass_types.h
runtimes/ass.dll
```

Those files are dependency artifacts and are intentionally ignored by Git
(`/include`, `*.dll`). Populate them with:

```powershell
# Prepare headers (and other dependency archives) into the repo root:
dotnet run --project build/package-win-portable.cs -- `
  --mode prepare-dependency `
  --spec build/package-win-portable.thirdparty.json

# Or manually copy from the extracted prebuilt package:
#   include/ass/*  <- package include/ass/*
#   runtimes/ass.dll <- package bin/ass.dll
```

The Windows package used by the portable tooling is declared in
`build/package-win-portable.thirdparty.json` under the `libass` archive
entries (dependency stage for headers, package stage for the DLL).

The shipped DLL is expected to contain its non-system dependencies
statically (or ship companion DLLs in the same package). The current
0.17.5 x64 package only imports GDI32, KERNEL32, and USER32.

## Regenerate the symbol list

From the repository root after headers are in place:

```powershell
dotnet vendor/libass/gen_libass_functions.cs -- --mode used
```

To inspect all functions declared by a particular header:

```powershell
dotnet vendor/libass/gen_libass_functions.cs -- --mode all --header include/ass/ass.h --output obj/libass_all_functions.inc
```

The generated comment records `LIBASS_VERSION` and the SHA-256 of `ass.h`, so
header changes are visible in review. Regenerate the list whenever the external
header is updated or libass API calls are added or removed.
