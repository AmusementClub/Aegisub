using System.Diagnostics;
using System.Diagnostics.CodeAnalysis;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Text.Encodings.Web;
using System.Text.Json;
using Aegisub.Managed.Contracts;

namespace Aegisub.CoreClr.Adapter;

internal sealed record BridgeErrorEnvelope(
    int SchemaVersion,
    string Code,
    string Category,
    string Message,
    string Details,
    string ExceptionType,
    bool Retryable);

internal sealed class InvalidPluginHandleException(ulong handle)
    : KeyNotFoundException($"Unknown CLR plugin handle {handle}.");

internal sealed class ContributionNotFoundException(string contributionId)
    : KeyNotFoundException($"The plugin does not define contribution '{contributionId}'.");

internal sealed class ContributionOperationNotFoundException(
    string contributionId,
    string operationId)
    : KeyNotFoundException(
        $"Plugin contribution '{contributionId}' does not define operation '{operationId}'.");

internal sealed class PluginEventHandlerNotFoundException()
    : KeyNotFoundException("The plugin does not implement an event handler.");

internal sealed class HostContractException(string message, Exception? innerException = null)
    : InvalidOperationException(message, innerException);

internal sealed class ExtensionContractException(string message, Exception? innerException = null)
    : InvalidOperationException(message, innerException);

internal sealed class PluginManager
{
    private static readonly TimeSpan LegacyShadowRootRetention = TimeSpan.FromDays(30);
    private static readonly BridgeJsonContext InvocationJson = new(CreateJsonOptions());

    private static JsonSerializerOptions CreateJsonOptions() => new()
    {
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        PropertyNameCaseInsensitive = true,
        RespectNullableAnnotations = true,
        RespectRequiredConstructorParameters = true
    };

    private static void ValidateHostJsonPayload(string value, string description)
    {
        try
        {
            using JsonDocument document = JsonDocument.Parse(value);
        }
        catch (JsonException error)
        {
            throw new HostContractException(
                $"The native host supplied invalid {description} JSON.",
                error);
        }
    }

    private static void ValidatePluginJsonPayload(string value, string description)
    {
        try
        {
            using JsonDocument document = JsonDocument.Parse(value);
        }
        catch (JsonException error)
        {
            throw new ExtensionContractException(
                $"The CLR plugin supplied invalid {description} JSON.",
                error);
        }
    }

    private sealed class MacroContext(
        long invocationToken,
        Action<string> log,
        Action<long, long, long, string>? reportProgress,
        SubtitleDocumentSnapshot? subtitles) : IAegisubMacroContext
    {
        public SubtitleDocumentSnapshot? Subtitles { get; } = subtitles;

        public void Log(string message)
        {
            if (message is null)
                throw new ExtensionContractException("C# Macro log messages cannot be null.");
            log(message);
        }

        public void ReportProgress(long current, long maximum, string message = "")
        {
            if (message is null)
                throw new ExtensionContractException("C# Macro progress messages cannot be null.");
            if (current < 0 || maximum <= 0)
                throw new ExtensionContractException("C# Macro progress values must be non-negative with a positive maximum.");
            if (current > maximum)
                throw new ExtensionContractException("C# Macro progress cannot exceed its maximum.");
            reportProgress?.Invoke(invocationToken, current, maximum, message);
        }

        public void ReportIndeterminate(string message = "")
        {
            if (message is null)
                throw new ExtensionContractException("C# Macro progress messages cannot be null.");
            reportProgress?.Invoke(invocationToken, 0, 0, message);
        }
    }

    private sealed class PluginContext(
        ulong pluginHandle,
        Action<string> log,
        Func<long> getCurrentInvocationToken,
        Func<ulong, long, string, string, string>? invokeHostService,
        Action<ulong, string, string>? emitPluginEvent) : IAegisubPluginContext
    {
        public void Log(string message)
        {
            if (message is null)
                throw new ExtensionContractException("C# plugin log messages cannot be null.");
            log(message);
        }

        public ValueTask<string> InvokeHostServiceAsync(
            string serviceId,
            string requestJson,
            CancellationToken cancellationToken)
        {
            ArgumentException.ThrowIfNullOrWhiteSpace(serviceId);
            ArgumentException.ThrowIfNullOrWhiteSpace(requestJson);
            ValidatePluginJsonPayload(requestJson, "host-service request");
            cancellationToken.ThrowIfCancellationRequested();
            if (invokeHostService is null)
                throw new InvalidOperationException(
                    "The native host does not expose the plugin host-service Bridge.");
            string result = invokeHostService(
                pluginHandle,
                getCurrentInvocationToken(),
                serviceId,
                requestJson);
            ValidateHostJsonPayload(result, "host-service response");
            cancellationToken.ThrowIfCancellationRequested();
            return ValueTask.FromResult(result);
        }

        public void EmitEvent(string eventId, string payloadJson)
        {
            ArgumentException.ThrowIfNullOrWhiteSpace(eventId);
            ArgumentException.ThrowIfNullOrWhiteSpace(payloadJson);
            ValidatePluginJsonPayload(payloadJson, "event payload");
            if (emitPluginEvent is null)
                throw new InvalidOperationException(
                    "The native host does not expose the plugin-event Bridge.");
            emitPluginEvent(pluginHandle, eventId, payloadJson);
        }
    }

    private sealed class LegacyAutomationContribution(
        ExtensionMetadata extension,
        IReadOnlyList<IAegisubMacro> macros) : IAegisubAutomationContribution
    {
        public ContributionMetadata Metadata { get; } = new(
            Id: $"{extension.Id}.automation",
            Kind: ContributionKind.Automation,
            Name: extension.Name,
            Description: extension.Description,
            NameResourceKey: extension.NameResourceKey,
            DescriptionResourceKey: extension.DescriptionResourceKey);

        public IReadOnlyList<IAegisubMacro> Macros { get; } = macros;
    }

    private sealed record ResolvedEntryPoint(
        object Instance,
        PluginMetadata Metadata,
        IReadOnlyList<IAegisubContribution> Contributions,
        IAegisubPluginLifecycle? Lifecycle,
        string EntryModel);

    private sealed record ValidatedContribution(
        IAegisubContribution Instance,
        ContributionMetadata Metadata,
        IReadOnlyList<IAegisubMacro> Macros,
        IReadOnlyList<string> Operations);

    internal sealed record MetadataQueryState(
        bool RequiresSubtitleFile,
        int MinimumSelectedEvents,
        bool RequiresActiveEvent,
        bool RequiresVideo,
        bool RequiresAudio,
        bool RequiresKeyframes);

    internal sealed record MetadataMacro(
        string Id,
        string Name,
        string Description,
        string NameResourceKey,
        string DescriptionResourceKey,
        bool RequiresProject,
        string SubtitleAccess,
        string SnapshotScope,
        MetadataQueryState QueryState);

    internal sealed record MetadataContribution(
        string Id,
        string Kind,
        string Name,
        string Description,
        string NameResourceKey,
        string DescriptionResourceKey,
        IReadOnlyList<string> Operations,
        IReadOnlyList<MetadataMacro> Macros);

    internal sealed record MetadataDocument(
        string Id,
        string Name,
        string Description,
        string NameResourceKey,
        string DescriptionResourceKey,
        string Author,
        string Version,
        string ContractsVersion,
        string EntryModel,
        IReadOnlyList<MetadataContribution> Contributions);

    private interface IContributionDispatcher
    {
        bool SupportsOperation(string operationId);
        string Invoke(string operationId, string requestJson);
    }

    private sealed class AutomationContributionDispatcher(
        IReadOnlyList<IAegisubMacro> macros,
        Func<IAegisubMacro, string, string> invokeMacro) : IContributionDispatcher
    {
        private readonly Dictionary<string, IAegisubMacro> _macros = macros.ToDictionary(
            macro => macro.Metadata.Id,
            StringComparer.Ordinal);

        public bool SupportsOperation(string operationId) => _macros.ContainsKey(operationId);

        public string Invoke(string operationId, string requestJson)
        {
            if (!_macros.TryGetValue(operationId, out IAegisubMacro? macro))
                throw new ContributionOperationNotFoundException("automation", operationId);
            return invokeMacro(macro, requestJson);
        }
    }

    private sealed class ServiceProviderContributionDispatcher(
        IAegisubServiceProviderContribution serviceProvider,
        IReadOnlyList<string> operations) : IContributionDispatcher
    {
        private readonly HashSet<string> _operations = new(
            operations,
            StringComparer.Ordinal);

        public bool SupportsOperation(string operationId) => _operations.Contains(operationId);

        public string Invoke(string operationId, string requestJson)
        {
            if (!_operations.Contains(operationId))
                throw new ContributionOperationNotFoundException(
                    serviceProvider.Metadata.Id,
                    operationId);
            ValueTask<string> invocation = serviceProvider.InvokeAsync(
                operationId,
                requestJson,
                CancellationToken.None);
            return invocation.IsCompleted
                ? invocation.GetAwaiter().GetResult()
                : invocation.AsTask().GetAwaiter().GetResult();
        }
    }

    private sealed record LoadedContribution(
        ValidatedContribution Contract,
        IContributionDispatcher? Dispatcher);

    private sealed class LoadedPlugin(
        PluginLoadContext? loadContext,
        object entryPoint,
        IAegisubPluginLifecycle? lifecycle,
        IAegisubPluginEventHandler? eventHandler,
        Dictionary<string, LoadedContribution> contributions,
        Dictionary<string, LoadedContribution> operations,
        string metadataJson,
        string shadowDirectory)
    {
        private PluginLoadContext? _loadContext = loadContext;
        private object? _entryPoint = entryPoint;
        private IAegisubPluginLifecycle? _lifecycle = lifecycle;
        private IAegisubPluginEventHandler? _eventHandler = eventHandler;
        private Dictionary<string, LoadedContribution>? _contributions = contributions;
        private Dictionary<string, LoadedContribution>? _operations = operations;
        private readonly object _executionGate = new();
        private bool _activationAttempted;
        private bool _active;

        public string MetadataJson { get; } = metadataJson;
        public string ShadowDirectory { get; } = shadowDirectory;

        public void Activate(IAegisubPluginContext context)
        {
            lock (_executionGate)
            {
                if (_active || _activationAttempted)
                    throw new InvalidOperationException("The plugin activation state is invalid.");
                _activationAttempted = true;
                if (_lifecycle is not null)
                {
                    ValueTask activation = _lifecycle.ActivateAsync(context, CancellationToken.None);
                    if (activation.IsCompleted)
                        activation.GetAwaiter().GetResult();
                    else
                        activation.AsTask().GetAwaiter().GetResult();
                }
                _active = true;
            }
        }

        public string InvokeContribution(
            string contributionId,
            string operationId,
            string requestJson)
        {
            lock (_executionGate)
            {
                EnsureActive();
                LoadedContribution contribution;
                if (string.IsNullOrEmpty(contributionId))
                {
                    if (_operations is null ||
                        !_operations.TryGetValue(
                            operationId,
                            out LoadedContribution? foundContribution))
                        throw new ContributionOperationNotFoundException(
                            "<unique-automation-lookup>",
                            operationId);
                    contribution = foundContribution;
                }
                else
                {
                    if (_contributions is null ||
                        !_contributions.TryGetValue(
                            contributionId,
                            out LoadedContribution? foundContribution))
                        throw new ContributionNotFoundException(contributionId);
                    contribution = foundContribution;
                }
                IContributionDispatcher dispatcher = contribution.Dispatcher
                    ?? throw new ContributionOperationNotFoundException(
                        contribution.Contract.Metadata.Id,
                        operationId);
                if (!dispatcher.SupportsOperation(operationId))
                    throw new ContributionOperationNotFoundException(
                        contribution.Contract.Metadata.Id,
                        operationId);
                return dispatcher.Invoke(operationId, requestJson);
            }
        }

        public void DispatchEvent(PluginEvent pluginEvent)
        {
            lock (_executionGate)
            {
                EnsureActive();
                IAegisubPluginEventHandler handler = _eventHandler
                    ?? throw new PluginEventHandlerNotFoundException();
                ValueTask dispatch = handler.HandleEventAsync(pluginEvent, CancellationToken.None);
                if (dispatch.IsCompleted)
                    dispatch.GetAwaiter().GetResult();
                else
                    dispatch.AsTask().GetAwaiter().GetResult();
            }
        }

        public void Deactivate()
        {
            lock (_executionGate)
            {
                _active = false;
                IAegisubPluginLifecycle? lifecycle = _lifecycle;
                _lifecycle = null;
                if (lifecycle is null || !_activationAttempted)
                    return;
                ValueTask deactivation = lifecycle.DeactivateAsync(CancellationToken.None);
                if (deactivation.IsCompleted)
                    deactivation.GetAwaiter().GetResult();
                else
                    deactivation.AsTask().GetAwaiter().GetResult();
            }
        }

        public PluginLoadContext? ReleaseReferences()
        {
            lock (_executionGate)
            {
                PluginLoadContext? context = _loadContext;
                _active = false;
                _contributions = null;
                _operations = null;
                _eventHandler = null;
                _lifecycle = null;
                _entryPoint = null;
                _loadContext = null;
                return context;
            }
        }

        private void EnsureActive()
        {
            if (!_active)
                throw new InvalidOperationException("The plugin is not active.");
        }
    }

    private sealed record PendingUnload(
        ulong Handle,
        WeakReference LoadContextReference,
        string ShadowDirectory);

    private readonly object _gate = new();
    private readonly AsyncLocal<long> _currentInvocationToken = new();
    private readonly Dictionary<ulong, LoadedPlugin> _extensions = [];
    private readonly HashSet<ulong> _reservedHandles = [];
    private readonly List<PendingUnload> _pendingUnloads = [];
    private readonly HashSet<string> _pendingShadowCleanup = new(StringComparer.OrdinalIgnoreCase);
    private readonly Action<string> _log;
    private readonly Func<long, bool>? _isCancellationRequested;
    private readonly Action<long, long, long, string>? _reportProgress;
    private readonly Func<ulong, long, string, string, string>? _invokeHostService;
    private readonly Action<ulong, string, string>? _emitPluginEvent;
    private readonly string _shadowBaseDirectory;
    private readonly string _processShadowRoot;
    private ulong _nextHandle = 1;
    private int _debuggerWaitCompleted;
    private bool _shutdownRequested;

    public PluginManager(
        Action<string> log,
        Func<long, bool>? isCancellationRequested,
        Action<long, long, long, string>? reportProgress,
        Func<ulong, long, string, string, string>? invokeHostService,
        Action<ulong, string, string>? emitPluginEvent)
    {
        _log = log;
        _isCancellationRequested = isCancellationRequested;
        _reportProgress = reportProgress;
        _invokeHostService = invokeHostService;
        _emitPluginEvent = emitPluginEvent;
        _shadowBaseDirectory = Path.Combine(
            Path.GetTempPath(),
            "Aegisub",
            "PluginBridge");
        _processShadowRoot = Path.Combine(
            _shadowBaseDirectory,
            $"{Environment.ProcessId}-{GetCurrentProcessStartTicks()}");
        ScavengeStaleShadowRoots();
    }

    private LoadedPlugin CreateLoadedPlugin(
        ResolvedEntryPoint resolved,
        PluginLoadContext? loadContext,
        string shadowDirectory)
    {
        ValidatePluginMetadata(resolved.Metadata);
        IReadOnlyList<ValidatedContribution> contributions =
            ValidateContributions(resolved.Contributions);
        Dictionary<string, LoadedContribution> contributionsById =
            new(StringComparer.Ordinal);
        Dictionary<string, LoadedContribution> operations =
            new(StringComparer.Ordinal);
        foreach (ValidatedContribution contribution in contributions)
        {
            IContributionDispatcher? dispatcher = contribution.Metadata.Kind switch
            {
                ContributionKind.Automation => new AutomationContributionDispatcher(
                    contribution.Macros,
                    InvokeAutomationMacro),
                ContributionKind.ServiceProvider => new ServiceProviderContributionDispatcher(
                    (IAegisubServiceProviderContribution)contribution.Instance,
                    contribution.Operations),
                _ => null
            };
            LoadedContribution loadedContribution = new(contribution, dispatcher);
            contributionsById.Add(contribution.Metadata.Id, loadedContribution);
            foreach (IAegisubMacro macro in contribution.Macros)
                operations.Add(macro.Metadata.Id, loadedContribution);
            foreach (string operation in contribution.Operations)
                operations.Add(operation, loadedContribution);
        }

        return new LoadedPlugin(
            loadContext,
            resolved.Instance,
            resolved.Lifecycle,
            resolved.Instance as IAegisubPluginEventHandler,
            contributionsById,
            operations,
            SerializeMetadata(resolved.Metadata, resolved.EntryModel, contributions),
            shadowDirectory);
    }

    [RequiresUnreferencedCode(
        "CoreCLR plugin loading uses runtime assembly discovery and is not a NativeAOT path.")]
    public ulong Load(string entryAssemblyPath, string entryType)
    {
        string sourceAssembly = Path.GetFullPath(entryAssemblyPath);
        if (!File.Exists(sourceAssembly))
            throw new FileNotFoundException("The C# extension entry assembly was not found.", sourceAssembly);
        WaitForDebuggerIfRequested(sourceAssembly);

        string shadowDirectory = CreateShadowCopy(Path.GetDirectoryName(sourceAssembly)!);
        string shadowAssembly = Path.Combine(shadowDirectory, Path.GetFileName(sourceAssembly));
        PluginLoadContext? loadContext = null;
        LoadedPlugin? loaded = null;
        ulong reservedHandle = 0;
        bool registered = false;

        try
        {
            loadContext = new PluginLoadContext(shadowAssembly);
            Assembly assembly = loadContext.LoadFromAssemblyPath(shadowAssembly);
            Type moduleType = assembly.GetType(entryType, throwOnError: true, ignoreCase: false)!;
            object instance = Activator.CreateInstance(moduleType)
                ?? throw new ExtensionContractException(
                    $"Entry type '{entryType}' could not be instantiated.");
            ResolvedEntryPoint resolved = ResolveEntryPoint(instance, entryType);
            lock (_gate)
            {
                if (_shutdownRequested)
                    throw new InvalidOperationException("The CLR plugin manager is shutting down.");
                TryCleanupPendingLocked();
                reservedHandle = NextHandleLocked();
                _reservedHandles.Add(reservedHandle);
            }
            loaded = CreateLoadedPlugin(resolved, loadContext, shadowDirectory);
            loadContext = null;
            loaded.Activate(new PluginContext(
                reservedHandle,
                _log,
                () => _currentInvocationToken.Value,
                _invokeHostService,
                _emitPluginEvent));

            lock (_gate)
            {
                if (_shutdownRequested)
                    throw new InvalidOperationException("The CLR plugin manager is shutting down.");
                _reservedHandles.Remove(reservedHandle);
                _extensions.Add(reservedHandle, loaded);
                registered = true;
            }
            _log(
                $"Loaded CLR plugin '{resolved.Metadata.Id}' as handle {reservedHandle} from a shadow copy");
            return reservedHandle;
        }
        catch
        {
            if (reservedHandle != 0)
            {
                lock (_gate)
                {
                    _reservedHandles.Remove(reservedHandle);
                    if (registered)
                        _extensions.Remove(reservedHandle);
                }
            }
            if (loaded is not null)
            {
                try
                {
                    loaded.Deactivate();
                }
                catch (Exception error)
                {
                    _log($"CLR plugin deactivation after a failed load also failed: {error.Message}");
                }
                PendingUnload? pending = BeginUnload(loaded, 0);
                if (pending is not null)
                {
                    lock (_gate)
                    {
                        _pendingUnloads.Add(pending);
                        TryCleanupPendingLocked();
                    }
                    _log("Failed CLR plugin load requested collectible ALC cleanup");
                }
            }
            else if (loadContext is not null)
            {
                PendingUnload pending = BeginUnload(loadContext, 0, shadowDirectory);
                loadContext = null;
                lock (_gate)
                {
                    _pendingUnloads.Add(pending);
                    TryCleanupPendingLocked();
                }
                _log("Failed CLR plugin load requested collectible ALC cleanup");
            }
            else
            {
                TryDeleteOrQueue(shadowDirectory);
            }
            throw;
        }
    }

    public void LoadStatic(ulong handle, IAegisubPlugin plugin)
    {
        if (handle == 0)
            throw new ArgumentOutOfRangeException(nameof(handle));
        ArgumentNullException.ThrowIfNull(plugin);

        ResolvedEntryPoint resolved = ResolveEntryPoint(plugin, plugin.Metadata.Id);
        LoadedPlugin loaded = CreateLoadedPlugin(resolved, null, string.Empty);
        try
        {
            lock (_gate)
            {
                if (_shutdownRequested)
                    throw new InvalidOperationException("The AOT plugin manager is shutting down.");
                if (_extensions.ContainsKey(handle) || _reservedHandles.Contains(handle))
                    throw new InvalidOperationException(
                        $"The native AOT plugin handle {handle} is already registered.");
                _reservedHandles.Add(handle);
            }
            loaded.Activate(new PluginContext(
                handle,
                _log,
                () => _currentInvocationToken.Value,
                _invokeHostService,
                _emitPluginEvent));
            lock (_gate)
            {
                if (_shutdownRequested)
                    throw new InvalidOperationException("The AOT plugin manager is shutting down.");
                _reservedHandles.Remove(handle);
                _extensions.Add(handle, loaded);
            }
            _log($"Loaded NativeAOT plugin '{resolved.Metadata.Id}' as handle {handle}");
        }
        catch
        {
            lock (_gate)
                _reservedHandles.Remove(handle);
            try
            {
                loaded.Deactivate();
            }
            catch (Exception error)
            {
                _log($"NativeAOT plugin deactivation after a failed load also failed: {error.Message}");
            }
            loaded.ReleaseReferences();
            throw;
        }
    }

    public string GetMetadata(ulong handle)
    {
        lock (_gate)
            return GetLoadedLocked(handle).MetadataJson;
    }

    private static ResolvedEntryPoint ResolveEntryPoint(object instance, string entryType)
    {
        if (instance is IAegisubPlugin plugin)
        {
            PluginMetadata metadata = plugin.Metadata
                ?? throw new ExtensionContractException("The C# plugin returned null metadata.");
            IReadOnlyList<IAegisubContribution> contributions = plugin.Contributions
                ?? throw new ExtensionContractException("The C# plugin returned a null contribution list.");
            return new(
                instance,
                metadata,
                contributions,
                instance as IAegisubPluginLifecycle,
                "plugin");
        }

        if (instance is IAegisubAutomationModule module)
        {
            ExtensionMetadata extension = module.Metadata
                ?? throw new ExtensionContractException("The legacy C# Automation module returned null metadata.");
            IReadOnlyList<IAegisubMacro> macros = module.Macros
                ?? throw new ExtensionContractException("The legacy C# Automation module returned a null Macro list.");
            PluginMetadata metadata = new(
                extension.Id,
                extension.Name,
                extension.Description,
                extension.Author,
                extension.Version,
                extension.NameResourceKey,
                extension.DescriptionResourceKey);
            IAegisubContribution[] contributions = [new LegacyAutomationContribution(extension, macros)];
            return new(instance, metadata, contributions, null, "legacyAutomationModule");
        }

        throw new ExtensionContractException(
            $"Entry type '{entryType}' implements neither {nameof(IAegisubPlugin)} nor " +
            $"the legacy {nameof(IAegisubAutomationModule)} contract.");
    }

    private static void ValidatePluginMetadata(PluginMetadata metadata)
    {
        if (string.IsNullOrWhiteSpace(metadata.Id) || string.IsNullOrWhiteSpace(metadata.Name))
            throw new ExtensionContractException(
                "An Aegisub plugin requires non-empty ID and fallback name metadata.");
        if (metadata.Description is null || metadata.Author is null || metadata.Version is null ||
            metadata.NameResourceKey is null || metadata.DescriptionResourceKey is null)
            throw new ExtensionContractException(
                "An Aegisub plugin returned null text or resource-key metadata.");
    }

    private static IReadOnlyList<ValidatedContribution> ValidateContributions(
        IReadOnlyList<IAegisubContribution> registeredContributions)
    {
        List<ValidatedContribution> contributions = new(registeredContributions.Count);
        HashSet<string> contributionIds = new(StringComparer.Ordinal);
        HashSet<string> operationIds = new(StringComparer.Ordinal);
        foreach (IAegisubContribution? candidate in registeredContributions)
        {
            IAegisubContribution contribution = candidate
                ?? throw new ExtensionContractException("A C# plugin returned a null contribution.");
            ContributionMetadata metadata = contribution.Metadata
                ?? throw new ExtensionContractException("A C# plugin contribution returned null metadata.");
            if (string.IsNullOrWhiteSpace(metadata.Id))
                throw new ExtensionContractException("A C# plugin contribution has an empty ID.");
            if (!Enum.IsDefined(metadata.Kind))
                throw new ExtensionContractException(
                    $"C# plugin contribution '{metadata.Id}' has an unknown kind value.");
            if (metadata.Name is null || metadata.Description is null ||
                metadata.NameResourceKey is null || metadata.DescriptionResourceKey is null)
                throw new ExtensionContractException(
                    $"C# plugin contribution '{metadata.Id}' returned null text or resource-key metadata.");
            if (!contributionIds.Add(metadata.Id))
                throw new ExtensionContractException(
                    $"Duplicate C# plugin contribution ID '{metadata.Id}'.");

            bool isAutomation = contribution is IAegisubAutomationContribution;
            bool isServiceProvider = contribution is IAegisubServiceProviderContribution;
            if (isAutomation && isServiceProvider)
                throw new ExtensionContractException(
                    $"C# plugin contribution '{metadata.Id}' implements multiple contribution contracts.");

            if (contribution is IAegisubServiceProviderContribution serviceProvider)
            {
                if (metadata.Kind != ContributionKind.ServiceProvider)
                    throw new ExtensionContractException(
                        $"C# plugin service-provider contribution '{metadata.Id}' declares kind " +
                        $"'{metadata.Kind}'.");
                IReadOnlyList<string> registeredOperations = serviceProvider.Operations
                    ?? throw new ExtensionContractException(
                        $"C# plugin service-provider contribution '{metadata.Id}' returned a null " +
                        "operation list.");
                if (registeredOperations.Count == 0)
                    throw new ExtensionContractException(
                        $"C# plugin service-provider contribution '{metadata.Id}' must declare at " +
                        "least one operation.");
                List<string> operations = new(registeredOperations.Count);
                foreach (string? operation in registeredOperations)
                {
                    if (string.IsNullOrWhiteSpace(operation))
                        throw new ExtensionContractException(
                            $"C# plugin service-provider contribution '{metadata.Id}' has an empty " +
                            "operation ID.");
                    if (!operationIds.Add(operation))
                        throw new ExtensionContractException(
                            $"Duplicate C# plugin operation ID '{operation}'.");
                    operations.Add(operation);
                }
                contributions.Add(new(contribution, metadata, [], operations));
                continue;
            }

            if (contribution is not IAegisubAutomationContribution automation)
            {
                if (metadata.Kind == ContributionKind.Automation)
                    throw new ExtensionContractException(
                        $"C# plugin contribution '{metadata.Id}' declares the Automation kind but does not " +
                        $"implement {nameof(IAegisubAutomationContribution)}.");
                if (metadata.Kind == ContributionKind.ServiceProvider)
                    throw new ExtensionContractException(
                        $"C# plugin contribution '{metadata.Id}' declares the service-provider kind but " +
                        $"does not implement {nameof(IAegisubServiceProviderContribution)}.");
                contributions.Add(new(contribution, metadata, [], []));
                continue;
            }

            if (metadata.Kind != ContributionKind.Automation)
                throw new ExtensionContractException(
                    $"C# plugin Automation contribution '{metadata.Id}' declares kind '{metadata.Kind}'.");
            IReadOnlyList<IAegisubMacro> registeredMacros = automation.Macros
                ?? throw new ExtensionContractException(
                    $"C# plugin Automation contribution '{metadata.Id}' returned a null Macro list.");
            List<IAegisubMacro> macros = new(registeredMacros.Count);
            foreach (IAegisubMacro? macroCandidate in registeredMacros)
            {
                IAegisubMacro macro = macroCandidate
                    ?? throw new ExtensionContractException(
                        $"C# plugin Automation contribution '{metadata.Id}' returned a null Macro.");
                MacroMetadata macroMetadata = macro.Metadata
                    ?? throw new ExtensionContractException("A C# extension Macro returned null metadata.");
                if (string.IsNullOrWhiteSpace(macroMetadata.Id))
                    throw new ExtensionContractException("A C# extension Macro has an empty ID.");
                if (string.IsNullOrWhiteSpace(macroMetadata.Name) || macroMetadata.Description is null ||
                    macroMetadata.NameResourceKey is null || macroMetadata.DescriptionResourceKey is null)
                    throw new ExtensionContractException(
                        $"C# extension Macro '{macroMetadata.Id}' returned invalid text or resource-key metadata.");
                if (macroMetadata.QueryState?.MinimumSelectedEvents < 0)
                    throw new ExtensionContractException(
                        $"C# extension Macro '{macroMetadata.Id}' has a negative minimum selection count.");
                if (macroMetadata.SubtitleAccess == SubtitleAccess.None &&
                    macroMetadata.SnapshotScope != SubtitleSnapshotScope.Full)
                    throw new ExtensionContractException(
                        $"C# extension Macro '{macroMetadata.Id}' cannot request a subtitle snapshot scope " +
                        "when subtitle access is none.");
                if (!operationIds.Add(macroMetadata.Id))
                    throw new ExtensionContractException(
                        $"Duplicate C# plugin operation ID '{macroMetadata.Id}'.");
                macros.Add(macro);
            }
            contributions.Add(new(contribution, metadata, macros, []));
        }
        return contributions;
    }

    public string InvokeContribution(
        ulong handle,
        string contributionId,
        string operationId,
        string requestJson)
    {
        ValidateHostJsonPayload(requestJson, "contribution request");
        LoadedPlugin loaded;
        lock (_gate)
            loaded = GetLoadedLocked(handle);
        string result = loaded.InvokeContribution(
            contributionId,
            operationId,
            requestJson);
        ValidatePluginJsonPayload(result, "contribution result");
        return result;
    }

    public void DispatchEvent(ulong handle, string eventId, string payloadJson)
    {
        ValidateHostJsonPayload(payloadJson, "plugin event payload");
        LoadedPlugin loaded;
        lock (_gate)
            loaded = GetLoadedLocked(handle);
        loaded.DispatchEvent(new PluginEvent(eventId, payloadJson));
    }

    private string InvokeAutomationMacro(IAegisubMacro macro, string contextJson)
    {
        MacroInvocationRequest request;
        try
        {
            request = JsonSerializer.Deserialize(
                contextJson,
                InvocationJson.MacroInvocationRequest)
                ?? throw new HostContractException("The native host supplied an empty Macro context.");
        }
        catch (JsonException error)
        {
            throw new HostContractException("The native host supplied invalid Macro context JSON.", error);
        }
        if (request.InvocationToken <= 0)
            throw new HostContractException("The native host supplied an invalid invocation token.");
        if (request.Subtitles?.DocumentToken < 0)
            throw new HostContractException("The subtitle document revision cannot be negative.");

        using CancellationTokenSource cancellation = new();
        if (_isCancellationRequested?.Invoke(request.InvocationToken) == true)
            cancellation.Cancel();
        using CancellationTokenSource stopPolling = new();
        Task? cancellationPoll = _isCancellationRequested is null
            ? null
            : PollCancellationAsync(
                request.InvocationToken,
                cancellation,
                stopPolling.Token);
        long previousInvocationToken = _currentInvocationToken.Value;
        _currentInvocationToken.Value = request.InvocationToken;
        try
        {
            ValueTask<MacroResult> invocation = macro.ExecuteAsync(
                new MacroContext(
                    request.InvocationToken,
                    _log,
                    _reportProgress,
                    request.Subtitles),
                cancellation.Token);
            MacroResult result = (invocation.IsCompleted
                    ? invocation.GetAwaiter().GetResult()
                    : invocation.AsTask().GetAwaiter().GetResult())
                ?? throw new ExtensionContractException("The C# Macro returned a null result.");
            ValidateMacroResult(result);
            return JsonSerializer.Serialize(result, InvocationJson.MacroResult);
        }
        finally
        {
            _currentInvocationToken.Value = previousInvocationToken;
            stopPolling.Cancel();
            if (cancellationPoll is not null)
            {
                try
                {
                    cancellationPoll.GetAwaiter().GetResult();
                }
                catch (OperationCanceledException)
                {
                }
            }
        }
    }

    private async Task PollCancellationAsync(
        long invocationToken,
        CancellationTokenSource cancellation,
        CancellationToken stopPolling)
    {
        while (!stopPolling.IsCancellationRequested)
        {
            if (_isCancellationRequested?.Invoke(invocationToken) == true)
            {
                cancellation.Cancel();
                return;
            }
            await Task.Delay(25, stopPolling).ConfigureAwait(false);
        }
    }

    private static void ValidateMacroResult(MacroResult result)
    {
        if (result.StatusMessage is null)
            throw new ExtensionContractException("The C# Macro returned a null status message.");
        if (result.Mutation is not { } mutation)
            return;
        if (string.IsNullOrEmpty(mutation.UndoDescription))
            throw new ExtensionContractException(
                "A C# subtitle mutation requires a non-empty undo description.");
        if (mutation.EventPatches is null)
            throw new ExtensionContractException(
                "A C# subtitle mutation returned a null event patch list.");
        foreach (SubtitleEventPatch? patch in mutation.EventPatches)
        {
            if (patch is null)
                throw new ExtensionContractException(
                    "A C# subtitle mutation returned a null event patch.");
        }
    }

    public void Unload(ulong handle)
    {
        LoadedPlugin loaded;
        lock (_gate)
        {
            loaded = GetLoadedLocked(handle);
            _extensions.Remove(handle);
        }

        Exception? deactivationFailure = null;
        try
        {
            loaded.Deactivate();
        }
        catch (Exception error)
        {
            deactivationFailure = error;
        }

        PendingUnload? pending = BeginUnload(loaded, handle);
        if (pending is not null)
        {
            lock (_gate)
            {
                _pendingUnloads.Add(pending);
                TryCleanupPendingLocked();
            }
        }
        _log(pending is null
            ? $"Unloaded NativeAOT plugin handle {handle}; the native module remains resident"
            : $"Unload requested for CLR plugin handle {handle}; ALC cleanup is pending GC");
        if (deactivationFailure is not null)
            throw new ExtensionContractException(
                $"C# plugin deactivation failed during unload: {deactivationFailure.Message}",
                deactivationFailure);
    }

    public void Shutdown()
    {
        List<Exception>? failures = null;
        List<(ulong Handle, LoadedPlugin Plugin)> extensions;
        lock (_gate)
        {
            _shutdownRequested = true;
            extensions = _extensions
                .Select(pair => (pair.Key, pair.Value))
                .ToList();
            _extensions.Clear();
        }

        List<PendingUnload> pendingUnloads = [];
        foreach ((ulong handle, LoadedPlugin extension) in extensions)
        {
            try
            {
                extension.Deactivate();
            }
            catch (Exception error)
            {
                (failures ??= []).Add(new ExtensionContractException(
                    $"C# plugin deactivation failed during shutdown: {error.Message}",
                    error));
            }
            try
            {
                PendingUnload? pending = BeginUnload(extension, handle);
                if (pending is not null)
                    pendingUnloads.Add(pending);
            }
            catch (Exception error)
            {
                (failures ??= []).Add(error);
            }
        }
        lock (_gate)
        {
            _pendingUnloads.AddRange(pendingUnloads);
        }

        CollectPendingUnloadsForShutdown();
        lock (_gate)
        {
            TryCleanupPendingLocked();
            foreach (PendingUnload pending in _pendingUnloads)
            {
                (failures ??= []).Add(new InvalidOperationException(
                    pending.Handle == 0
                        ? "Collectible AssemblyLoadContext for a failed extension load " +
                            "remained alive during Bridge shutdown."
                        : $"Collectible AssemblyLoadContext for extension handle {pending.Handle} " +
                    "remained alive during Bridge shutdown."));
            }
            foreach (string directory in _pendingShadowCleanup)
            {
                (failures ??= []).Add(new IOException(
                    $"Plugin Bridge shadow directory remained after shutdown: {directory}"));
            }
            TryDeleteEmptyDirectory(_processShadowRoot);
        }

        if (failures is not null)
            throw new AggregateException("One or more C# extensions failed to unload.", failures);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static PendingUnload? BeginUnload(LoadedPlugin loaded, ulong handle)
    {
        PluginLoadContext? loadContext = loaded.ReleaseReferences();
        return loadContext is null
            ? null
            : BeginUnload(loadContext, handle, loaded.ShadowDirectory);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static PendingUnload BeginUnload(
        PluginLoadContext loadContext,
        ulong handle,
        string shadowDirectory)
    {
        WeakReference reference = new(loadContext, trackResurrection: false);
        loadContext.Unload();
        return new PendingUnload(handle, reference, shadowDirectory);
    }

    private void CollectPendingUnloadsForShutdown()
    {
        lock (_gate)
        {
            if (_pendingUnloads.Count == 0)
                return;
        }

        // Coalesce every collectible ALC into a single shutdown collection.
        // Normal script reloads rely on natural GC and never block the UI on GC.
        GC.Collect();
        GC.WaitForPendingFinalizers();
        GC.Collect();
    }

    private LoadedPlugin GetLoadedLocked(ulong handle)
    {
        if (!_extensions.TryGetValue(handle, out LoadedPlugin? loaded))
            throw new InvalidPluginHandleException(handle);
        return loaded;
    }

    private ulong NextHandleLocked()
    {
        while (_nextHandle == 0 || _extensions.ContainsKey(_nextHandle) ||
            _reservedHandles.Contains(_nextHandle))
            ++_nextHandle;
        return _nextHandle++;
    }

    private void WaitForDebuggerIfRequested(string sourceAssembly)
    {
        string? configured = Environment.GetEnvironmentVariable(
            "AEGISUB_CSHARP_DEBUG_WAIT_SECONDS");
        if (string.IsNullOrWhiteSpace(configured))
            return;
        if (!int.TryParse(configured, out int timeoutSeconds) || timeoutSeconds <= 0)
            throw new InvalidOperationException(
                "AEGISUB_CSHARP_DEBUG_WAIT_SECONDS must be a positive integer.");
        if (Interlocked.Exchange(ref _debuggerWaitCompleted, 1) != 0)
            return;

        _log(
            $"Waiting up to {timeoutSeconds}s for a managed debugger; " +
            $"PID={Environment.ProcessId}, extension={sourceAssembly}");
        Stopwatch timeout = Stopwatch.StartNew();
        while (!Debugger.IsAttached && timeout.Elapsed < TimeSpan.FromSeconds(timeoutSeconds))
            Thread.Sleep(100);
        _log(Debugger.IsAttached
            ? $"Managed debugger attached to PID {Environment.ProcessId}"
            : $"Managed debugger wait timed out for PID {Environment.ProcessId}");
    }

    private static string SerializeContributionKind(ContributionKind kind) => kind switch
    {
        ContributionKind.Automation => "automation",
        ContributionKind.Command => "command",
        ContributionKind.Settings => "settings",
        ContributionKind.ToolView => "toolView",
        ContributionKind.ServiceProvider => "serviceProvider",
        _ => throw new InvalidOperationException($"Unknown contribution kind value {kind}.")
    };

    private static string SerializeMetadata(
        PluginMetadata plugin,
        string entryModel,
        IEnumerable<ValidatedContribution> contributions)
    {
        string contractsVersion = typeof(IAegisubPlugin).Assembly
            .GetName().Version?.ToString(3) ?? "0.0.0";

        List<MetadataContribution> metadataContributions = [];
        foreach (ValidatedContribution contribution in contributions)
        {
            List<MetadataMacro> metadataMacros = [];
            foreach (IAegisubMacro macro in contribution.Macros)
            {
                metadataMacros.Add(new MetadataMacro(
                    macro.Metadata.Id,
                    macro.Metadata.Name,
                    macro.Metadata.Description,
                    macro.Metadata.NameResourceKey,
                    macro.Metadata.DescriptionResourceKey,
                    macro.Metadata.RequiresProject,
                    macro.Metadata.SubtitleAccess switch
                    {
                        SubtitleAccess.None => "none",
                        SubtitleAccess.Read => "read",
                        SubtitleAccess.ReadWrite => "readWrite",
                        _ => throw new InvalidOperationException(
                            $"Unknown subtitle access value {macro.Metadata.SubtitleAccess}.")
                    },
                    macro.Metadata.SnapshotScope switch
                    {
                        SubtitleSnapshotScope.Full => "full",
                        SubtitleSnapshotScope.Selection => "selection",
                        _ => throw new InvalidOperationException(
                            $"Unknown subtitle snapshot scope {macro.Metadata.SnapshotScope}.")
                    },
                    new MetadataQueryState(
                        macro.Metadata.QueryState?.RequiresSubtitleFile ?? false,
                        macro.Metadata.QueryState?.MinimumSelectedEvents ?? 0,
                        macro.Metadata.QueryState?.RequiresActiveEvent ?? false,
                        macro.Metadata.QueryState?.RequiresVideo ?? false,
                        macro.Metadata.QueryState?.RequiresAudio ?? false,
                        macro.Metadata.QueryState?.RequiresKeyframes ?? false)));
            }
            metadataContributions.Add(new MetadataContribution(
                contribution.Metadata.Id,
                SerializeContributionKind(contribution.Metadata.Kind),
                contribution.Metadata.Name,
                contribution.Metadata.Description,
                contribution.Metadata.NameResourceKey,
                contribution.Metadata.DescriptionResourceKey,
                contribution.Operations,
                metadataMacros));
        }

        MetadataDocument document = new(
            plugin.Id,
            plugin.Name,
            plugin.Description,
            plugin.NameResourceKey,
            plugin.DescriptionResourceKey,
            plugin.Author,
            plugin.Version,
            contractsVersion,
            entryModel,
            metadataContributions);
        return JsonSerializer.Serialize(document, InvocationJson.MetadataDocument);
    }

    private string CreateShadowCopy(string sourceDirectory)
    {
        string shadowDirectory = Path.Combine(
            _processShadowRoot,
            Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(shadowDirectory);
        try
        {
            foreach (string directory in Directory.EnumerateDirectories(sourceDirectory, "*", SearchOption.AllDirectories))
            {
                string relative = Path.GetRelativePath(sourceDirectory, directory);
                Directory.CreateDirectory(Path.Combine(shadowDirectory, relative));
            }
            foreach (string file in Directory.EnumerateFiles(sourceDirectory, "*", SearchOption.AllDirectories))
            {
                string relative = Path.GetRelativePath(sourceDirectory, file);
                string destination = Path.Combine(shadowDirectory, relative);
                Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
                File.Copy(file, destination, overwrite: true);
            }

            return shadowDirectory;
        }
        catch
        {
            TryDeleteShadow(shadowDirectory);
            TryDeleteEmptyDirectory(_processShadowRoot);
            throw;
        }
    }

    private void TryDeleteOrQueue(string shadowDirectory)
    {
        lock (_gate)
        {
            if (!TryDeleteShadow(shadowDirectory))
            {
                _pendingShadowCleanup.Add(shadowDirectory);
                _log($"Shadow directory cleanup was deferred: {shadowDirectory}");
            }
        }
    }

    private void TryCleanupPendingLocked()
    {
        for (int index = _pendingUnloads.Count - 1; index >= 0; --index)
        {
            PendingUnload pending = _pendingUnloads[index];
            if (pending.LoadContextReference.IsAlive)
                continue;
            if (!TryDeleteShadow(pending.ShadowDirectory))
            {
                _pendingShadowCleanup.Add(pending.ShadowDirectory);
                _log($"Shadow directory cleanup was deferred: {pending.ShadowDirectory}");
            }
            _pendingUnloads.RemoveAt(index);
            _log(pending.Handle == 0
                ? "Collectible ALC released after a failed C# extension load"
                : $"Collectible ALC released for C# extension handle {pending.Handle}");
        }

        foreach (string directory in _pendingShadowCleanup.ToArray())
        {
            if (TryDeleteShadow(directory))
                _pendingShadowCleanup.Remove(directory);
        }
        TryDeleteEmptyDirectory(_processShadowRoot);
    }

    private static long GetCurrentProcessStartTicks()
    {
        using Process process = Process.GetCurrentProcess();
        return process.StartTime.ToUniversalTime().Ticks;
    }

    private void ScavengeStaleShadowRoots()
    {
        try
        {
            if (!Directory.Exists(_shadowBaseDirectory))
                return;

            foreach (string directory in Directory.EnumerateDirectories(_shadowBaseDirectory))
            {
                if (string.Equals(directory, _processShadowRoot, StringComparison.OrdinalIgnoreCase))
                    continue;

                string name = Path.GetFileName(directory);
                bool remove = TryParseProcessShadowRoot(name, out int processId, out long startTicks)
                    ? !IsMatchingProcess(processId, startTicks)
                    : IsExpiredLegacyShadowRoot(directory, name);
                if (remove && TryDeleteShadow(directory))
                    _log($"Removed stale Plugin Bridge shadow root: {directory}");
            }
        }
        catch (IOException error)
        {
            _log($"Could not scavenge Plugin Bridge shadow roots: {error.Message}");
        }
        catch (UnauthorizedAccessException error)
        {
            _log($"Could not scavenge Plugin Bridge shadow roots: {error.Message}");
        }
    }

    private static bool TryParseProcessShadowRoot(
        string name,
        out int processId,
        out long startTicks)
    {
        processId = 0;
        startTicks = 0;
        int separator = name.IndexOf('-');
        return separator > 0 && separator < name.Length - 1 &&
            int.TryParse(name.AsSpan(0, separator), out processId) &&
            processId > 0 &&
            long.TryParse(name.AsSpan(separator + 1), out startTicks) &&
            startTicks > 0;
    }

    private static bool IsMatchingProcess(int processId, long startTicks)
    {
        try
        {
            using Process process = Process.GetProcessById(processId);
            return process.StartTime.ToUniversalTime().Ticks == startTicks;
        }
        catch (ArgumentException)
        {
            return false;
        }
        catch (InvalidOperationException)
        {
            return false;
        }
        catch
        {
            // If the platform does not permit inspecting another process,
            // preserving its directory is safer than deleting a live load.
            return true;
        }
    }

    private static bool IsExpiredLegacyShadowRoot(string directory, string name)
    {
        if (!Guid.TryParseExact(name, "N", out _))
            return false;
        try
        {
            return Directory.GetLastWriteTimeUtc(directory) <
                DateTime.UtcNow - LegacyShadowRootRetention;
        }
        catch
        {
            return false;
        }
    }

    private static bool TryDeleteEmptyDirectory(string directory)
    {
        try
        {
            if (Directory.Exists(directory) &&
                !Directory.EnumerateFileSystemEntries(directory).Any())
                Directory.Delete(directory);
            return true;
        }
        catch (IOException)
        {
            return false;
        }
        catch (UnauthorizedAccessException)
        {
            return false;
        }
    }

    private static bool TryDeleteShadow(string shadowDirectory)
    {
        try
        {
            if (Directory.Exists(shadowDirectory))
                Directory.Delete(shadowDirectory, recursive: true);
            return true;
        }
        catch (IOException)
        {
            return false;
        }
        catch (UnauthorizedAccessException)
        {
            return false;
        }
    }
}
