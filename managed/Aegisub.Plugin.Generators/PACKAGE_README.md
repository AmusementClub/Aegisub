# Aegisub.Plugin.Generators

This package generates static plugin construction, stable contribution/UI ID
constants, and the optional `aegisub_plugin_init_v1` NativeAOT export. It also
reports invalid IDs, duplicate symbolic names, incompatible plugin types, and
missing public parameterless constructors at compile time.

The generator only adds C# source to the current compilation. Manifest and
NuGet layout generation belong to `Aegisub.Plugin.BuildTasks`.
