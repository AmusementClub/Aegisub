using System.Text.Json.Serialization;

namespace Aegisub.Managed.Contracts;

public enum UiControlKind
{
    Label,
    Text,
    MultilineText,
    Integer,
    Number,
    Checkbox,
    Select,
    Progress
}

public enum ToolViewPlacement
{
    Auto,
    Floating,
    Docked
}

public sealed record UiChoiceDefinition(string Id, string Label);

public sealed record UiControlDefinition(
    string Id,
    UiControlKind Kind,
    string Label = "",
    string Text = "",
    bool Checked = false,
    double Number = 0,
    double? Minimum = null,
    double? Maximum = null,
    double? Step = null,
    IReadOnlyList<UiChoiceDefinition>? Choices = null,
    string SelectedChoiceId = "",
    string Help = "",
    int Row = 0,
    int Column = 0,
    int ColumnSpan = 1,
    bool Enabled = true,
    bool Visible = true,
    bool Required = false);

public sealed record UiActionDefinition(
    string Id,
    string Label,
    bool IsDefault = false,
    bool IsCancel = false,
    bool Enabled = true);

public sealed record UiValue(string Id, string JsonValue);

public sealed record FormDefinition(
    string Id,
    string Title,
    IReadOnlyList<UiControlDefinition> Controls,
    IReadOnlyList<UiActionDefinition> Actions,
    int MinimumWidth = 0,
    int MinimumHeight = 0,
    bool Resizable = true,
    int SchemaVersion = 1);

public sealed record FormResult(
    string FormId,
    string ActionId,
    IReadOnlyList<UiValue> Values,
    bool Cancelled,
    int SchemaVersion = 1);

public sealed record ToolViewColumnDefinition(
    string Id,
    string Label,
    int Width = 120);

public sealed record ToolViewRowDefinition(
    string Id,
    IReadOnlyList<UiValue> Cells);

public sealed record ToolViewTableDefinition(
    string Id,
    IReadOnlyList<ToolViewColumnDefinition> Columns,
    IReadOnlyList<ToolViewRowDefinition> Rows,
    bool MultiSelect = false,
    int MinimumHeight = 120);

public sealed record ToolViewTabDefinition(
    string Id,
    string Label,
    IReadOnlyList<UiControlDefinition> Controls,
    IReadOnlyList<ToolViewTableDefinition> Tables,
    IReadOnlyList<UiActionDefinition> Actions);

public sealed record ToolViewDefinition(
    string Id,
    string Title,
    IReadOnlyList<UiControlDefinition> Controls,
    IReadOnlyList<ToolViewTableDefinition> Tables,
    IReadOnlyList<UiActionDefinition> Actions,
    ToolViewPlacement Placement = ToolViewPlacement.Auto,
    int MinimumWidth = 360,
    int MinimumHeight = 240,
    bool Resizable = true,
    int SchemaVersion = 1,
    IReadOnlyList<ToolViewTabDefinition>? Tabs = null);

public sealed record UiControlPatch(
    string Id,
    string? Text = null,
    bool? Checked = null,
    double? Number = null,
    string? SelectedChoiceId = null,
    bool? Enabled = null,
    bool? Visible = null);

public sealed record ToolViewTablePatch(
    string Id,
    IReadOnlyList<ToolViewRowDefinition> Rows,
    bool Replace = true);

public sealed record ToolViewPatch(
    string ViewId,
    long Revision,
    IReadOnlyList<UiControlPatch> Controls,
    IReadOnlyList<ToolViewTablePatch> Tables,
    IReadOnlyList<string>? SelectedRowIds = null,
    int SchemaVersion = 1,
    string? SelectedTabId = null);

public sealed record ToolViewEvent(
    string ViewId,
    string EventId,
    string SourceId,
    IReadOnlyList<UiValue> Values,
    IReadOnlyList<string> SelectedRowIds,
    long Revision,
    int SchemaVersion = 1,
    string ActiveTabId = "");

public sealed record OpenFormRequest(
    FormDefinition Definition,
    string OwnerViewId = "");
public sealed record OpenToolViewRequest(ToolViewDefinition Definition);
public sealed record PatchToolViewRequest(ToolViewPatch Patch);
public sealed record CloseToolViewRequest(string ViewId);
public sealed record ToolViewOperationResult(
    string ViewId,
    bool Succeeded,
    string Status = "");

public enum VideoCoordinateSpace
{
    Script,
    Frame
}

public sealed record VideoPoint(double X, double Y);

public sealed record VideoPointSelectionRequest(
    string ViewId,
    string SessionId,
    int PointCount = 2,
    VideoCoordinateSpace CoordinateSpace = VideoCoordinateSpace.Script,
    bool IncludeDistance = true);

public sealed record VideoPointSelectionResult(
    string ViewId,
    string SessionId,
    IReadOnlyList<VideoPoint> Points,
    double DeltaX,
    double DeltaY,
    double Distance,
    int Frame,
    bool Cancelled);

[JsonSourceGenerationOptions(
    PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase,
    UseStringEnumConverter = true)]
[JsonSerializable(typeof(FormDefinition))]
[JsonSerializable(typeof(FormResult))]
[JsonSerializable(typeof(ToolViewDefinition))]
[JsonSerializable(typeof(ToolViewPatch))]
[JsonSerializable(typeof(ToolViewEvent))]
[JsonSerializable(typeof(OpenFormRequest))]
[JsonSerializable(typeof(OpenToolViewRequest))]
[JsonSerializable(typeof(PatchToolViewRequest))]
[JsonSerializable(typeof(CloseToolViewRequest))]
[JsonSerializable(typeof(ToolViewOperationResult))]
[JsonSerializable(typeof(VideoPointSelectionRequest))]
[JsonSerializable(typeof(VideoPointSelectionResult))]
public sealed partial class DeclarativeUiJsonContext : JsonSerializerContext
{
}
