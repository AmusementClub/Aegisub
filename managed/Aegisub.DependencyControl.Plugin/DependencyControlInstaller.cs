using System.Globalization;
using System.Security.Cryptography;
using System.Text.Json;
using Aegisub.DependencyControl;
using Aegisub.Managed.Contracts;

namespace Aegisub.DependencyControl.Plugin;

internal sealed class DependencyControlInstaller(
    IAegisubPluginContext context,
    DependencyControlHttpTransport transport)
{
    private const string BeginService =
        "aegisub.dependency-control.begin-transaction";
    private const string CommitService =
        "aegisub.dependency-control.commit-transaction";
    private const string AbortService =
        "aegisub.dependency-control.abort-transaction";
    private const int MaximumFileBytes = 64 * 1024 * 1024;
    private const long MaximumTransactionBytes = 256L * 1024 * 1024;
    private const int MaximumPackages = 256;
    private const int MaximumFiles = 2048;
    private const int MaximumDepth = 32;

    private readonly DependencyControlFeedCatalog _catalog = new(transport);
    private readonly HashSet<string> _visiting = new(StringComparer.Ordinal);
    private readonly HashSet<string> _resolved = new(StringComparer.Ordinal);
    private readonly Dictionary<string, PlannedFile> _files = new(TargetComparer);
    private readonly List<DependencyControlResolvedPackage> _packages = [];

    public async Task<DependencyControlInstallResult> EnsureAsync(
        RegisteredRecord record,
        IReadOnlyList<RequiredModule> requirements,
        DependencyControlInstallJournal journal,
        DependencyControlInstalledStateStore installedState,
        CancellationToken cancellationToken)
    {
        foreach (RequiredModule requirement in requirements)
        {
            try
            {
                await ResolvePackageAsync(
                    DependencyControlPackageKind.Module,
                    requirement,
                    record.Feed,
                    depth: 0,
                    cancellationToken).ConfigureAwait(false);
            }
            catch (Exception error) when (
                requirement.Optional && error is not OperationCanceledException)
            {
                // Optional modules preserve the legacy facade's nil result and do not
                // prevent required modules from being committed.
            }
        }

        return await CommitAsync(
            journal, installedState, cancellationToken).ConfigureAwait(false);
    }

    public async Task<DependencyControlInstallResult> InstallPackageAsync(
        RegisteredRecord record,
        string requestedChannel,
        string targetVersion,
        DependencyControlInstallJournal journal,
        DependencyControlInstalledStateStore installedState,
        CancellationToken cancellationToken)
    {
        if (record.Feed.Length == 0)
            throw new InvalidOperationException(
                $"DependencyControl package '{record.Namespace}' has no feed URL.");
        DependencyControlPackageKind kind = record.RecordType == "module"
            ? DependencyControlPackageKind.Module
            : DependencyControlPackageKind.Macro;
        await ResolvePackageAsync(
            kind,
            new RequiredModule(
                record.Namespace,
                targetVersion,
                record.Feed,
                requestedChannel,
                false),
            record.Feed,
            depth: 0,
            cancellationToken).ConfigureAwait(false);
        return await CommitAsync(
            journal, installedState, cancellationToken).ConfigureAwait(false);
    }

    private async Task<DependencyControlInstallResult> CommitAsync(
        DependencyControlInstallJournal journal,
        DependencyControlInstalledStateStore installedState,
        CancellationToken cancellationToken)
    {
        ApplyOwnershipPlan(installedState.PlanInstallOwnership(_packages));
        if (_files.Count == 0)
            throw new InvalidOperationException(
                "DependencyControl resolved no installable files for the package transaction.");

        string transactionId = "";
        bool committed = false;
        string automationRoot = "";
        bool journalPrepared = false;
        try
        {
            string beginResponse = await context.InvokeHostServiceAsync(
                BeginService, "{}", cancellationToken).ConfigureAwait(false);
            using (JsonDocument begin = JsonDocument.Parse(beginResponse))
            {
                transactionId = ReadRequiredString(begin.RootElement, "transactionId");
                string stagingRoot = ReadRequiredString(begin.RootElement, "stagingRoot");
                automationRoot = ReadRequiredString(begin.RootElement, "automationRoot");
                await DownloadToStagingAsync(
                    stagingRoot,
                    cancellationToken).ConfigureAwait(false);
            }

            DependencyControlResolvedFile[] actions = _files.Values
                .Select(planned => new DependencyControlResolvedFile(
                    planned.File.RelativeTargetPath,
                    planned.File.Sha1,
                    planned.File.Delete))
                .OrderBy(action => action.Target, StringComparer.Ordinal)
                .ToArray();
            journal.Prepare(
                transactionId,
                automationRoot,
                installedState.Revision,
                _packages,
                actions);
            journalPrepared = true;
            string commitRequest = BuildCommitRequest(transactionId);
            string commitResponse = await context.InvokeHostServiceAsync(
                CommitService, commitRequest, cancellationToken).ConfigureAwait(false);
            using (JsonDocument commit = JsonDocument.Parse(commitResponse))
            {
                if (!commit.RootElement.TryGetProperty("committed", out JsonElement value) ||
                    value.ValueKind != JsonValueKind.True)
                    throw new InvalidOperationException(
                        "DependencyControl native host did not commit the transaction.");
            }
            committed = true;
            return BuildResult(transactionId, automationRoot);
        }
        finally
        {
            if (!committed && transactionId.Length > 0)
            {
                try
                {
                    await context.InvokeHostServiceAsync(
                        AbortService,
                        BuildAbortRequest(transactionId),
                        CancellationToken.None).ConfigureAwait(false);
                }
                catch
                {
                    // A native commit failure already performs rollback and consumes
                    // the transaction. Cleanup failure must not mask the root error.
                }
            }
            if (!committed && journalPrepared)
            {
                try
                {
                    journal.Cancel(transactionId);
                }
                catch
                {
                    // Startup reconciliation safely abandons a journal whose native
                    // transaction did not commit. Preserve the original failure.
                }
            }
        }
    }

    private async Task ResolvePackageAsync(
        DependencyControlPackageKind kind,
        RequiredModule requirement,
        string fallbackFeed,
        int depth,
        CancellationToken cancellationToken)
    {
        if (depth > MaximumDepth)
            throw new InvalidOperationException(
                "DependencyControl dependency graph exceeds the recursion limit.");
        string feedUrl = requirement.Feed.Length > 0 ? requirement.Feed : fallbackFeed;
        if (feedUrl.Length == 0)
            throw new InvalidOperationException(
                $"DependencyControl package '{requirement.ModuleName}' has no feed URL.");

        (DependencyControlFeed feed, DependencyControlPackage package) =
            await _catalog.FindPackageAsync(
                feedUrl,
                kind,
                requirement.ModuleName,
                cancellationToken).ConfigureAwait(false);
        DependencyControlChannel channel = SelectChannel(package, requirement.Channel);
        if (!VersionSatisfies(channel.Version, requirement.Version))
            throw new InvalidOperationException(
                $"DependencyControl package '{requirement.ModuleName}' version " +
                $"'{channel.Version}' does not satisfy '{requirement.Version}'.");

        string key = $"{kind}\n{feed.SourceUrl}\n{package.Namespace}\n{channel.Name}";
        if (_resolved.Contains(key))
            return;
        if (!_visiting.Add(key))
            return;
        if (_resolved.Count + _visiting.Count > MaximumPackages)
            throw new InvalidOperationException(
                "DependencyControl dependency graph exceeds the package limit.");
        try
        {
            foreach (DependencyControlRequirement dependency in channel.RequiredModules)
            {
                RequiredModule nested = new(
                    dependency.ModuleName,
                    dependency.Version,
                    dependency.Feed,
                    dependency.Channel,
                    dependency.Optional);
                try
                {
                    await ResolvePackageAsync(
                        DependencyControlPackageKind.Module,
                        nested,
                        feed.SourceUrl,
                        depth + 1,
                        cancellationToken)
                        .ConfigureAwait(false);
                }
                catch (Exception error) when (
                    nested.Optional && error is not OperationCanceledException)
                {
                }
            }

            List<DependencyControlResolvedFile> packageFiles = [];
            foreach (DependencyControlFile file in channel.Files)
            {
                if (file.RelativeTargetPath.Length == 0 ||
                    !IsPlatformCompatible(file.Platform))
                    continue;
                packageFiles.Add(new(
                    file.RelativeTargetPath,
                    file.Sha1,
                    file.Delete));
                AddPlannedFile(file);
            }
            _packages.Add(new(
                kind == DependencyControlPackageKind.Module ? "module" : "automation",
                package.Namespace,
                package.Name,
                channel.Version,
                package.Description,
                package.Author,
                channel.Name,
                feed.SourceUrl,
                channel.RequiredModules.Select(requirement => new RequiredModule(
                    requirement.ModuleName,
                    requirement.Version,
                    requirement.Feed,
                    requirement.Channel,
                    requirement.Optional)).ToArray(),
                packageFiles));
            _resolved.Add(key);
        }
        finally
        {
            _visiting.Remove(key);
        }
    }

    private async Task DownloadToStagingAsync(
        string stagingRoot,
        CancellationToken cancellationToken)
    {
        long totalBytes = 0;
        int index = 0;
        foreach ((string target, PlannedFile planned) in _files.ToArray())
        {
            if (planned.File.Delete)
                continue;
            byte[] payload = await transport.GetBytesAsync(
                planned.File.Url,
                MaximumFileBytes,
                cancellationToken).ConfigureAwait(false);
            totalBytes = checked(totalBytes + payload.Length);
            if (totalBytes > MaximumTransactionBytes)
                throw new InvalidOperationException(
                    "DependencyControl transaction exceeds the download size limit.");
            string actualHash = Convert.ToHexString(SHA1.HashData(payload));
            if (!actualHash.Equals(planned.File.Sha1, StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException(
                    $"DependencyControl SHA-1 validation failed for '{target}'.");
            string stagedName = $"payload-{index:D6}.bin";
            await File.WriteAllBytesAsync(
                Path.Combine(stagingRoot, stagedName),
                payload,
                cancellationToken).ConfigureAwait(false);
            _files[target] = planned with { StagedName = stagedName };
            ++index;
        }
    }

    private void ApplyOwnershipPlan(DependencyControlOwnershipPlan ownership)
    {
        foreach ((string target, PlannedFile planned) in _files.ToArray())
        {
            if (planned.File.Delete && !ownership.DeleteTargets.Contains(target))
                _files.Remove(target);
        }
        foreach (string target in ownership.DeleteTargets)
        {
            AddPlannedFile(new(
                "",
                "",
                "",
                "script",
                "",
                true,
                target));
        }
    }

    private void AddPlannedFile(DependencyControlFile file)
    {
        PlannedFile planned = new(file, "");
        if (_files.TryGetValue(file.RelativeTargetPath, out PlannedFile? existing))
        {
            if (existing.File.Delete && !file.Delete)
            {
                _files[file.RelativeTargetPath] = planned;
                return;
            }
            if (!existing.File.Delete && file.Delete)
                return;
            if (existing.File.Delete)
                return;
            if (!string.Equals(
                    existing.File.Sha1,
                    file.Sha1,
                    StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException(
                    $"DependencyControl packages conflict on target " +
                    $"'{file.RelativeTargetPath}'.");
            return;
        }
        if (_files.Count >= MaximumFiles)
            throw new InvalidOperationException(
                "DependencyControl transaction exceeds the file limit.");
        _files.Add(file.RelativeTargetPath, planned);
    }

    private static DependencyControlChannel SelectChannel(
        DependencyControlPackage package,
        string requestedChannel)
    {
        DependencyControlChannel channel;
        if (requestedChannel.Length > 0)
        {
            if (!package.Channels.TryGetValue(requestedChannel, out channel!))
                throw new InvalidOperationException(
                    $"DependencyControl module '{package.Namespace}' has no channel " +
                    $"'{requestedChannel}'.");
        }
        else
        {
            DependencyControlChannel[] defaults = package.Channels.Values
                .Where(candidate => candidate.IsDefault)
                .ToArray();
            if (defaults.Length == 1)
                channel = defaults[0];
            else if (defaults.Length == 0 && package.Channels.Count == 1)
                channel = package.Channels.Values.Single();
            else
                throw new InvalidOperationException(
                    $"DependencyControl module '{package.Namespace}' requires a channel.");
        }
        if (channel.Platforms.Count > 0 &&
            !channel.Platforms.Any(IsPlatformCompatible))
            throw new InvalidOperationException(
                $"DependencyControl module '{package.Namespace}' channel '{channel.Name}' " +
                "does not support this platform.");
        return channel;
    }

    private static bool IsPlatformCompatible(string platform)
    {
        if (platform.Length == 0 || platform.Equals("all", StringComparison.OrdinalIgnoreCase) ||
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

    private static StringComparer TargetComparer => OperatingSystem.IsWindows()
        ? StringComparer.OrdinalIgnoreCase
        : StringComparer.Ordinal;

    private static bool VersionSatisfies(string available, string required)
    {
        if (required.Length == 0 || string.Equals(available, required, StringComparison.Ordinal))
            return true;
        if (!TryParseVersion(available, out int[] availableParts) ||
            !TryParseVersion(required, out int[] requiredParts))
            return false;
        int count = Math.Max(availableParts.Length, requiredParts.Length);
        for (int index = 0; index < count; ++index)
        {
            int left = index < availableParts.Length ? availableParts[index] : 0;
            int right = index < requiredParts.Length ? requiredParts[index] : 0;
            if (left != right)
                return left > right;
        }
        return true;
    }

    private static bool TryParseVersion(string value, out int[] parts)
    {
        string[] rawParts = value.Split('.', StringSplitOptions.None);
        parts = new int[rawParts.Length];
        if (rawParts.Length == 0)
            return false;
        for (int index = 0; index < rawParts.Length; ++index)
        {
            if (!int.TryParse(
                rawParts[index],
                NumberStyles.None,
                CultureInfo.InvariantCulture,
                out parts[index]) || parts[index] < 0)
                return false;
        }
        return true;
    }

    private string BuildCommitRequest(string transactionId) =>
        DependencyControlServiceContribution.BuildJson(writer =>
        {
            writer.WriteStartObject();
            writer.WriteString("transactionId", transactionId);
            writer.WritePropertyName("files");
            writer.WriteStartArray();
            foreach ((string target, PlannedFile planned) in _files)
            {
                writer.WriteStartObject();
                if (planned.StagedName.Length > 0)
                    writer.WriteString("stagedName", planned.StagedName);
                writer.WriteString("target", target);
                if (planned.File.Delete)
                    writer.WriteBoolean("delete", true);
                writer.WriteEndObject();
            }
            writer.WriteEndArray();
            writer.WriteEndObject();
        });

    private static string BuildAbortRequest(string transactionId) =>
        DependencyControlServiceContribution.BuildJson(writer =>
        {
            writer.WriteStartObject();
            writer.WriteString("transactionId", transactionId);
            writer.WriteEndObject();
        });

    private DependencyControlInstallResult BuildResult(
        string transactionId,
        string automationRoot) => new(
        transactionId,
        automationRoot,
        _packages.ToArray(),
        _files.Keys.Order(StringComparer.Ordinal).ToArray());

    private static string ReadRequiredString(JsonElement root, string property)
    {
        if (!root.TryGetProperty(property, out JsonElement value) ||
            value.ValueKind != JsonValueKind.String ||
            string.IsNullOrEmpty(value.GetString()))
            throw new InvalidOperationException(
                $"DependencyControl native host response requires string '{property}'.");
        return value.GetString()!;
    }

    private sealed record PlannedFile(DependencyControlFile File, string StagedName);
}

internal sealed record DependencyControlResolvedFile(
    string Target,
    string Sha1,
    bool Delete);

internal sealed record DependencyControlResolvedPackage(
    string RecordType,
    string Namespace,
    string Name,
    string Version,
    string Description,
    string Author,
    string Channel,
    string Feed,
    IReadOnlyList<RequiredModule> RequiredModules,
    IReadOnlyList<DependencyControlResolvedFile> Files);

internal sealed record DependencyControlInstallResult(
    string TransactionId,
    string AutomationRoot,
    IReadOnlyList<DependencyControlResolvedPackage> Packages,
    IReadOnlyList<string> InstalledFiles)
{
    public string ToJson() => DependencyControlServiceContribution.BuildJson(writer =>
    {
        writer.WriteStartObject();
        writer.WriteNumber("schemaVersion", 1);
        writer.WriteBoolean("committed", true);
        writer.WriteString("automationRoot", AutomationRoot);
        writer.WritePropertyName("modules");
        writer.WriteStartArray();
        foreach (DependencyControlResolvedPackage package in Packages)
        {
            if (package.RecordType != "module")
                continue;
            writer.WriteStartObject();
            writer.WriteString("moduleName", package.Namespace);
            writer.WriteString("version", package.Version);
            writer.WriteString("channel", package.Channel);
            writer.WriteString("feed", package.Feed);
            writer.WritePropertyName("files");
            writer.WriteStartArray();
            foreach (DependencyControlResolvedFile file in package.Files)
            {
                writer.WriteStartObject();
                writer.WriteString("target", file.Target);
                writer.WriteString("sha1", file.Sha1);
                writer.WriteBoolean("delete", file.Delete);
                writer.WriteEndObject();
            }
            writer.WriteEndArray();
            writer.WriteEndObject();
        }
        writer.WriteEndArray();
        writer.WritePropertyName("installedFiles");
        writer.WriteStartArray();
        foreach (string target in InstalledFiles)
            writer.WriteStringValue(target);
        writer.WriteEndArray();
        writer.WriteEndObject();
    });
}
