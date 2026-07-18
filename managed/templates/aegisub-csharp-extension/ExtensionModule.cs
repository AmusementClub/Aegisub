using Aegisub.Managed.Contracts;

namespace AegisubExtension;

public sealed class ExtensionModule : IAegisubPlugin
{
    private static readonly IReadOnlyList<IAegisubMacro> RegisteredMacros =
        [new HelloMacro()];
    private static readonly IReadOnlyList<IAegisubContribution> RegisteredContributions =
        [new ExtensionAutomationContribution()];

    public PluginMetadata Metadata { get; } = new(
        Id: "sample.extension",
        Name: "AegisubExtension",
        Description: "Template extension description.",
        Author: "Template Author",
        Version: "0.1.0");

    public IReadOnlyList<IAegisubContribution> Contributions => RegisteredContributions;

    private sealed class ExtensionAutomationContribution : IAegisubAutomationContribution
    {
        public ContributionMetadata Metadata { get; } = new(
            Id: "sample.extension.automation",
            Kind: ContributionKind.Automation,
            Name: "AegisubExtension Automation",
            Description: "Automation Macros contributed by the generated extension.");

        public IReadOnlyList<IAegisubMacro> Macros => RegisteredMacros;
    }
}

internal sealed class HelloMacro : IAegisubMacro
{
    public MacroMetadata Metadata { get; } = new(
        Id: "sample.extension.hello",
        Name: "AegisubExtension/Hello from C#",
        Description: "Runs the generated C# extension template Macro.");

    public ValueTask<MacroResult> ExecuteAsync(
        IAegisubMacroContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        context.ReportIndeterminate("Starting generated extension");
        context.Log("Generated Aegisub C# extension executed");
        context.ReportProgress(1, 1, "Generated extension completed");
        return ValueTask.FromResult(MacroResult.Completed(
            "Generated C# extension executed successfully"));
    }
}
