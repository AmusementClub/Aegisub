using System.Globalization;
using System.Text.Encodings.Web;
using System.Text.Json;
using Aegisub.DependencyControl;
using Aegisub.Managed.Contracts;

namespace Aegisub.DependencyControl.Plugin;

public sealed class DependencyControlPlugin :
    IAegisubPlugin,
    IAegisubPluginLifecycle,
    IAegisubPluginEventHandler
{
    private readonly DependencyControlServiceContribution _service;
    private readonly DependencyControlToolViewController _toolView;
    private readonly IReadOnlyList<IAegisubContribution> _contributions;

    public DependencyControlPlugin()
    {
        _service = new();
        _toolView = new(_service);
        _contributions =
        [
            _service,
            new DependencyControlAutomationContribution(_toolView)
        ];
    }

    public PluginMetadata Metadata { get; } = new(
        Id: "aegisub.dependency-control",
        Name: "DependencyControl",
        Description: "Manages compatible Lua Automation dependencies and updates.",
        Author: "Aegisub",
        Version: "0.1.0");

    public IReadOnlyList<IAegisubContribution> Contributions => _contributions;

    public async ValueTask ActivateAsync(
        IAegisubPluginContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        await _service.ActivateAsync(context, cancellationToken).ConfigureAwait(false);
        _toolView.Activate(context);
    }

    public ValueTask DeactivateAsync(CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        _toolView.Deactivate();
        _service.Deactivate();
        return ValueTask.CompletedTask;
    }

    public ValueTask HandleEventAsync(
        PluginEvent pluginEvent,
        CancellationToken cancellationToken) =>
        _toolView.HandleEventAsync(pluginEvent, cancellationToken);
}

internal sealed class DependencyControlServiceContribution :
    IAegisubServiceProviderContribution,
    IDisposable
{
    private const int MaxRequiredModules = 256;
    private const int MaxUpdateBatchPackages = 256;
    private const int MaxTextLength = 16 * 1024;

    private static readonly IReadOnlyList<string> RegisteredOperations =
        [
            "compatibility.probe",
            "record.register",
            "module.ensure",
            "installed.list",
            "installed.get",
            "catalog.lookup",
            "catalog.list",
            "packages.install",
            "updates.check",
            "updates.apply",
            "packages.uninstall",
            "feeds.list",
            "feeds.upsert",
            "feeds.remove",
            "feeds.test",
            "feeds.inspect",
            "logs.append",
            "logs.list",
            "logs.trim",
            "config.read",
            "config.write",
            "config.delete",
            "network.settings.get",
            "network.settings.update",
            "network.test"
        ];

    private const string StateRootService =
        "aegisub.dependency-control.get-state-root";
    private const string ReconcileTransactionsService =
        "aegisub.dependency-control.reconcile-transactions";

    private readonly object _gate = new();
    private readonly Dictionary<string, RegisteredRecord> _records = new(StringComparer.Ordinal);
    private readonly DependencyControlHttpTransport _transport = new();
    private readonly SemaphoreSlim _installGate = new(1, 1);
    private IAegisubPluginContext? _context;
    private DependencyControlNetworkSettingsStore? _settingsStore;
    private DependencyControlConfigurationStore? _configurationStore;
    private DependencyControlInstalledStateStore? _installedStateStore;
    private DependencyControlInstallJournal? _installJournal;
    private DependencyControlFeedStore? _feedStore;
    private DependencyControlLogStore? _logStore;
    private DependencyControlCatalogSnapshot? _catalogSnapshot;
    private long _catalogSnapshotFeedRevision = -1;
    private bool _disposed;
    private long _revision;

    public ContributionMetadata Metadata { get; } = new(
        Id: "aegisub.dependency-control.service",
        Kind: ContributionKind.ServiceProvider,
        Name: "DependencyControl application service",
        Description: "Application-level transport used while Lua Automation scripts are loading.");

    public IReadOnlyList<string> Operations => RegisteredOperations;

    public async ValueTask ActivateAsync(
        IAegisubPluginContext context,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(context);
        string response = await context.InvokeHostServiceAsync(
            StateRootService, "{}", cancellationToken).ConfigureAwait(false);
        string stateRoot;
        string legacyConfigRoot;
        using (JsonDocument document = JsonDocument.Parse(response))
        {
            stateRoot = ReadRequiredString(document.RootElement, "stateRoot", 32 * 1024);
            legacyConfigRoot = ReadOptionalString(
                document.RootElement, "legacyConfigRoot", 32 * 1024) ?? "";
        }
        string reconciliationResponse = await context.InvokeHostServiceAsync(
            ReconcileTransactionsService, "{}", cancellationToken).ConfigureAwait(false);
        string automationRoot;
        int recoveredTransactions;
        using (JsonDocument document = JsonDocument.Parse(reconciliationResponse))
        {
            automationRoot = ReadRequiredString(
                document.RootElement, "automationRoot", 32 * 1024);
            recoveredTransactions = ReadOptionalNonNegativeInt(
                document.RootElement,
                "recoveredTransactions",
                0,
                2048);
        }
        DependencyControlNetworkSettingsStore store = new(stateRoot);
        DependencyControlConfigurationStore configurationStore = new(
            stateRoot, legacyConfigRoot);
        DependencyControlInstalledStateStore installedStateStore = new(stateRoot);
        DependencyControlInstallJournal installJournal = new(stateRoot);
        DependencyControlFeedStore feedStore = new(stateRoot);
        DependencyControlLogStore logStore = new(stateRoot);
        DependencyControlReconciliationResult reconciliation =
            installJournal.Reconcile(automationRoot, installedStateStore);
        DependencyControlNetworkSettings settings;
        try
        {
            settings = store.Load();
        }
        catch
        {
            context.Log(
                "DependencyControl ignored invalid network settings and selected System proxy mode.");
            settings = DependencyControlNetworkSettings.Default;
        }
        _transport.ApplySettings(settings);
        if (installedStateStore.Corrupted)
            context.Log(
                "DependencyControl isolated invalid installed state and started with an empty catalog.");
        if (feedStore.Corrupted)
            context.Log(
                "DependencyControl isolated invalid feed settings and started with no configured feeds.");
        if (recoveredTransactions > 0)
            context.Log(
                "DependencyControl restored an interrupted native package transaction.");
        if (reconciliation.Status == DependencyControlReconciliationStatus.Finalized)
            context.Log(
                "DependencyControl finalized installed state after an interrupted application commit.");
        else if (reconciliation.Status is
                 DependencyControlReconciliationStatus.Abandoned or
                 DependencyControlReconciliationStatus.Corrupted)
            context.Log(
                "DependencyControl isolated an unresolved pending install journal.");
        lock (_gate)
        {
            _context = context;
            _settingsStore = store;
            _configurationStore = configurationStore;
            _installedStateStore = installedStateStore;
            _installJournal = installJournal;
            _feedStore = feedStore;
            _logStore = logStore;
            _catalogSnapshot = null;
            _catalogSnapshotFeedRevision = -1;
        }
    }

    public void Deactivate()
    {
        lock (_gate)
        {
            _context = null;
            _configurationStore = null;
            _installedStateStore = null;
            _installJournal = null;
            _feedStore = null;
            _logStore = null;
            _catalogSnapshot = null;
            _catalogSnapshotFeedRevision = -1;
        }
        Dispose();
    }

    public async ValueTask<string> InvokeAsync(
        string operationId,
        string requestJson,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return operationId switch
        {
            "compatibility.probe" => BuildProbeResponse(),
            "record.register" => await RegisterRecordAsync(
                requestJson, cancellationToken).ConfigureAwait(false),
            "module.ensure" => await EnsureModulesAsync(requestJson, cancellationToken)
                .ConfigureAwait(false),
            "installed.list" => await ListInstalledAsync(requestJson, cancellationToken)
                .ConfigureAwait(false),
            "installed.get" => await GetInstalledAsync(requestJson, cancellationToken)
                .ConfigureAwait(false),
            "catalog.lookup" => await LookupCatalogAsync(requestJson, cancellationToken)
                .ConfigureAwait(false),
            "catalog.list" => await ListCatalogAsync(requestJson, cancellationToken)
                .ConfigureAwait(false),
            "packages.install" => await InstallPackageAsync(
                requestJson, cancellationToken).ConfigureAwait(false),
            "updates.check" => await CheckUpdatesAsync(requestJson, cancellationToken)
                .ConfigureAwait(false),
            "updates.apply" => await ApplyUpdatesAsync(requestJson, cancellationToken)
                .ConfigureAwait(false),
            "packages.uninstall" => await UninstallPackageAsync(
                requestJson, cancellationToken).ConfigureAwait(false),
            "feeds.list" => await ListFeedsAsync(requestJson, cancellationToken)
                .ConfigureAwait(false),
            "feeds.upsert" => await UpsertFeedAsync(requestJson, cancellationToken)
                .ConfigureAwait(false),
            "feeds.remove" => await RemoveFeedAsync(requestJson, cancellationToken)
                .ConfigureAwait(false),
            "feeds.test" => await TestFeedAsync(requestJson, cancellationToken)
                .ConfigureAwait(false),
            "feeds.inspect" => await InspectFeedAsync(requestJson, cancellationToken)
                .ConfigureAwait(false),
            "logs.append" => AppendLog(requestJson),
            "logs.list" => ListLogs(requestJson),
            "logs.trim" => TrimLogs(requestJson),
            "config.read" => await ReadConfigurationAsync(
                requestJson, cancellationToken).ConfigureAwait(false),
            "config.write" => await WriteConfigurationAsync(
                requestJson, cancellationToken).ConfigureAwait(false),
            "config.delete" => await DeleteConfigurationAsync(
                requestJson, cancellationToken).ConfigureAwait(false),
            "network.settings.get" => _transport.Settings.ToJson(),
            "network.settings.update" => await UpdateNetworkSettingsAsync(
                requestJson, cancellationToken).ConfigureAwait(false),
            "network.test" => await TestNetworkAsync(
                requestJson, cancellationToken).ConfigureAwait(false),
            _ => throw new KeyNotFoundException(
                $"Unknown DependencyControl service operation '{operationId}'.")
        };
    }

    private async Task<string> RegisterRecordAsync(
        string requestJson,
        CancellationToken cancellationToken)
    {
        RegisteredRecord record;
        try
        {
            using JsonDocument document = JsonDocument.Parse(requestJson);
            record = ParseRecord(document.RootElement);
        }
        catch (JsonException error)
        {
            throw new InvalidOperationException(
                "The DependencyControl registration request is not valid JSON.",
                error);
        }

        await _installGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            DependencyControlInstalledWriteResult installed =
                GetInstalledStateStore().UpsertDiscovered(record);
            long revision;
            lock (_gate)
            {
                revision = checked(++_revision);
                _records[RecordKey(record.RecordType, record.Namespace)] = record;
            }

            return BuildJson(writer =>
            {
                writer.WriteStartObject();
                writer.WriteNumber("schemaVersion", 1);
                writer.WriteBoolean("registered", true);
                writer.WriteBoolean("installed", installed.Installed);
                writer.WriteString("recordType", record.RecordType);
                writer.WriteString("namespace", record.Namespace);
                writer.WriteString("name", record.Name);
                writer.WriteString("version", record.Version);
                writer.WriteNumber("requiredModuleCount", record.RequiredModules.Count);
                writer.WriteNumber("registrationRevision", revision);
                writer.WriteNumber("installedStateRevision", installed.Revision);
                writer.WriteEndObject();
            });
        }
        finally
        {
            _installGate.Release();
        }
    }

    private async Task<string> EnsureModulesAsync(
        string requestJson,
        CancellationToken cancellationToken)
    {
        RegisteredRecord record;
        IReadOnlyList<RequiredModule> requirements;
        try
        {
            using JsonDocument document = JsonDocument.Parse(requestJson);
            JsonElement root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object)
                throw new InvalidOperationException(
                    "A DependencyControl module.ensure request must be a JSON object.");
            string recordType = ReadRequiredString(root, "recordType", 32);
            string recordNamespace = ReadRequiredString(root, "namespace", 512);
            lock (_gate)
            {
                if (!_records.TryGetValue(
                    RecordKey(recordType, recordNamespace), out RegisteredRecord? found))
                    throw new InvalidOperationException(
                        $"DependencyControl record '{recordNamespace}' is not registered.");
                record = found;
            }
            requirements = root.TryGetProperty("requiredModules", out _)
                ? ReadRequiredModules(root)
                : record.RequiredModules;
        }
        catch (JsonException error)
        {
            throw new InvalidOperationException(
                "The DependencyControl module.ensure request is not valid JSON.",
                error);
        }

        if (requirements.Count == 0)
            return BuildEmptyEnsureResponse();

        await _installGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            DependencyControlInstaller installer = new(GetPluginContext(), _transport);
            DependencyControlInstalledStateStore installedState =
                GetInstalledStateStore();
            DependencyControlInstallResult result = await installer.EnsureAsync(
                record,
                requirements,
                GetInstallJournal(),
                installedState,
                cancellationToken).ConfigureAwait(false);
            ExitAfterCommitForTest();
            installedState.ApplyInstall(result.Packages);
            GetInstallJournal().Complete(result.TransactionId);
            return result.ToJson();
        }
        finally
        {
            _installGate.Release();
        }
    }

    private async Task<string> ListInstalledAsync(
        string requestJson,
        CancellationToken cancellationToken)
    {
        int offset;
        int limit;
        try
        {
            using JsonDocument document = JsonDocument.Parse(requestJson);
            JsonElement root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object)
                throw new InvalidOperationException(
                    "A DependencyControl installed.list request must be an object.");
            offset = ReadOptionalNonNegativeInt(root, "offset", 0, int.MaxValue);
            limit = ReadOptionalNonNegativeInt(root, "limit", 16, 16);
            if (limit == 0)
                throw new InvalidOperationException(
                    "DependencyControl installed.list limit must be positive.");
        }
        catch (JsonException error)
        {
            throw new InvalidOperationException(
                "The DependencyControl installed.list request is not valid JSON.",
                error);
        }
        await _installGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            return BuildJson(writer =>
                GetInstalledStateStore().WriteList(writer, offset, limit));
        }
        finally
        {
            _installGate.Release();
        }
    }

    private async Task<string> GetInstalledAsync(
        string requestJson,
        CancellationToken cancellationToken)
    {
        string recordType;
        string packageNamespace;
        int requirementOffset;
        int requirementLimit;
        int fileOffset;
        int fileLimit;
        try
        {
            using JsonDocument document = JsonDocument.Parse(requestJson);
            JsonElement root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object)
                throw new InvalidOperationException(
                    "A DependencyControl installed.get request must be an object.");
            recordType = ReadRequiredString(root, "recordType", 32);
            if (recordType is not ("automation" or "module"))
                throw new InvalidOperationException(
                    "DependencyControl installed.get recordType is invalid.");
            packageNamespace = ReadRequiredString(root, "namespace", 512);
            if (!IsSafeNamespace(packageNamespace))
                throw new InvalidOperationException(
                    "DependencyControl installed.get namespace is invalid.");
            requirementOffset = ReadOptionalNonNegativeInt(
                root, "requirementOffset", 0, int.MaxValue);
            requirementLimit = ReadOptionalNonNegativeInt(
                root, "requirementLimit", 32, 32);
            fileOffset = ReadOptionalNonNegativeInt(root, "fileOffset", 0, int.MaxValue);
            fileLimit = ReadOptionalNonNegativeInt(root, "fileLimit", 64, 64);
            if (requirementLimit == 0 || fileLimit == 0)
                throw new InvalidOperationException(
                    "DependencyControl installed.get limits must be positive.");
        }
        catch (JsonException error)
        {
            throw new InvalidOperationException(
                "The DependencyControl installed.get request is not valid JSON.",
                error);
        }
        await _installGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            return BuildJson(writer => GetInstalledStateStore().WritePackage(
                writer,
                recordType,
                packageNamespace,
                requirementOffset,
                requirementLimit,
                fileOffset,
                fileLimit));
        }
        finally
        {
            _installGate.Release();
        }
    }

    private async Task<string> LookupCatalogAsync(
        string requestJson,
        CancellationToken cancellationToken)
    {
        (string recordType, string packageNamespace, string feed) =
            ParseCatalogLocation(requestJson);
        int channelOffset;
        int channelLimit;
        using (JsonDocument document = JsonDocument.Parse(requestJson))
        {
            channelOffset = ReadOptionalNonNegativeInt(
                document.RootElement, "channelOffset", 0, int.MaxValue);
            channelLimit = ReadOptionalNonNegativeInt(
                document.RootElement, "channelLimit", 16, 16);
        }
        if (channelLimit == 0)
            throw new InvalidOperationException(
                "DependencyControl catalog.lookup channelLimit must be positive.");

        await _installGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            DependencyControlInstalledPackage? installed = null;
            if (feed.Length == 0)
            {
                if (!GetInstalledStateStore().TryGet(
                    recordType, packageNamespace, out installed) ||
                    installed is null || installed.Feed.Length == 0)
                    throw new InvalidOperationException(
                        "DependencyControl catalog lookup requires an explicit or installed feed.");
                feed = installed.Feed;
            }
            DependencyControlPackageKind kind = recordType == "module"
                ? DependencyControlPackageKind.Module
                : DependencyControlPackageKind.Macro;
            DependencyControlFeedCatalog catalog = new(_transport);
            (DependencyControlFeed source, DependencyControlPackage package) =
                await catalog.FindPackageAsync(
                    feed, kind, packageNamespace, cancellationToken).ConfigureAwait(false);
            return BuildCatalogResponse(
                recordType, source, package, channelOffset, channelLimit);
        }
        finally
        {
            _installGate.Release();
        }
    }

    private async Task<string> ListFeedsAsync(
        string requestJson,
        CancellationToken cancellationToken)
    {
        int offset;
        int limit;
        try
        {
            using JsonDocument document = JsonDocument.Parse(requestJson);
            offset = ReadOptionalNonNegativeInt(
                document.RootElement, "offset", 0, int.MaxValue);
            limit = ReadOptionalNonNegativeInt(document.RootElement, "limit", 64, 64);
        }
        catch (JsonException error)
        {
            throw new InvalidOperationException(
                "The DependencyControl feeds.list request is not valid JSON.", error);
        }
        if (limit == 0)
            throw new InvalidOperationException(
                "DependencyControl feeds.list limit must be positive.");

        await _installGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            DependencyControlFeedStore store = GetFeedStore();
            IReadOnlyList<DependencyControlConfiguredFeed> feeds = store.List();
            return BuildJson(writer =>
            {
                writer.WriteStartObject();
                writer.WriteNumber("schemaVersion", 1);
                writer.WriteNumber("revision", store.Revision);
                writer.WriteNumber("totalCount", feeds.Count);
                writer.WriteNumber("offset", offset);
                writer.WritePropertyName("feeds");
                writer.WriteStartArray();
                foreach (DependencyControlConfiguredFeed feed in feeds.Skip(offset).Take(limit))
                    WriteConfiguredFeed(writer, feed);
                writer.WriteEndArray();
                writer.WriteEndObject();
            });
        }
        finally
        {
            _installGate.Release();
        }
    }

    private async Task<string> UpsertFeedAsync(
        string requestJson,
        CancellationToken cancellationToken)
    {
        string id;
        string label;
        string url;
        bool enabled;
        try
        {
            using JsonDocument document = JsonDocument.Parse(requestJson);
            JsonElement root = document.RootElement;
            id = ReadOptionalString(root, "id", 64) ?? "";
            label = ReadRequiredString(root, "label", 256);
            url = ReadRequiredString(root, "url", MaxTextLength);
            enabled = !root.TryGetProperty("enabled", out _) ||
                ReadOptionalBoolean(root, "enabled");
        }
        catch (JsonException error)
        {
            throw new InvalidOperationException(
                "The DependencyControl feeds.upsert request is not valid JSON.", error);
        }

        await _installGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            DependencyControlConfiguredFeed feed = GetFeedStore().Upsert(
                id, label, url, enabled);
            InvalidateCatalogSnapshot();
            return BuildJson(writer =>
            {
                writer.WriteStartObject();
                writer.WriteNumber("schemaVersion", 1);
                writer.WriteBoolean("saved", true);
                WriteConfiguredFeedProperties(writer, feed);
                writer.WriteEndObject();
            });
        }
        finally
        {
            _installGate.Release();
        }
    }

    private async Task<string> RemoveFeedAsync(
        string requestJson,
        CancellationToken cancellationToken)
    {
        string id;
        try
        {
            using JsonDocument document = JsonDocument.Parse(requestJson);
            id = ReadRequiredString(document.RootElement, "id", 64);
        }
        catch (JsonException error)
        {
            throw new InvalidOperationException(
                "The DependencyControl feeds.remove request is not valid JSON.", error);
        }

        await _installGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            bool removed = GetFeedStore().Remove(id);
            if (removed)
                InvalidateCatalogSnapshot();
            return BuildJson(writer =>
            {
                writer.WriteStartObject();
                writer.WriteNumber("schemaVersion", 1);
                writer.WriteBoolean("removed", removed);
                writer.WriteString("id", id);
                writer.WriteEndObject();
            });
        }
        finally
        {
            _installGate.Release();
        }
    }

    private async Task<string> TestFeedAsync(
        string requestJson,
        CancellationToken cancellationToken)
    {
        string id;
        string url;
        try
        {
            using JsonDocument document = JsonDocument.Parse(requestJson);
            JsonElement root = document.RootElement;
            id = ReadOptionalString(root, "id", 64) ?? "";
            url = ReadOptionalString(root, "url", MaxTextLength) ?? "";
        }
        catch (JsonException error)
        {
            throw new InvalidOperationException(
                "The DependencyControl feeds.test request is not valid JSON.", error);
        }

        await _installGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (url.Length == 0)
            {
                if (id.Length == 0 || !GetFeedStore().TryGet(
                        DependencyControlFeedStore.ValidateId(id),
                        out DependencyControlConfiguredFeed? configured) || configured is null)
                    throw new InvalidOperationException(
                        "DependencyControl feeds.test requires a configured feed ID or URL.");
                url = configured.Url;
            }
            url = DependencyControlFeedStore.NormalizeUrl(url);
            DependencyControlFeedCatalog catalog = new(_transport);
            DependencyControlFeed feed = await catalog.LoadFeedAsync(
                url, cancellationToken).ConfigureAwait(false);
            return BuildJson(writer =>
            {
                writer.WriteStartObject();
                writer.WriteNumber("schemaVersion", 1);
                writer.WriteBoolean("success", true);
                writer.WriteString("url", feed.SourceUrl);
                writer.WriteString("name", feed.Name);
                writer.WriteString("description", feed.Description);
                writer.WriteString("maintainer", feed.Maintainer);
                writer.WriteString("formatVersion", feed.FormatVersion);
                writer.WriteNumber("macroCount", feed.Macros.Count);
                writer.WriteNumber("moduleCount", feed.Modules.Count);
                writer.WriteNumber("knownFeedCount", feed.KnownFeeds.Count);
                writer.WriteEndObject();
            });
        }
        finally
        {
            _installGate.Release();
        }
    }

    private async Task<string> InspectFeedAsync(
        string requestJson,
        CancellationToken cancellationToken)
    {
        string url;
        try
        {
            using JsonDocument document = JsonDocument.Parse(requestJson);
            url = DependencyControlFeedStore.NormalizeUrl(
                ReadRequiredString(document.RootElement, "url", MaxTextLength));
        }
        catch (JsonException error)
        {
            throw new InvalidOperationException(
                "The DependencyControl feeds.inspect request is not valid JSON.", error);
        }
        await _installGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            DependencyControlFeed feed = await new DependencyControlFeedCatalog(_transport)
                .LoadFeedAsync(url, cancellationToken).ConfigureAwait(false);
            if (feed.KnownFeeds.Count > 32)
                throw new InvalidOperationException(
                    "DependencyControl feed inspection exceeds the known-feed limit.");
            return BuildJson(writer =>
            {
                writer.WriteStartObject();
                writer.WriteNumber("schemaVersion", 1);
                writer.WriteString("url", feed.SourceUrl);
                writer.WriteString("formatVersion", feed.FormatVersion);
                writer.WriteString("name", feed.Name);
                writer.WriteString("description", feed.Description);
                writer.WriteString("maintainer", feed.Maintainer);
                writer.WriteNumber("macroCount", feed.Macros.Count);
                writer.WriteNumber("moduleCount", feed.Modules.Count);
                writer.WritePropertyName("knownFeeds");
                writer.WriteStartObject();
                foreach ((string id, string knownUrl) in feed.KnownFeeds
                             .OrderBy(item => item.Key, StringComparer.Ordinal))
                    writer.WriteString(id, knownUrl);
                writer.WriteEndObject();
                writer.WriteEndObject();
            });
        }
        finally
        {
            _installGate.Release();
        }
    }

    private string AppendLog(string requestJson)
    {
        try
        {
            using JsonDocument document = JsonDocument.Parse(requestJson);
            JsonElement root = document.RootElement;
            int level = ReadOptionalNonNegativeInt(root, "level", 2, 5);
            string source = ReadRequiredString(root, "source", 256);
            string message = ReadRequiredString(root, "message", MaxTextLength);
            GetLogStore().Append(level, source, message);
            return BuildJson(writer =>
            {
                writer.WriteStartObject();
                writer.WriteNumber("schemaVersion", 1);
                writer.WriteBoolean("written", true);
                writer.WriteEndObject();
            });
        }
        catch (JsonException error)
        {
            throw new InvalidOperationException(
                "The DependencyControl logs.append request is not valid JSON.", error);
        }
    }

    private string ListLogs(string requestJson)
    {
        int offset;
        int limit;
        try
        {
            using JsonDocument document = JsonDocument.Parse(requestJson);
            offset = ReadOptionalNonNegativeInt(
                document.RootElement, "offset", 0, int.MaxValue);
            limit = ReadOptionalNonNegativeInt(document.RootElement, "limit", 64, 64);
        }
        catch (JsonException error)
        {
            throw new InvalidOperationException(
                "The DependencyControl logs.list request is not valid JSON.", error);
        }
        if (limit == 0)
            throw new InvalidOperationException(
                "DependencyControl logs.list limit must be positive.");
        IReadOnlyList<DependencyControlLogEntry> entries = GetLogStore().List();
        return BuildJson(writer =>
        {
            writer.WriteStartObject();
            writer.WriteNumber("schemaVersion", 1);
            writer.WriteNumber("totalCount", entries.Count);
            writer.WriteNumber("offset", offset);
            writer.WritePropertyName("entries");
            writer.WriteStartArray();
            foreach (DependencyControlLogEntry entry in entries.Skip(offset).Take(limit))
            {
                writer.WriteStartObject();
                writer.WriteString("timestamp", entry.Timestamp);
                writer.WriteNumber("level", entry.Level);
                writer.WriteString("source", entry.Source);
                writer.WriteString("message", entry.Message);
                writer.WriteEndObject();
            }
            writer.WriteEndArray();
            writer.WriteEndObject();
        });
    }

    private string TrimLogs(string requestJson)
    {
        bool wipe;
        int maximumAgeSeconds;
        int maximumSize;
        int maximumFiles;
        try
        {
            using JsonDocument document = JsonDocument.Parse(requestJson);
            JsonElement root = document.RootElement;
            wipe = ReadOptionalBoolean(root, "wipe");
            maximumAgeSeconds = ReadOptionalNonNegativeInt(
                root, "maximumAgeSeconds", 7 * 24 * 60 * 60, 366 * 24 * 60 * 60);
            maximumSize = ReadOptionalNonNegativeInt(
                root, "maximumBytes", 100 * 1024 * 1024, 1024 * 1024 * 1024);
            maximumFiles = ReadOptionalNonNegativeInt(root, "maximumFiles", 200, 1000);
        }
        catch (JsonException error)
        {
            throw new InvalidOperationException(
                "The DependencyControl logs.trim request is not valid JSON.", error);
        }
        DependencyControlLogTrimResult result = GetLogStore().Trim(
            wipe,
            TimeSpan.FromSeconds(maximumAgeSeconds),
            maximumSize,
            maximumFiles);
        return BuildJson(writer =>
        {
            writer.WriteStartObject();
            writer.WriteNumber("schemaVersion", 1);
            writer.WriteNumber("deletedFiles", result.DeletedFiles);
            writer.WriteNumber("deletedBytes", result.DeletedBytes);
            writer.WriteNumber("totalFiles", result.TotalFiles);
            writer.WriteNumber("totalBytes", result.TotalBytes);
            writer.WriteEndObject();
        });
    }

    private async Task<string> ListCatalogAsync(
        string requestJson,
        CancellationToken cancellationToken)
    {
        int offset;
        int limit;
        bool refresh;
        try
        {
            using JsonDocument document = JsonDocument.Parse(requestJson);
            JsonElement root = document.RootElement;
            offset = ReadOptionalNonNegativeInt(root, "offset", 0, int.MaxValue);
            limit = ReadOptionalNonNegativeInt(root, "limit", 64, 64);
            refresh = ReadOptionalBoolean(root, "refresh");
        }
        catch (JsonException error)
        {
            throw new InvalidOperationException(
                "The DependencyControl catalog.list request is not valid JSON.", error);
        }
        if (limit == 0)
            throw new InvalidOperationException(
                "DependencyControl catalog.list limit must be positive.");

        await _installGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            DependencyControlFeedStore feedStore = GetFeedStore();
            if (refresh || _catalogSnapshot is null ||
                _catalogSnapshotFeedRevision != feedStore.Revision)
            {
                string[] roots = feedStore.List()
                    .Where(feed => feed.Enabled)
                    .Select(feed => feed.Url)
                    .ToArray();
                _catalogSnapshot = await new DependencyControlFeedCatalog(_transport)
                    .BuildSnapshotAsync(roots, cancellationToken).ConfigureAwait(false);
                _catalogSnapshotFeedRevision = feedStore.Revision;
            }
            DependencyControlCatalogSnapshot snapshot = _catalogSnapshot;
            DependencyControlInstalledStateStore installedState = GetInstalledStateStore();
            return BuildJson(writer =>
            {
                writer.WriteStartObject();
                writer.WriteNumber("schemaVersion", 1);
                writer.WriteNumber("feedRevision", feedStore.Revision);
                writer.WriteNumber("totalCount", snapshot.Packages.Count);
                writer.WriteNumber("offset", offset);
                writer.WriteNumber("feedCount", snapshot.Feeds.Count);
                writer.WriteNumber("failureCount", snapshot.Failures.Count);
                writer.WritePropertyName("packages");
                writer.WriteStartArray();
                foreach (DependencyControlCatalogPackage item in
                         snapshot.Packages.Skip(offset).Take(limit))
                    WriteCatalogSummary(writer, item, installedState);
                writer.WriteEndArray();
                writer.WritePropertyName("failures");
                writer.WriteStartArray();
                foreach (DependencyControlFeedFailure failure in snapshot.Failures)
                {
                    writer.WriteStartObject();
                    writer.WriteString("url", failure.Url);
                    writer.WriteBoolean("root", failure.IsRoot);
                    writer.WriteString("error", failure.Error);
                    writer.WriteEndObject();
                }
                writer.WriteEndArray();
                writer.WriteEndObject();
            });
        }
        finally
        {
            _installGate.Release();
        }
    }

    private async Task<string> InstallPackageAsync(
        string requestJson,
        CancellationToken cancellationToken)
    {
        (string recordType, string packageNamespace, string feed) =
            ParseCatalogLocation(requestJson);
        string requestedChannel;
        string targetVersion;
        using (JsonDocument document = JsonDocument.Parse(requestJson))
        {
            requestedChannel = ReadOptionalString(
                document.RootElement, "channel", 256) ?? "";
            targetVersion = ReadOptionalString(
                document.RootElement, "targetVersion", 256) ?? "";
        }
        if (feed.Length == 0)
            throw new InvalidOperationException(
                "DependencyControl packages.install requires an explicit feed.");

        await _installGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            DependencyControlInstalledStateStore installedState = GetInstalledStateStore();
            if (installedState.TryGet(recordType, packageNamespace, out var installed) &&
                installed is not null)
                return BuildUpdateApplyResponse(
                    "alreadyInstalled", false, recordType, packageNamespace,
                    installed.Version, installed.Version, installed.Channel,
                    installed.Feed, "", []);

            DependencyControlPackageKind kind = recordType == "module"
                ? DependencyControlPackageKind.Module
                : DependencyControlPackageKind.Macro;
            DependencyControlFeedCatalog catalog = new(_transport);
            (DependencyControlFeed source, DependencyControlPackage package) =
                await catalog.FindPackageAsync(
                    feed, kind, packageNamespace, cancellationToken).ConfigureAwait(false);
            DependencyControlChannel channel = SelectUpdateChannel(package, requestedChannel);
            bool supported = channel.Platforms.Count == 0 ||
                channel.Platforms.Any(IsPlatformCompatible);
            if (!supported)
                return BuildUpdateApplyResponse(
                    "unsupportedPlatform", false, recordType, packageNamespace,
                    "", channel.Version, channel.Name, source.SourceUrl, "", []);
            if (targetVersion.Length > 0 &&
                CompareVersions(channel.Version, targetVersion) < 0)
                throw new InvalidOperationException(
                    $"DependencyControl package '{packageNamespace}' does not provide " +
                    $"the requested version '{targetVersion}'.");

            RegisteredRecord record = new(
                recordType,
                package.Namespace,
                package.Name,
                "0",
                package.Description,
                package.Author,
                source.SourceUrl,
                channel.Name,
                $"{package.Namespace}.json",
                false,
                []);
            DependencyControlInstaller installer = new(GetPluginContext(), _transport);
            DependencyControlInstallResult result = await installer.InstallPackageAsync(
                record,
                channel.Name,
                targetVersion.Length > 0 ? targetVersion : channel.Version,
                GetInstallJournal(),
                installedState,
                cancellationToken).ConfigureAwait(false);
            ExitAfterCommitForTest();
            installedState.ApplyInstall(result.Packages);
            GetInstallJournal().Complete(result.TransactionId);
            return BuildUpdateApplyResponse(
                "installed", true, recordType, packageNamespace, "", channel.Version,
                channel.Name, source.SourceUrl, result.AutomationRoot,
                result.InstalledFiles);
        }
        finally
        {
            _installGate.Release();
        }
    }

    private async Task<string> CheckUpdatesAsync(
        string requestJson,
        CancellationToken cancellationToken)
    {
        string recordType;
        string packageNamespace;
        string requestedChannel;
        try
        {
            using JsonDocument document = JsonDocument.Parse(requestJson);
            JsonElement root = document.RootElement;
            recordType = ReadRequiredString(root, "recordType", 32);
            packageNamespace = ReadRequiredString(root, "namespace", 512);
            requestedChannel = ReadOptionalString(root, "channel", 256) ?? "";
        }
        catch (JsonException error)
        {
            throw new InvalidOperationException(
                "The DependencyControl updates.check request is not valid JSON.",
                error);
        }
        ValidateCatalogIdentity(recordType, packageNamespace);

        await _installGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (!GetInstalledStateStore().TryGet(
                recordType, packageNamespace, out DependencyControlInstalledPackage? installed) ||
                installed is null)
                throw new InvalidOperationException(
                    $"DependencyControl package '{packageNamespace}' is not installed.");
            if (installed.Feed.Length == 0)
                throw new InvalidOperationException(
                    $"DependencyControl package '{packageNamespace}' has no update feed.");
            DependencyControlPackageKind kind = recordType == "module"
                ? DependencyControlPackageKind.Module
                : DependencyControlPackageKind.Macro;
            DependencyControlFeedCatalog catalog = new(_transport);
            (DependencyControlFeed source, DependencyControlPackage package) =
                await catalog.FindPackageAsync(
                    installed.Feed,
                    kind,
                    packageNamespace,
                    cancellationToken).ConfigureAwait(false);
            DependencyControlChannel channel = SelectUpdateChannel(
                package,
                requestedChannel.Length > 0 ? requestedChannel : installed.Channel);
            bool supported = channel.Platforms.Count == 0 ||
                channel.Platforms.Any(IsPlatformCompatible);
            int comparison = CompareVersions(channel.Version, installed.Version);
            string status = !supported
                ? "unsupportedPlatform"
                : comparison > 0
                    ? "updateAvailable"
                    : comparison == 0 ? "upToDate" : "installedNewer";
            return BuildJson(writer =>
            {
                writer.WriteStartObject();
                writer.WriteNumber("schemaVersion", 1);
                writer.WriteString("status", status);
                writer.WriteString("recordType", recordType);
                writer.WriteString("namespace", packageNamespace);
                writer.WriteString("installedVersion", installed.Version);
                writer.WriteString("availableVersion", channel.Version);
                writer.WriteString("channel", channel.Name);
                writer.WriteString("feed", source.SourceUrl);
                writer.WriteBoolean("platformSupported", supported);
                writer.WriteEndObject();
            });
        }
        finally
        {
            _installGate.Release();
        }
    }

    private async Task<string> ApplyUpdatesAsync(
        string requestJson,
        CancellationToken cancellationToken)
    {
        List<UpdateRequest> requests;
        bool batchRequest;
        try
        {
            using JsonDocument document = JsonDocument.Parse(requestJson);
            JsonElement root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object)
                throw new InvalidOperationException(
                    "A DependencyControl updates.apply request must be an object.");
            if (root.TryGetProperty("packages", out JsonElement packages))
            {
                batchRequest = true;
                if (packages.ValueKind != JsonValueKind.Array || packages.GetArrayLength() == 0)
                    throw new InvalidOperationException(
                        "DependencyControl updates.apply packages must be a non-empty array.");
                requests = packages.EnumerateArray().Select(ParseUpdateRequest).ToList();
                if (requests.Count > MaxUpdateBatchPackages)
                    throw new InvalidOperationException(
                        "DependencyControl updates.apply exceeds the package limit.");
                if (requests.Select(request =>
                        $"{request.RecordType}\n{request.Namespace}")
                    .Distinct(StringComparer.Ordinal).Count() != requests.Count)
                    throw new InvalidOperationException(
                        "DependencyControl updates.apply contains duplicate packages.");
            }
            else
            {
                batchRequest = false;
                requests = [ParseUpdateRequest(root)];
            }
        }
        catch (JsonException error)
        {
            throw new InvalidOperationException(
                "The DependencyControl updates.apply request is not valid JSON.",
                error);
        }

        await _installGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            DependencyControlInstalledStateStore installedState =
                GetInstalledStateStore();
            DependencyControlFeedCatalog catalog = new(_transport);
            List<BatchUpdateItem> updates = [];
            foreach (UpdateRequest request in requests)
            {
                ValidateCatalogIdentity(request.RecordType, request.Namespace);
                if (!installedState.TryGet(
                    request.RecordType,
                    request.Namespace,
                    out DependencyControlInstalledPackage? installed) ||
                    installed is null)
                    throw new InvalidOperationException(
                        $"DependencyControl package '{request.Namespace}' is not installed.");
                if (installed.Feed.Length == 0)
                    throw new InvalidOperationException(
                        $"DependencyControl package '{request.Namespace}' has no update feed.");
                DependencyControlPackageKind kind = request.RecordType == "module"
                    ? DependencyControlPackageKind.Module
                    : DependencyControlPackageKind.Macro;
                (DependencyControlFeed source, DependencyControlPackage package) =
                    await catalog.FindPackageAsync(
                        installed.Feed,
                        kind,
                        request.Namespace,
                        cancellationToken).ConfigureAwait(false);
                DependencyControlChannel channel = SelectUpdateChannel(
                    package,
                    request.Channel.Length > 0 ? request.Channel : installed.Channel);
                bool supported = channel.Platforms.Count == 0 ||
                    channel.Platforms.Any(IsPlatformCompatible);
                int comparison = CompareVersions(channel.Version, installed.Version);
                if (request.TargetVersion.Length > 0 &&
                    CompareVersions(channel.Version, request.TargetVersion) < 0)
                    throw new InvalidOperationException(
                        $"DependencyControl package '{request.Namespace}' does not provide " +
                        $"the requested version '{request.TargetVersion}'.");
                string status = !supported
                    ? "unsupportedPlatform"
                    : comparison > 0
                        ? "updateAvailable"
                        : comparison == 0 ? "upToDate" : "installedNewer";
                updates.Add(new(
                    request,
                    installed,
                    source,
                    channel,
                    status));
            }

            BatchUpdateItem[] pending = updates
                .Where(item => item.Status == "updateAvailable")
                .ToArray();
            if (pending.Length == 0)
                return BuildBatchUpdateResponse(
                    updates, batchRequest, committed: false, "", [], null);

            DependencyControlInstaller installer = new(GetPluginContext(), _transport);
            DependencyControlInstallResult result = await installer.InstallPackagesAsync(
                pending.Select(item => new DependencyControlInstallRequest(
                    new RegisteredRecord(
                        item.Request.RecordType,
                        item.Request.Namespace,
                        item.Installed.Name,
                        item.Installed.Version,
                        item.Installed.Description,
                        item.Installed.Author,
                        item.Installed.Feed,
                        item.Channel.Name,
                        item.Installed.ConfigFile,
                        false,
                        item.Installed.RequiredModules),
                    item.Channel.Name,
                    item.Request.TargetVersion.Length > 0
                        ? item.Request.TargetVersion
                        : item.Channel.Version)).ToArray(),
                GetInstallJournal(),
                installedState,
                cancellationToken).ConfigureAwait(false);
            ExitAfterCommitForTest();
            installedState.ApplyInstall(result.Packages);
            GetInstallJournal().Complete(result.TransactionId);
            return BuildBatchUpdateResponse(
                updates,
                batchRequest,
                committed: true,
                result.AutomationRoot,
                result.InstalledFiles,
                result.Packages);
        }
        finally
        {
            _installGate.Release();
        }
    }

    private static UpdateRequest ParseUpdateRequest(JsonElement root)
    {
        string targetVersion = ReadOptionalString(root, "targetVersion", 256) ?? "";
        if (targetVersion.Length > 0)
            _ = ParseComparableVersion(targetVersion);
        return new(
            ReadRequiredString(root, "recordType", 32),
            ReadRequiredString(root, "namespace", 512),
            ReadOptionalString(root, "channel", 256) ?? "",
            targetVersion);
    }

    private async Task<string> UninstallPackageAsync(
        string requestJson,
        CancellationToken cancellationToken)
    {
        string recordType;
        string packageNamespace;
        bool removeConfig;
        try
        {
            using JsonDocument document = JsonDocument.Parse(requestJson);
            JsonElement root = document.RootElement;
            recordType = ReadRequiredString(root, "recordType", 32);
            packageNamespace = ReadRequiredString(root, "namespace", 512);
            removeConfig = !root.TryGetProperty("removeConfig", out _) ||
                ReadOptionalBoolean(root, "removeConfig");
        }
        catch (JsonException error)
        {
            throw new InvalidOperationException(
                "The DependencyControl packages.uninstall request is not valid JSON.",
                error);
        }
        ValidateCatalogIdentity(recordType, packageNamespace);

        await _installGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            DependencyControlInstalledStateStore installedState =
                GetInstalledStateStore();
            DependencyControlUninstallPlan plan = installedState.PlanUninstall(
                recordType, packageNamespace);
            if (plan.Package.Source != "transaction")
                throw new InvalidOperationException(
                    $"DependencyControl package '{packageNamespace}' is not " +
                    "managed by a package transaction.");
            DependencyControlUninstaller uninstaller = new(GetPluginContext());
            DependencyControlUninstallResult result = await uninstaller.UninstallAsync(
                plan,
                GetInstallJournal(),
                installedState,
                cancellationToken).ConfigureAwait(false);
            if (result.TransactionId.Length > 0)
                ExitAfterCommitForTest();
            installedState.ApplyTransaction([], [result.Removal]);
            if (result.TransactionId.Length > 0)
                GetInstallJournal().Complete(result.TransactionId);

            bool configDeleted = false;
            if (removeConfig)
            {
                try
                {
                    configDeleted = GetConfigurationStore().Delete(
                        plan.Package.ConfigFile, []);
                }
                catch
                {
                    GetPluginContext().Log(
                        "DependencyControl preserved package configuration after uninstall.");
                }
            }
            return BuildJson(writer =>
            {
                writer.WriteStartObject();
                writer.WriteNumber("schemaVersion", 1);
                writer.WriteBoolean("committed", true);
                writer.WriteString("recordType", recordType);
                writer.WriteString("namespace", packageNamespace);
                writer.WriteBoolean("configDeleted", configDeleted);
                writer.WriteBoolean("reloadRequired", true);
                writer.WriteString("automationRoot", result.AutomationRoot);
                writer.WritePropertyName("removedFiles");
                writer.WriteStartArray();
                foreach (string target in result.RemovedFiles)
                    writer.WriteStringValue(target);
                writer.WriteEndArray();
                writer.WriteEndObject();
            });
        }
        finally
        {
            _installGate.Release();
        }
    }

    private async Task<string> UpdateNetworkSettingsAsync(
        string requestJson,
        CancellationToken cancellationToken)
    {
        DependencyControlNetworkSettings settings;
        try
        {
            using JsonDocument document = JsonDocument.Parse(requestJson);
            settings = DependencyControlNetworkSettings.Parse(document.RootElement);
        }
        catch (JsonException error)
        {
            throw new InvalidOperationException(
                "The DependencyControl network settings request is not valid JSON.",
                error);
        }

        await _installGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            DependencyControlNetworkSettingsStore store;
            lock (_gate)
                store = _settingsStore ?? throw new InvalidOperationException(
                    "DependencyControl network settings are unavailable before activation.");
            store.Save(settings);
            _transport.ApplySettings(settings);
            return settings.ToJson();
        }
        finally
        {
            _installGate.Release();
        }
    }

    private async Task<string> ReadConfigurationAsync(
        string requestJson,
        CancellationToken cancellationToken)
    {
        (string fileName, IReadOnlyList<string> section) =
            ParseConfigurationLocation(requestJson);
        await _installGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            DependencyControlConfigurationReadResult result =
                GetConfigurationStore().Read(fileName, section);
            if (result.Migrated)
                GetPluginContext().Log(
                    $"DependencyControl imported legacy configuration '{fileName}'.");
            return BuildJson(writer =>
            {
                writer.WriteStartObject();
                writer.WriteNumber("schemaVersion", 1);
                writer.WriteBoolean("exists", result.Exists);
                writer.WriteBoolean("corrupted", result.Corrupted);
                writer.WriteBoolean("migrated", result.Migrated);
                if (result.BackupName.Length > 0)
                    writer.WriteString("backupName", result.BackupName);
                writer.WritePropertyName("value");
                result.Value.WriteTo(writer);
                writer.WriteEndObject();
            });
        }
        finally
        {
            _installGate.Release();
        }
    }

    private async Task<string> WriteConfigurationAsync(
        string requestJson,
        CancellationToken cancellationToken)
    {
        string fileName;
        IReadOnlyList<string> section;
        JsonElement value;
        try
        {
            using JsonDocument document = JsonDocument.Parse(requestJson);
            JsonElement root = document.RootElement;
            fileName = ReadRequiredString(root, "fileName", 256);
            section = ReadConfigurationSection(root);
            if (!root.TryGetProperty("value", out JsonElement rawValue))
                throw new InvalidOperationException(
                    "DependencyControl configuration write requires field 'value'.");
            value = rawValue.Clone();
        }
        catch (JsonException error)
        {
            throw new InvalidOperationException(
                "The DependencyControl configuration write request is not valid JSON.",
                error);
        }

        await _installGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            GetConfigurationStore().Write(fileName, section, value);
            return BuildJson(writer =>
            {
                writer.WriteStartObject();
                writer.WriteNumber("schemaVersion", 1);
                writer.WriteBoolean("written", true);
                writer.WriteEndObject();
            });
        }
        finally
        {
            _installGate.Release();
        }
    }

    private async Task<string> DeleteConfigurationAsync(
        string requestJson,
        CancellationToken cancellationToken)
    {
        (string fileName, IReadOnlyList<string> section) =
            ParseConfigurationLocation(requestJson);
        await _installGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            bool deleted = GetConfigurationStore().Delete(fileName, section);
            return BuildJson(writer =>
            {
                writer.WriteStartObject();
                writer.WriteNumber("schemaVersion", 1);
                writer.WriteBoolean("deleted", deleted);
                writer.WriteEndObject();
            });
        }
        finally
        {
            _installGate.Release();
        }
    }

    private DependencyControlConfigurationStore GetConfigurationStore()
    {
        lock (_gate)
            return _configurationStore ?? throw new InvalidOperationException(
                "DependencyControl configuration is unavailable before activation.");
    }

    private IAegisubPluginContext GetPluginContext()
    {
        lock (_gate)
            return _context ?? throw new InvalidOperationException(
                "DependencyControl is not attached to the Aegisub host.");
    }

    private DependencyControlInstalledStateStore GetInstalledStateStore()
    {
        lock (_gate)
            return _installedStateStore ?? throw new InvalidOperationException(
                "DependencyControl installed state is unavailable before activation.");
    }

    private DependencyControlInstallJournal GetInstallJournal()
    {
        lock (_gate)
            return _installJournal ?? throw new InvalidOperationException(
                "DependencyControl install journal is unavailable before activation.");
    }

    private DependencyControlFeedStore GetFeedStore()
    {
        lock (_gate)
            return _feedStore ?? throw new InvalidOperationException(
                "DependencyControl feed settings are unavailable before activation.");
    }

    private DependencyControlLogStore GetLogStore()
    {
        lock (_gate)
            return _logStore ?? throw new InvalidOperationException(
                "DependencyControl logs are unavailable before activation.");
    }

    private void InvalidateCatalogSnapshot()
    {
        _catalogSnapshot = null;
        _catalogSnapshotFeedRevision = -1;
    }

    private static (string FileName, IReadOnlyList<string> Section)
        ParseConfigurationLocation(string requestJson)
    {
        try
        {
            using JsonDocument document = JsonDocument.Parse(requestJson);
            JsonElement root = document.RootElement;
            return (
                ReadRequiredString(root, "fileName", 256),
                ReadConfigurationSection(root));
        }
        catch (JsonException error)
        {
            throw new InvalidOperationException(
                "The DependencyControl configuration request is not valid JSON.",
                error);
        }
    }

    private static IReadOnlyList<string> ReadConfigurationSection(JsonElement root)
    {
        if (!root.TryGetProperty("section", out JsonElement value))
            return [];
        if (value.ValueKind != JsonValueKind.Array || value.GetArrayLength() > 16)
            throw new InvalidOperationException(
                "DependencyControl configuration section must be a bounded array.");
        List<string> section = new(value.GetArrayLength());
        foreach (JsonElement item in value.EnumerateArray())
        {
            if (item.ValueKind != JsonValueKind.String ||
                string.IsNullOrEmpty(item.GetString()) ||
                item.GetString()!.Length > 256)
                throw new InvalidOperationException(
                    "DependencyControl configuration section keys must be bounded strings.");
            section.Add(item.GetString()!);
        }
        return section;
    }

    private async Task<string> TestNetworkAsync(
        string requestJson,
        CancellationToken cancellationToken)
    {
        string url;
        try
        {
            using JsonDocument document = JsonDocument.Parse(requestJson);
            url = ReadRequiredString(document.RootElement, "url", MaxTextLength);
        }
        catch (JsonException error)
        {
            throw new InvalidOperationException(
                "The DependencyControl network test request is not valid JSON.",
                error);
        }

        await _installGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            byte[] response = await _transport.GetBytesAsync(
                url, 64 * 1024, cancellationToken).ConfigureAwait(false);
            DependencyControlNetworkSettings settings = _transport.Settings;
            return BuildJson(writer =>
            {
                writer.WriteStartObject();
                writer.WriteNumber("schemaVersion", 1);
                writer.WriteBoolean("success", true);
                writer.WriteString("mode", settings.Mode.ToString());
                writer.WriteNumber("responseBytes", response.Length);
                writer.WriteEndObject();
            });
        }
        finally
        {
            _installGate.Release();
        }
    }

    private static (string RecordType, string Namespace, string Feed)
        ParseCatalogLocation(string requestJson)
    {
        try
        {
            using JsonDocument document = JsonDocument.Parse(requestJson);
            JsonElement root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object)
                throw new InvalidOperationException(
                    "A DependencyControl catalog.lookup request must be an object.");
            string recordType = ReadRequiredString(root, "recordType", 32);
            string packageNamespace = ReadRequiredString(root, "namespace", 512);
            ValidateCatalogIdentity(recordType, packageNamespace);
            string feed = ReadOptionalString(root, "feed", MaxTextLength) ?? "";
            ValidateOptionalHttpUrl(feed, "catalog feed");
            return (recordType, packageNamespace, feed);
        }
        catch (JsonException error)
        {
            throw new InvalidOperationException(
                "The DependencyControl catalog.lookup request is not valid JSON.",
                error);
        }
    }

    private static void ValidateCatalogIdentity(string recordType, string packageNamespace)
    {
        if (recordType is not ("automation" or "module"))
            throw new InvalidOperationException(
                "DependencyControl catalog recordType is invalid.");
        if (!IsSafeNamespace(packageNamespace))
            throw new InvalidOperationException(
                "DependencyControl catalog namespace is invalid.");
    }

    private static string BuildCatalogResponse(
        string recordType,
        DependencyControlFeed source,
        DependencyControlPackage package,
        int channelOffset,
        int channelLimit) => BuildJson(writer =>
    {
        DependencyControlChannel[] channels = package.Channels.Values
            .OrderBy(channel => channel.Name, StringComparer.Ordinal)
            .ToArray();
        writer.WriteStartObject();
        writer.WriteNumber("schemaVersion", 1);
        writer.WriteString("recordType", recordType);
        writer.WriteString("namespace", package.Namespace);
        writer.WriteString("name", package.Name);
        writer.WriteString("description", package.Description);
        writer.WriteString("author", package.Author);
        writer.WriteString("url", package.Url);
        writer.WriteString("feed", source.SourceUrl);
        writer.WriteNumber("channelCount", channels.Length);
        writer.WriteNumber("channelOffset", channelOffset);
        writer.WritePropertyName("channels");
        writer.WriteStartArray();
        foreach (DependencyControlChannel channel in channels
                     .Skip(channelOffset)
                     .Take(channelLimit))
        {
            writer.WriteStartObject();
            writer.WriteString("name", channel.Name);
            writer.WriteString("version", channel.Version);
            writer.WriteString("released", channel.Released);
            writer.WriteBoolean("default", channel.IsDefault);
            writer.WriteBoolean(
                "platformSupported",
                channel.Platforms.Count == 0 || channel.Platforms.Any(IsPlatformCompatible));
            writer.WritePropertyName("platforms");
            writer.WriteStartArray();
            foreach (string platform in channel.Platforms)
                writer.WriteStringValue(platform);
            writer.WriteEndArray();
            writer.WriteNumber("platformCount", channel.Platforms.Count);
            writer.WriteNumber("fileCount", channel.Files.Count);
            writer.WriteNumber("requiredModuleCount", channel.RequiredModules.Count);
            writer.WriteEndObject();
        }
        writer.WriteEndArray();
        writer.WriteEndObject();
    });

    private static void WriteConfiguredFeed(
        Utf8JsonWriter writer,
        DependencyControlConfiguredFeed feed)
    {
        writer.WriteStartObject();
        WriteConfiguredFeedProperties(writer, feed);
        writer.WriteEndObject();
    }

    private static void WriteConfiguredFeedProperties(
        Utf8JsonWriter writer,
        DependencyControlConfiguredFeed feed)
    {
        writer.WriteString("id", feed.Id);
        writer.WriteString("label", feed.Label);
        writer.WriteString("url", feed.Url);
        writer.WriteBoolean("enabled", feed.Enabled);
    }

    private static void WriteCatalogSummary(
        Utf8JsonWriter writer,
        DependencyControlCatalogPackage item,
        DependencyControlInstalledStateStore installedState)
    {
        DependencyControlChannel? preferred = SelectPreferredChannel(item.Package);
        bool supported = preferred is null || preferred.Platforms.Count == 0 ||
            preferred.Platforms.Any(IsPlatformCompatible);
        bool installed = installedState.TryGet(
            item.RecordType,
            item.Package.Namespace,
            out DependencyControlInstalledPackage? installedPackage) &&
            installedPackage is not null;
        string status = installed
            ? "installed"
            : preferred is null ? "chooseChannel" : supported ? "available" :
                "unsupportedPlatform";

        writer.WriteStartObject();
        writer.WriteString("recordType", item.RecordType);
        writer.WriteString("namespace", item.Package.Namespace);
        writer.WriteString("name", item.Package.Name);
        writer.WriteString("description", item.Package.Description);
        writer.WriteString("author", item.Package.Author);
        writer.WriteString("url", item.Package.Url);
        writer.WriteString("feed", item.Feed.SourceUrl);
        writer.WriteNumber("channelCount", item.Package.Channels.Count);
        writer.WriteString("channel", preferred?.Name ?? "");
        writer.WriteString("availableVersion", preferred?.Version ?? "");
        writer.WriteString("released", preferred?.Released ?? "");
        writer.WriteBoolean("platformSupported", supported);
        writer.WriteBoolean("installed", installed);
        writer.WriteString("installedVersion", installedPackage?.Version ?? "");
        writer.WriteString("status", status);
        writer.WriteEndObject();
    }

    private static DependencyControlChannel? SelectPreferredChannel(
        DependencyControlPackage package)
    {
        DependencyControlChannel[] defaults = package.Channels.Values
            .Where(channel => channel.IsDefault)
            .ToArray();
        if (defaults.Length == 1)
            return defaults[0];
        return package.Channels.Count == 1 ? package.Channels.Values.Single() : null;
    }

    private static DependencyControlChannel SelectUpdateChannel(
        DependencyControlPackage package,
        string requestedChannel)
    {
        if (requestedChannel.Length > 0)
        {
            if (package.Channels.TryGetValue(
                requestedChannel, out DependencyControlChannel? selected))
                return selected;
            throw new InvalidOperationException(
                $"DependencyControl package '{package.Namespace}' has no channel " +
                $"'{requestedChannel}'.");
        }
        DependencyControlChannel[] defaults = package.Channels.Values
            .Where(channel => channel.IsDefault)
            .ToArray();
        if (defaults.Length == 1)
            return defaults[0];
        if (defaults.Length == 0 && package.Channels.Count == 1)
            return package.Channels.Values.Single();
        throw new InvalidOperationException(
            $"DependencyControl package '{package.Namespace}' requires an explicit channel.");
    }

    private static int CompareVersions(string left, string right)
    {
        int[] leftParts = ParseComparableVersion(left);
        int[] rightParts = ParseComparableVersion(right);
        for (int index = 0; index < 3; ++index)
        {
            int comparison = leftParts[index].CompareTo(rightParts[index]);
            if (comparison != 0)
                return comparison;
        }
        return 0;
    }

    private static int[] ParseComparableVersion(string value)
    {
        string normalized = value.Trim();
        if (normalized.StartsWith('v') || normalized.StartsWith('V'))
            normalized = normalized[1..];
        int suffix = normalized.IndexOfAny(['-', '+']);
        if (suffix >= 0)
            normalized = normalized[..suffix];
        string[] rawParts = normalized.Split('.', StringSplitOptions.None);
        if (rawParts.Length is < 1 or > 3)
            throw new InvalidOperationException(
                $"DependencyControl version '{value}' is not comparable.");
        int[] parts = new int[3];
        for (int index = 0; index < rawParts.Length; ++index)
        {
            if (!int.TryParse(
                rawParts[index],
                NumberStyles.None,
                CultureInfo.InvariantCulture,
                out parts[index]) || parts[index] < 0)
                throw new InvalidOperationException(
                    $"DependencyControl version '{value}' is not comparable.");
        }
        return parts;
    }

    private static bool IsPlatformCompatible(string platform)
    {
        if (platform.Length == 0 ||
            platform.Equals("all", StringComparison.OrdinalIgnoreCase) ||
            platform.Equals("any", StringComparison.OrdinalIgnoreCase))
            return true;
        if (OperatingSystem.IsWindows())
            return platform.Equals("windows", StringComparison.OrdinalIgnoreCase) ||
                platform.Equals("win", StringComparison.OrdinalIgnoreCase);
        if (OperatingSystem.IsLinux())
            return platform.Equals("linux", StringComparison.OrdinalIgnoreCase);
        if (OperatingSystem.IsMacOS())
            return platform.Equals("mac", StringComparison.OrdinalIgnoreCase) ||
                platform.Equals("macos", StringComparison.OrdinalIgnoreCase) ||
                platform.Equals("osx", StringComparison.OrdinalIgnoreCase);
        return false;
    }

    private static RegisteredRecord ParseRecord(JsonElement root)
    {
        if (root.ValueKind != JsonValueKind.Object)
            throw new InvalidOperationException(
                "A DependencyControl registration request must be a JSON object.");

        string recordType = ReadRequiredString(root, "recordType", 32);
        if (recordType is not ("automation" or "module"))
            throw new InvalidOperationException(
                "A DependencyControl recordType must be 'automation' or 'module'.");
        string recordNamespace = ReadRequiredString(root, "namespace", 512);
        if (!IsSafeNamespace(recordNamespace))
            throw new InvalidOperationException(
                $"DependencyControl namespace '{recordNamespace}' is invalid.");
        string name = ReadOptionalString(root, "name", MaxTextLength) ?? recordNamespace;
        string version = ReadVersion(root);
        string description = ReadOptionalString(root, "description", MaxTextLength) ?? "";
        string author = ReadOptionalString(root, "author", MaxTextLength) ?? "";
        string feed = ReadOptionalString(root, "feed", MaxTextLength) ?? "";
        ValidateOptionalHttpUrl(feed, "feed");
        string activeChannel = ReadOptionalString(root, "activeChannel", 256) ?? "";
        string configFile = ReadOptionalString(root, "configFile", 256) ??
            $"{recordNamespace}.json";
        DependencyControlInstalledStateStore.ValidateConfigFile(configFile);
        bool virtualRecord = ReadOptionalBoolean(root, "virtual");
        IReadOnlyList<RequiredModule> requiredModules = ReadRequiredModules(root);
        return new(
            recordType,
            recordNamespace,
            name,
            version,
            description,
            author,
            feed,
            activeChannel,
            configFile,
            virtualRecord,
            requiredModules);
    }

    private static IReadOnlyList<RequiredModule> ReadRequiredModules(JsonElement root)
    {
        if (!root.TryGetProperty("requiredModules", out JsonElement value))
            return [];
        if (value.ValueKind != JsonValueKind.Array)
            throw new InvalidOperationException(
                "DependencyControl requiredModules must be an array.");
        if (value.GetArrayLength() > MaxRequiredModules)
            throw new InvalidOperationException(
                "DependencyControl requiredModules exceeds the transport limit.");

        List<RequiredModule> modules = new(value.GetArrayLength());
        foreach (JsonElement item in value.EnumerateArray())
        {
            if (item.ValueKind != JsonValueKind.Object)
                throw new InvalidOperationException(
                    "Each DependencyControl module requirement must be an object.");
            string moduleName = ReadRequiredString(item, "moduleName", 512);
            if (!IsSafeNamespace(moduleName))
                throw new InvalidOperationException(
                    "DependencyControl required module namespace is invalid.");
            string feed = ReadOptionalString(item, "feed", MaxTextLength) ?? "";
            ValidateOptionalHttpUrl(feed, "required module feed");
            modules.Add(new(
                moduleName,
                ReadOptionalString(item, "version", 256) ?? "",
                feed,
                ReadOptionalString(item, "channel", 256) ?? "",
                ReadOptionalBoolean(item, "optional")));
        }
        return modules;
    }

    private static string ReadVersion(JsonElement root)
    {
        if (!root.TryGetProperty("version", out JsonElement value))
            return "0";
        string version = value.ValueKind switch
        {
            JsonValueKind.String => value.GetString()!,
            JsonValueKind.Number => value.GetRawText(),
            _ => throw new InvalidOperationException(
                "A DependencyControl version must be a string or number.")
        };
        if (version.Length is 0 or > 256)
            throw new InvalidOperationException(
                "A DependencyControl version is empty or exceeds the transport limit.");
        return version;
    }

    private static string ReadRequiredString(JsonElement root, string property, int maximumLength)
    {
        string? value = ReadOptionalString(root, property, maximumLength);
        if (string.IsNullOrWhiteSpace(value))
            throw new InvalidOperationException(
                $"DependencyControl field '{property}' must be a non-empty string.");
        return value;
    }

    private static string? ReadOptionalString(
        JsonElement root,
        string property,
        int maximumLength)
    {
        if (!root.TryGetProperty(property, out JsonElement value) ||
            value.ValueKind == JsonValueKind.Null)
            return null;
        if (value.ValueKind != JsonValueKind.String)
            throw new InvalidOperationException(
                $"DependencyControl field '{property}' must be a string.");
        string result = value.GetString()!;
        if (result.Length > maximumLength)
            throw new InvalidOperationException(
                $"DependencyControl field '{property}' exceeds the transport limit.");
        return result;
    }

    private static bool ReadOptionalBoolean(JsonElement root, string property)
    {
        if (!root.TryGetProperty(property, out JsonElement value))
            return false;
        return value.ValueKind switch
        {
            JsonValueKind.True => true,
            JsonValueKind.False => false,
            _ => throw new InvalidOperationException(
                $"DependencyControl field '{property}' must be boolean.")
        };
    }

    private static int ReadOptionalNonNegativeInt(
        JsonElement root,
        string property,
        int defaultValue,
        int maximumValue)
    {
        if (!root.TryGetProperty(property, out JsonElement value))
            return defaultValue;
        if (!value.TryGetInt32(out int result) || result < 0 || result > maximumValue)
            throw new InvalidOperationException(
                $"DependencyControl field '{property}' must be a bounded non-negative integer.");
        return result;
    }

    private static void ValidateOptionalHttpUrl(string value, string description)
    {
        if (value.Length > 0 &&
            (!Uri.TryCreate(value, UriKind.Absolute, out Uri? uri) ||
             uri.Scheme is not ("http" or "https")))
            throw new InvalidOperationException(
                $"DependencyControl {description} must be an HTTP(S) URL.");
    }

    private static bool IsSafeNamespace(string value)
    {
        bool sawDot = false;
        bool componentHasCharacters = false;
        foreach (char character in value)
        {
            if (character == '.')
            {
                if (!componentHasCharacters)
                    return false;
                sawDot = true;
                componentHasCharacters = false;
                continue;
            }
            bool allowed = character is >= 'a' and <= 'z' or
                >= 'A' and <= 'Z' or
                >= '0' and <= '9' or '-' or '_';
            if (!allowed)
                return false;
            componentHasCharacters = true;
        }
        return sawDot && componentHasCharacters;
    }

    private static string RecordKey(string recordType, string recordNamespace) =>
        $"{recordType}\n{recordNamespace}";

    private static string BuildProbeResponse() => BuildJson(writer =>
    {
        writer.WriteStartObject();
        writer.WriteNumber("schemaVersion", 1);
        writer.WriteString("serviceId", "aegisub.dependency-control");
        writer.WriteString("version", "0.1.0");
        writer.WritePropertyName("operations");
        writer.WriteStartArray();
        foreach (string operation in RegisteredOperations)
            writer.WriteStringValue(operation);
        writer.WriteEndArray();
        writer.WritePropertyName("capabilities");
        writer.WriteStartObject();
        writer.WriteBoolean("recordRegistration", true);
        writer.WriteBoolean("moduleResolution", true);
        writer.WriteBoolean("packageTransactions", true);
        writer.WriteBoolean("knownFeedDiscovery", true);
        writer.WriteBoolean("proxyConfiguration", true);
        writer.WriteBoolean("configurationPersistence", true);
        writer.WriteBoolean("installedStatePersistence", true);
        writer.WriteBoolean("catalogLookup", true);
        writer.WriteBoolean("catalogBrowsing", true);
        writer.WriteBoolean("feedConfiguration", true);
        writer.WriteBoolean("packageInstall", true);
        writer.WriteBoolean("updateFeed", true);
        writer.WriteBoolean("fileLogging", true);
        writer.WriteBoolean("updateChecks", true);
        writer.WriteBoolean("updateApplication", true);
        writer.WriteBoolean("packageUninstall", true);
        writer.WriteEndObject();
        writer.WriteEndObject();
    });

    private static string BuildUpdateApplyResponse(
        string status,
        bool committed,
        string recordType,
        string packageNamespace,
        string installedVersion,
        string availableVersion,
        string channel,
        string feed,
        string automationRoot,
        IReadOnlyList<string> installedFiles) => BuildJson(writer =>
    {
        writer.WriteStartObject();
        writer.WriteNumber("schemaVersion", 1);
        writer.WriteString("status", status);
        writer.WriteBoolean("committed", committed);
        writer.WriteString("recordType", recordType);
        writer.WriteString("namespace", packageNamespace);
        writer.WriteString("installedVersion", installedVersion);
        writer.WriteString("availableVersion", availableVersion);
        writer.WriteString("channel", channel);
        writer.WriteString("feed", feed);
        writer.WriteBoolean("platformSupported", status != "unsupportedPlatform");
        writer.WriteString("automationRoot", automationRoot);
        writer.WritePropertyName("installedFiles");
        writer.WriteStartArray();
        foreach (string target in installedFiles)
            writer.WriteStringValue(target);
        writer.WriteEndArray();
        writer.WriteEndObject();
    });

    private static string BuildBatchUpdateResponse(
        IReadOnlyList<BatchUpdateItem> updates,
        bool batchRequest,
        bool committed,
        string automationRoot,
        IReadOnlyList<string> installedFiles,
        IReadOnlyList<DependencyControlResolvedPackage>? committedPackages)
    {
        Dictionary<string, DependencyControlResolvedPackage> actualPackages =
            committedPackages is null
                ? new(StringComparer.Ordinal)
                : committedPackages.ToDictionary(
                    package => RecordKey(package.RecordType, package.Namespace),
                    StringComparer.Ordinal);
        if (!batchRequest)
        {
            BatchUpdateItem item = updates[0];
            actualPackages.TryGetValue(
                RecordKey(item.Request.RecordType, item.Request.Namespace),
                out DependencyControlResolvedPackage? actual);
            return BuildUpdateApplyResponse(
                committed && item.Status == "updateAvailable" ? "updated" : item.Status,
                committed,
                item.Request.RecordType,
                item.Request.Namespace,
                item.Installed.Version,
                actual?.Version ?? item.Channel.Version,
                actual?.Channel ?? item.Channel.Name,
                actual?.Feed ?? item.Source.SourceUrl,
                automationRoot,
                installedFiles);
        }

        return BuildJson(writer =>
        {
            writer.WriteStartObject();
            writer.WriteNumber("schemaVersion", 1);
            writer.WriteString("status", committed ? "updated" : BatchStatus(updates));
            writer.WriteBoolean("committed", committed);
            writer.WriteBoolean("atomic", true);
            writer.WriteString("automationRoot", automationRoot);
            writer.WriteNumber("packageCount", updates.Count);
            writer.WriteNumber(
                "updatedCount",
                committed ? updates.Count(item => item.Status == "updateAvailable") : 0);
            writer.WritePropertyName("packages");
            writer.WriteStartArray();
            foreach (BatchUpdateItem item in updates)
            {
                string status = committed && item.Status == "updateAvailable"
                    ? "updated"
                    : item.Status;
                actualPackages.TryGetValue(
                    RecordKey(item.Request.RecordType, item.Request.Namespace),
                    out DependencyControlResolvedPackage? actual);
                writer.WriteStartObject();
                writer.WriteString("status", status);
                writer.WriteString("recordType", item.Request.RecordType);
                writer.WriteString("namespace", item.Request.Namespace);
                writer.WriteString("installedVersion", item.Installed.Version);
                writer.WriteString("availableVersion", actual?.Version ?? item.Channel.Version);
                writer.WriteString("channel", actual?.Channel ?? item.Channel.Name);
                writer.WriteString("feed", actual?.Feed ?? item.Source.SourceUrl);
                writer.WriteBoolean("platformSupported", status != "unsupportedPlatform");
                writer.WriteEndObject();
            }
            writer.WriteEndArray();
            writer.WritePropertyName("installedFiles");
            writer.WriteStartArray();
            foreach (string target in installedFiles)
                writer.WriteStringValue(target);
            writer.WriteEndArray();
            writer.WriteEndObject();
        });
    }

    private static string BatchStatus(IReadOnlyList<BatchUpdateItem> updates)
    {
        string[] statuses = updates
            .Select(item => item.Status)
            .Distinct(StringComparer.Ordinal)
            .ToArray();
        return statuses.Length == 1 ? statuses[0] : "mixed";
    }

    private static string BuildEmptyEnsureResponse() => BuildJson(writer =>
    {
        writer.WriteStartObject();
        writer.WriteNumber("schemaVersion", 1);
        writer.WriteBoolean("committed", false);
        writer.WriteString("automationRoot", "");
        writer.WritePropertyName("modules");
        writer.WriteStartArray();
        writer.WriteEndArray();
        writer.WritePropertyName("installedFiles");
        writer.WriteStartArray();
        writer.WriteEndArray();
        writer.WriteEndObject();
    });

    private static void ExitAfterCommitForTest()
    {
        if (Environment.GetEnvironmentVariable(
                "AEGISUB_DEPENDENCY_CONTROL_TEST_EXIT_AFTER_COMMIT") == "1")
            Environment.Exit(86);
    }

    internal static string BuildJson(Action<Utf8JsonWriter> write)
    {
        using MemoryStream stream = new();
        using (Utf8JsonWriter writer = new(stream, new JsonWriterOptions
        {
            Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping
        }))
            write(writer);
        return System.Text.Encoding.UTF8.GetString(stream.ToArray());
    }

    public void Dispose()
    {
        lock (_gate)
        {
            if (_disposed)
                return;
            _disposed = true;
        }
        _transport.Dispose();
        _installGate.Dispose();
    }

    private sealed record UpdateRequest(
        string RecordType,
        string Namespace,
        string Channel,
        string TargetVersion);

    private sealed record BatchUpdateItem(
        UpdateRequest Request,
        DependencyControlInstalledPackage Installed,
        DependencyControlFeed Source,
        DependencyControlChannel Channel,
        string Status);
}

internal sealed record RegisteredRecord(
    string RecordType,
    string Namespace,
    string Name,
    string Version,
    string Description,
    string Author,
    string Feed,
    string ActiveChannel,
    string ConfigFile,
    bool Virtual,
    IReadOnlyList<RequiredModule> RequiredModules);

internal sealed record RequiredModule(
    string ModuleName,
    string Version,
    string Feed,
    string Channel,
    bool Optional);
