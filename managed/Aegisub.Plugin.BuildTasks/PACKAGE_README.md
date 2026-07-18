# Aegisub.Plugin.BuildTasks

This experimental package generates manifest v2 JSON from MSBuild properties,
supports CoreCLR and RID-specific native runtime records, and validates staged
plugin files before packaging. It does not use source-generator side effects.

Plugin packages should place the manifest and payload under `aegisub/`, keeping
native libraries at `runtimes/<rid>/native/` relative to the plugin root.
