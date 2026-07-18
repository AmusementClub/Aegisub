using System.Text;
using System.Text.Json;

namespace Aegisub.DependencyControl.Plugin;

internal enum DependencyControlProxyMode
{
    System,
    Direct,
    Manual
}

internal sealed record DependencyControlNetworkSettings(
    DependencyControlProxyMode Mode,
    string ManualProxyUri,
    IReadOnlyList<string> Bypass,
    bool UseDefaultCredentials)
{
    private const int MaximumProxyUriLength = 2048;
    private const int MaximumBypassEntries = 64;
    private const int MaximumBypassEntryLength = 256;

    public static DependencyControlNetworkSettings Default { get; } = new(
        DependencyControlProxyMode.System,
        "",
        [],
        false);

    public static DependencyControlNetworkSettings Parse(JsonElement root)
    {
        if (root.ValueKind != JsonValueKind.Object)
            throw new InvalidOperationException(
                "DependencyControl network settings must be a JSON object.");
        string modeText = ReadOptionalString(root, "mode", 32) ?? "System";
        if (!Enum.TryParse(modeText, ignoreCase: true, out DependencyControlProxyMode mode) ||
            !Enum.IsDefined(mode))
            throw new InvalidOperationException(
                "DependencyControl proxy mode must be System, Direct, or Manual.");
        string manualProxyUri = ReadOptionalString(
            root, "manualProxyUri", MaximumProxyUriLength) ?? "";
        IReadOnlyList<string> bypass = ReadBypass(root);
        bool useDefaultCredentials = ReadOptionalBoolean(
            root, "useDefaultCredentials");

        if (mode == DependencyControlProxyMode.Manual)
            manualProxyUri = NormalizeManualProxyUri(manualProxyUri);
        else if (manualProxyUri.Length > 0)
            _ = NormalizeManualProxyUri(manualProxyUri);

        return new(mode, manualProxyUri, bypass, useDefaultCredentials);
    }

    public string ToJson() => DependencyControlServiceContribution.BuildJson(Write);

    public void Write(Utf8JsonWriter writer)
    {
        writer.WriteStartObject();
        writer.WriteNumber("schemaVersion", 1);
        writer.WriteString("mode", Mode.ToString());
        writer.WriteString("manualProxyUri", ManualProxyUri);
        writer.WritePropertyName("bypass");
        writer.WriteStartArray();
        foreach (string entry in Bypass)
            writer.WriteStringValue(entry);
        writer.WriteEndArray();
        writer.WriteBoolean("useDefaultCredentials", UseDefaultCredentials);
        writer.WriteEndObject();
    }

    public (bool BypassOnLocal, string[] Patterns) BuildBypassPatterns()
    {
        bool bypassOnLocal = false;
        List<string> patterns = [];
        foreach (string entry in Bypass)
        {
            if (entry == "<local>")
            {
                bypassOnLocal = true;
                continue;
            }
            string value = entry.StartsWith("*.", StringComparison.Ordinal)
                ? entry[2..]
                : entry;
            string escaped = System.Text.RegularExpressions.Regex.Escape(value);
            patterns.Add(entry.StartsWith("*.", StringComparison.Ordinal)
                ? $"^https?://(?:.+\\.)?{escaped}(?::[0-9]+)?(?:/|$)"
                : $"^https?://{escaped}(?::[0-9]+)?(?:/|$)");
        }
        return (bypassOnLocal, [.. patterns]);
    }

    private static string NormalizeManualProxyUri(string value)
    {
        if (string.IsNullOrWhiteSpace(value) ||
            !Uri.TryCreate(value, UriKind.Absolute, out Uri? uri) ||
            (uri.Scheme != Uri.UriSchemeHttp &&
                uri.Scheme != Uri.UriSchemeHttps &&
                !uri.Scheme.Equals("socks5", StringComparison.OrdinalIgnoreCase)) ||
            string.IsNullOrEmpty(uri.Host) ||
            uri.UserInfo.Length > 0 ||
            uri.Query.Length > 0 ||
            uri.Fragment.Length > 0 ||
            (uri.AbsolutePath.Length > 0 && uri.AbsolutePath != "/"))
            throw new InvalidOperationException(
                "DependencyControl manual proxy URI must be an HTTP(S) or SOCKS5 origin without credentials.");
        return uri.GetComponents(
            UriComponents.SchemeAndServer,
            UriFormat.UriEscaped);
    }

    private static IReadOnlyList<string> ReadBypass(JsonElement root)
    {
        if (!root.TryGetProperty("bypass", out JsonElement value))
            return [];
        if (value.ValueKind != JsonValueKind.Array ||
            value.GetArrayLength() > MaximumBypassEntries)
            throw new InvalidOperationException(
                "DependencyControl proxy bypass must be a bounded array.");
        List<string> result = new(value.GetArrayLength());
        HashSet<string> unique = new(StringComparer.OrdinalIgnoreCase);
        foreach (JsonElement item in value.EnumerateArray())
        {
            if (item.ValueKind != JsonValueKind.String)
                throw new InvalidOperationException(
                    "DependencyControl proxy bypass entries must be strings.");
            string entry = (item.GetString() ?? "").Trim();
            if (!IsSafeBypassEntry(entry))
                throw new InvalidOperationException(
                    "DependencyControl proxy bypass contains an invalid host pattern.");
            if (unique.Add(entry))
                result.Add(entry.ToLowerInvariant());
        }
        return result;
    }

    private static bool IsSafeBypassEntry(string value)
    {
        if (value == "<local>")
            return true;
        if (value.Length is 0 or > MaximumBypassEntryLength ||
            value.Any(char.IsControl) ||
            value.Any(char.IsWhiteSpace) ||
            value.Contains('/') || value.Contains('\\') || value.Contains('@') ||
            value.Contains('?') || value.Contains('#') || value.Contains('%'))
            return false;
        int wildcard = value.IndexOf('*');
        if (wildcard >= 0 && (wildcard != 0 || !value.StartsWith("*.", StringComparison.Ordinal)))
            return false;
        return value.All(character =>
            char.IsAsciiLetterOrDigit(character) ||
            character is '.' or '-' or '_' or '*' or ':' or '[' or ']');
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
                $"DependencyControl network field '{property}' must be a string.");
        string result = value.GetString()!;
        if (result.Length > maximumLength || result.Any(char.IsControl))
            throw new InvalidOperationException(
                $"DependencyControl network field '{property}' is invalid.");
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
                $"DependencyControl network field '{property}' must be boolean.")
        };
    }
}

internal sealed class DependencyControlNetworkSettingsStore(string stateRoot)
{
    private const int MaximumSettingsBytes = 64 * 1024;
    private readonly string _path = Path.Combine(stateRoot, "network.json");

    public DependencyControlNetworkSettings Load()
    {
        if (!File.Exists(_path))
            return DependencyControlNetworkSettings.Default;
        byte[] payload = File.ReadAllBytes(_path);
        if (payload.Length > MaximumSettingsBytes)
            throw new InvalidOperationException(
                "DependencyControl network settings exceed the size limit.");
        using JsonDocument document = JsonDocument.Parse(payload, new JsonDocumentOptions
        {
            CommentHandling = JsonCommentHandling.Disallow,
            MaxDepth = 16
        });
        return DependencyControlNetworkSettings.Parse(document.RootElement);
    }

    public void Save(DependencyControlNetworkSettings settings)
    {
        ArgumentNullException.ThrowIfNull(settings);
        string directory = Path.GetDirectoryName(_path)!;
        Directory.CreateDirectory(directory);
        string temporary = Path.Combine(
            directory, $"network-{Guid.NewGuid():N}.tmp");
        try
        {
            byte[] payload = Encoding.UTF8.GetBytes(settings.ToJson());
            if (payload.Length > MaximumSettingsBytes)
                throw new InvalidOperationException(
                    "DependencyControl network settings exceed the size limit.");
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
            File.Move(temporary, _path, overwrite: true);
        }
        finally
        {
            File.Delete(temporary);
        }
    }
}
