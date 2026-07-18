namespace Aegisub.Managed.Contracts;

public sealed record PluginMetadata(
    string Id,
    string Name,
    string Description,
    string Author,
    string Version,
    string NameResourceKey = "",
    string DescriptionResourceKey = "");

public enum ContributionKind
{
    Automation,
    Command,
    Settings,
    ToolView,
    ServiceProvider
}

public sealed record ContributionMetadata(
    string Id,
    ContributionKind Kind,
    string Name = "",
    string Description = "",
    string NameResourceKey = "",
    string DescriptionResourceKey = "");

public interface IAegisubContribution
{
    ContributionMetadata Metadata { get; }
}

public interface IAegisubAutomationContribution : IAegisubContribution
{
    IReadOnlyList<IAegisubMacro> Macros { get; }
}

public interface IAegisubServiceProviderContribution : IAegisubContribution
{
    IReadOnlyList<string> Operations { get; }

    ValueTask<string> InvokeAsync(
        string operationId,
        string requestJson,
        CancellationToken cancellationToken);
}

public interface IAegisubPlugin
{
    PluginMetadata Metadata { get; }
    IReadOnlyList<IAegisubContribution> Contributions { get; }
}

public interface IAegisubPluginContext
{
    void Log(string message);

    ValueTask<string> InvokeHostServiceAsync(
        string serviceId,
        string requestJson,
        CancellationToken cancellationToken);

    void EmitEvent(string eventId, string payloadJson);
}

public interface IAegisubPluginLifecycle
{
    ValueTask ActivateAsync(
        IAegisubPluginContext context,
        CancellationToken cancellationToken);

    ValueTask DeactivateAsync(CancellationToken cancellationToken);
}

public sealed record PluginEvent(string Id, string PayloadJson);

public interface IAegisubPluginEventHandler
{
    ValueTask HandleEventAsync(
        PluginEvent pluginEvent,
        CancellationToken cancellationToken);
}
