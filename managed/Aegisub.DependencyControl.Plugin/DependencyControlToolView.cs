using System.Text.Json;
using Aegisub.Managed.Contracts;

namespace Aegisub.DependencyControl.Plugin;

internal sealed class DependencyControlToolViewController(
    DependencyControlServiceContribution service)
{
    internal const string ViewId = "aegisub.dependency-control.package-manager";
    internal const string MacroId = "aegisub.dependency-control.package-manager";

    private readonly Dictionary<string, PackageViewModel> _packages =
        new(StringComparer.Ordinal);
    private readonly Dictionary<string, AvailablePackageViewModel> _availablePackages =
        new(StringComparer.Ordinal);
    private readonly Dictionary<string, FeedViewModel> _feeds =
        new(StringComparer.Ordinal);
    private readonly List<LogViewModel> _logs = [];
    private readonly HashSet<string> _selectedPackageRowIds = new(StringComparer.Ordinal);
    private readonly object _operationGate = new();
    private IAegisubPluginContext? _context;
    private DependencyControlNetworkSettings _network =
        DependencyControlNetworkSettings.Default;
    private string _search = "";
    private string _availableSearch = "";
    private string _scope = "all";
    private string _testUrl = "";
    private string? _selectedRowId;
    private string? _selectedAvailableRowId;
    private string? _selectedFeedRowId;
    private string _activeTabId = "installed";
    private CancellationTokenSource? _operationCancellation;
    private Task? _operationTask;
    private long _revision;
    private bool _open;

    public void Activate(IAegisubPluginContext context)
    {
        ArgumentNullException.ThrowIfNull(context);
        _context = context;
    }

    public void Deactivate()
    {
        Task? operation;
        lock (_operationGate)
        {
            _operationCancellation?.Cancel();
            operation = _operationTask;
        }
        if (operation is not null)
        {
            try
            {
                operation.GetAwaiter().GetResult();
            }
            catch (OperationCanceledException)
            {
            }
        }
        _context = null;
        _packages.Clear();
        _availablePackages.Clear();
        _feeds.Clear();
        _logs.Clear();
        _selectedRowId = null;
        _selectedPackageRowIds.Clear();
        _selectedAvailableRowId = null;
        _selectedFeedRowId = null;
        _open = false;
        _revision = 0;
    }

    public async ValueTask<ToolViewOperationResult> OpenAsync(
        CancellationToken cancellationToken)
    {
        IAegisubPluginContext context = RequireContext();
        await ReloadPackagesAsync(cancellationToken).ConfigureAwait(false);
        await ReloadFeedsAsync(cancellationToken).ConfigureAwait(false);
        await ReloadAvailableAsync(refresh: false, cancellationToken).ConfigureAwait(false);
        await ReloadLogsAsync(cancellationToken).ConfigureAwait(false);
        await LoadNetworkSettingsAsync(cancellationToken).ConfigureAwait(false);
        if (!_open)
            _revision = 0;

        ToolViewOperationResult result = Deserialize(
            await context.InvokeHostServiceAsync(
                "aegisub.ui.openToolView",
                JsonSerializer.Serialize(
                    new OpenToolViewRequest(CreateDefinition()),
                    DeclarativeUiJsonContext.Default.OpenToolViewRequest),
                cancellationToken).ConfigureAwait(false),
            DeclarativeUiJsonContext.Default.ToolViewOperationResult,
            "ToolView result");
        _open = !string.Equals(result.Status, "unavailable", StringComparison.Ordinal);
        if (string.Equals(result.Status, "alreadyOpen", StringComparison.Ordinal))
            await PatchAllAsync("Package list refreshed.", cancellationToken)
                .ConfigureAwait(false);
        return result;
    }

    public async ValueTask HandleEventAsync(
        PluginEvent pluginEvent,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(pluginEvent);
        if (!string.Equals(
                pluginEvent.Id,
                "aegisub.ui.toolViewEvent",
                StringComparison.Ordinal))
            return;

        ToolViewEvent toolEvent = Deserialize(
            pluginEvent.PayloadJson,
            DeclarativeUiJsonContext.Default.ToolViewEvent,
            "ToolView event");
        if (!string.Equals(toolEvent.ViewId, ViewId, StringComparison.Ordinal))
            return;
        if (string.Equals(toolEvent.EventId, "closed", StringComparison.Ordinal))
        {
            CancelCurrentOperation();
            _open = false;
            _selectedRowId = null;
            _selectedAvailableRowId = null;
            _selectedFeedRowId = null;
            return;
        }

        if (IsOperationRunning())
        {
            if (string.Equals(toolEvent.EventId, "action", StringComparison.Ordinal) &&
                string.Equals(toolEvent.SourceId, "cancel-operation", StringComparison.Ordinal))
            {
                CancelCurrentOperation();
                await PatchStatusAsync("Cancelling current operation...", CancellationToken.None)
                    .ConfigureAwait(false);
            }
            return;
        }

        ReadViewState(toolEvent);
        try
        {
            if (string.Equals(toolEvent.EventId, "change", StringComparison.Ordinal))
            {
                await HandleChangeAsync(toolEvent.SourceId, cancellationToken)
                    .ConfigureAwait(false);
                return;
            }
            if (string.Equals(
                    toolEvent.EventId,
                    "selectionChanged",
                    StringComparison.Ordinal))
            {
                await PatchStatusAsync(SelectionStatus(), cancellationToken)
                    .ConfigureAwait(false);
                return;
            }
            if (!string.Equals(toolEvent.EventId, "action", StringComparison.Ordinal))
                return;

            await HandleActionAsync(toolEvent.SourceId, cancellationToken)
                .ConfigureAwait(false);
        }
        catch (Exception error) when (error is not OperationCanceledException)
        {
            RequireContext().Log(
                $"DependencyControl package-manager action failed: {error.Message}");
            await PatchStatusAsync($"Error: {error.Message}", CancellationToken.None)
                .ConfigureAwait(false);
        }
    }

    private async ValueTask HandleChangeAsync(
        string sourceId,
        CancellationToken cancellationToken)
    {
        switch (sourceId)
        {
            case "search":
            case "scope":
                await PatchRowsAsync("Package filter updated.", cancellationToken)
                    .ConfigureAwait(false);
                break;
            case "available-search":
                await PatchAvailableRowsAsync(
                    "Available package filter updated.", cancellationToken)
                    .ConfigureAwait(false);
                break;
            case "proxy-mode":
                await PatchNetworkControlsAsync(
                    "Network settings are not saved.", cancellationToken)
                    .ConfigureAwait(false);
                break;
        }
    }

    private async ValueTask HandleActionAsync(
        string sourceId,
        CancellationToken cancellationToken)
    {
        switch (sourceId)
        {
            case "refresh":
                await ReloadPackagesAsync(cancellationToken).ConfigureAwait(false);
                await PatchAllAsync("Package list refreshed.", cancellationToken)
                    .ConfigureAwait(false);
                break;
            case "check":
                await StartOperationAsync(
                    "Checking selected packages...",
                    CheckSelectedPackagesAsync,
                    cancellationToken).ConfigureAwait(false);
                break;
            case "apply":
                await StartOperationAsync(
                    "Updating selected packages...",
                    UpdateSelectedPackagesAsync,
                    cancellationToken).ConfigureAwait(false);
                break;
            case "apply-all":
                await StartOperationAsync(
                    "Checking and updating all packages...",
                    UpdateAllPackagesAsync,
                    cancellationToken).ConfigureAwait(false);
                break;
            case "uninstall":
                await UninstallSelectedAsync(cancellationToken).ConfigureAwait(false);
                break;
            case "available-refresh":
                await StartOperationAsync(
                    "Refreshing available catalog...",
                    RefreshAvailableAsync,
                    cancellationToken).ConfigureAwait(false);
                break;
            case "available-install":
                await StartOperationAsync(
                    "Preparing package installation...",
                    InstallAvailableAsync,
                    cancellationToken).ConfigureAwait(false);
                break;
            case "feeds-refresh":
                await ReloadFeedsAsync(cancellationToken).ConfigureAwait(false);
                await PatchFeedRowsAsync("Configured feeds refreshed.", cancellationToken)
                    .ConfigureAwait(false);
                break;
            case "feed-add":
                await EditFeedAsync(null, cancellationToken).ConfigureAwait(false);
                break;
            case "feed-edit":
                await EditFeedAsync(RequireSelectedFeed(), cancellationToken)
                    .ConfigureAwait(false);
                break;
            case "feed-remove":
                await RemoveSelectedFeedAsync(cancellationToken).ConfigureAwait(false);
                break;
            case "feed-test":
                await StartOperationAsync(
                    "Testing configured feed...",
                    TestSelectedFeedAsync,
                    cancellationToken).ConfigureAwait(false);
                break;
            case "logs-refresh":
                await ReloadLogsAsync(cancellationToken).ConfigureAwait(false);
                await PatchLogRowsAsync("Logs refreshed.", cancellationToken)
                    .ConfigureAwait(false);
                break;
            case "logs-clear":
                await ClearLogsAsync(cancellationToken).ConfigureAwait(false);
                break;
            case "save-network":
                await SaveNetworkAsync(cancellationToken).ConfigureAwait(false);
                break;
            case "test-network":
                await StartOperationAsync(
                    "Testing network settings...",
                    TestNetworkAsync,
                    cancellationToken).ConfigureAwait(false);
                break;
            case "cancel-operation":
                await PatchStatusAsync("No operation is running.", cancellationToken)
                    .ConfigureAwait(false);
                break;
        }
    }

    private Task CheckSelectedPackagesAsync(CancellationToken cancellationToken) =>
        CheckPackagesAsync(SelectedPackages(), cancellationToken);

    private async Task CheckPackagesAsync(
        IReadOnlyList<PackageViewModel> packages,
        CancellationToken cancellationToken)
    {
        int checkedCount = 0;
        for (int index = 0; index < packages.Count; ++index)
        {
            cancellationToken.ThrowIfCancellationRequested();
            PackageViewModel package = packages[index];
            await PatchProgressAsync(
                index,
                packages.Count,
                $"Checking {package.DisplayName}...",
                cancellationToken).ConfigureAwait(false);
            if (package.Feed.Length == 0)
            {
                _packages[package.RowId] = package with { UpdateStatus = "noFeed" };
                continue;
            }
            using JsonDocument response = JsonDocument.Parse(await service.InvokeAsync(
                "updates.check",
                BuildPackageRequest(package),
                cancellationToken).ConfigureAwait(false));
            string status = RequiredString(response.RootElement, "status");
            string available = RequiredString(response.RootElement, "availableVersion");
            _packages[package.RowId] = package with
            {
                AvailableVersion = available,
                UpdateStatus = status
            };
            ++checkedCount;
            await PatchProgressRowsAsync(
                index + 1,
                packages.Count,
                $"Checked {package.DisplayName}: {DescribeUpdateStatus(status, available)}",
                cancellationToken).ConfigureAwait(false);
        }
        await PatchRowsAsync(
            $"Checked {checkedCount} package(s).", cancellationToken).ConfigureAwait(false);
    }

    private Task UpdateSelectedPackagesAsync(CancellationToken cancellationToken) =>
        UpdatePackagesAsync(SelectedPackages(), cancellationToken);

    private Task UpdateAllPackagesAsync(CancellationToken cancellationToken) =>
        UpdateAllPackagesAtomicallyAsync(cancellationToken);

    private async Task UpdateAllPackagesAtomicallyAsync(
        CancellationToken cancellationToken)
    {
        PackageViewModel[] packages = _packages.Values
            .Where(package => package.Feed.Length > 0)
            .OrderBy(package => package.RecordType, StringComparer.Ordinal)
            .ThenBy(package => package.Namespace, StringComparer.Ordinal)
            .ToArray();
        if (packages.Length == 0)
            throw new InvalidOperationException("No updatable packages were found.");

        try
        {
            await PatchProgressAsync(
                0, packages.Length, "Checking packages...", cancellationToken)
                .ConfigureAwait(false);
            using JsonDocument response = JsonDocument.Parse(await service.InvokeAsync(
                "updates.apply",
                BuildBatchPackageRequest(packages),
                cancellationToken).ConfigureAwait(false));
            JsonElement root = response.RootElement;
            bool committed = root.GetProperty("committed").GetBoolean();
            if (!root.TryGetProperty("packages", out JsonElement results) ||
                results.ValueKind != JsonValueKind.Array)
                throw new InvalidOperationException(
                    "DependencyControl batch update response is missing package results.");
            Dictionary<string, JsonElement> byKey = new(StringComparer.Ordinal);
            foreach (JsonElement item in results.EnumerateArray())
            {
                string key = $"{RequiredString(item, "recordType")}\n" +
                    RequiredString(item, "namespace");
                byKey[key] = item;
            }
            foreach (PackageViewModel package in packages)
            {
                string key = $"{package.RecordType}\n{package.Namespace}";
                if (!byKey.TryGetValue(key, out JsonElement item))
                    continue;
                string status = RequiredString(item, "status");
                string available = RequiredString(item, "availableVersion");
                _packages[package.RowId] = package with
                {
                    Version = committed && status == "updated"
                        ? available
                        : package.Version,
                    AvailableVersion = available,
                    UpdateStatus = status
                };
            }
            await PatchProgressRowsAsync(
                packages.Length,
                packages.Length,
                committed ? "Update All committed atomically." : "All packages are current.",
                cancellationToken).ConfigureAwait(false);
            await ReloadPackagesAsync(cancellationToken).ConfigureAwait(false);
            await ReloadAvailableAsync(refresh: false, cancellationToken)
                .ConfigureAwait(false);
            bool hasUnsupported = byKey.Values.Any(item =>
                RequiredString(item, "status") == "unsupportedPlatform");
            bool hasInstalledNewer = byKey.Values.Any(item =>
                RequiredString(item, "status") == "installedNewer");
            await PatchAllAsync(
                committed
                    ? "Updated all packages in one transaction; reload Automation to use them."
                    : hasUnsupported
                        ? "No updates applied; some packages are unsupported on this platform."
                        : hasInstalledNewer
                            ? "No updates applied; some installed versions are newer."
                            : "All selected packages are current.",
                cancellationToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            await ReloadPackagesAsync(CancellationToken.None).ConfigureAwait(false);
            await ReloadAvailableAsync(refresh: false, CancellationToken.None)
                .ConfigureAwait(false);
            throw;
        }
    }

    private async Task UpdatePackagesAsync(
        IReadOnlyList<PackageViewModel> packages,
        CancellationToken cancellationToken)
    {
        if (packages.Count == 0)
            throw new InvalidOperationException("No updatable packages were selected.");
        int updated = 0;
        try
        {
            for (int index = 0; index < packages.Count; ++index)
            {
                cancellationToken.ThrowIfCancellationRequested();
                PackageViewModel package = packages[index];
                await PatchProgressAsync(
                    index,
                    packages.Count,
                    $"Checking {package.DisplayName}...",
                    cancellationToken).ConfigureAwait(false);
                if (package.Feed.Length == 0)
                    continue;
                using JsonDocument check = JsonDocument.Parse(await service.InvokeAsync(
                    "updates.check",
                    BuildPackageRequest(package),
                    cancellationToken).ConfigureAwait(false));
                string checkStatus = RequiredString(check.RootElement, "status");
                string available = RequiredString(check.RootElement, "availableVersion");
                if (checkStatus == "updateAvailable")
                {
                    await PatchProgressAsync(
                        index,
                        packages.Count,
                        $"Updating {package.DisplayName}...",
                        cancellationToken).ConfigureAwait(false);
                    using JsonDocument apply = JsonDocument.Parse(await service.InvokeAsync(
                        "updates.apply",
                        BuildPackageRequest(package),
                        cancellationToken).ConfigureAwait(false));
                    if (apply.RootElement.GetProperty("committed").GetBoolean())
                        ++updated;
                    checkStatus = RequiredString(apply.RootElement, "status");
                    available = RequiredString(apply.RootElement, "availableVersion");
                }
                _packages[package.RowId] = package with
                {
                    Version = checkStatus == "updated" ? available : package.Version,
                    AvailableVersion = available,
                    UpdateStatus = checkStatus
                };
                await PatchProgressRowsAsync(
                    index + 1,
                    packages.Count,
                    $"{package.DisplayName}: {DescribeUpdateStatus(checkStatus, available)}",
                    cancellationToken).ConfigureAwait(false);
            }
        }
        catch (OperationCanceledException)
        {
            await ReloadPackagesAsync(CancellationToken.None).ConfigureAwait(false);
            await ReloadAvailableAsync(refresh: false, CancellationToken.None)
                .ConfigureAwait(false);
            throw;
        }
        await ReloadPackagesAsync(cancellationToken).ConfigureAwait(false);
        await ReloadAvailableAsync(refresh: false, cancellationToken).ConfigureAwait(false);
        await PatchAllAsync(
            updated > 0
                ? $"Updated {updated} package(s); reload Automation to use them."
                : "All selected packages are current.",
            cancellationToken).ConfigureAwait(false);
    }

    private async Task RefreshAvailableAsync(CancellationToken cancellationToken)
    {
        await PatchProgressAsync(
            0, 1, "Refreshing available catalog...", cancellationToken)
            .ConfigureAwait(false);
        await ReloadAvailableAsync(refresh: true, cancellationToken).ConfigureAwait(false);
        await PatchAllAsync("Available catalog refreshed.", cancellationToken)
            .ConfigureAwait(false);
    }

    private async Task UninstallSelectedAsync(CancellationToken cancellationToken)
    {
        PackageViewModel package = RequireSelectedPackage();
        if (!string.Equals(package.Source, "transaction", StringComparison.Ordinal))
            throw new InvalidOperationException(
                $"Package '{package.Namespace}' is managed outside DependencyControl.");
        bool? removeConfig = await ConfirmUninstallAsync(package, cancellationToken)
            .ConfigureAwait(false);
        if (removeConfig is null)
        {
            await PatchStatusAsync("Uninstall cancelled.", cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        await PatchStatusAsync(
            $"Uninstalling {package.DisplayName}...", cancellationToken)
            .ConfigureAwait(false);
        await service.InvokeAsync(
            "packages.uninstall",
            BuildPackageRequest(package, removeConfig.Value),
            cancellationToken).ConfigureAwait(false);
        _selectedRowId = null;
        _selectedPackageRowIds.Clear();
        await ReloadPackagesAsync(cancellationToken).ConfigureAwait(false);
        await PatchAllAsync(
            $"Uninstalled {package.DisplayName}; reload Automation to finish.",
            cancellationToken).ConfigureAwait(false);
    }

    private async Task<bool?> ConfirmUninstallAsync(
        PackageViewModel package,
        CancellationToken cancellationToken)
    {
        FormDefinition definition = new(
            $"{ViewId}.uninstall",
            "Uninstall package",
            [
                new UiControlDefinition(
                    "package",
                    UiControlKind.Label,
                    Text: $"{package.DisplayName} {package.Version}",
                    Row: 0,
                    Column: 0),
                new UiControlDefinition(
                    "remove-config",
                    UiControlKind.Checkbox,
                    Label: "Remove package configuration",
                    Checked: true,
                    Row: 1,
                    Column: 0)
            ],
            [
                new UiActionDefinition("uninstall", "Uninstall", IsDefault: true),
                new UiActionDefinition("cancel", "Cancel", IsCancel: true)
            ],
            MinimumWidth: 420,
            MinimumHeight: 170,
            Resizable: false);
        FormResult result = Deserialize(
            await RequireContext().InvokeHostServiceAsync(
                "aegisub.ui.openForm",
                JsonSerializer.Serialize(
                    new OpenFormRequest(definition, ViewId),
                    DeclarativeUiJsonContext.Default.OpenFormRequest),
                cancellationToken).ConfigureAwait(false),
            DeclarativeUiJsonContext.Default.FormResult,
            "uninstall confirmation");
        if (result.Cancelled || !string.Equals(
                result.ActionId, "uninstall", StringComparison.Ordinal))
            return null;
        return ReadBoolean(result.Values, "remove-config", fallback: true);
    }

    private async Task InstallAvailableAsync(CancellationToken cancellationToken)
    {
        AvailablePackageViewModel package = RequireSelectedAvailablePackage();
        if (package.Installed)
            throw new InvalidOperationException(
                $"Package '{package.Namespace}' is already installed.");
        IReadOnlyList<ChannelViewModel> channels = await LoadChannelsAsync(
            package, cancellationToken).ConfigureAwait(false);
        if (channels.Count == 0)
            throw new InvalidOperationException(
                $"Package '{package.Namespace}' has no installable channels.");
        string selectedChannel = package.Channel.Length > 0 &&
            channels.Any(channel => channel.Name == package.Channel)
                ? package.Channel
                : channels.FirstOrDefault(channel => channel.Default)?.Name ?? channels[0].Name;
        FormDefinition definition = new(
            $"{ViewId}.install",
            "Install package",
            [
                new UiControlDefinition(
                    "package",
                    UiControlKind.Label,
                    Text: package.DisplayName,
                    Row: 0,
                    Column: 0),
                new UiControlDefinition(
                    "install-channel",
                    UiControlKind.Select,
                    Label: "Channel",
                    Choices: channels.Select(channel => new UiChoiceDefinition(
                        channel.Name,
                        $"{channel.Name} ({channel.Version})")).ToArray(),
                    SelectedChoiceId: selectedChannel,
                    Row: 1,
                    Column: 0,
                    Required: true)
            ],
            [
                new UiActionDefinition("install", "Install", IsDefault: true),
                new UiActionDefinition("cancel", "Cancel", IsCancel: true)
            ],
            MinimumWidth: 460,
            MinimumHeight: 190,
            Resizable: false);
        FormResult result = await OpenOwnedFormAsync(
            definition, cancellationToken).ConfigureAwait(false);
        if (result.Cancelled || !string.Equals(
                result.ActionId, "install", StringComparison.Ordinal))
            return;
        string channel = ReadString(result.Values, "install-channel", selectedChannel);
        if (!channels.Any(candidate => candidate.Name == channel))
            throw new InvalidOperationException("Select a valid package channel.");

        await PatchStatusAsync(
            $"Installing {package.DisplayName}...", cancellationToken).ConfigureAwait(false);
        string request = DependencyControlServiceContribution.BuildJson(writer =>
        {
            writer.WriteStartObject();
            writer.WriteString("recordType", package.RecordType);
            writer.WriteString("namespace", package.Namespace);
            writer.WriteString("feed", package.Feed);
            writer.WriteString("channel", channel);
            writer.WriteEndObject();
        });
        using JsonDocument response = JsonDocument.Parse(await service.InvokeAsync(
            "packages.install", request, cancellationToken).ConfigureAwait(false));
        bool committed = response.RootElement.GetProperty("committed").GetBoolean();
        string status = RequiredString(response.RootElement, "status");
        await ReloadPackagesAsync(cancellationToken).ConfigureAwait(false);
        await ReloadAvailableAsync(refresh: false, cancellationToken).ConfigureAwait(false);
        await PatchAllAsync(
            committed
                ? $"Installed {package.DisplayName}; reload Automation to use it."
                : DescribeUpdateStatus(
                    status,
                    RequiredString(response.RootElement, "availableVersion")),
            cancellationToken).ConfigureAwait(false);
    }

    private async Task<IReadOnlyList<ChannelViewModel>> LoadChannelsAsync(
        AvailablePackageViewModel package,
        CancellationToken cancellationToken)
    {
        List<ChannelViewModel> channels = [];
        int offset = 0;
        int total;
        do
        {
            string request = DependencyControlServiceContribution.BuildJson(writer =>
            {
                writer.WriteStartObject();
                writer.WriteString("recordType", package.RecordType);
                writer.WriteString("namespace", package.Namespace);
                writer.WriteString("feed", package.Feed);
                writer.WriteNumber("channelOffset", offset);
                writer.WriteNumber("channelLimit", 16);
                writer.WriteEndObject();
            });
            using JsonDocument response = JsonDocument.Parse(await service.InvokeAsync(
                "catalog.lookup", request, cancellationToken).ConfigureAwait(false));
            JsonElement root = response.RootElement;
            total = root.GetProperty("channelCount").GetInt32();
            JsonElement page = root.GetProperty("channels");
            foreach (JsonElement item in page.EnumerateArray())
                channels.Add(new(
                    RequiredString(item, "name"),
                    RequiredString(item, "version"),
                    item.GetProperty("default").GetBoolean()));
            offset = checked(offset + page.GetArrayLength());
        }
        while (offset < total);
        return channels;
    }

    private async Task EditFeedAsync(
        FeedViewModel? feed,
        CancellationToken cancellationToken)
    {
        FormDefinition definition = new(
            $"{ViewId}.feed",
            feed is null ? "Add feed" : "Edit feed",
            [
                new UiControlDefinition(
                    "feed-label", UiControlKind.Text, Label: "Name",
                    Text: feed?.Label ?? "", Row: 0, Column: 0, Required: true),
                new UiControlDefinition(
                    "feed-url", UiControlKind.Text, Label: "URL",
                    Text: feed?.Url ?? "", Row: 1, Column: 0, Required: true),
                new UiControlDefinition(
                    "feed-enabled", UiControlKind.Checkbox, Label: "Enabled",
                    Checked: feed?.Enabled ?? true, Row: 2, Column: 0)
            ],
            [
                new UiActionDefinition("save", "Save", IsDefault: true),
                new UiActionDefinition("cancel", "Cancel", IsCancel: true)
            ],
            MinimumWidth: 620,
            MinimumHeight: 230,
            Resizable: false);
        FormResult result = await OpenOwnedFormAsync(
            definition, cancellationToken).ConfigureAwait(false);
        if (result.Cancelled || !string.Equals(result.ActionId, "save", StringComparison.Ordinal))
            return;
        string label = ReadString(result.Values, "feed-label", "");
        string url = ReadString(result.Values, "feed-url", "");
        bool enabled = ReadBoolean(result.Values, "feed-enabled", fallback: true);
        string request = DependencyControlServiceContribution.BuildJson(writer =>
        {
            writer.WriteStartObject();
            if (feed is not null)
                writer.WriteString("id", feed.Id);
            writer.WriteString("label", label);
            writer.WriteString("url", url);
            writer.WriteBoolean("enabled", enabled);
            writer.WriteEndObject();
        });
        using JsonDocument response = JsonDocument.Parse(await service.InvokeAsync(
            "feeds.upsert", request, cancellationToken).ConfigureAwait(false));
        string savedId = RequiredString(response.RootElement, "id");
        await ReloadFeedsAsync(cancellationToken).ConfigureAwait(false);
        _availablePackages.Clear();
        _selectedAvailableRowId = null;
        _selectedFeedRowId = $"feed:{savedId}";
        await PatchAllAsync(
            "Feed saved. Refresh Available to load its catalog.", cancellationToken)
            .ConfigureAwait(false);
    }

    private async Task RemoveSelectedFeedAsync(CancellationToken cancellationToken)
    {
        FeedViewModel feed = RequireSelectedFeed();
        FormDefinition definition = new(
            $"{ViewId}.remove-feed",
            "Remove feed",
            [new UiControlDefinition(
                "feed", UiControlKind.Label, Text: feed.Label, Row: 0, Column: 0)],
            [
                new UiActionDefinition("remove", "Remove", IsDefault: true),
                new UiActionDefinition("cancel", "Cancel", IsCancel: true)
            ],
            MinimumWidth: 420,
            MinimumHeight: 150,
            Resizable: false);
        FormResult result = await OpenOwnedFormAsync(
            definition, cancellationToken).ConfigureAwait(false);
        if (result.Cancelled || !string.Equals(result.ActionId, "remove", StringComparison.Ordinal))
            return;
        string request = DependencyControlServiceContribution.BuildJson(writer =>
        {
            writer.WriteStartObject();
            writer.WriteString("id", feed.Id);
            writer.WriteEndObject();
        });
        await service.InvokeAsync(
            "feeds.remove", request, cancellationToken).ConfigureAwait(false);
        _selectedFeedRowId = null;
        _availablePackages.Clear();
        _selectedAvailableRowId = null;
        await ReloadFeedsAsync(cancellationToken).ConfigureAwait(false);
        await PatchAllAsync(
            $"Removed feed {feed.Label}.", cancellationToken).ConfigureAwait(false);
    }

    private async Task TestSelectedFeedAsync(CancellationToken cancellationToken)
    {
        FeedViewModel feed = RequireSelectedFeed();
        await PatchStatusAsync(
            $"Testing {feed.Label}...", cancellationToken).ConfigureAwait(false);
        string request = DependencyControlServiceContribution.BuildJson(writer =>
        {
            writer.WriteStartObject();
            writer.WriteString("id", feed.Id);
            writer.WriteEndObject();
        });
        using JsonDocument response = JsonDocument.Parse(await service.InvokeAsync(
            "feeds.test", request, cancellationToken).ConfigureAwait(false));
        await PatchStatusAsync(
            $"{RequiredString(response.RootElement, "name")}: " +
            $"{response.RootElement.GetProperty("macroCount").GetInt32()} Macros, " +
            $"{response.RootElement.GetProperty("moduleCount").GetInt32()} modules.",
            cancellationToken).ConfigureAwait(false);
    }

    private async Task ClearLogsAsync(CancellationToken cancellationToken)
    {
        await service.InvokeAsync(
            "logs.trim", "{\"wipe\":true}", cancellationToken).ConfigureAwait(false);
        await ReloadLogsAsync(cancellationToken).ConfigureAwait(false);
        await PatchLogRowsAsync("Logs cleared.", cancellationToken).ConfigureAwait(false);
    }

    private async Task<FormResult> OpenOwnedFormAsync(
        FormDefinition definition,
        CancellationToken cancellationToken) => Deserialize(
        await RequireContext().InvokeHostServiceAsync(
            "aegisub.ui.openForm",
            JsonSerializer.Serialize(
                new OpenFormRequest(definition, ViewId),
                DeclarativeUiJsonContext.Default.OpenFormRequest),
            cancellationToken).ConfigureAwait(false),
        DeclarativeUiJsonContext.Default.FormResult,
        "form result");

    private async Task SaveNetworkAsync(CancellationToken cancellationToken)
    {
        string request = DependencyControlServiceContribution.BuildJson(writer =>
        {
            writer.WriteStartObject();
            writer.WriteString("mode", _network.Mode.ToString());
            writer.WriteString("manualProxyUri", _network.ManualProxyUri.Trim());
            writer.WritePropertyName("bypass");
            writer.WriteStartArray();
            foreach (string entry in ParseBypass(_network.Bypass))
                writer.WriteStringValue(entry);
            writer.WriteEndArray();
            writer.WriteBoolean(
                "useDefaultCredentials", _network.UseDefaultCredentials);
            writer.WriteEndObject();
        });
        using JsonDocument response = JsonDocument.Parse(await service.InvokeAsync(
            "network.settings.update", request, cancellationToken).ConfigureAwait(false));
        _network = DependencyControlNetworkSettings.Parse(response.RootElement);
        await PatchNetworkControlsAsync(
            $"Saved {_network.Mode} proxy settings.", cancellationToken)
            .ConfigureAwait(false);
    }

    private async Task TestNetworkAsync(CancellationToken cancellationToken)
    {
        if (string.IsNullOrWhiteSpace(_testUrl))
            throw new InvalidOperationException("Enter an HTTP or HTTPS test URL.");
        await PatchStatusAsync(
            $"Testing {_network.Mode} proxy settings...", cancellationToken)
            .ConfigureAwait(false);
        string request = DependencyControlServiceContribution.BuildJson(writer =>
        {
            writer.WriteStartObject();
            writer.WriteString("url", _testUrl.Trim());
            writer.WriteEndObject();
        });
        using JsonDocument response = JsonDocument.Parse(await service.InvokeAsync(
            "network.test", request, cancellationToken).ConfigureAwait(false));
        int bytes = response.RootElement.GetProperty("responseBytes").GetInt32();
        string mode = RequiredString(response.RootElement, "mode");
        await PatchStatusAsync(
            $"Connection succeeded through {mode} mode ({bytes} bytes).",
            cancellationToken).ConfigureAwait(false);
    }

    private async Task ReloadPackagesAsync(CancellationToken cancellationToken)
    {
        Dictionary<string, PackageViewModel> loaded = new(StringComparer.Ordinal);
        int offset = 0;
        int total;
        do
        {
            string request = DependencyControlServiceContribution.BuildJson(writer =>
            {
                writer.WriteStartObject();
                writer.WriteNumber("offset", offset);
                writer.WriteNumber("limit", 16);
                writer.WriteEndObject();
            });
            using JsonDocument response = JsonDocument.Parse(await service.InvokeAsync(
                "installed.list", request, cancellationToken).ConfigureAwait(false));
            JsonElement root = response.RootElement;
            total = root.GetProperty("totalCount").GetInt32();
            JsonElement packages = root.GetProperty("packages");
            foreach (JsonElement item in packages.EnumerateArray())
            {
                PackageViewModel package = ParsePackage(item);
                if (_packages.TryGetValue(package.RowId, out PackageViewModel? previous) &&
                    string.Equals(previous.Version, package.Version, StringComparison.Ordinal))
                    package = package with
                    {
                        AvailableVersion = previous.AvailableVersion,
                        UpdateStatus = previous.UpdateStatus
                    };
                loaded.Add(package.RowId, package);
            }
            offset = checked(offset + packages.GetArrayLength());
        }
        while (offset < total);

        _packages.Clear();
        foreach ((string key, PackageViewModel package) in loaded)
            _packages.Add(key, package);
        if (_selectedRowId is not null && !_packages.ContainsKey(_selectedRowId))
            _selectedRowId = null;
        _selectedPackageRowIds.RemoveWhere(rowId => !_packages.ContainsKey(rowId));
    }

    private async Task ReloadFeedsAsync(CancellationToken cancellationToken)
    {
        Dictionary<string, FeedViewModel> loaded = new(StringComparer.Ordinal);
        int offset = 0;
        int total;
        do
        {
            string request = DependencyControlServiceContribution.BuildJson(writer =>
            {
                writer.WriteStartObject();
                writer.WriteNumber("offset", offset);
                writer.WriteNumber("limit", 64);
                writer.WriteEndObject();
            });
            using JsonDocument response = JsonDocument.Parse(await service.InvokeAsync(
                "feeds.list", request, cancellationToken).ConfigureAwait(false));
            JsonElement root = response.RootElement;
            total = root.GetProperty("totalCount").GetInt32();
            JsonElement feeds = root.GetProperty("feeds");
            foreach (JsonElement item in feeds.EnumerateArray())
            {
                FeedViewModel feed = ParseFeed(item);
                loaded.Add(feed.RowId, feed);
            }
            offset = checked(offset + feeds.GetArrayLength());
        }
        while (offset < total);
        _feeds.Clear();
        foreach ((string key, FeedViewModel feed) in loaded)
            _feeds.Add(key, feed);
        if (_selectedFeedRowId is not null && !_feeds.ContainsKey(_selectedFeedRowId))
            _selectedFeedRowId = null;
    }

    private async Task ReloadAvailableAsync(
        bool refresh,
        CancellationToken cancellationToken)
    {
        Dictionary<string, AvailablePackageViewModel> loaded = new(StringComparer.Ordinal);
        int offset = 0;
        int total;
        bool firstPage = true;
        do
        {
            string request = DependencyControlServiceContribution.BuildJson(writer =>
            {
                writer.WriteStartObject();
                writer.WriteNumber("offset", offset);
                writer.WriteNumber("limit", 64);
                writer.WriteBoolean("refresh", refresh && firstPage);
                writer.WriteEndObject();
            });
            using JsonDocument response = JsonDocument.Parse(await service.InvokeAsync(
                "catalog.list", request, cancellationToken).ConfigureAwait(false));
            JsonElement root = response.RootElement;
            total = root.GetProperty("totalCount").GetInt32();
            JsonElement packages = root.GetProperty("packages");
            foreach (JsonElement item in packages.EnumerateArray())
            {
                AvailablePackageViewModel package = ParseAvailablePackage(item);
                loaded.Add(package.RowId, package);
            }
            offset = checked(offset + packages.GetArrayLength());
            firstPage = false;
        }
        while (offset < total);
        _availablePackages.Clear();
        foreach ((string key, AvailablePackageViewModel package) in loaded)
            _availablePackages.Add(key, package);
        if (_selectedAvailableRowId is not null &&
            !_availablePackages.ContainsKey(_selectedAvailableRowId))
            _selectedAvailableRowId = null;
    }

    private async Task ReloadLogsAsync(CancellationToken cancellationToken)
    {
        List<LogViewModel> loaded = [];
        int offset = 0;
        int total;
        do
        {
            string request = DependencyControlServiceContribution.BuildJson(writer =>
            {
                writer.WriteStartObject();
                writer.WriteNumber("offset", offset);
                writer.WriteNumber("limit", 64);
                writer.WriteEndObject();
            });
            using JsonDocument response = JsonDocument.Parse(await service.InvokeAsync(
                "logs.list", request, cancellationToken).ConfigureAwait(false));
            JsonElement root = response.RootElement;
            total = root.GetProperty("totalCount").GetInt32();
            JsonElement entries = root.GetProperty("entries");
            foreach (JsonElement item in entries.EnumerateArray())
                loaded.Add(ParseLog(item, loaded.Count));
            offset = checked(offset + entries.GetArrayLength());
        }
        while (offset < total);
        _logs.Clear();
        _logs.AddRange(loaded);
    }

    private async Task LoadNetworkSettingsAsync(CancellationToken cancellationToken)
    {
        using JsonDocument response = JsonDocument.Parse(await service.InvokeAsync(
            "network.settings.get", "{}", cancellationToken).ConfigureAwait(false));
        _network = DependencyControlNetworkSettings.Parse(response.RootElement);
    }

    private ToolViewDefinition CreateDefinition() => new(
        ViewId,
        "DependencyControl Package Manager",
        [
            new UiControlDefinition(
                "status", UiControlKind.Label,
                Text: $"{_packages.Count} package(s) installed.",
                Row: 0, Column: 0),
            new UiControlDefinition(
                "operation-progress", UiControlKind.Progress,
                Number: 0, Minimum: 0, Maximum: 100,
                Row: 1, Column: 0, Visible: false)
        ],
        [],
        [
            new UiActionDefinition("cancel-operation", "Cancel"),
            new UiActionDefinition("close", "Close", IsCancel: true)
        ],
        Placement: ToolViewPlacement.Auto,
        MinimumWidth: 1120,
        MinimumHeight: 660,
        Tabs:
        [
            new ToolViewTabDefinition(
                "installed",
                "Installed",
                [
                    new UiControlDefinition(
                        "search", UiControlKind.Text, Label: "Search", Text: _search,
                        Row: 0, Column: 0, ColumnSpan: 2),
                    new UiControlDefinition(
                        "scope", UiControlKind.Select, Label: "Show",
                        Choices:
                        [
                            new UiChoiceDefinition("all", "All packages"),
                            new UiChoiceDefinition("managed", "Managed"),
                            new UiChoiceDefinition("discovered", "Discovered"),
                            new UiChoiceDefinition("updates", "Updates available")
                        ],
                        SelectedChoiceId: _scope,
                        Row: 0, Column: 2)
                ],
                [
                    new ToolViewTableDefinition(
                        "packages",
                        [
                            new ToolViewColumnDefinition("name", "Name", 175),
                            new ToolViewColumnDefinition("namespace", "Namespace", 220),
                            new ToolViewColumnDefinition("type", "Type", 75),
                            new ToolViewColumnDefinition("installed", "Installed", 90),
                            new ToolViewColumnDefinition("available", "Available", 90),
                            new ToolViewColumnDefinition("channel", "Channel", 90),
                            new ToolViewColumnDefinition("source", "Source", 90),
                            new ToolViewColumnDefinition("status", "Status", 150)
                        ],
                        BuildRows(),
                        MultiSelect: true,
                        MinimumHeight: 300)
                ],
                [
                    new UiActionDefinition("refresh", "Refresh"),
                    new UiActionDefinition("check", "Check Selected"),
                    new UiActionDefinition("apply", "Update Selected"),
                    new UiActionDefinition("apply-all", "Update All"),
                    new UiActionDefinition("uninstall", "Uninstall")
                ]),
            new ToolViewTabDefinition(
                "available",
                "Available",
                [
                    new UiControlDefinition(
                        "available-search", UiControlKind.Text, Label: "Search",
                        Text: _availableSearch, Row: 0, Column: 0, ColumnSpan: 3)
                ],
                [
                    new ToolViewTableDefinition(
                        "available-packages",
                        [
                            new ToolViewColumnDefinition("name", "Name", 175),
                            new ToolViewColumnDefinition("namespace", "Namespace", 220),
                            new ToolViewColumnDefinition("type", "Type", 75),
                            new ToolViewColumnDefinition("version", "Version", 90),
                            new ToolViewColumnDefinition("channel", "Channel", 100),
                            new ToolViewColumnDefinition("feed", "Feed", 210),
                            new ToolViewColumnDefinition("status", "Status", 130)
                        ],
                        BuildAvailableRows(),
                        MinimumHeight: 340)
                ],
                [
                    new UiActionDefinition("available-refresh", "Refresh"),
                    new UiActionDefinition("available-install", "Install")
                ]),
            new ToolViewTabDefinition(
                "feeds",
                "Feeds",
                [],
                [
                    new ToolViewTableDefinition(
                        "feed-list",
                        [
                            new ToolViewColumnDefinition("label", "Name", 220),
                            new ToolViewColumnDefinition("url", "URL", 610),
                            new ToolViewColumnDefinition("status", "Status", 100)
                        ],
                        BuildFeedRows(),
                        MinimumHeight: 340)
                ],
                [
                    new UiActionDefinition("feeds-refresh", "Refresh"),
                    new UiActionDefinition("feed-add", "Add"),
                    new UiActionDefinition("feed-edit", "Edit"),
                    new UiActionDefinition("feed-remove", "Remove"),
                    new UiActionDefinition("feed-test", "Test")
                ]),
            new ToolViewTabDefinition(
                "logs",
                "Logs",
                [],
                [
                    new ToolViewTableDefinition(
                        "log-list",
                        [
                            new ToolViewColumnDefinition("time", "Time", 165),
                            new ToolViewColumnDefinition("level", "Level", 80),
                            new ToolViewColumnDefinition("source", "Source", 210),
                            new ToolViewColumnDefinition("message", "Message", 560)
                        ],
                        BuildLogRows(),
                        MinimumHeight: 340)
                ],
                [
                    new UiActionDefinition("logs-refresh", "Refresh"),
                    new UiActionDefinition("logs-clear", "Clear")
                ]),
            new ToolViewTabDefinition(
                "network",
                "Network",
                [
                    new UiControlDefinition(
                        "proxy-mode", UiControlKind.Select, Label: "Proxy",
                        Choices:
                        [
                            new UiChoiceDefinition("System", "System"),
                            new UiChoiceDefinition("Direct", "Direct"),
                            new UiChoiceDefinition("Manual", "Manual")
                        ],
                        SelectedChoiceId: _network.Mode.ToString(),
                        Row: 0, Column: 0),
                    new UiControlDefinition(
                        "proxy-uri", UiControlKind.Text, Label: "Proxy URI",
                        Text: _network.ManualProxyUri,
                        Row: 0, Column: 1, ColumnSpan: 2,
                        Enabled: _network.Mode == DependencyControlProxyMode.Manual),
                    new UiControlDefinition(
                        "proxy-bypass", UiControlKind.Text, Label: "Bypass",
                        Text: string.Join(", ", _network.Bypass),
                        Help: "Comma-separated hosts, *.domain patterns, or <local>.",
                        Row: 1, Column: 0, ColumnSpan: 2,
                        Enabled: _network.Mode == DependencyControlProxyMode.Manual),
                    new UiControlDefinition(
                        "proxy-default-credentials", UiControlKind.Checkbox,
                        Label: "Use current user credentials",
                        Checked: _network.UseDefaultCredentials,
                        Row: 1, Column: 2,
                        Enabled: _network.Mode != DependencyControlProxyMode.Direct),
                    new UiControlDefinition(
                        "test-url", UiControlKind.Text, Label: "Test URL", Text: _testUrl,
                        Row: 2, Column: 0, ColumnSpan: 3)
                ],
                [],
                [
                    new UiActionDefinition("save-network", "Save Proxy"),
                    new UiActionDefinition("test-network", "Test")
                ])
        ]);

    private void ReadViewState(ToolViewEvent toolEvent)
    {
        _search = ReadString(toolEvent.Values, "search", _search);
        _availableSearch = ReadString(
            toolEvent.Values, "available-search", _availableSearch);
        _scope = ReadString(toolEvent.Values, "scope", _scope);
        _testUrl = ReadString(toolEvent.Values, "test-url", _testUrl);
        string mode = ReadString(
            toolEvent.Values, "proxy-mode", _network.Mode.ToString());
        if (Enum.TryParse(mode, ignoreCase: true, out DependencyControlProxyMode parsedMode) &&
            Enum.IsDefined(parsedMode))
            _network = _network with { Mode = parsedMode };
        _network = _network with
        {
            ManualProxyUri = ReadString(
                toolEvent.Values, "proxy-uri", _network.ManualProxyUri),
            Bypass = SplitBypass(ReadString(
                toolEvent.Values,
                "proxy-bypass",
                string.Join(", ", _network.Bypass))),
            UseDefaultCredentials = ReadBoolean(
                toolEvent.Values,
                "proxy-default-credentials",
                _network.UseDefaultCredentials)
        };
        if (toolEvent.ActiveTabId.Length > 0)
            _activeTabId = toolEvent.ActiveTabId;
        switch (_activeTabId)
        {
            case "installed":
                _selectedPackageRowIds.Clear();
                foreach (string rowId in toolEvent.SelectedRowIds.Where(_packages.ContainsKey))
                    _selectedPackageRowIds.Add(rowId);
                _selectedRowId = _selectedPackageRowIds.FirstOrDefault();
                break;
            case "available":
                _selectedAvailableRowId = toolEvent.SelectedRowIds
                    .FirstOrDefault(_availablePackages.ContainsKey);
                break;
            case "feeds":
                _selectedFeedRowId = toolEvent.SelectedRowIds
                    .FirstOrDefault(_feeds.ContainsKey);
                break;
        }
    }

    private bool IsOperationRunning()
    {
        lock (_operationGate)
            return _operationCancellation is not null;
    }

    private void CancelCurrentOperation()
    {
        lock (_operationGate)
            _operationCancellation?.Cancel();
    }

    private async Task StartOperationAsync(
        string status,
        Func<CancellationToken, Task> operation,
        CancellationToken eventCancellationToken)
    {
        CancellationTokenSource cancellation = new();
        TaskCompletionSource completion = new(
            TaskCreationOptions.RunContinuationsAsynchronously);
        lock (_operationGate)
        {
            if (_operationCancellation is not null)
                throw new InvalidOperationException(
                    "A DependencyControl package-manager operation is already running.");
            _operationCancellation = cancellation;
            _operationTask = completion.Task;
        }
        try
        {
            await PatchProgressAsync(
                0, 1, status, eventCancellationToken).ConfigureAwait(false);
        }
        catch
        {
            lock (_operationGate)
            {
                _operationCancellation = null;
                _operationTask = null;
            }
            cancellation.Dispose();
            completion.SetResult();
            throw;
        }

        _ = Task.Run(async () =>
        {
            try
            {
                await operation(cancellation.Token).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (cancellation.IsCancellationRequested)
            {
                await PatchStatusAsync(
                    "Operation cancelled. Completed package commits were preserved.",
                    CancellationToken.None).ConfigureAwait(false);
            }
            catch (Exception error)
            {
                _context?.Log(
                    $"DependencyControl package-manager operation failed: {error.Message}");
                await PatchStatusAsync(
                    $"Error: {error.Message}", CancellationToken.None).ConfigureAwait(false);
            }
            finally
            {
                try
                {
                    await PatchAsync(
                        [new UiControlPatch("operation-progress", Visible: false)],
                        [],
                        null,
                        CancellationToken.None).ConfigureAwait(false);
                }
                finally
                {
                    lock (_operationGate)
                    {
                        if (ReferenceEquals(_operationCancellation, cancellation))
                        {
                            _operationCancellation = null;
                            _operationTask = null;
                        }
                    }
                    cancellation.Dispose();
                    completion.SetResult();
                }
            }
        });
    }

    private async Task PatchAllAsync(
        string status,
        CancellationToken cancellationToken)
    {
        await PatchAsync(
            [
                new UiControlPatch("search", Text: _search),
                new UiControlPatch("available-search", Text: _availableSearch),
                new UiControlPatch("scope", SelectedChoiceId: _scope),
                new UiControlPatch(
                    "proxy-mode", SelectedChoiceId: _network.Mode.ToString()),
                new UiControlPatch(
                    "proxy-uri",
                    Text: _network.ManualProxyUri,
                    Enabled: _network.Mode == DependencyControlProxyMode.Manual),
                new UiControlPatch(
                    "proxy-bypass",
                    Text: string.Join(", ", _network.Bypass),
                    Enabled: _network.Mode == DependencyControlProxyMode.Manual),
                new UiControlPatch(
                    "proxy-default-credentials",
                    Checked: _network.UseDefaultCredentials,
                    Enabled: _network.Mode != DependencyControlProxyMode.Direct),
                new UiControlPatch("test-url", Text: _testUrl),
                new UiControlPatch("status", Text: status)
            ],
            [
                new ToolViewTablePatch("packages", BuildRows()),
                new ToolViewTablePatch("available-packages", BuildAvailableRows()),
                new ToolViewTablePatch("feed-list", BuildFeedRows()),
                new ToolViewTablePatch("log-list", BuildLogRows())
            ],
            SelectedRows(),
            cancellationToken).ConfigureAwait(false);
    }

    private async Task PatchRowsAsync(
        string status,
        CancellationToken cancellationToken)
    {
        await PatchAsync(
            [new UiControlPatch("status", Text: status)],
            [new ToolViewTablePatch("packages", BuildRows())],
            SelectedRows(),
            cancellationToken).ConfigureAwait(false);
    }

    private async Task PatchAvailableRowsAsync(
        string status,
        CancellationToken cancellationToken)
    {
        await PatchAsync(
            [
                new UiControlPatch("available-search", Text: _availableSearch),
                new UiControlPatch("status", Text: status)
            ],
            [new ToolViewTablePatch("available-packages", BuildAvailableRows())],
            SelectedRows(),
            cancellationToken).ConfigureAwait(false);
    }

    private async Task PatchFeedRowsAsync(
        string status,
        CancellationToken cancellationToken)
    {
        await PatchAsync(
            [new UiControlPatch("status", Text: status)],
            [new ToolViewTablePatch("feed-list", BuildFeedRows())],
            SelectedRows(),
            cancellationToken).ConfigureAwait(false);
    }

    private async Task PatchLogRowsAsync(
        string status,
        CancellationToken cancellationToken)
    {
        await PatchAsync(
            [new UiControlPatch("status", Text: status)],
            [new ToolViewTablePatch("log-list", BuildLogRows())],
            null,
            cancellationToken).ConfigureAwait(false);
    }

    private async Task PatchNetworkControlsAsync(
        string status,
        CancellationToken cancellationToken)
    {
        await PatchAsync(
            [
                new UiControlPatch(
                    "proxy-mode", SelectedChoiceId: _network.Mode.ToString()),
                new UiControlPatch(
                    "proxy-uri",
                    Text: _network.ManualProxyUri,
                    Enabled: _network.Mode == DependencyControlProxyMode.Manual),
                new UiControlPatch(
                    "proxy-bypass",
                    Text: string.Join(", ", _network.Bypass),
                    Enabled: _network.Mode == DependencyControlProxyMode.Manual),
                new UiControlPatch(
                    "proxy-default-credentials",
                    Checked: _network.UseDefaultCredentials,
                    Enabled: _network.Mode != DependencyControlProxyMode.Direct),
                new UiControlPatch("status", Text: status)
            ],
            [],
            null,
            cancellationToken).ConfigureAwait(false);
    }

    private Task PatchStatusAsync(string status, CancellationToken cancellationToken) =>
        PatchAsync(
            [new UiControlPatch("status", Text: status)],
            [],
            null,
            cancellationToken);

    private Task PatchProgressAsync(
        int current,
        int maximum,
        string status,
        CancellationToken cancellationToken) => PatchAsync(
        [
            new UiControlPatch("status", Text: status),
            new UiControlPatch(
                "operation-progress",
                Number: ProgressPercentage(current, maximum),
                Visible: true)
        ],
        [],
        null,
        cancellationToken);

    private Task PatchProgressRowsAsync(
        int current,
        int maximum,
        string status,
        CancellationToken cancellationToken) => PatchAsync(
        [
            new UiControlPatch("status", Text: status),
            new UiControlPatch(
                "operation-progress",
                Number: ProgressPercentage(current, maximum),
                Visible: true)
        ],
        [new ToolViewTablePatch("packages", BuildRows())],
        SelectedRows(),
        cancellationToken);

    private static double ProgressPercentage(int current, int maximum) =>
        maximum <= 0 ? 0 : Math.Clamp(current * 100.0 / maximum, 0, 100);

    private async Task PatchAsync(
        IReadOnlyList<UiControlPatch> controls,
        IReadOnlyList<ToolViewTablePatch> tables,
        IReadOnlyList<string>? selectedRows,
        CancellationToken cancellationToken)
    {
        if (!_open)
            return;
        ToolViewPatch patch = new(
            ViewId,
            checked(++_revision),
            controls,
            tables,
            selectedRows);
        await RequireContext().InvokeHostServiceAsync(
            "aegisub.ui.patchToolView",
            JsonSerializer.Serialize(
                new PatchToolViewRequest(patch),
                DeclarativeUiJsonContext.Default.PatchToolViewRequest),
            cancellationToken).ConfigureAwait(false);
    }

    private ToolViewRowDefinition[] BuildRows() => _packages.Values
        .Where(MatchesFilter)
        .OrderBy(package => package.RecordType, StringComparer.Ordinal)
        .ThenBy(package => package.Namespace, StringComparer.Ordinal)
        .Select(package => new ToolViewRowDefinition(
            package.RowId,
            [
                Cell("name", package.DisplayName),
                Cell("namespace", package.Namespace),
                Cell("type", package.RecordType == "module" ? "Module" : "Macro"),
                Cell("installed", package.Version),
                Cell("available", package.AvailableVersion),
                Cell("channel", package.Channel),
                Cell(
                    "source",
                    package.Source == "transaction" ? "Managed" : "Discovered"),
                Cell("status", PackageStatus(package))
            ]))
        .ToArray();

    private ToolViewRowDefinition[] BuildAvailableRows() => _availablePackages.Values
        .Where(MatchesAvailableFilter)
        .OrderBy(package => package.RecordType, StringComparer.Ordinal)
        .ThenBy(package => package.Namespace, StringComparer.Ordinal)
        .Select(package => new ToolViewRowDefinition(
            package.RowId,
            [
                Cell("name", package.DisplayName),
                Cell("namespace", package.Namespace),
                Cell("type", package.RecordType == "module" ? "Module" : "Macro"),
                Cell("version", package.AvailableVersion),
                Cell("channel", package.Channel),
                Cell("feed", FeedDisplayName(package.Feed)),
                Cell("status", AvailableStatus(package))
            ]))
        .ToArray();

    private ToolViewRowDefinition[] BuildFeedRows() => _feeds.Values
        .OrderBy(feed => feed.Label, StringComparer.OrdinalIgnoreCase)
        .ThenBy(feed => feed.Id, StringComparer.Ordinal)
        .Select(feed => new ToolViewRowDefinition(
            feed.RowId,
            [
                Cell("label", feed.Label),
                Cell("url", feed.Url),
                Cell("status", feed.Enabled ? "Enabled" : "Disabled")
            ]))
        .ToArray();

    private ToolViewRowDefinition[] BuildLogRows() => _logs
        .Select(log => new ToolViewRowDefinition(
            log.RowId,
            [
                Cell("time", log.Timestamp),
                Cell("level", LogLevelName(log.Level)),
                Cell("source", log.Source),
                Cell("message", log.Message)
            ]))
        .ToArray();

    private bool MatchesFilter(PackageViewModel package)
    {
        if (_search.Length > 0 &&
            !package.DisplayName.Contains(_search, StringComparison.OrdinalIgnoreCase) &&
            !package.Namespace.Contains(_search, StringComparison.OrdinalIgnoreCase) &&
            !package.Feed.Contains(_search, StringComparison.OrdinalIgnoreCase))
            return false;
        return _scope switch
        {
            "managed" => package.Source == "transaction",
            "discovered" => package.Source == "discovered",
            "updates" => package.UpdateStatus == "updateAvailable",
            _ => true
        };
    }

    private bool MatchesAvailableFilter(AvailablePackageViewModel package) =>
        _availableSearch.Length == 0 ||
        package.DisplayName.Contains(_availableSearch, StringComparison.OrdinalIgnoreCase) ||
        package.Namespace.Contains(_availableSearch, StringComparison.OrdinalIgnoreCase) ||
        package.Author.Contains(_availableSearch, StringComparison.OrdinalIgnoreCase) ||
        package.Feed.Contains(_availableSearch, StringComparison.OrdinalIgnoreCase);

    private string FeedDisplayName(string url) => _feeds.Values
        .FirstOrDefault(feed => string.Equals(
            feed.Url, url, StringComparison.OrdinalIgnoreCase))?.Label ?? url;

    private IReadOnlyList<string> SelectedRows()
    {
        List<string> selected = [];
        _selectedPackageRowIds.RemoveWhere(rowId =>
            !_packages.TryGetValue(rowId, out PackageViewModel? package) ||
            !MatchesFilter(package));
        _selectedRowId = _selectedPackageRowIds.FirstOrDefault();
        selected.AddRange(_selectedPackageRowIds.Order(StringComparer.Ordinal));
        if (_selectedAvailableRowId is null ||
            !_availablePackages.TryGetValue(
                _selectedAvailableRowId, out AvailablePackageViewModel? available) ||
            !MatchesAvailableFilter(available))
        {
            _selectedAvailableRowId = null;
        }
        else
            selected.Add(_selectedAvailableRowId);
        if (_selectedFeedRowId is null || !_feeds.ContainsKey(_selectedFeedRowId))
            _selectedFeedRowId = null;
        else
            selected.Add(_selectedFeedRowId);
        return selected;
    }

    private string SelectionStatus() => _activeTabId switch
    {
        "available" when _selectedAvailableRowId is not null &&
            _availablePackages.TryGetValue(
                _selectedAvailableRowId, out AvailablePackageViewModel? package) =>
            $"Selected {package.DisplayName} {package.AvailableVersion}.",
        "feeds" when _selectedFeedRowId is not null &&
            _feeds.TryGetValue(_selectedFeedRowId, out FeedViewModel? feed) =>
            $"Selected feed {feed.Label}.",
        _ when _selectedPackageRowIds.Count > 1 =>
            $"Selected {_selectedPackageRowIds.Count} packages.",
        _ when _selectedRowId is not null &&
            _packages.TryGetValue(_selectedRowId, out PackageViewModel? installed) =>
            $"Selected {installed.DisplayName} {installed.Version}.",
        _ => "No item selected."
    };

    private PackageViewModel RequireSelectedPackage()
    {
        if (_selectedPackageRowIds.Count != 1 || _selectedRowId is null ||
            !_packages.TryGetValue(_selectedRowId, out PackageViewModel? package))
            throw new InvalidOperationException("Select one package first.");
        return package;
    }

    private IReadOnlyList<PackageViewModel> SelectedPackages()
    {
        PackageViewModel[] packages = _selectedPackageRowIds
            .Select(rowId => _packages[rowId])
            .OrderBy(package => package.RecordType, StringComparer.Ordinal)
            .ThenBy(package => package.Namespace, StringComparer.Ordinal)
            .ToArray();
        if (packages.Length == 0)
            throw new InvalidOperationException("Select one or more packages first.");
        return packages;
    }

    private AvailablePackageViewModel RequireSelectedAvailablePackage()
    {
        if (_selectedAvailableRowId is null ||
            !_availablePackages.TryGetValue(
                _selectedAvailableRowId, out AvailablePackageViewModel? package))
            throw new InvalidOperationException("Select one available package first.");
        return package;
    }

    private FeedViewModel RequireSelectedFeed()
    {
        if (_selectedFeedRowId is null ||
            !_feeds.TryGetValue(_selectedFeedRowId, out FeedViewModel? feed))
            throw new InvalidOperationException("Select one feed first.");
        return feed;
    }

    private IAegisubPluginContext RequireContext() => _context ??
        throw new InvalidOperationException("DependencyControl is not active.");

    private static PackageViewModel ParsePackage(JsonElement item)
    {
        string recordType = RequiredString(item, "recordType");
        string packageNamespace = RequiredString(item, "namespace");
        return new(
            $"{recordType}:{packageNamespace}",
            recordType,
            packageNamespace,
            RequiredString(item, "name"),
            RequiredString(item, "version"),
            RequiredString(item, "feed"),
            RequiredString(item, "channel"),
            RequiredString(item, "source"));
    }

    private static AvailablePackageViewModel ParseAvailablePackage(JsonElement item)
    {
        string recordType = RequiredString(item, "recordType");
        string packageNamespace = RequiredString(item, "namespace");
        return new(
            $"available:{recordType}:{packageNamespace}",
            recordType,
            packageNamespace,
            RequiredString(item, "name"),
            RequiredString(item, "author"),
            RequiredString(item, "feed"),
            RequiredString(item, "channel"),
            RequiredString(item, "availableVersion"),
            RequiredString(item, "installedVersion"),
            RequiredString(item, "status"),
            item.GetProperty("installed").GetBoolean(),
            item.GetProperty("platformSupported").GetBoolean());
    }

    private static FeedViewModel ParseFeed(JsonElement item)
    {
        string id = RequiredString(item, "id");
        return new(
            $"feed:{id}",
            id,
            RequiredString(item, "label"),
            RequiredString(item, "url"),
            item.GetProperty("enabled").GetBoolean());
    }

    private static LogViewModel ParseLog(JsonElement item, int index) => new(
        $"log:{index}",
        RequiredString(item, "timestamp"),
        item.GetProperty("level").GetInt32(),
        RequiredString(item, "source"),
        RequiredString(item, "message"));

    private static string BuildPackageRequest(
        PackageViewModel package,
        bool? removeConfig = null) =>
        DependencyControlServiceContribution.BuildJson(writer =>
        {
            writer.WriteStartObject();
            writer.WriteString("recordType", package.RecordType);
            writer.WriteString("namespace", package.Namespace);
            if (removeConfig is not null)
                writer.WriteBoolean("removeConfig", removeConfig.Value);
            writer.WriteEndObject();
        });

    private static string BuildBatchPackageRequest(
        IReadOnlyList<PackageViewModel> packages) =>
        DependencyControlServiceContribution.BuildJson(writer =>
        {
            writer.WriteStartObject();
            writer.WritePropertyName("packages");
            writer.WriteStartArray();
            foreach (PackageViewModel package in packages)
            {
                writer.WriteStartObject();
                writer.WriteString("recordType", package.RecordType);
                writer.WriteString("namespace", package.Namespace);
                writer.WriteEndObject();
            }
            writer.WriteEndArray();
            writer.WriteEndObject();
        });

    private static string PackageStatus(PackageViewModel package) =>
        package.UpdateStatus.Length > 0
            ? DescribeUpdateStatus(package.UpdateStatus, package.AvailableVersion)
            : package.Feed.Length == 0 ? "No feed" : "Not checked";

    private static string AvailableStatus(AvailablePackageViewModel package) =>
        package.Status switch
        {
            "installed" => $"Installed {package.InstalledVersion}",
            "chooseChannel" => "Choose channel",
            "unsupportedPlatform" => "Unsupported platform",
            "available" => "Available",
            _ => package.Status
        };

    private static string DescribeUpdateStatus(string status, string available) =>
        status switch
        {
            "updateAvailable" => $"Update {available}",
            "upToDate" => "Up to date",
            "installedNewer" => "Installed newer",
            "unsupportedPlatform" => "Unsupported platform",
            "updated" => $"Updated to {available}",
            "installed" => $"Installed {available}",
            "alreadyInstalled" => "Already installed",
            "noFeed" => "No feed",
            _ => status
        };

    private static string LogLevelName(int level) => level switch
    {
        0 => "Fatal",
        1 => "Error",
        2 => "Warning",
        3 => "Hint",
        4 => "Debug",
        5 => "Trace",
        _ => level.ToString(System.Globalization.CultureInfo.InvariantCulture)
    };

    private static UiValue Cell(string id, string value) => new(
        id,
        DependencyControlServiceContribution.BuildJson(
            writer => writer.WriteStringValue(value)));

    private static string RequiredString(JsonElement root, string property)
    {
        if (!root.TryGetProperty(property, out JsonElement value) ||
            value.ValueKind != JsonValueKind.String)
            throw new InvalidOperationException(
                $"DependencyControl UI response requires string '{property}'.");
        return value.GetString()!;
    }

    private static string ReadString(
        IReadOnlyList<UiValue> values,
        string id,
        string fallback)
    {
        UiValue? value = values.FirstOrDefault(
            candidate => string.Equals(candidate.Id, id, StringComparison.Ordinal));
        if (value is null)
            return fallback;
        using JsonDocument document = JsonDocument.Parse(value.JsonValue);
        return document.RootElement.ValueKind == JsonValueKind.String
            ? document.RootElement.GetString() ?? ""
            : fallback;
    }

    private static bool ReadBoolean(
        IReadOnlyList<UiValue> values,
        string id,
        bool fallback)
    {
        UiValue? value = values.FirstOrDefault(
            candidate => string.Equals(candidate.Id, id, StringComparison.Ordinal));
        if (value is null)
            return fallback;
        using JsonDocument document = JsonDocument.Parse(value.JsonValue);
        return document.RootElement.ValueKind switch
        {
            JsonValueKind.True => true,
            JsonValueKind.False => false,
            _ => fallback
        };
    }

    private static IReadOnlyList<string> SplitBypass(string value) => value
        .Split([',', ';', '\r', '\n'], StringSplitOptions.TrimEntries |
            StringSplitOptions.RemoveEmptyEntries);

    private static IReadOnlyList<string> ParseBypass(
        IReadOnlyList<string> values) => values
        .SelectMany(SplitBypass)
        .Distinct(StringComparer.OrdinalIgnoreCase)
        .ToArray();

    private static T Deserialize<T>(
        string json,
        System.Text.Json.Serialization.Metadata.JsonTypeInfo<T> typeInfo,
        string source) where T : class =>
        JsonSerializer.Deserialize(json, typeInfo) ??
        throw new InvalidOperationException(
            $"The host returned an empty DependencyControl {source}.");

    private sealed record PackageViewModel(
        string RowId,
        string RecordType,
        string Namespace,
        string Name,
        string Version,
        string Feed,
        string Channel,
        string Source,
        string AvailableVersion = "",
        string UpdateStatus = "")
    {
        public string DisplayName => Name.Length > 0 ? Name : Namespace;
    }

    private sealed record AvailablePackageViewModel(
        string RowId,
        string RecordType,
        string Namespace,
        string Name,
        string Author,
        string Feed,
        string Channel,
        string AvailableVersion,
        string InstalledVersion,
        string Status,
        bool Installed,
        bool PlatformSupported)
    {
        public string DisplayName => Name.Length > 0 ? Name : Namespace;
    }

    private sealed record FeedViewModel(
        string RowId,
        string Id,
        string Label,
        string Url,
        bool Enabled);

    private sealed record LogViewModel(
        string RowId,
        string Timestamp,
        int Level,
        string Source,
        string Message);

    private sealed record ChannelViewModel(
        string Name,
        string Version,
        bool Default);
}

internal sealed class DependencyControlAutomationContribution(
    DependencyControlToolViewController controller) : IAegisubAutomationContribution
{
    private readonly IReadOnlyList<IAegisubMacro> _macros =
        [new DependencyControlPackageManagerMacro(controller)];

    public ContributionMetadata Metadata { get; } = new(
        Id: "aegisub.dependency-control.automation",
        Kind: ContributionKind.Automation,
        Name: "DependencyControl package manager",
        Description: "Opens the host-rendered DependencyControl package manager.");

    public IReadOnlyList<IAegisubMacro> Macros => _macros;
}

internal sealed class DependencyControlPackageManagerMacro(
    DependencyControlToolViewController controller) : IAegisubMacro
{
    public MacroMetadata Metadata { get; } = new(
        Id: DependencyControlToolViewController.MacroId,
        Name: "DependencyControl/Package Manager",
        Description: "Opens the host-rendered DependencyControl package manager.");

    public async ValueTask<MacroResult> ExecuteAsync(
        IAegisubMacroContext context,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(context);
        ToolViewOperationResult result = await controller.OpenAsync(cancellationToken)
            .ConfigureAwait(false);
        return MacroResult.Completed(result.Status switch
        {
            "opened" => "Opened DependencyControl Package Manager.",
            "alreadyOpen" => "DependencyControl Package Manager is already open.",
            "unavailable" =>
                "DependencyControl Package Manager requires the graphical Aegisub UI.",
            _ when result.Succeeded => "Opened DependencyControl Package Manager.",
            _ => "DependencyControl Package Manager could not be opened."
        });
    }
}
