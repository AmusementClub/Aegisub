using Aegisub.Managed.Contracts;
using System.Text.Json.Serialization;

[assembly: Aegisub.Plugin.Sdk.AegisubJsonContext(
    "Plugin",
    typeof(Aegisub.Plugin.Sdk.Smoke.SmokeJsonContext))]

namespace Aegisub.Plugin.Sdk.Smoke;

public sealed record SmokePayload(string Message);

[JsonSourceGenerationOptions(PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase)]
[JsonSerializable(typeof(SmokePayload))]
public partial class SmokeJsonContext : JsonSerializerContext;

[Aegisub.Plugin.Sdk.AegisubPlugin(
    "aegisub.plugin-sdk.smoke",
    "Aegisub Plugin SDK smoke",
    "Validates generated registration and manifest packaging.",
    "Aegisub",
    "0.1.0")]
[Aegisub.Plugin.Sdk.AegisubContributionId(
    "Automation",
    "aegisub.plugin-sdk.smoke.automation",
    ContributionKind.Automation)]
[Aegisub.Plugin.Sdk.AegisubUiId(
    "MainToolView",
    "aegisub.plugin-sdk.smoke.tool-view",
    Aegisub.Plugin.Sdk.AegisubUiIdKind.ToolView)]
[Aegisub.Plugin.Sdk.AegisubUiId(
    "RunEvent",
    "aegisub.plugin-sdk.smoke.run",
    Aegisub.Plugin.Sdk.AegisubUiIdKind.Event)]
public sealed class SdkSmokePlugin : IAegisubPlugin
{
    private static readonly IReadOnlyList<IAegisubContribution> RegisteredContributions =
        [new AutomationContribution()];

    public PluginMetadata Metadata { get; } = new(
        Id: "aegisub.plugin-sdk.smoke",
        Name: "Aegisub Plugin SDK smoke",
        Description: "Validates generated registration and manifest packaging.",
        Author: "Aegisub",
        Version: "0.1.0");

    public IReadOnlyList<IAegisubContribution> Contributions => RegisteredContributions;

    private sealed class AutomationContribution : IAegisubAutomationContribution
    {
        public ContributionMetadata Metadata { get; } = new(
            Id: "aegisub.plugin-sdk.smoke.automation",
            Kind: ContributionKind.Automation,
            Name: "Plugin SDK smoke Automation");

        public IReadOnlyList<IAegisubMacro> Macros { get; } = [new SmokeMacro()];
    }

    private sealed class SmokeMacro : IAegisubMacro
    {
        public MacroMetadata Metadata { get; } = new(
            Id: "aegisub.plugin-sdk.smoke.run",
            Name: "Plugin SDK/Run smoke",
            Description: "Runs the generated SDK smoke plugin.");

        public ValueTask<MacroResult> ExecuteAsync(
            IAegisubMacroContext context,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            return ValueTask.FromResult(MacroResult.Completed("SDK smoke completed"));
        }
    }
}
