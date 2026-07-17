# libass runtime API list

`libass_functions.inc` is the generated list of libass entry points that
Aegisub resolves at runtime. It is generated from the external package's
`ass/ass.h` and the actual `api.ass_*`/`loaded.ass_*` uses in the libass
provider and runtime loader.

The local Windows external-package layout is:

```text
include/ass/ass.h
include/ass/ass_types.h
runtimes/ass.dll
```

Those files are dependency artifacts and are intentionally ignored by Git.
They may be populated from a prebuilt package; an existing dynamic vcpkg
installation can be used locally to simulate that package while the packaging
manifest is being integrated.

The Windows DLL must either contain its non-system dependencies statically or
ship those dependency DLLs in the same runtime package. The current local
vcpkg-built DLL only imports Windows, UCRT, and MSVC runtime libraries.

Regenerate the used-symbol list from the repository root:

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
