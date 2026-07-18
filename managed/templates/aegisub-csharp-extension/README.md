# AegisubExtension

This project targets the experimental Aegisub managed plugin Contracts `0.1.0`
on .NET 10. It uses manifest v2 and contributes Automation Macros through
`IAegisubPlugin`.
The stable extension ID is `sample.extension`; do not derive persisted IDs from
translated names or C# type names.

## Debug

The project includes three IDE launch profiles:

- `Managed fixture` runs the Macro in the standalone DevHost.
- `Aegisub CLI integration` runs a repeatable real-host integration session.
- `Aegisub GUI integration` starts the normal wx host for menu and UI checks.

A Debug build deploys the DLL, PDB, dependencies, and manifest when
`__AEGISUB_DEVELOPMENT_ROOT__/Aegisub.exe` exists. Set
`AegisubDeployDevelopmentBuild=false` to disable this behavior, or override
`AegisubDevelopmentRoot` for another build directory.

The repository-local Contracts project is used when
`__AEGISUB_CONTRACTS_PROJECT__` exists. Otherwise restore falls back to the
`Aegisub.Managed.Contracts` NuGet package at the version declared in the project.
`RestorePackagesWithLockFile` is enabled; commit `packages.lock.json` after the
first successful restore and use `dotnet restore --locked-mode` in CI.
