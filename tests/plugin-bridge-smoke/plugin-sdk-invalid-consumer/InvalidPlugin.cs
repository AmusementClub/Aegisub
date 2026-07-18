using Aegisub.Managed.Contracts;
using Aegisub.Plugin.Sdk;

namespace Aegisub.Plugin.Sdk.InvalidSmoke;

[AegisubPlugin(
    "Invalid Plugin ID",
    "Invalid plugin fixture",
    "This project must fail with AEGISUBSDK001.",
    "Aegisub",
    "0.1.0")]
public sealed class InvalidPlugin : IAegisubPlugin
{
    public PluginMetadata Metadata { get; } = new(
        "invalid",
        "Invalid plugin fixture",
        "",
        "Aegisub",
        "0.1.0");

    public IReadOnlyList<IAegisubContribution> Contributions => [];
}
