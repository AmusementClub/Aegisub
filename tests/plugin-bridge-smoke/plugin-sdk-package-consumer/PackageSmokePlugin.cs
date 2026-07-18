using Aegisub.Managed.Contracts;
using Aegisub.Plugin.Generated;
using Aegisub.Plugin.Sdk;

namespace Aegisub.Plugin.Sdk.PackageSmoke;

[AegisubPlugin(
    "aegisub.plugin-sdk.package-smoke",
    "Aegisub packaged SDK smoke",
    "Validates transitive generator and BuildTasks assets.",
    "Aegisub",
    "0.1.0")]
[AegisubContributionId(
    "Service",
    "aegisub.plugin-sdk.package-smoke.service",
    ContributionKind.ServiceProvider)]
public sealed class PackageSmokePlugin : IAegisubPlugin
{
    public PluginMetadata Metadata { get; } = new(
        "aegisub.plugin-sdk.package-smoke",
        "Aegisub packaged SDK smoke",
        "Validates transitive generator and BuildTasks assets.",
        "Aegisub",
        "0.1.0");

    public IReadOnlyList<IAegisubContribution> Contributions => [];

    public static IAegisubPlugin CreateGenerated() =>
        Registration_global__Aegisub_Plugin_Sdk_PackageSmoke_PackageSmokePlugin
            .CreatePlugin();
}
