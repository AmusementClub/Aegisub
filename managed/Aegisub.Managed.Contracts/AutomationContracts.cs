namespace Aegisub.Managed.Contracts;

public sealed class AutomationFailureException : Exception
{
    public string Code { get; }
    public bool IsRetryable { get; }

    public AutomationFailureException(
        string code,
        string message,
        bool isRetryable = false,
        Exception? innerException = null)
        : base(message, innerException)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(code);
        ArgumentException.ThrowIfNullOrWhiteSpace(message);
        if (code.Length > 128 || !code.All(IsCodeCharacter))
        {
            throw new ArgumentException(
                "Automation failure codes must contain only lowercase ASCII letters, digits, '.', '_' or '-'.",
                nameof(code));
        }
        Code = code;
        IsRetryable = isRetryable;
    }

    private static bool IsCodeCharacter(char value) =>
        value is >= 'a' and <= 'z' or >= '0' and <= '9' or '.' or '_' or '-';
}

public sealed record ExtensionMetadata(
    string Id,
    string Name,
    string Description,
    string Author,
    string Version,
    string NameResourceKey = "",
    string DescriptionResourceKey = "");

public enum SubtitleAccess
{
    None,
    Read,
    ReadWrite
}

public enum SubtitleSnapshotScope
{
    Full,
    Selection
}

public sealed record MacroQueryState(
    bool RequiresSubtitleFile = false,
    int MinimumSelectedEvents = 0,
    bool RequiresActiveEvent = false,
    bool RequiresVideo = false,
    bool RequiresAudio = false,
    bool RequiresKeyframes = false);

public sealed record MacroMetadata(
    string Id,
    string Name,
    string Description,
    bool RequiresProject = false,
    SubtitleAccess SubtitleAccess = SubtitleAccess.None,
    MacroQueryState? QueryState = null,
    SubtitleSnapshotScope SnapshotScope = SubtitleSnapshotScope.Full,
    string NameResourceKey = "",
    string DescriptionResourceKey = "");

public sealed record SubtitleColor(byte Red, byte Green, byte Blue, byte Alpha);

public sealed record SubtitleInfoEntry(string Key, string Value);

public sealed record SubtitleExtradataEntry(uint Id, string Key, string Value);

public sealed record SubtitleStyleSnapshot(
    string Name,
    string FontName,
    double FontSize,
    SubtitleColor PrimaryColor,
    SubtitleColor SecondaryColor,
    SubtitleColor OutlineColor,
    SubtitleColor ShadowColor,
    bool Bold,
    bool Italic,
    bool Underline,
    bool Strikeout,
    double ScaleX,
    double ScaleY,
    double Spacing,
    double Angle,
    int BorderStyle,
    double OutlineWidth,
    double ShadowWidth,
    int Alignment,
    int MarginLeft,
    int MarginRight,
    int MarginVertical,
    int Encoding);

public sealed record SubtitleEventSnapshot(
    int EventId,
    int RowIndex,
    bool Comment,
    int Layer,
    int StartMilliseconds,
    int EndMilliseconds,
    string Style,
    string Actor,
    string Effect,
    int MarginLeft,
    int MarginRight,
    int MarginVertical,
    string Text,
    IReadOnlyList<uint> ExtradataIds);

public sealed record SubtitleDocumentSnapshot(
    long DocumentToken,
    string FileName,
    IReadOnlyList<SubtitleInfoEntry> ScriptInfo,
    IReadOnlyList<SubtitleStyleSnapshot> Styles,
    IReadOnlyList<SubtitleExtradataEntry> Extradata,
    IReadOnlyList<SubtitleEventSnapshot> Events,
    IReadOnlyList<int> SelectedEventIds,
    int? ActiveEventId);

public sealed record SubtitleEventPatch(
    int EventId,
    bool? Comment = null,
    int? Layer = null,
    int? StartMilliseconds = null,
    int? EndMilliseconds = null,
    string? Style = null,
    string? Actor = null,
    string? Effect = null,
    int? MarginLeft = null,
    int? MarginRight = null,
    int? MarginVertical = null,
    string? Text = null);

public sealed record SubtitleMutationBatch(
    long ExpectedDocumentToken,
    string UndoDescription,
    IReadOnlyList<SubtitleEventPatch> EventPatches,
    IReadOnlyList<int>? SelectedEventIds = null,
    int? ActiveEventId = null);

public sealed record MacroResult(
    string StatusMessage,
    SubtitleMutationBatch? Mutation = null)
{
    public static MacroResult Completed(
        string statusMessage,
        SubtitleMutationBatch? mutation = null) => new(statusMessage, mutation);
}

public interface IAegisubMacroContext
{
    SubtitleDocumentSnapshot? Subtitles { get; }

    void Log(string message);

    void ReportProgress(long current, long maximum, string message = "");

    void ReportIndeterminate(string message = "");
}

public interface IAegisubMacro
{
    MacroMetadata Metadata { get; }

    ValueTask<MacroResult> ExecuteAsync(
        IAegisubMacroContext context,
        CancellationToken cancellationToken);
}

public interface IAegisubAutomationModule
{
    ExtensionMetadata Metadata { get; }
    IReadOnlyList<IAegisubMacro> Macros { get; }
}
