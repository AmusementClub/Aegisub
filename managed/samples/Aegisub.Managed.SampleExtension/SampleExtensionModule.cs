using System.Globalization;
using System.Runtime.InteropServices;
using System.Text.Json;
using Aegisub.Managed.Contracts;

namespace Aegisub.Managed.SampleExtension;

public sealed class SampleExtensionModule :
    IAegisubPlugin,
    IAegisubPluginLifecycle,
    IAegisubPluginEventHandler
{
    private static readonly SamplePluginServices Services = new();
    internal static readonly IReadOnlyList<IAegisubMacro> RegisteredMacros =
        [new RuntimeInfoMacro(), new TrimSelectedLineEndingsMacro(), new VideoMeasurementToolMacro(Services)];
    private static readonly IReadOnlyList<IAegisubContribution> RegisteredContributions =
        [new SampleAutomationContribution(), new SampleReservedSettingsContribution()];
    private IAegisubPluginContext? _context;

    public PluginMetadata Metadata { get; } = new(
        Id: "aegisub.plugin-bridge.demo",
        Name: "Plugin Bridge Demo",
        Description: "Validates lazy hostfxr activation through an Automation Macro.",
        Author: "Aegisub",
        Version: "0.2.0");

    public IReadOnlyList<IAegisubContribution> Contributions => RegisteredContributions;

    public async ValueTask ActivateAsync(
        IAegisubPluginContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        const string activationPayload = "{\"phase\":\"activate\"}";
        string echoed = await context.InvokeHostServiceAsync(
            "aegisub.bridge.echo",
            activationPayload,
            cancellationToken).ConfigureAwait(false);
        if (!string.Equals(echoed, activationPayload, StringComparison.Ordinal))
            throw new InvalidOperationException("The native host-service Bridge returned an invalid echo.");
        _context = context;
        Services.Context = context;
        context.EmitEvent("aegisub.plugin.activated", echoed);
        context.Log("Activated managed plugin contribution model");
    }

    public ValueTask DeactivateAsync(CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        Services.Context = null;
        _context = null;
        return ValueTask.CompletedTask;
    }

    public async ValueTask HandleEventAsync(
        PluginEvent pluginEvent,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        ArgumentNullException.ThrowIfNull(pluginEvent);
        _context?.Log($"Handled plugin event '{pluginEvent.Id}': {pluginEvent.PayloadJson}");
        await Services.HandleEventAsync(pluginEvent, cancellationToken).ConfigureAwait(false);
    }
}

internal sealed class SamplePluginServices
{
    private const string ViewId = "aegisub.plugin-bridge.demo.video-measurement";
    private long _revision;

    public IAegisubPluginContext? Context { get; set; }

    public async ValueTask HandleEventAsync(
        PluginEvent pluginEvent,
        CancellationToken cancellationToken)
    {
        IAegisubPluginContext? context = Context;
        if (context is null)
            return;

        if (string.Equals(
            pluginEvent.Id,
            "aegisub.ui.toolViewEvent",
            StringComparison.Ordinal))
        {
            ToolViewEvent toolEvent = JsonSerializer.Deserialize(
                pluginEvent.PayloadJson,
                DeclarativeUiJsonContext.Default.ToolViewEvent)
                ?? throw new InvalidOperationException("The host returned an empty ToolView event.");
            if (!string.Equals(toolEvent.ViewId, ViewId, StringComparison.Ordinal) ||
                !string.Equals(toolEvent.EventId, "action", StringComparison.Ordinal) ||
                !string.Equals(toolEvent.SourceId, "measure", StringComparison.Ordinal))
                return;

            VideoPointSelectionRequest request = new(
                ViewId,
                $"two-point-{Interlocked.Increment(ref _revision)}",
                PointCount: 2,
                CoordinateSpace: VideoCoordinateSpace.Script,
                IncludeDistance: true);
            await context.InvokeHostServiceAsync(
                "aegisub.video.beginPointSelection",
                JsonSerializer.Serialize(
                    request,
                    DeclarativeUiJsonContext.Default.VideoPointSelectionRequest),
                cancellationToken).ConfigureAwait(false);
            return;
        }

        if (!string.Equals(
            pluginEvent.Id,
            "aegisub.video.pointSelectionCompleted",
            StringComparison.Ordinal))
            return;

        VideoPointSelectionResult result = JsonSerializer.Deserialize(
            pluginEvent.PayloadJson,
            DeclarativeUiJsonContext.Default.VideoPointSelectionResult)
            ?? throw new InvalidOperationException("The host returned an empty video selection result.");
        if (!string.Equals(result.ViewId, ViewId, StringComparison.Ordinal))
            return;

        string status = result.Cancelled
            ? "点选已取消"
            : $"ΔX={result.DeltaX:0.###}, ΔY={result.DeltaY:0.###}, 距离={result.Distance:0.###}, 帧={result.Frame}";
        ToolViewRowDefinition[] rows = result.Points
            .Select((point, index) => new ToolViewRowDefinition(
                $"point-{index + 1}",
                [
                    new UiValue("index", (index + 1).ToString(CultureInfo.InvariantCulture)),
                    new UiValue("x", point.X.ToString("0.###", CultureInfo.InvariantCulture)),
                    new UiValue("y", point.Y.ToString("0.###", CultureInfo.InvariantCulture))
                ]))
            .ToArray();
        ToolViewPatch patch = new(
            ViewId,
            Interlocked.Increment(ref _revision),
            [new UiControlPatch("status", Text: status)],
            [new ToolViewTablePatch("points", rows)]);
        await context.InvokeHostServiceAsync(
            "aegisub.ui.patchToolView",
            JsonSerializer.Serialize(
                new PatchToolViewRequest(patch),
                DeclarativeUiJsonContext.Default.PatchToolViewRequest),
            cancellationToken).ConfigureAwait(false);
    }

    public ToolViewDefinition CreateToolView() => new(
        ViewId,
        "C# 视频两点测量",
        [
            new UiControlDefinition(
                "instructions",
                UiControlKind.Label,
                Text: "点击“测量两点”，然后在视频中依次点击两个位置。",
                Row: 0,
                Column: 0,
                ColumnSpan: 2),
            new UiControlDefinition(
                "status",
                UiControlKind.Label,
                Text: "尚未测量",
                Row: 1,
                Column: 0,
                ColumnSpan: 2)
        ],
        [
            new ToolViewTableDefinition(
                "points",
                [
                    new ToolViewColumnDefinition("index", "点", 60),
                    new ToolViewColumnDefinition("x", "X", 110),
                    new ToolViewColumnDefinition("y", "Y", 110)
                ],
                [])
        ],
        [
            new UiActionDefinition("measure", "测量两点", IsDefault: true),
            new UiActionDefinition("close", "关闭", IsCancel: true)
        ],
        Placement: ToolViewPlacement.Auto,
        MinimumWidth: 430,
        MinimumHeight: 300);
}

internal sealed class SampleAutomationContribution : IAegisubAutomationContribution
{
    public ContributionMetadata Metadata { get; } = new(
        Id: "aegisub.plugin-bridge.demo.automation",
        Kind: ContributionKind.Automation,
        Name: "Plugin Bridge Demo Automation",
        Description: "Automation Macros contributed by the Plugin Bridge demo.");

    public IReadOnlyList<IAegisubMacro> Macros => SampleExtensionModule.RegisteredMacros;
}

internal sealed class SampleReservedSettingsContribution : IAegisubContribution
{
    public ContributionMetadata Metadata { get; } = new(
        Id: "aegisub.plugin-bridge.demo.settings",
        Kind: ContributionKind.Settings,
        Name: "Plugin Bridge Demo Settings",
        Description: "Descriptor reserved for future host-rendered settings support.");
}

public sealed class LegacySampleExtensionModule : IAegisubAutomationModule
{
    public ExtensionMetadata Metadata { get; } = new(
        Id: "aegisub.plugin-bridge.legacy-demo",
        Name: "Legacy C# Automation Demo",
        Description: "Exercises the IAegisubAutomationModule compatibility adapter.",
        Author: "Aegisub",
        Version: "0.2.0");

    public IReadOnlyList<IAegisubMacro> Macros => SampleExtensionModule.RegisteredMacros;
}

public sealed class DuplicateContributionPlugin : IAegisubPlugin
{
    private static readonly IAegisubContribution Duplicate = new SampleAutomationContribution();

    public PluginMetadata Metadata { get; } = new(
        "aegisub.plugin-bridge.invalid-duplicate",
        "Invalid duplicate contribution plugin",
        "Adapter validation fixture.",
        "Aegisub",
        "0.2.0");

    public IReadOnlyList<IAegisubContribution> Contributions { get; } = [Duplicate, Duplicate];
}

public sealed class WrongContributionKindPlugin : IAegisubPlugin
{
    private sealed class WrongKindAutomationContribution : IAegisubAutomationContribution
    {
        public ContributionMetadata Metadata { get; } = new(
            "aegisub.plugin-bridge.invalid-kind.automation",
            ContributionKind.Command);

        public IReadOnlyList<IAegisubMacro> Macros => SampleExtensionModule.RegisteredMacros;
    }

    public PluginMetadata Metadata { get; } = new(
        "aegisub.plugin-bridge.invalid-kind",
        "Invalid contribution kind plugin",
        "Adapter validation fixture.",
        "Aegisub",
        "0.2.0");

    public IReadOnlyList<IAegisubContribution> Contributions { get; } =
        [new WrongKindAutomationContribution()];
}

public sealed class MissingHostServicePlugin : IAegisubPlugin, IAegisubPluginLifecycle
{
    public PluginMetadata Metadata { get; } = new(
        "aegisub.plugin-bridge.missing-host-service",
        "Missing host service plugin",
        "Bridge host-service failure fixture.",
        "Aegisub",
        "0.2.0");

    public IReadOnlyList<IAegisubContribution> Contributions { get; } =
        [new SampleReservedSettingsContribution()];

    public async ValueTask ActivateAsync(
        IAegisubPluginContext context,
        CancellationToken cancellationToken)
    {
        await context.InvokeHostServiceAsync(
            "aegisub.bridge.missing-service",
            "{}",
            cancellationToken).ConfigureAwait(false);
    }

    public ValueTask DeactivateAsync(CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;
}

internal sealed class TrimSelectedLineEndingsMacro : IAegisubMacro
{
    public MacroMetadata Metadata { get; } = new(
        Id: "aegisub.plugin-bridge.demo.trim-selected-line-endings",
        Name: "Plugin Bridge Demo/清理所选行行末空白",
        Description: "Removes spaces and tabs before ASS line breaks and at the end of selected dialogue lines.",
        RequiresProject: true,
        SubtitleAccess: SubtitleAccess.ReadWrite,
        QueryState: new MacroQueryState(MinimumSelectedEvents: 1),
        SnapshotScope: SubtitleSnapshotScope.Selection);

    public ValueTask<MacroResult> ExecuteAsync(
        IAegisubMacroContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        SubtitleDocumentSnapshot subtitles = context.Subtitles
            ?? throw new AutomationFailureException(
                "aegisub.subtitle.required",
                "This Macro requires an open subtitle document.");

        HashSet<int> selectedIds = [.. subtitles.SelectedEventIds];
        List<SubtitleEventPatch> patches = [];
        foreach (SubtitleEventSnapshot line in subtitles.Events)
        {
            if (!selectedIds.Contains(line.EventId))
                continue;

            string trimmed = TrimLineEndWhitespace(line.Text);
            if (!string.Equals(trimmed, line.Text, StringComparison.Ordinal))
                patches.Add(new SubtitleEventPatch(line.EventId, Text: trimmed));
        }

        if (patches.Count == 0)
            return ValueTask.FromResult(MacroResult.Completed("所选字幕行没有需要清理的行末空白"));

        context.Log($"Prepared {patches.Count} subtitle event patch(es) in one mutation batch");
        SubtitleMutationBatch mutation = new(
            ExpectedDocumentToken: subtitles.DocumentToken,
            UndoDescription: "清理所选行行末空白",
            EventPatches: patches);
        return ValueTask.FromResult(MacroResult.Completed(
            $"已清理 {patches.Count} 行字幕的行末空白",
            mutation));
    }

    private static string TrimLineEndWhitespace(string text)
    {
        int firstCandidate = text.IndexOfAny([' ', '\t']);
        if (firstCandidate < 0)
            return text;

        System.Text.StringBuilder result = new(text.Length);
        result.Append(text, 0, firstCandidate);
        int index = firstCandidate;
        while (index < text.Length)
        {
            if (text[index] is not (' ' or '\t'))
            {
                result.Append(text[index++]);
                continue;
            }

            int whitespaceStart = index;
            while (index < text.Length && text[index] is ' ' or '\t')
                ++index;

            bool beforeAssLineBreak = index + 1 < text.Length &&
                text[index] == '\\' && text[index + 1] is 'N' or 'n';
            if (index < text.Length && !beforeAssLineBreak)
                result.Append(text, whitespaceStart, index - whitespaceStart);
        }
        return result.ToString();
    }
}

internal sealed class RuntimeInfoMacro : IAegisubMacro
{
    public MacroMetadata Metadata { get; } = new(
        Id: "aegisub.plugin-bridge.demo.runtime-info",
        Name: "Plugin Bridge Demo/Runtime 信息",
        Description: "Loads an external C# extension in a collectible ALC and reports the selected CLR.");

    public ValueTask<MacroResult> ExecuteAsync(
        IAegisubMacroContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        context.ReportIndeterminate("正在读取 .NET Runtime 信息");
        context.Log($"Executing {Metadata.Id} from {typeof(RuntimeInfoMacro).Assembly.GetName().Name}");
        context.ReportProgress(1, 1, "Runtime 信息读取完成");
        return ValueTask.FromResult(MacroResult.Completed(
            $"外部 C# Automation Macro 已执行；Runtime={RuntimeInformation.FrameworkDescription}"));
    }
}

internal sealed class VideoMeasurementToolMacro(SamplePluginServices services) : IAegisubMacro
{
    public MacroMetadata Metadata { get; } = new(
        Id: "aegisub.plugin-bridge.demo.video-measurement",
        Name: "Plugin Bridge Demo/视频两点测量工具",
        Description: "Opens a host-rendered ToolView and measures two points in the video.",
        RequiresProject: true,
        QueryState: new MacroQueryState(RequiresVideo: true));

    public async ValueTask<MacroResult> ExecuteAsync(
        IAegisubMacroContext context,
        CancellationToken cancellationToken)
    {
        IAegisubPluginContext pluginContext = services.Context
            ?? throw new AutomationFailureException(
                "aegisub.plugin.not_active",
                "The sample plugin context is not active.");
        ToolViewOperationResult result = JsonSerializer.Deserialize(
            await pluginContext.InvokeHostServiceAsync(
                "aegisub.ui.openToolView",
                JsonSerializer.Serialize(
                    new OpenToolViewRequest(services.CreateToolView()),
                    DeclarativeUiJsonContext.Default.OpenToolViewRequest),
                cancellationToken).ConfigureAwait(false),
            DeclarativeUiJsonContext.Default.ToolViewOperationResult)
            ?? throw new InvalidOperationException("The host returned an empty ToolView result.");
        return MacroResult.Completed(result.Succeeded
            ? "已打开 C# 视频两点测量工具"
            : "C# 视频两点测量工具已经打开，已将其置于前台");
    }
}
