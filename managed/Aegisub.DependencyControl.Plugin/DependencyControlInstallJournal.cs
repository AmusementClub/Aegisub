using System.Security.Cryptography;
using System.Text.Json;

namespace Aegisub.DependencyControl.Plugin;

internal enum DependencyControlReconciliationStatus
{
    None,
    Finalized,
    Abandoned,
    Corrupted
}

internal sealed record DependencyControlReconciliationResult(
    DependencyControlReconciliationStatus Status,
    string BackupName);

internal sealed class DependencyControlInstallJournal
{
    private const int MaximumJournalBytes = 16 * 1024 * 1024;
    private const int MaximumPackages = 256;
    private const int MaximumActions = 2048;
    private const int MaximumPackageFiles = 8192;
    private const int MaximumTextLength = 16 * 1024;
    private const long MaximumPayloadBytes = 64L * 1024 * 1024;
    private readonly string _root;
    private readonly string _path;

    public DependencyControlInstallJournal(string stateRoot)
    {
        _root = stateRoot;
        _path = Path.Combine(stateRoot, "pending-install.json");
        Directory.CreateDirectory(_root);
        RejectReparsePoint(_root, "state directory");
    }

    public void Prepare(
        string transactionId,
        string automationRoot,
        long baseRevision,
        IReadOnlyList<DependencyControlResolvedPackage> packages,
        IReadOnlyList<DependencyControlResolvedFile> actions,
        IReadOnlyList<DependencyControlPackageIdentity>? removals = null)
    {
        removals ??= [];
        ValidateTransactionId(transactionId);
        ValidateAutomationRoot(automationRoot);
        if (baseRevision < 0 || packages.Count > MaximumPackages ||
            removals.Count > MaximumPackages ||
            packages.Count + removals.Count > MaximumPackages ||
            actions.Count > MaximumActions)
            throw new InvalidOperationException(
                "DependencyControl pending install exceeds its limits.");
        HashSet<string> packageKeys = new(StringComparer.Ordinal);
        foreach (DependencyControlResolvedPackage package in packages)
        {
            if (!packageKeys.Add(PackageKey(package.RecordType, package.Namespace)))
                throw new InvalidOperationException(
                    "DependencyControl pending install contains duplicate packages.");
        }
        HashSet<string> removalKeys = new(StringComparer.Ordinal);
        foreach (DependencyControlPackageIdentity removal in removals)
        {
            ValidatePackageIdentity(removal);
            string key = PackageKey(removal.RecordType, removal.Namespace);
            if (!removalKeys.Add(key) || packageKeys.Contains(key))
                throw new InvalidOperationException(
                    "DependencyControl pending install contains conflicting package changes.");
        }
        if (File.Exists(_path))
        {
            RejectReparsePoint(_path, "pending-install journal");
            throw new InvalidOperationException(
                "DependencyControl has an unresolved pending install journal.");
        }

        using MemoryStream output = new();
        using (Utf8JsonWriter writer = new(output, new JsonWriterOptions
        {
            Indented = true
        }))
        {
            writer.WriteStartObject();
            writer.WriteNumber("schemaVersion", 1);
            writer.WriteString("transactionId", transactionId);
            writer.WriteString("automationRoot", Path.GetFullPath(automationRoot));
            writer.WriteNumber("baseRevision", baseRevision);
            WritePackages(writer, packages);
            writer.WritePropertyName("actions");
            writer.WriteStartArray();
            foreach (DependencyControlResolvedFile action in actions)
                WriteFile(writer, action);
            writer.WriteEndArray();
            WriteRemovals(writer, removals);
            writer.WriteEndObject();
        }
        byte[] payload = output.ToArray();
        if (payload.Length > MaximumJournalBytes)
            throw new InvalidOperationException(
                "DependencyControl pending install journal exceeds the size limit.");
        Save(payload);
    }

    public void Complete(string transactionId)
    {
        if (!File.Exists(_path))
            throw new InvalidOperationException(
                "DependencyControl pending install journal is missing.");
        PendingInstall pending = Read();
        if (pending.TransactionId != transactionId)
            throw new InvalidOperationException(
                "DependencyControl pending install transaction does not match.");
        File.Delete(_path);
    }

    public void Cancel(string transactionId)
    {
        if (!File.Exists(_path))
            return;
        PendingInstall pending = Read();
        if (pending.TransactionId != transactionId)
            throw new InvalidOperationException(
                "DependencyControl pending install transaction does not match.");
        File.Delete(_path);
    }

    public DependencyControlReconciliationResult Reconcile(
        string automationRoot,
        DependencyControlInstalledStateStore installedState)
    {
        ArgumentNullException.ThrowIfNull(installedState);
        if (!File.Exists(_path))
            return new(DependencyControlReconciliationStatus.None, "");

        PendingInstall pending;
        try
        {
            pending = Read();
        }
        catch (Exception error) when (
            error is JsonException or InvalidDataException or OverflowException)
        {
            return Quarantine("corrupted", DependencyControlReconciliationStatus.Corrupted);
        }

        if (!SamePath(pending.AutomationRoot, automationRoot) ||
            installedState.Revision < pending.BaseRevision ||
            installedState.Revision > pending.BaseRevision + 1)
            return Quarantine("abandoned", DependencyControlReconciliationStatus.Abandoned);

        if (installedState.MatchesTransaction(pending.Packages, pending.Removals))
        {
            File.Delete(_path);
            return new(DependencyControlReconciliationStatus.Finalized, "");
        }
        if (installedState.Revision != pending.BaseRevision ||
            !VerifyActions(automationRoot, pending.Actions))
            return Quarantine("abandoned", DependencyControlReconciliationStatus.Abandoned);

        installedState.ApplyTransaction(pending.Packages, pending.Removals);
        File.Delete(_path);
        return new(DependencyControlReconciliationStatus.Finalized, "");
    }

    private PendingInstall Read()
    {
        RejectReparsePoint(_path, "pending-install journal");
        byte[] payload = File.ReadAllBytes(_path);
        if (payload.Length > MaximumJournalBytes)
            throw new InvalidDataException(
                "DependencyControl pending install journal exceeds the size limit.");
        using JsonDocument document = JsonDocument.Parse(
            payload,
            new JsonDocumentOptions
            {
                AllowTrailingCommas = false,
                CommentHandling = JsonCommentHandling.Disallow,
                MaxDepth = 32
            });
        JsonElement root = document.RootElement;
        if (root.ValueKind != JsonValueKind.Object ||
            !root.TryGetProperty("schemaVersion", out JsonElement schemaVersion) ||
            !schemaVersion.TryGetInt32(out int schema) || schema != 1 ||
            !root.TryGetProperty("baseRevision", out JsonElement revision) ||
            !revision.TryGetInt64(out long baseRevision) || baseRevision < 0)
            throw new InvalidDataException(
                "DependencyControl pending install journal header is invalid.");
        string transactionId = ReadRequiredString(root, "transactionId", 128);
        ValidateTransactionId(transactionId);
        string automationRoot = ReadRequiredString(root, "automationRoot", 32 * 1024);
        ValidateAutomationRoot(automationRoot);
        IReadOnlyList<DependencyControlResolvedPackage> packages = ReadPackages(root);
        IReadOnlyList<DependencyControlResolvedFile> actions = ReadFiles(
            root, "actions", MaximumActions);
        IReadOnlyList<DependencyControlPackageIdentity> removals = ReadRemovals(root);
        HashSet<string> packageKeys = packages
            .Select(package => PackageKey(package.RecordType, package.Namespace))
            .ToHashSet(StringComparer.Ordinal);
        if (removals.Any(removal => packageKeys.Contains(
                PackageKey(removal.RecordType, removal.Namespace))))
            throw new InvalidDataException(
                "DependencyControl pending install contains conflicting package changes.");
        return new(transactionId, automationRoot, baseRevision, packages, removals, actions);
    }

    private static IReadOnlyList<DependencyControlPackageIdentity> ReadRemovals(
        JsonElement root)
    {
        if (!root.TryGetProperty("removedPackages", out JsonElement removals))
            return [];
        if (removals.ValueKind != JsonValueKind.Array ||
            removals.GetArrayLength() > MaximumPackages)
            throw new InvalidDataException(
                "DependencyControl pending package removals are invalid.");
        List<DependencyControlPackageIdentity> result = new(removals.GetArrayLength());
        HashSet<string> keys = new(StringComparer.Ordinal);
        foreach (JsonElement removal in removals.EnumerateArray())
        {
            if (removal.ValueKind != JsonValueKind.Object)
                throw new InvalidDataException(
                    "DependencyControl pending package removal must be an object.");
            string recordType = ReadRequiredString(removal, "recordType", 32);
            string packageNamespace = ReadRequiredString(removal, "namespace", 512);
            DependencyControlPackageIdentity identity = new(recordType, packageNamespace);
            ValidatePackageIdentity(identity);
            if (!keys.Add(PackageKey(recordType, packageNamespace)))
                throw new InvalidDataException(
                    "DependencyControl pending package removals contain duplicates.");
            result.Add(identity);
        }
        return result;
    }

    private static IReadOnlyList<DependencyControlResolvedPackage> ReadPackages(
        JsonElement root)
    {
        if (!root.TryGetProperty("packages", out JsonElement packages) ||
            packages.ValueKind != JsonValueKind.Array ||
            packages.GetArrayLength() > MaximumPackages)
            throw new InvalidDataException(
                "DependencyControl pending install packages are invalid.");
        List<DependencyControlResolvedPackage> result = new(packages.GetArrayLength());
        HashSet<string> keys = new(StringComparer.Ordinal);
        int totalFiles = 0;
        foreach (JsonElement package in packages.EnumerateArray())
        {
            if (package.ValueKind != JsonValueKind.Object)
                throw new InvalidDataException(
                    "DependencyControl pending package must be an object.");
            string recordType = ReadRequiredString(package, "recordType", 32);
            if (recordType is not ("automation" or "module"))
                throw new InvalidDataException(
                    "DependencyControl pending package type is invalid.");
            string packageNamespace = ReadRequiredString(package, "namespace", 512);
            ValidateNamespace(packageNamespace);
            if (!keys.Add($"{recordType}\n{packageNamespace}"))
                throw new InvalidDataException(
                    "DependencyControl pending install contains duplicate packages.");
            string feed = ReadRequiredString(package, "feed", MaximumTextLength);
            ValidateHttpUrl(feed);
            IReadOnlyList<RequiredModule> requirements = ReadRequirements(package);
            IReadOnlyList<DependencyControlResolvedFile> files = ReadFiles(
                package, "files", MaximumActions);
            totalFiles = checked(totalFiles + files.Count);
            if (totalFiles > MaximumPackageFiles)
                throw new InvalidDataException(
                    "DependencyControl pending package files exceed the limit.");
            result.Add(new(
                recordType,
                packageNamespace,
                ReadRequiredString(package, "name", MaximumTextLength),
                ReadRequiredString(package, "version", 256),
                ReadOptionalString(package, "description", MaximumTextLength),
                ReadOptionalString(package, "author", MaximumTextLength),
                ReadRequiredString(package, "channel", 256),
                feed,
                requirements,
                files));
        }
        return result;
    }

    private static IReadOnlyList<RequiredModule> ReadRequirements(JsonElement root)
    {
        if (!root.TryGetProperty("requiredModules", out JsonElement modules) ||
            modules.ValueKind != JsonValueKind.Array || modules.GetArrayLength() > 256)
            throw new InvalidDataException(
                "DependencyControl pending requirements are invalid.");
        List<RequiredModule> result = new(modules.GetArrayLength());
        foreach (JsonElement module in modules.EnumerateArray())
        {
            if (module.ValueKind != JsonValueKind.Object)
                throw new InvalidDataException(
                    "DependencyControl pending requirement must be an object.");
            string moduleName = ReadRequiredString(module, "moduleName", 512);
            ValidateNamespace(moduleName);
            string feed = ReadOptionalString(module, "feed", MaximumTextLength);
            if (feed.Length > 0)
                ValidateHttpUrl(feed);
            result.Add(new(
                moduleName,
                ReadOptionalString(module, "version", 256),
                feed,
                ReadOptionalString(module, "channel", 256),
                ReadOptionalBoolean(module, "optional")));
        }
        return result;
    }

    private static IReadOnlyList<DependencyControlResolvedFile> ReadFiles(
        JsonElement root,
        string property,
        int maximumCount)
    {
        if (!root.TryGetProperty(property, out JsonElement files) ||
            files.ValueKind != JsonValueKind.Array || files.GetArrayLength() > maximumCount)
            throw new InvalidDataException(
                "DependencyControl pending install files are invalid.");
        List<DependencyControlResolvedFile> result = new(files.GetArrayLength());
        HashSet<string> targets = new(TargetComparer);
        foreach (JsonElement file in files.EnumerateArray())
        {
            if (file.ValueKind != JsonValueKind.Object)
                throw new InvalidDataException(
                    "DependencyControl pending file must be an object.");
            string target = ReadRequiredString(file, "target", 4096);
            ValidateTarget(target);
            if (!targets.Add(target))
                throw new InvalidDataException(
                    "DependencyControl pending files contain duplicate targets.");
            bool delete = ReadOptionalBoolean(file, "delete");
            string sha1 = ReadOptionalString(file, "sha1", 40);
            if (!delete && (sha1.Length != 40 || !sha1.All(Uri.IsHexDigit)))
                throw new InvalidDataException(
                    "DependencyControl pending file hash is invalid.");
            if (delete && sha1.Length > 0 &&
                (sha1.Length != 40 || !sha1.All(Uri.IsHexDigit)))
                throw new InvalidDataException(
                    "DependencyControl pending deletion hash is invalid.");
            result.Add(new(target, sha1.ToUpperInvariant(), delete));
        }
        return result;
    }

    private static bool VerifyActions(
        string automationRoot,
        IReadOnlyList<DependencyControlResolvedFile> actions)
    {
        string trustedRoot = Path.GetFullPath(automationRoot);
        RejectReparsePoint(trustedRoot, "Automation root");
        foreach (DependencyControlResolvedFile action in actions)
        {
            string target = ResolveTarget(trustedRoot, action.Target);
            if (action.Delete)
            {
                if (File.Exists(target) || Directory.Exists(target))
                    return false;
                continue;
            }
            if (!File.Exists(target))
                return false;
            RejectPathComponents(trustedRoot, action.Target);
            FileInfo info = new(target);
            if (info.Length > MaximumPayloadBytes)
                return false;
            using FileStream stream = new(
                target, FileMode.Open, FileAccess.Read, FileShare.Read);
            string actual = Convert.ToHexString(SHA1.HashData(stream));
            if (!actual.Equals(action.Sha1, StringComparison.OrdinalIgnoreCase))
                return false;
        }
        return true;
    }

    private void Save(byte[] payload)
    {
        string temporary = Path.Combine(
            _root, $"pending-install-{Guid.NewGuid():N}.tmp");
        try
        {
            using (FileStream stream = new(
                temporary,
                FileMode.CreateNew,
                FileAccess.Write,
                FileShare.None,
                bufferSize: 16 * 1024,
                FileOptions.WriteThrough))
            {
                stream.Write(payload);
                stream.Flush(flushToDisk: true);
            }
            File.Move(temporary, _path);
        }
        finally
        {
            File.Delete(temporary);
        }
    }

    private DependencyControlReconciliationResult Quarantine(
        string reason,
        DependencyControlReconciliationStatus status)
    {
        string backupName = $"pending-install.json.{reason}-{Guid.NewGuid():N}";
        File.Move(_path, Path.Combine(_root, backupName));
        return new(status, backupName);
    }

    private static void WritePackages(
        Utf8JsonWriter writer,
        IReadOnlyList<DependencyControlResolvedPackage> packages)
    {
        writer.WritePropertyName("packages");
        writer.WriteStartArray();
        foreach (DependencyControlResolvedPackage package in packages)
        {
            writer.WriteStartObject();
            writer.WriteString("recordType", package.RecordType);
            writer.WriteString("namespace", package.Namespace);
            writer.WriteString("name", package.Name);
            writer.WriteString("version", package.Version);
            writer.WriteString("description", package.Description);
            writer.WriteString("author", package.Author);
            writer.WriteString("channel", package.Channel);
            writer.WriteString("feed", package.Feed);
            writer.WritePropertyName("requiredModules");
            writer.WriteStartArray();
            foreach (RequiredModule requirement in package.RequiredModules)
            {
                writer.WriteStartObject();
                writer.WriteString("moduleName", requirement.ModuleName);
                writer.WriteString("version", requirement.Version);
                writer.WriteString("feed", requirement.Feed);
                writer.WriteString("channel", requirement.Channel);
                writer.WriteBoolean("optional", requirement.Optional);
                writer.WriteEndObject();
            }
            writer.WriteEndArray();
            writer.WritePropertyName("files");
            writer.WriteStartArray();
            foreach (DependencyControlResolvedFile file in package.Files)
                WriteFile(writer, file);
            writer.WriteEndArray();
            writer.WriteEndObject();
        }
        writer.WriteEndArray();
    }

    private static void WriteRemovals(
        Utf8JsonWriter writer,
        IReadOnlyList<DependencyControlPackageIdentity> removals)
    {
        writer.WritePropertyName("removedPackages");
        writer.WriteStartArray();
        foreach (DependencyControlPackageIdentity removal in removals)
        {
            writer.WriteStartObject();
            writer.WriteString("recordType", removal.RecordType);
            writer.WriteString("namespace", removal.Namespace);
            writer.WriteEndObject();
        }
        writer.WriteEndArray();
    }

    private static void WriteFile(Utf8JsonWriter writer, DependencyControlResolvedFile file)
    {
        writer.WriteStartObject();
        writer.WriteString("target", file.Target);
        writer.WriteString("sha1", file.Sha1);
        writer.WriteBoolean("delete", file.Delete);
        writer.WriteEndObject();
    }

    private static string ResolveTarget(string root, string relative)
    {
        ValidateTarget(relative);
        string target = Path.GetFullPath(Path.Combine(
            root,
            relative.Replace('/', Path.DirectorySeparatorChar)));
        string prefix = root.EndsWith(Path.DirectorySeparatorChar)
            ? root
            : root + Path.DirectorySeparatorChar;
        if (!target.StartsWith(prefix, PathComparison))
            throw new InvalidDataException(
                "DependencyControl pending target escapes the Automation root.");
        return target;
    }

    private static void RejectPathComponents(string root, string relative)
    {
        string current = root;
        string[] components = relative.Split('/');
        for (int index = 0; index < components.Length; ++index)
        {
            current = Path.Combine(current, components[index]);
            if (!File.Exists(current) && !Directory.Exists(current))
                continue;
            RejectReparsePoint(current, "pending-install target");
            if (index + 1 < components.Length && !Directory.Exists(current))
                throw new InvalidDataException(
                    "DependencyControl pending target crosses a non-directory.");
        }
    }

    private static void ValidateTransactionId(string value)
    {
        if (value.Length is 0 or > 128 ||
            value.Any(character => !(character is >= 'a' and <= 'z' or
                >= 'A' and <= 'Z' or >= '0' and <= '9' or '-' or '_')))
            throw new InvalidDataException(
                "DependencyControl pending transaction ID is invalid.");
    }

    private static void ValidatePackageIdentity(
        DependencyControlPackageIdentity identity)
    {
        if (identity.RecordType is not ("automation" or "module"))
            throw new InvalidDataException(
                "DependencyControl pending package type is invalid.");
        ValidateNamespace(identity.Namespace);
    }

    private static string PackageKey(string recordType, string packageNamespace) =>
        $"{recordType}\n{packageNamespace}";

    private static void ValidateAutomationRoot(string value)
    {
        if (string.IsNullOrWhiteSpace(value) || value.Length > 32 * 1024 ||
            !Path.IsPathFullyQualified(value))
            throw new InvalidDataException(
                "DependencyControl pending Automation root is invalid.");
    }

    private static void ValidateNamespace(string value)
    {
        bool sawDot = false;
        bool componentHasCharacters = false;
        foreach (char character in value)
        {
            if (character == '.')
            {
                if (!componentHasCharacters)
                    throw new InvalidDataException(
                        "DependencyControl pending namespace is invalid.");
                sawDot = true;
                componentHasCharacters = false;
                continue;
            }
            if (!(character is >= 'a' and <= 'z' or >= 'A' and <= 'Z' or
                  >= '0' and <= '9' or '-' or '_'))
                throw new InvalidDataException(
                    "DependencyControl pending namespace is invalid.");
            componentHasCharacters = true;
        }
        if (!sawDot || !componentHasCharacters)
            throw new InvalidDataException(
                "DependencyControl pending namespace is invalid.");
    }

    private static void ValidateHttpUrl(string value)
    {
        if (!Uri.TryCreate(value, UriKind.Absolute, out Uri? uri) ||
            uri.Scheme is not ("http" or "https"))
            throw new InvalidDataException(
                "DependencyControl pending feed must be HTTP(S).");
    }

    private static void ValidateTarget(string value)
    {
        if (value.Length is 0 or > 4096 || value[0] is '/' or '\\' ||
            value.Contains('\\') || value.Contains(':') || value.Any(char.IsControl))
            throw new InvalidDataException(
                "DependencyControl pending target is invalid.");
        string[] components = value.Split('/');
        if (components.Any(component => component.Length == 0 || component is "." or ".."))
            throw new InvalidDataException(
                "DependencyControl pending target is invalid.");
    }

    private static string ReadRequiredString(
        JsonElement root,
        string property,
        int maximumLength)
    {
        string result = ReadOptionalString(root, property, maximumLength);
        if (string.IsNullOrWhiteSpace(result))
            throw new InvalidDataException(
                $"DependencyControl pending field '{property}' is required.");
        return result;
    }

    private static string ReadOptionalString(
        JsonElement root,
        string property,
        int maximumLength)
    {
        if (!root.TryGetProperty(property, out JsonElement value))
            return "";
        if (value.ValueKind != JsonValueKind.String)
            throw new InvalidDataException(
                $"DependencyControl pending field '{property}' must be a string.");
        string result = value.GetString()!;
        if (result.Length > maximumLength)
            throw new InvalidDataException(
                $"DependencyControl pending field '{property}' exceeds its limit.");
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
            _ => throw new InvalidDataException(
                $"DependencyControl pending field '{property}' must be boolean.")
        };
    }

    private static bool SamePath(string left, string right) =>
        string.Equals(Path.GetFullPath(left), Path.GetFullPath(right), PathComparison);

    private static StringComparison PathComparison => OperatingSystem.IsWindows()
        ? StringComparison.OrdinalIgnoreCase
        : StringComparison.Ordinal;

    private static void RejectReparsePoint(string path, string description)
    {
        if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
            throw new InvalidOperationException(
                $"DependencyControl {description} must not be a link or reparse point.");
    }

    private static StringComparer TargetComparer => OperatingSystem.IsWindows()
        ? StringComparer.OrdinalIgnoreCase
        : StringComparer.Ordinal;

    private sealed record PendingInstall(
        string TransactionId,
        string AutomationRoot,
        long BaseRevision,
        IReadOnlyList<DependencyControlResolvedPackage> Packages,
        IReadOnlyList<DependencyControlPackageIdentity> Removals,
        IReadOnlyList<DependencyControlResolvedFile> Actions);
}
