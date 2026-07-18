using System.Text.Json;
using System.Text.Json.Nodes;

namespace Aegisub.DependencyControl.Plugin;

internal sealed record DependencyControlConfigurationReadResult(
    bool Exists,
    bool Corrupted,
    bool Migrated,
    string BackupName,
    JsonObject Value);

internal sealed class DependencyControlConfigurationStore
{
    private const int MaximumFileBytes = 4 * 1024 * 1024;
    private const int MaximumFileNameLength = 256;
    private const int MaximumSectionDepth = 16;
    private const int MaximumSectionKeyLength = 256;
    private readonly string _root;
    private readonly string _legacyRoot;

    public DependencyControlConfigurationStore(string stateRoot, string legacyRoot = "")
    {
        _root = Path.Combine(stateRoot, "config");
        _legacyRoot = legacyRoot;
    }

    public DependencyControlConfigurationReadResult Read(
        string fileName,
        IReadOnlyList<string> section)
    {
        string path = ResolvePath(fileName);
        bool migrated = TryMigrate(fileName, path);
        DocumentReadResult document = ReadDocument(path, fileName);
        if (document.Root is null)
            return new(
                Exists: false,
                document.Corrupted,
                migrated,
                document.BackupName,
                new JsonObject());
        JsonNode? current = document.Root;
        foreach (string key in ValidateSection(section))
        {
            if (current is not JsonObject currentObject ||
                !currentObject.TryGetPropertyValue(key, out current))
                return new(
                    false,
                    document.Corrupted,
                    migrated,
                    document.BackupName,
                    new JsonObject());
        }
        if (current is not JsonObject value)
            throw new InvalidOperationException(
                "DependencyControl configuration section must be a JSON object.");
        return new(
            Exists: true,
            document.Corrupted,
            migrated,
            document.BackupName,
            (JsonObject)value.DeepClone());
    }

    public void Write(
        string fileName,
        IReadOnlyList<string> section,
        JsonElement value)
    {
        if (value.ValueKind != JsonValueKind.Object)
            throw new InvalidOperationException(
                "DependencyControl configuration values must be a JSON object.");
        string path = ResolvePath(fileName);
        _ = TryMigrate(fileName, path);
        DocumentReadResult document = ReadDocument(path, fileName);
        JsonObject root = document.Root switch
        {
            null => new JsonObject(),
            JsonObject existing => existing,
            _ => throw new InvalidOperationException(
                "DependencyControl configuration root must be a JSON object.")
        };
        JsonObject incoming = JsonNode.Parse(
            value.GetRawText(),
            documentOptions: DocumentOptions)!.AsObject();
        IReadOnlyList<string> validatedSection = ValidateSection(section);
        JsonObject target = root;
        foreach (string key in validatedSection)
        {
            if (!target.TryGetPropertyValue(key, out JsonNode? child) || child is null)
            {
                JsonObject created = new();
                target[key] = created;
                target = created;
            }
            else if (child is JsonObject childObject)
            {
                target = childObject;
            }
            else
            {
                throw new InvalidOperationException(
                    "DependencyControl configuration section crosses a non-object value.");
            }
        }
        foreach ((string key, JsonNode? child) in incoming)
            target[key] = child?.DeepClone();
        SaveDocument(path, root);
    }

    public bool Delete(string fileName, IReadOnlyList<string> section)
    {
        string path = ResolvePath(fileName);
        _ = TryMigrate(fileName, path);
        IReadOnlyList<string> validatedSection = ValidateSection(section);
        if (validatedSection.Count == 0)
        {
            if (!File.Exists(path))
                return false;
            RejectReparsePoint(path, "configuration file");
            File.Delete(path);
            return true;
        }
        DocumentReadResult document = ReadDocument(path, fileName);
        if (document.Root is not JsonObject root)
            return false;
        JsonObject parent = root;
        for (int index = 0; index + 1 < validatedSection.Count; ++index)
        {
            if (!parent.TryGetPropertyValue(
                    validatedSection[index], out JsonNode? child) ||
                child is not JsonObject childObject)
                return false;
            parent = childObject;
        }
        if (!parent.Remove(validatedSection[^1]))
            return false;
        SaveDocument(path, root);
        return true;
    }

    private string ResolvePath(string fileName)
    {
        ValidateFileName(fileName);
        Directory.CreateDirectory(_root);
        RejectReparsePoint(_root, "configuration directory");
        return Path.Combine(_root, fileName);
    }

    private bool TryMigrate(string fileName, string destination)
    {
        if (File.Exists(destination) || _legacyRoot.Length == 0 ||
            !Directory.Exists(_legacyRoot))
            return false;
        RejectReparsePoint(_legacyRoot, "legacy configuration directory");
        string source = Path.Combine(_legacyRoot, fileName);
        if (!File.Exists(source))
            return false;
        RejectReparsePoint(source, "legacy configuration file");
        byte[] payload = File.ReadAllBytes(source);
        if (payload.Length > MaximumFileBytes)
            throw new InvalidOperationException(
                "DependencyControl legacy configuration exceeds the size limit.");
        JsonNode? parsed;
        try
        {
            parsed = ParseJson(payload);
        }
        catch (JsonException error)
        {
            throw new InvalidOperationException(
                "DependencyControl legacy configuration is not valid JSON.", error);
        }
        if (parsed is not JsonObject root)
            throw new InvalidOperationException(
                "DependencyControl legacy configuration root must be an object.");
        SaveDocument(destination, root);
        return true;
    }

    private static void ValidateFileName(string fileName)
    {
        if (string.IsNullOrWhiteSpace(fileName) ||
            fileName.Length > MaximumFileNameLength ||
            fileName is "." or ".." ||
            !fileName.EndsWith(".json", StringComparison.OrdinalIgnoreCase) ||
            fileName[0] is '.' or ' ' ||
            fileName[^1] is '.' or ' ' ||
            fileName.Any(character =>
                char.IsControl(character) ||
                character is '/' or '\\' or ':' or '*' or '?' or '"' or '<' or '>' or '|'))
            throw new InvalidOperationException(
                "DependencyControl configuration requires a safe JSON file name.");
        string stem = Path.GetFileNameWithoutExtension(fileName);
        string device = stem.Split('.')[0];
        if (device.Equals("CON", StringComparison.OrdinalIgnoreCase) ||
            device.Equals("PRN", StringComparison.OrdinalIgnoreCase) ||
            device.Equals("AUX", StringComparison.OrdinalIgnoreCase) ||
            device.Equals("NUL", StringComparison.OrdinalIgnoreCase) ||
            IsNumberedDevice(device, "COM") ||
            IsNumberedDevice(device, "LPT"))
            throw new InvalidOperationException(
                "DependencyControl configuration file name is reserved by the platform.");
    }

    private static bool IsNumberedDevice(string value, string prefix) =>
        value.Length == prefix.Length + 1 &&
        value.StartsWith(prefix, StringComparison.OrdinalIgnoreCase) &&
        value[^1] is >= '1' and <= '9';

    private static IReadOnlyList<string> ValidateSection(IReadOnlyList<string> section)
    {
        ArgumentNullException.ThrowIfNull(section);
        if (section.Count > MaximumSectionDepth)
            throw new InvalidOperationException(
                "DependencyControl configuration section exceeds the depth limit.");
        foreach (string key in section)
        {
            if (string.IsNullOrEmpty(key) ||
                key.Length > MaximumSectionKeyLength ||
                key.Any(char.IsControl))
                throw new InvalidOperationException(
                    "DependencyControl configuration section contains an invalid key.");
        }
        return section;
    }

    private DocumentReadResult ReadDocument(string path, string fileName)
    {
        if (!File.Exists(path))
            return new(null, false, "");
        RejectReparsePoint(path, "configuration file");
        byte[] payload = File.ReadAllBytes(path);
        if (payload.Length > MaximumFileBytes)
            return Quarantine(path, fileName);
        try
        {
            JsonNode? root = ParseJson(payload);
            if (root is not JsonObject)
                return Quarantine(path, fileName);
            return new(root, false, "");
        }
        catch (JsonException)
        {
            return Quarantine(path, fileName);
        }
    }

    private DocumentReadResult Quarantine(string path, string fileName)
    {
        string backupName = $"{fileName}.corrupted-{Guid.NewGuid():N}";
        string backupPath = Path.Combine(_root, backupName);
        File.Move(path, backupPath);
        return new(null, true, backupName);
    }

    private static void SaveDocument(string path, JsonObject root)
    {
        using MemoryStream output = new();
        using (Utf8JsonWriter writer = new(output, new JsonWriterOptions
        {
            Indented = true
        }))
            root.WriteTo(writer);
        byte[] payload = output.ToArray();
        if (payload.Length > MaximumFileBytes)
            throw new InvalidOperationException(
                "DependencyControl configuration exceeds the size limit.");
        string directory = Path.GetDirectoryName(path)!;
        string temporary = Path.Combine(
            directory, $"config-{Guid.NewGuid():N}.tmp");
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
            if (File.Exists(path))
                RejectReparsePoint(path, "configuration file");
            File.Move(temporary, path, overwrite: true);
        }
        finally
        {
            File.Delete(temporary);
        }
    }

    private static JsonNode? ParseJson(ReadOnlySpan<byte> payload)
    {
        ReadOnlySpan<byte> utf8Bom = [0xEF, 0xBB, 0xBF];
        if (payload.StartsWith(utf8Bom))
            payload = payload[utf8Bom.Length..];
        return JsonNode.Parse(payload, documentOptions: DocumentOptions);
    }

    private static void RejectReparsePoint(string path, string description)
    {
        if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
            throw new InvalidOperationException(
                $"DependencyControl {description} must not be a link or reparse point.");
    }

    private static JsonDocumentOptions DocumentOptions { get; } = new()
    {
        AllowTrailingCommas = false,
        CommentHandling = JsonCommentHandling.Disallow,
        MaxDepth = 32
    };

    private sealed record DocumentReadResult(
        JsonNode? Root,
        bool Corrupted,
        string BackupName);
}
