# Aegisub.Plugin.Sdk

This experimental package supplies compile-time attributes and descriptors for
Aegisub managed plugins. Pair it with `Aegisub.Plugin.Generators` for static
registration, stable ID constants, and an optional NativeAOT C ABI entry point.

The package targets the internal manifest v2, Contracts `0.1.0`, and Bridge ABI
1 baseline. Exact package versions must be pinned while the API remains `0.x`.
