using System.Reflection;
using System.Text.Encodings.Web;
using System.Text.Json;
using Aegisub.Managed.Contracts;

namespace Aegisub.Managed.DevHost;

internal sealed record MacroInvocationRequest(
    long InvocationToken,
    SubtitleDocumentSnapshot? Subtitles);

internal sealed class DevMacroContext(SubtitleDocumentSnapshot? subtitles) :
    IAegisubMacroContext,
    IAegisubPluginContext
{
    public SubtitleDocumentSnapshot? Subtitles { get; } = subtitles;

    public void Log(string message)
    {
        ArgumentNullException.ThrowIfNull(message);
        Console.Error.WriteLine($"log={message}");
    }

    public void ReportProgress(long current, long maximum, string message = "")
    {
        ArgumentNullException.ThrowIfNull(message);
        if (current < 0 || maximum <= 0 || current > maximum)
            throw new InvalidOperationException(
                "C# Macro progress values must be non-negative, within a positive maximum.");
        Console.Error.WriteLine($"progress={current}/{maximum} {message}");
    }

    public void ReportIndeterminate(string message = "")
    {
        ArgumentNullException.ThrowIfNull(message);
        Console.Error.WriteLine($"progress=indeterminate {message}");
    }

    public ValueTask<string> InvokeHostServiceAsync(
        string serviceId,
        string requestJson,
        CancellationToken cancellationToken)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(serviceId);
        ArgumentException.ThrowIfNullOrWhiteSpace(requestJson);
        cancellationToken.ThrowIfCancellationRequested();
        if (!string.Equals(serviceId, "aegisub.bridge.echo", StringComparison.Ordinal))
            throw new InvalidOperationException(
                $"The standalone DevHost does not provide service '{serviceId}'.");
        return ValueTask.FromResult(requestJson);
    }

    public void EmitEvent(string eventId, string payloadJson)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(eventId);
        ArgumentException.ThrowIfNullOrWhiteSpace(payloadJson);
        Console.Error.WriteLine($"plugin_event={eventId} {payloadJson}");
    }
}

internal sealed record DevHostOptions(
    string AssemblyPath,
    string EntryType,
    string MacroId,
    string ContextPath,
    string? ResultPath)
{
    public static DevHostOptions Parse(string[] args)
    {
        string assembly = Path.Combine(
            AppContext.BaseDirectory, "Aegisub.Managed.SampleExtension.dll");
        string entryType = "Aegisub.Managed.SampleExtension.SampleExtensionModule";
        string macro = "aegisub.plugin-bridge.demo.trim-selected-line-endings";
        string context = Path.Combine(
            AppContext.BaseDirectory, "fixtures", "trim-selected-context.json");
        string? result = null;

        for (int index = 0; index < args.Length; ++index)
        {
            string option = args[index];
            if (option is "--help" or "-h")
            {
                PrintUsage();
                Environment.Exit(0);
            }
            if (index + 1 >= args.Length)
                throw new ArgumentException($"Missing value after '{option}'.");
            string value = args[++index];
            switch (option)
            {
                case "--assembly": assembly = value; break;
                case "--type": entryType = value; break;
                case "--macro": macro = value; break;
                case "--context": context = value; break;
                case "--result": result = value; break;
                default: throw new ArgumentException($"Unknown option '{option}'.");
            }
        }

        return new(
            Path.GetFullPath(assembly),
            entryType,
            macro,
            Path.GetFullPath(context),
            result is null ? null : Path.GetFullPath(result));
    }

    private static void PrintUsage() => Console.WriteLine(
        "Aegisub.Managed.DevHost [--assembly path] [--type name] " +
        "[--macro id] [--context context.json] [--result result.json]");
}

internal static class Program
{
    private enum DevHostPhase
    {
        Host,
        Invocation
    }

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        PropertyNameCaseInsensitive = true,
        RespectNullableAnnotations = true,
        RespectRequiredConstructorParameters = true,
        WriteIndented = true
    };

    public static async Task<int> Main(string[] args)
    {
        DevHostPhase phase = DevHostPhase.Host;
        try
        {
            DevHostOptions options = DevHostOptions.Parse(args);
            MacroInvocationRequest request = JsonSerializer.Deserialize<MacroInvocationRequest>(
                await File.ReadAllTextAsync(options.ContextPath).ConfigureAwait(false),
                JsonOptions)
                ?? throw new InvalidOperationException("The debug context file is empty.");
            if (request.InvocationToken <= 0)
                throw new InvalidOperationException("The debug invocation token must be positive.");
            if (request.Subtitles?.DocumentToken < 0)
                throw new InvalidOperationException(
                    "The subtitle document revision cannot be negative.");

            DevHostExtensionLoadContext loadContext = new(options.AssemblyPath);
            Assembly assembly = loadContext.LoadFromAssemblyPath(options.AssemblyPath);
            Type moduleType = assembly.GetType(
                options.EntryType,
                throwOnError: true,
                ignoreCase: false)!;
            object entryPoint = Activator.CreateInstance(moduleType)
                ?? throw new InvalidOperationException(
                    $"Entry type '{options.EntryType}' could not be instantiated.");
            IAegisubPluginLifecycle? lifecycle = entryPoint as IAegisubPluginLifecycle;
            string pluginId;
            IReadOnlyList<IReadOnlyList<IAegisubMacro>> automationMacroLists;
            if (entryPoint is IAegisubPlugin plugin)
            {
                PluginMetadata pluginMetadata = plugin.Metadata
                    ?? throw new InvalidOperationException("The plugin returned null metadata.");
                pluginId = pluginMetadata.Id;
                IReadOnlyList<IAegisubContribution> contributions = plugin.Contributions
                    ?? throw new InvalidOperationException("The plugin returned a null contribution list.");
                List<IReadOnlyList<IAegisubMacro>> lists = [];
                HashSet<string> contributionIds = new(StringComparer.Ordinal);
                foreach (IAegisubContribution? candidate in contributions)
                {
                    IAegisubContribution contribution = candidate
                        ?? throw new InvalidOperationException("The plugin returned a null contribution.");
                    ContributionMetadata metadata = contribution.Metadata
                        ?? throw new InvalidOperationException(
                            "A plugin contribution returned null metadata.");
                    if (string.IsNullOrWhiteSpace(metadata.Id))
                        throw new InvalidOperationException("A plugin contribution has an empty ID.");
                    if (!Enum.IsDefined(metadata.Kind))
                        throw new InvalidOperationException(
                            $"Plugin contribution '{metadata.Id}' has an unknown kind value.");
                    if (!contributionIds.Add(metadata.Id))
                        throw new InvalidOperationException(
                            $"Duplicate plugin contribution ID '{metadata.Id}'.");
                    if (contribution is IAegisubAutomationContribution automation)
                    {
                        if (metadata.Kind != ContributionKind.Automation)
                            throw new InvalidOperationException(
                                $"Automation contribution '{metadata.Id}' declares kind '{metadata.Kind}'.");
                        lists.Add(automation.Macros
                            ?? throw new InvalidOperationException(
                                $"Automation contribution '{metadata.Id}' returned a null Macro list."));
                    }
                    else if (metadata.Kind == ContributionKind.Automation)
                    {
                        throw new InvalidOperationException(
                            $"Contribution '{metadata.Id}' declares Automation but does not implement " +
                            $"{nameof(IAegisubAutomationContribution)}.");
                    }
                }
                automationMacroLists = lists;
            }
            else if (entryPoint is IAegisubAutomationModule legacyModule)
            {
                ExtensionMetadata extensionMetadata = legacyModule.Metadata
                    ?? throw new InvalidOperationException(
                        "The legacy Automation module returned null metadata.");
                pluginId = extensionMetadata.Id;
                automationMacroLists = [legacyModule.Macros
                    ?? throw new InvalidOperationException(
                        "The legacy Automation module returned a null Macro list.")];
                lifecycle = null;
            }
            else
            {
                throw new InvalidOperationException(
                    $"Entry type '{options.EntryType}' is neither an Aegisub plugin nor a legacy " +
                    "Automation module.");
            }
            if (string.IsNullOrWhiteSpace(pluginId))
                throw new InvalidOperationException("The plugin has an empty ID.");

            IAegisubMacro? macro = null;
            HashSet<string> macroIds = new(StringComparer.Ordinal);
            foreach (IReadOnlyList<IAegisubMacro> registeredMacros in automationMacroLists)
            {
                foreach (IAegisubMacro? candidate in registeredMacros)
                {
                    IAegisubMacro current = candidate
                        ?? throw new InvalidOperationException("The extension returned a null Macro.");
                    MacroMetadata metadata = current.Metadata
                        ?? throw new InvalidOperationException("An extension Macro returned null metadata.");
                    if (string.IsNullOrWhiteSpace(metadata.Id))
                        throw new InvalidOperationException("An extension Macro has an empty ID.");
                    if (metadata.QueryState?.MinimumSelectedEvents < 0)
                        throw new InvalidOperationException(
                            $"Extension Macro '{metadata.Id}' has a negative minimum selection count.");
                    if (metadata.SubtitleAccess == SubtitleAccess.None &&
                        metadata.SnapshotScope != SubtitleSnapshotScope.Full)
                        throw new InvalidOperationException(
                            $"Extension Macro '{metadata.Id}' cannot request a subtitle snapshot scope " +
                            "when subtitle access is none.");
                    if (!macroIds.Add(metadata.Id))
                        throw new InvalidOperationException(
                            $"Duplicate extension Macro ID '{metadata.Id}'.");
                    if (string.Equals(metadata.Id, options.MacroId, StringComparison.Ordinal))
                        macro = current;
                }
            }
            if (macro is null)
                throw new InvalidOperationException(
                    $"Plugin '{pluginId}' does not define Macro '{options.MacroId}'.");

            using CancellationTokenSource cancellation = new();
            ConsoleCancelEventHandler cancelHandler = (_, eventArgs) =>
            {
                eventArgs.Cancel = true;
                cancellation.Cancel();
            };
            Console.CancelKeyPress += cancelHandler;
            DevMacroContext context = new(request.Subtitles);
            try
            {
                if (lifecycle is not null)
                    await lifecycle.ActivateAsync(context, cancellation.Token).ConfigureAwait(false);
                try
                {
                    phase = DevHostPhase.Invocation;
                    MacroResult result = await macro.ExecuteAsync(
                        context,
                        cancellation.Token).ConfigureAwait(false)
                        ?? throw new InvalidOperationException("The C# Macro returned a null result.");
                    ValidateMacroResult(result);
                    phase = DevHostPhase.Host;
                    string resultJson = JsonSerializer.Serialize(result, JsonOptions);
                    Console.WriteLine(resultJson);
                    if (options.ResultPath is not null)
                    {
                        string? directory = Path.GetDirectoryName(options.ResultPath);
                        if (!string.IsNullOrEmpty(directory))
                            Directory.CreateDirectory(directory);
                        await File.WriteAllTextAsync(
                            options.ResultPath,
                            resultJson,
                            cancellation.Token).ConfigureAwait(false);
                    }
                }
                finally
                {
                    phase = DevHostPhase.Host;
                    if (lifecycle is not null)
                        await lifecycle.DeactivateAsync(CancellationToken.None).ConfigureAwait(false);
                }
            }
            finally
            {
                Console.CancelKeyPress -= cancelHandler;
            }
            return 0;
        }
        catch (OperationCanceledException)
        {
            Console.Error.WriteLine("Macro execution cancelled.");
            return 130;
        }
        catch (AutomationFailureException error)
        {
            WriteErrorEnvelope(error.Code, "extension", error.IsRetryable, error);
            return 2;
        }
        catch (Exception error)
        {
            WriteErrorEnvelope(
                phase == DevHostPhase.Invocation
                    ? "extension.unhandled_exception"
                    : "host.devhost_failed",
                phase == DevHostPhase.Invocation ? "extension" : "host",
                retryable: false,
                error);
            return 2;
        }
    }

    private static void WriteErrorEnvelope(
        string code,
        string category,
        bool retryable,
        Exception error) => Console.Error.WriteLine(JsonSerializer.Serialize(
            new
            {
                schemaVersion = 1,
                code,
                category,
                message = error.Message,
                details = error.ToString(),
                exceptionType = error.GetType().FullName ?? error.GetType().Name,
                retryable
            },
            JsonOptions));

    private static void ValidateMacroResult(MacroResult result)
    {
        if (result.StatusMessage is null)
            throw new InvalidOperationException("The C# Macro returned a null status message.");
        if (result.Mutation is not { } mutation)
            return;
        if (string.IsNullOrEmpty(mutation.UndoDescription))
            throw new InvalidOperationException(
                "A C# subtitle mutation requires a non-empty undo description.");
        if (mutation.EventPatches is null)
            throw new InvalidOperationException(
                "A C# subtitle mutation returned a null event patch list.");
        foreach (SubtitleEventPatch? patch in mutation.EventPatches)
        {
            if (patch is null)
                throw new InvalidOperationException(
                    "A C# subtitle mutation returned a null event patch.");
        }
    }
}
