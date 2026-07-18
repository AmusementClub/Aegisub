using System.Text.Json;

namespace Aegisub.DependencyControl.Plugin;

internal sealed record DependencyControlInstalledFile(
    string Target,
    string Sha1);

internal sealed record DependencyControlInstalledPackage(
    string RecordType,
    string Namespace,
    string Name,
    string Version,
    string Description,
    string Author,
    string Feed,
    string Channel,
    string ConfigFile,
    string Source,
    IReadOnlyList<RequiredModule> RequiredModules,
    IReadOnlyList<DependencyControlInstalledFile> Files);

internal sealed record DependencyControlInstalledWriteResult(
    bool Installed,
    bool Changed,
    long Revision);

internal sealed record DependencyControlPackageIdentity(
    string RecordType,
    string Namespace);

internal sealed record DependencyControlOwnershipPlan(
    IReadOnlySet<string> DeleteTargets);

internal sealed record DependencyControlUninstallPlan(
    DependencyControlInstalledPackage Package,
    DependencyControlPackageIdentity Removal,
    IReadOnlyList<DependencyControlResolvedFile> Actions);

internal sealed class DependencyControlInstalledStateStore
{
    private const int MaximumFileBytes = 8 * 1024 * 1024;
    private const int MaximumPackages = 4096;
    private const int MaximumFiles = 32768;
    private const int MaximumTextLength = 16 * 1024;
    private readonly string _root;
    private readonly string _path;
    private readonly Dictionary<string, DependencyControlInstalledPackage> _packages =
        new(StringComparer.Ordinal);
    private long _revision;

    public DependencyControlInstalledStateStore(string stateRoot)
    {
        _root = stateRoot;
        _path = Path.Combine(stateRoot, "installed.json");
        Directory.CreateDirectory(_root);
        RejectReparsePoint(_root, "state directory");
        Load();
    }

    public bool Corrupted { get; private set; }

    public string BackupName { get; private set; } = "";

    public long Revision => _revision;

    public bool TryGet(
        string recordType,
        string packageNamespace,
        out DependencyControlInstalledPackage? package) =>
        _packages.TryGetValue(PackageKey(recordType, packageNamespace), out package);

    public DependencyControlOwnershipPlan PlanInstallOwnership(
        IReadOnlyList<DependencyControlResolvedPackage> packages)
    {
        ArgumentNullException.ThrowIfNull(packages);
        Dictionary<string, DependencyControlResolvedPackage> resolvedPackages =
            new(StringComparer.Ordinal);
        Dictionary<string, string> resolvedFiles = new(TargetComparer);
        HashSet<string> deleteCandidates = new(TargetComparer);

        foreach (DependencyControlResolvedPackage package in packages)
        {
            string key = PackageKey(package.RecordType, package.Namespace);
            if (!resolvedPackages.TryAdd(key, package))
                throw new InvalidOperationException(
                    $"DependencyControl resolved package '{package.Namespace}' more than once.");
            HashSet<string> packageFiles = new(TargetComparer);
            foreach (DependencyControlResolvedFile file in package.Files)
            {
                if (!packageFiles.Add(file.Target))
                    throw new InvalidOperationException(
                        $"DependencyControl package '{package.Namespace}' contains duplicate targets.");
                if (file.Delete)
                {
                    deleteCandidates.Add(file.Target);
                    continue;
                }
                if (resolvedFiles.TryGetValue(file.Target, out string? resolvedHash) &&
                    !resolvedHash.Equals(file.Sha1, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidOperationException(
                        $"DependencyControl packages conflict on target '{file.Target}'.");
                resolvedFiles[file.Target] = file.Sha1;
            }
        }

        HashSet<string> outsideOwnedTargets = new(TargetComparer);
        foreach ((string key, DependencyControlInstalledPackage installed) in _packages)
        {
            if (resolvedPackages.ContainsKey(key))
                continue;
            foreach (DependencyControlInstalledFile file in installed.Files)
            {
                outsideOwnedTargets.Add(file.Target);
                if (resolvedFiles.TryGetValue(file.Target, out string? resolvedHash) &&
                    !resolvedHash.Equals(file.Sha1, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidOperationException(
                        $"DependencyControl target '{file.Target}' is owned by another " +
                        "package with different content.");
            }
        }

        foreach ((string key, DependencyControlResolvedPackage package) in resolvedPackages)
        {
            if (!_packages.TryGetValue(
                    key, out DependencyControlInstalledPackage? installed))
                continue;
            HashSet<string> retainedTargets = package.Files
                .Where(file => !file.Delete)
                .Select(file => file.Target)
                .ToHashSet(TargetComparer);
            foreach (DependencyControlInstalledFile file in installed.Files)
            {
                if (!retainedTargets.Contains(file.Target))
                    deleteCandidates.Add(file.Target);
            }
        }

        deleteCandidates.ExceptWith(resolvedFiles.Keys);
        deleteCandidates.ExceptWith(outsideOwnedTargets);
        return new(deleteCandidates);
    }

    public DependencyControlUninstallPlan PlanUninstall(
        string recordType,
        string packageNamespace)
    {
        string key = PackageKey(recordType, packageNamespace);
        if (!_packages.TryGetValue(key, out DependencyControlInstalledPackage? package))
            throw new InvalidOperationException(
                $"DependencyControl package '{packageNamespace}' is not installed.");
        HashSet<string> outsideOwnedTargets = _packages
            .Where(entry => entry.Key != key)
            .SelectMany(entry => entry.Value.Files)
            .Select(file => file.Target)
            .ToHashSet(TargetComparer);
        DependencyControlResolvedFile[] actions = package.Files
            .Where(file => !outsideOwnedTargets.Contains(file.Target))
            .Select(file => new DependencyControlResolvedFile(
                file.Target,
                file.Sha1,
                true))
            .OrderBy(file => file.Target, StringComparer.Ordinal)
            .ToArray();
        return new(
            package,
            new(recordType, packageNamespace),
            actions);
    }

    public DependencyControlInstalledWriteResult UpsertDiscovered(RegisteredRecord record)
    {
        ArgumentNullException.ThrowIfNull(record);
        if (record.Virtual)
            return new(false, false, _revision);

        string key = PackageKey(record.RecordType, record.Namespace);
        _packages.TryGetValue(key, out DependencyControlInstalledPackage? existing);
        DependencyControlInstalledPackage updated = new(
            record.RecordType,
            record.Namespace,
            record.Name,
            record.Version,
            record.Description,
            record.Author,
            record.Feed.Length > 0 ? record.Feed : existing?.Feed ?? "",
            record.ActiveChannel.Length > 0 ? record.ActiveChannel : existing?.Channel ?? "",
            record.ConfigFile,
            existing?.Source ?? "discovered",
            record.RequiredModules.ToArray(),
            existing?.Files ?? []);
        if (existing is not null && PackageEquals(existing, updated))
            return new(true, false, _revision);

        _packages[key] = updated;
        try
        {
            SaveNextRevision();
        }
        catch
        {
            if (existing is null)
                _packages.Remove(key);
            else
                _packages[key] = existing;
            throw;
        }
        return new(true, true, _revision);
    }

    public DependencyControlInstalledWriteResult ApplyInstall(
        IReadOnlyList<DependencyControlResolvedPackage> packages)
        => ApplyTransaction(packages, []);

    public DependencyControlInstalledWriteResult ApplyTransaction(
        IReadOnlyList<DependencyControlResolvedPackage> packages,
        IReadOnlyList<DependencyControlPackageIdentity> removals)
    {
        ArgumentNullException.ThrowIfNull(packages);
        ArgumentNullException.ThrowIfNull(removals);
        HashSet<string> removalKeys = new(StringComparer.Ordinal);
        foreach (DependencyControlPackageIdentity removal in removals)
        {
            string key = PackageKey(removal.RecordType, removal.Namespace);
            if (!removalKeys.Add(key))
                throw new InvalidOperationException(
                    "DependencyControl transaction contains duplicate package removals.");
        }
        foreach (DependencyControlResolvedPackage package in packages)
        {
            if (removalKeys.Contains(PackageKey(package.RecordType, package.Namespace)))
                throw new InvalidOperationException(
                    $"DependencyControl transaction both installs and removes " +
                    $"package '{package.Namespace}'.");
        }

        Dictionary<string, DependencyControlInstalledPackage> previous =
            new(_packages, StringComparer.Ordinal);
        bool changed = false;
        foreach (string key in removalKeys)
            changed |= _packages.Remove(key);
        foreach (DependencyControlResolvedPackage package in packages)
        {
            string key = PackageKey(package.RecordType, package.Namespace);
            _packages.TryGetValue(key, out DependencyControlInstalledPackage? existing);
            DependencyControlInstalledPackage updated = BuildResolvedPackage(
                package, existing);
            if (existing is not null && PackageEquals(existing, updated))
                continue;
            _packages[key] = updated;
            changed = true;
        }
        if (changed)
        {
            try
            {
                SaveNextRevision();
            }
            catch
            {
                _packages.Clear();
                foreach ((string key, DependencyControlInstalledPackage value) in previous)
                    _packages.Add(key, value);
                throw;
            }
        }
        return new(packages.Count > 0 || removals.Count > 0, changed, _revision);
    }

    public bool MatchesInstall(IReadOnlyList<DependencyControlResolvedPackage> packages)
        => MatchesTransaction(packages, []);

    public bool MatchesTransaction(
        IReadOnlyList<DependencyControlResolvedPackage> packages,
        IReadOnlyList<DependencyControlPackageIdentity> removals)
    {
        ArgumentNullException.ThrowIfNull(packages);
        ArgumentNullException.ThrowIfNull(removals);
        foreach (DependencyControlPackageIdentity removal in removals)
        {
            if (_packages.ContainsKey(PackageKey(removal.RecordType, removal.Namespace)))
                return false;
        }
        foreach (DependencyControlResolvedPackage package in packages)
        {
            string key = PackageKey(package.RecordType, package.Namespace);
            if (!_packages.TryGetValue(
                    key, out DependencyControlInstalledPackage? existing) ||
                !PackageEquals(existing, BuildResolvedPackage(package, existing)))
                return false;
        }
        return true;
    }

    public void WriteList(Utf8JsonWriter writer, int offset, int limit)
    {
        ArgumentNullException.ThrowIfNull(writer);
        writer.WriteStartObject();
        writer.WriteNumber("schemaVersion", 1);
        writer.WriteNumber("revision", _revision);
        writer.WriteBoolean("corrupted", Corrupted);
        if (BackupName.Length > 0)
            writer.WriteString("backupName", BackupName);
        writer.WriteNumber("totalCount", _packages.Count);
        writer.WriteNumber("offset", offset);
        writer.WritePropertyName("packages");
        writer.WriteStartArray();
        foreach (DependencyControlInstalledPackage package in OrderedPackages()
                     .Skip(offset)
                     .Take(limit))
            WritePackageSummary(writer, package);
        writer.WriteEndArray();
        writer.WriteEndObject();
    }

    public void WritePackage(
        Utf8JsonWriter writer,
        string recordType,
        string packageNamespace,
        int requirementOffset,
        int requirementLimit,
        int fileOffset,
        int fileLimit)
    {
        ArgumentNullException.ThrowIfNull(writer);
        bool exists = _packages.TryGetValue(
            PackageKey(recordType, packageNamespace),
            out DependencyControlInstalledPackage? package);
        writer.WriteStartObject();
        writer.WriteNumber("schemaVersion", 1);
        writer.WriteNumber("revision", _revision);
        writer.WriteBoolean("corrupted", Corrupted);
        if (BackupName.Length > 0)
            writer.WriteString("backupName", BackupName);
        writer.WriteBoolean("exists", exists);
        if (package is not null)
        {
            writer.WritePropertyName("package");
            writer.WriteStartObject();
            WritePackageIdentity(writer, package);
            writer.WriteNumber("requiredModuleCount", package.RequiredModules.Count);
            writer.WriteNumber("requirementOffset", requirementOffset);
            writer.WritePropertyName("requiredModules");
            writer.WriteStartArray();
            foreach (RequiredModule module in package.RequiredModules
                         .Skip(requirementOffset)
                         .Take(requirementLimit))
                WriteRequiredModule(writer, module);
            writer.WriteEndArray();
            writer.WriteNumber("fileCount", package.Files.Count);
            writer.WriteNumber("fileOffset", fileOffset);
            writer.WritePropertyName("files");
            writer.WriteStartArray();
            foreach (DependencyControlInstalledFile file in package.Files
                         .Skip(fileOffset)
                         .Take(fileLimit))
                WriteInstalledFile(writer, file);
            writer.WriteEndArray();
            writer.WriteEndObject();
        }
        writer.WriteEndObject();
    }

    private void Load()
    {
        if (!File.Exists(_path))
            return;
        RejectReparsePoint(_path, "installed-state file");
        byte[] payload = File.ReadAllBytes(_path);
        if (payload.Length > MaximumFileBytes)
        {
            Quarantine();
            return;
        }
        try
        {
            using JsonDocument document = JsonDocument.Parse(
                payload,
                new JsonDocumentOptions
                {
                    AllowTrailingCommas = false,
                    CommentHandling = JsonCommentHandling.Disallow,
                    MaxDepth = 32
                });
            ReadState(document.RootElement);
        }
        catch (Exception error) when (
            error is JsonException or InvalidDataException or OverflowException)
        {
            _packages.Clear();
            _revision = 0;
            Quarantine();
        }
    }

    private void ReadState(JsonElement root)
    {
        if (root.ValueKind != JsonValueKind.Object ||
            !root.TryGetProperty("schemaVersion", out JsonElement schemaVersion) ||
            !schemaVersion.TryGetInt32(out int schema) || schema != 1 ||
            !root.TryGetProperty("revision", out JsonElement revision) ||
            !revision.TryGetInt64(out long parsedRevision) || parsedRevision < 0 ||
            !root.TryGetProperty("packages", out JsonElement packages) ||
            packages.ValueKind != JsonValueKind.Array ||
            packages.GetArrayLength() > MaximumPackages)
            throw new InvalidDataException("DependencyControl installed state is invalid.");

        Dictionary<string, DependencyControlInstalledPackage> parsed =
            new(StringComparer.Ordinal);
        int totalFiles = 0;
        foreach (JsonElement package in packages.EnumerateArray())
        {
            DependencyControlInstalledPackage value = ReadPackage(package, ref totalFiles);
            if (!parsed.TryAdd(PackageKey(value.RecordType, value.Namespace), value))
                throw new InvalidDataException(
                    "DependencyControl installed state contains a duplicate package.");
        }
        _revision = parsedRevision;
        _packages.Clear();
        foreach ((string key, DependencyControlInstalledPackage value) in parsed)
            _packages.Add(key, value);
    }

    private static DependencyControlInstalledPackage ReadPackage(
        JsonElement root,
        ref int totalFiles)
    {
        if (root.ValueKind != JsonValueKind.Object)
            throw new InvalidDataException(
                "DependencyControl installed package must be an object.");
        string recordType = ReadRequiredString(root, "recordType", 32);
        if (recordType is not ("automation" or "module"))
            throw new InvalidDataException(
                "DependencyControl installed package type is invalid.");
        string packageNamespace = ReadRequiredString(root, "namespace", 512);
        if (!IsSafeNamespace(packageNamespace))
            throw new InvalidDataException(
                "DependencyControl installed package namespace is invalid.");
        string feed = ReadOptionalString(root, "feed", MaximumTextLength);
        if (feed.Length > 0 &&
            (!Uri.TryCreate(feed, UriKind.Absolute, out Uri? feedUri) ||
             feedUri.Scheme is not ("http" or "https")))
            throw new InvalidDataException(
                "DependencyControl installed package feed is invalid.");
        string configFile = ReadRequiredString(root, "configFile", 256);
        ValidateConfigFile(configFile);
        string source = ReadRequiredString(root, "source", 32);
        if (source is not ("discovered" or "transaction"))
            throw new InvalidDataException(
                "DependencyControl installed package source is invalid.");
        IReadOnlyList<RequiredModule> requiredModules = ReadRequiredModules(root);
        IReadOnlyList<DependencyControlInstalledFile> files = ReadFiles(root);
        totalFiles = checked(totalFiles + files.Count);
        if (totalFiles > MaximumFiles)
            throw new InvalidDataException(
                "DependencyControl installed state exceeds the file limit.");
        return new(
            recordType,
            packageNamespace,
            ReadRequiredString(root, "name", MaximumTextLength),
            ReadRequiredString(root, "version", 256),
            ReadOptionalString(root, "description", MaximumTextLength),
            ReadOptionalString(root, "author", MaximumTextLength),
            feed,
            ReadOptionalString(root, "channel", 256),
            configFile,
            source,
            requiredModules,
            files);
    }

    private static IReadOnlyList<RequiredModule> ReadRequiredModules(JsonElement root)
    {
        if (!root.TryGetProperty("requiredModules", out JsonElement modules) ||
            modules.ValueKind != JsonValueKind.Array || modules.GetArrayLength() > 256)
            throw new InvalidDataException(
                "DependencyControl installed package requirements are invalid.");
        List<RequiredModule> result = new(modules.GetArrayLength());
        foreach (JsonElement module in modules.EnumerateArray())
        {
            if (module.ValueKind != JsonValueKind.Object)
                throw new InvalidDataException(
                    "DependencyControl installed requirement must be an object.");
            string moduleName = ReadRequiredString(module, "moduleName", 512);
            if (!IsSafeNamespace(moduleName))
                throw new InvalidDataException(
                    "DependencyControl installed requirement namespace is invalid.");
            string feed = ReadOptionalString(module, "feed", MaximumTextLength);
            if (feed.Length > 0 &&
                (!Uri.TryCreate(feed, UriKind.Absolute, out Uri? feedUri) ||
                 feedUri.Scheme is not ("http" or "https")))
                throw new InvalidDataException(
                    "DependencyControl installed requirement feed is invalid.");
            result.Add(new(
                moduleName,
                ReadOptionalString(module, "version", 256),
                feed,
                ReadOptionalString(module, "channel", 256),
                ReadOptionalBoolean(module, "optional")));
        }
        return result;
    }

    private static IReadOnlyList<DependencyControlInstalledFile> ReadFiles(JsonElement root)
    {
        if (!root.TryGetProperty("files", out JsonElement files) ||
            files.ValueKind != JsonValueKind.Array || files.GetArrayLength() > MaximumFiles)
            throw new InvalidDataException(
                "DependencyControl installed package files are invalid.");
        List<DependencyControlInstalledFile> result = new(files.GetArrayLength());
        HashSet<string> targets = new(TargetComparer);
        foreach (JsonElement file in files.EnumerateArray())
        {
            if (file.ValueKind != JsonValueKind.Object)
                throw new InvalidDataException(
                    "DependencyControl installed file must be an object.");
            string target = ReadRequiredString(file, "target", 4096);
            if (!IsSafeTarget(target) || !targets.Add(target))
                throw new InvalidDataException(
                    "DependencyControl installed file target is invalid or duplicated.");
            string sha1 = ReadRequiredString(file, "sha1", 40);
            if (sha1.Length != 40 || !sha1.All(Uri.IsHexDigit))
                throw new InvalidDataException(
                    "DependencyControl installed file hash is invalid.");
            result.Add(new(target, sha1.ToUpperInvariant()));
        }
        return result;
    }

    private void SaveNextRevision()
    {
        long previous = _revision;
        _revision = checked(_revision + 1);
        try
        {
            Save();
        }
        catch
        {
            _revision = previous;
            throw;
        }
    }

    private void Save()
    {
        using MemoryStream output = new();
        using (Utf8JsonWriter writer = new(output, new JsonWriterOptions
        {
            Indented = true
        }))
        {
            writer.WriteStartObject();
            writer.WriteNumber("schemaVersion", 1);
            writer.WriteNumber("revision", _revision);
            WritePackages(writer);
            writer.WriteEndObject();
        }
        byte[] payload = output.ToArray();
        if (payload.Length > MaximumFileBytes)
            throw new InvalidOperationException(
                "DependencyControl installed state exceeds the size limit.");
        string temporary = Path.Combine(
            _root, $"installed-{Guid.NewGuid():N}.tmp");
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
            if (File.Exists(_path))
                RejectReparsePoint(_path, "installed-state file");
            File.Move(temporary, _path, overwrite: true);
        }
        finally
        {
            File.Delete(temporary);
        }
    }

    private void WritePackages(Utf8JsonWriter writer)
    {
        writer.WritePropertyName("packages");
        writer.WriteStartArray();
        foreach (DependencyControlInstalledPackage package in OrderedPackages())
        {
            writer.WriteStartObject();
            WritePackageIdentity(writer, package);
            writer.WritePropertyName("requiredModules");
            writer.WriteStartArray();
            foreach (RequiredModule module in package.RequiredModules)
                WriteRequiredModule(writer, module);
            writer.WriteEndArray();
            writer.WritePropertyName("files");
            writer.WriteStartArray();
            foreach (DependencyControlInstalledFile file in package.Files)
                WriteInstalledFile(writer, file);
            writer.WriteEndArray();
            writer.WriteEndObject();
        }
        writer.WriteEndArray();
    }

    private IEnumerable<DependencyControlInstalledPackage> OrderedPackages() =>
        _packages.Values
            .OrderBy(package => package.RecordType, StringComparer.Ordinal)
            .ThenBy(package => package.Namespace, StringComparer.Ordinal);

    private static void WritePackageSummary(
        Utf8JsonWriter writer,
        DependencyControlInstalledPackage package)
    {
        writer.WriteStartObject();
        writer.WriteString("recordType", package.RecordType);
        writer.WriteString("namespace", package.Namespace);
        writer.WriteString("name", package.Name);
        writer.WriteString("version", package.Version);
        writer.WriteString("feed", package.Feed);
        writer.WriteString("channel", package.Channel);
        writer.WriteString("source", package.Source);
        writer.WriteNumber("requiredModuleCount", package.RequiredModules.Count);
        writer.WriteNumber("fileCount", package.Files.Count);
        writer.WriteEndObject();
    }

    private static void WritePackageIdentity(
        Utf8JsonWriter writer,
        DependencyControlInstalledPackage package)
    {
        writer.WriteString("recordType", package.RecordType);
        writer.WriteString("namespace", package.Namespace);
        writer.WriteString("name", package.Name);
        writer.WriteString("version", package.Version);
        writer.WriteString("description", package.Description);
        writer.WriteString("author", package.Author);
        writer.WriteString("feed", package.Feed);
        writer.WriteString("channel", package.Channel);
        writer.WriteString("configFile", package.ConfigFile);
        writer.WriteString("source", package.Source);
    }

    private static void WriteRequiredModule(Utf8JsonWriter writer, RequiredModule module)
    {
        writer.WriteStartObject();
        writer.WriteString("moduleName", module.ModuleName);
        writer.WriteString("version", module.Version);
        writer.WriteString("feed", module.Feed);
        writer.WriteString("channel", module.Channel);
        writer.WriteBoolean("optional", module.Optional);
        writer.WriteEndObject();
    }

    private static void WriteInstalledFile(
        Utf8JsonWriter writer,
        DependencyControlInstalledFile file)
    {
        writer.WriteStartObject();
        writer.WriteString("target", file.Target);
        writer.WriteString("sha1", file.Sha1);
        writer.WriteEndObject();
    }

    private void Quarantine()
    {
        BackupName = $"installed.json.corrupted-{Guid.NewGuid():N}";
        File.Move(_path, Path.Combine(_root, BackupName));
        Corrupted = true;
    }

    private static bool PackageEquals(
        DependencyControlInstalledPackage left,
        DependencyControlInstalledPackage right) =>
        left.RecordType == right.RecordType &&
        left.Namespace == right.Namespace &&
        left.Name == right.Name &&
        left.Version == right.Version &&
        left.Description == right.Description &&
        left.Author == right.Author &&
        left.Feed == right.Feed &&
        left.Channel == right.Channel &&
        left.ConfigFile == right.ConfigFile &&
        left.Source == right.Source &&
        left.RequiredModules.SequenceEqual(right.RequiredModules) &&
        left.Files.SequenceEqual(right.Files);

    private static DependencyControlInstalledPackage BuildResolvedPackage(
        DependencyControlResolvedPackage package,
        DependencyControlInstalledPackage? existing)
    {
        DependencyControlInstalledFile[] files = package.Files
            .Where(file => !file.Delete)
            .Select(file => new DependencyControlInstalledFile(file.Target, file.Sha1))
            .OrderBy(file => file.Target, StringComparer.Ordinal)
            .ToArray();
        return new(
            package.RecordType,
            package.Namespace,
            package.Name,
            package.Version,
            package.Description,
            package.Author,
            package.Feed,
            package.Channel,
            existing?.ConfigFile ?? $"{package.Namespace}.json",
            "transaction",
            package.RequiredModules.ToArray(),
            files);
    }

    private static string PackageKey(string recordType, string packageNamespace) =>
        $"{recordType}\n{packageNamespace}";

    private static StringComparer TargetComparer => OperatingSystem.IsWindows()
        ? StringComparer.OrdinalIgnoreCase
        : StringComparer.Ordinal;

    private static string ReadRequiredString(
        JsonElement root,
        string property,
        int maximumLength)
    {
        string result = ReadOptionalString(root, property, maximumLength);
        if (string.IsNullOrWhiteSpace(result))
            throw new InvalidDataException(
                $"DependencyControl installed-state field '{property}' is required.");
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
                $"DependencyControl installed-state field '{property}' must be a string.");
        string result = value.GetString()!;
        if (result.Length > maximumLength)
            throw new InvalidDataException(
                $"DependencyControl installed-state field '{property}' exceeds its limit.");
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
                $"DependencyControl installed-state field '{property}' must be boolean.")
        };
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
            if (!(character is >= 'a' and <= 'z' or
                  >= 'A' and <= 'Z' or
                  >= '0' and <= '9' or '-' or '_'))
                return false;
            componentHasCharacters = true;
        }
        return sawDot && componentHasCharacters;
    }

    private static bool IsSafeTarget(string value)
    {
        if (value.Length == 0 || value[0] is '/' or '\\' ||
            value.Contains('\\', StringComparison.Ordinal) ||
            value.Contains(':', StringComparison.Ordinal) ||
            value.Any(char.IsControl))
            return false;
        string[] components = value.Split('/');
        if (components.Any(component => component.Length == 0 || component is "." or ".."))
            return false;
        return value.StartsWith("autoload/", StringComparison.Ordinal) ||
            value.StartsWith("include/", StringComparison.Ordinal) ||
            value.StartsWith("tests/DepUnit/macros/", StringComparison.Ordinal) ||
            value.StartsWith("tests/DepUnit/modules/", StringComparison.Ordinal);
    }

    internal static void ValidateConfigFile(string fileName)
    {
        if (fileName.Length == 0 || !fileName.EndsWith(".json", StringComparison.OrdinalIgnoreCase) ||
            fileName[0] is '.' or ' ' || fileName[^1] is '.' or ' ' ||
            fileName.Any(character => char.IsControl(character) ||
                character is '/' or '\\' or ':' or '*' or '?' or '"' or '<' or '>' or '|'))
            throw new InvalidDataException(
                "DependencyControl installed package config file is invalid.");
        string device = Path.GetFileNameWithoutExtension(fileName).Split('.')[0];
        if (device.Equals("CON", StringComparison.OrdinalIgnoreCase) ||
            device.Equals("PRN", StringComparison.OrdinalIgnoreCase) ||
            device.Equals("AUX", StringComparison.OrdinalIgnoreCase) ||
            device.Equals("NUL", StringComparison.OrdinalIgnoreCase) ||
            IsNumberedDevice(device, "COM") || IsNumberedDevice(device, "LPT"))
            throw new InvalidDataException(
                "DependencyControl installed package config file is reserved.");
    }

    private static bool IsNumberedDevice(string value, string prefix) =>
        value.Length == prefix.Length + 1 &&
        value.StartsWith(prefix, StringComparison.OrdinalIgnoreCase) &&
        value[^1] is >= '1' and <= '9';

    private static void RejectReparsePoint(string path, string description)
    {
        if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
            throw new InvalidOperationException(
                $"DependencyControl {description} must not be a link or reparse point.");
    }
}
