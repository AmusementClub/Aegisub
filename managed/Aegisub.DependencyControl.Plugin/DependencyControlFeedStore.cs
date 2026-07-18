using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;

namespace Aegisub.DependencyControl.Plugin;

internal sealed record DependencyControlConfiguredFeed(
    string Id,
    string Label,
    string Url,
    bool Enabled);

internal sealed class DependencyControlFeedStore
{
    private const int MaximumFileBytes = 256 * 1024;
    private const int MaximumFeeds = 64;
    private const int MaximumIdLength = 64;
    private const int MaximumLabelLength = 256;
    private const int MaximumUrlLength = 16 * 1024;

    private readonly string _path;
    private readonly Dictionary<string, DependencyControlConfiguredFeed> _feeds =
        new(StringComparer.Ordinal);

    public DependencyControlFeedStore(string stateRoot)
    {
        _path = Path.Combine(stateRoot, "feeds.json");
        if (!File.Exists(_path))
            return;
        try
        {
            Load();
        }
        catch (Exception error) when (
            error is JsonException or InvalidOperationException or DecoderFallbackException)
        {
            Corrupted = true;
            Quarantine();
            _feeds.Clear();
            Revision = 0;
        }
    }

    public bool Corrupted { get; }
    public long Revision { get; private set; }

    public IReadOnlyList<DependencyControlConfiguredFeed> List() => _feeds.Values
        .OrderBy(feed => feed.Label, StringComparer.OrdinalIgnoreCase)
        .ThenBy(feed => feed.Id, StringComparer.Ordinal)
        .ToArray();

    public bool TryGet(string id, out DependencyControlConfiguredFeed? feed) =>
        _feeds.TryGetValue(id, out feed);

    public DependencyControlConfiguredFeed Upsert(
        string id,
        string label,
        string url,
        bool enabled)
    {
        string normalizedId = id.Length == 0 ? Guid.NewGuid().ToString("N") : ValidateId(id);
        string normalizedLabel = ValidateLabel(label);
        string normalizedUrl = NormalizeUrl(url);
        if (!_feeds.ContainsKey(normalizedId) && _feeds.Count >= MaximumFeeds)
            throw new InvalidOperationException(
                "DependencyControl configured feeds exceed the feed limit.");
        if (_feeds.Values.Any(feed =>
                !string.Equals(feed.Id, normalizedId, StringComparison.Ordinal) &&
                string.Equals(feed.Url, normalizedUrl, StringComparison.OrdinalIgnoreCase)))
            throw new InvalidOperationException(
                "DependencyControl already has a configured feed with this URL.");

        Dictionary<string, DependencyControlConfiguredFeed> previous =
            new(_feeds, StringComparer.Ordinal);
        long previousRevision = Revision;
        DependencyControlConfiguredFeed updated = new(
            normalizedId, normalizedLabel, normalizedUrl, enabled);
        _feeds[normalizedId] = updated;
        Revision = checked(Revision + 1);
        try
        {
            Save();
            return updated;
        }
        catch
        {
            Restore(previous, previousRevision);
            throw;
        }
    }

    public bool Remove(string id)
    {
        string normalizedId = ValidateId(id);
        if (!_feeds.ContainsKey(normalizedId))
            return false;
        Dictionary<string, DependencyControlConfiguredFeed> previous =
            new(_feeds, StringComparer.Ordinal);
        long previousRevision = Revision;
        _feeds.Remove(normalizedId);
        Revision = checked(Revision + 1);
        try
        {
            Save();
            return true;
        }
        catch
        {
            Restore(previous, previousRevision);
            throw;
        }
    }

    public static string ValidateId(string value)
    {
        if (value.Length is 0 or > MaximumIdLength ||
            !value.All(character => char.IsAsciiLetterOrDigit(character) ||
                character is '-' or '_'))
            throw new InvalidOperationException(
                "DependencyControl feed ID is invalid.");
        return value;
    }

    public static string ValidateLabel(string value)
    {
        string normalized = value.Trim();
        if (normalized.Length is 0 or > MaximumLabelLength || normalized.Any(char.IsControl))
            throw new InvalidOperationException(
                "DependencyControl feed label is invalid.");
        return normalized;
    }

    public static string NormalizeUrl(string value)
    {
        string normalized = value.Trim();
        if (normalized.Length is 0 or > MaximumUrlLength ||
            normalized.Any(char.IsControl) ||
            !Uri.TryCreate(normalized, UriKind.Absolute, out Uri? uri) ||
            uri.Scheme is not ("http" or "https") ||
            string.IsNullOrEmpty(uri.Host) || uri.UserInfo.Length > 0 ||
            uri.Fragment.Length > 0)
            throw new InvalidOperationException(
                "DependencyControl feed URL must be an HTTP(S) URL without credentials or a fragment.");
        return uri.AbsoluteUri;
    }

    private void Load()
    {
        byte[] payload = File.ReadAllBytes(_path);
        if (payload.Length > MaximumFileBytes)
            throw new InvalidOperationException(
                "DependencyControl feed settings exceed the size limit.");
        string json = new UTF8Encoding(false, true).GetString(payload);
        using JsonDocument document = JsonDocument.Parse(json, new JsonDocumentOptions
        {
            CommentHandling = JsonCommentHandling.Disallow,
            MaxDepth = 16
        });
        JsonElement root = document.RootElement;
        if (root.ValueKind != JsonValueKind.Object ||
            !root.TryGetProperty("schemaVersion", out JsonElement schema) ||
            !schema.TryGetInt32(out int schemaVersion) || schemaVersion != 1 ||
            !root.TryGetProperty("revision", out JsonElement revision) ||
            !revision.TryGetInt64(out long parsedRevision) || parsedRevision < 0 ||
            !root.TryGetProperty("feeds", out JsonElement feeds) ||
            feeds.ValueKind != JsonValueKind.Array || feeds.GetArrayLength() > MaximumFeeds)
            throw new InvalidOperationException(
                "DependencyControl feed settings have an invalid structure.");

        HashSet<string> urls = new(StringComparer.OrdinalIgnoreCase);
        foreach (JsonElement item in feeds.EnumerateArray())
        {
            if (item.ValueKind != JsonValueKind.Object)
                throw new InvalidOperationException(
                    "DependencyControl configured feeds must be objects.");
            string id = ValidateId(ReadString(item, "id"));
            string label = ValidateLabel(ReadString(item, "label"));
            string url = NormalizeUrl(ReadString(item, "url"));
            bool enabled = !item.TryGetProperty("enabled", out JsonElement enabledValue) ||
                enabledValue.ValueKind switch
                {
                    JsonValueKind.True => true,
                    JsonValueKind.False => false,
                    _ => throw new InvalidOperationException(
                        "DependencyControl configured feed enabled state is invalid.")
                };
            if (!_feeds.TryAdd(id, new(id, label, url, enabled)) || !urls.Add(url))
                throw new InvalidOperationException(
                    "DependencyControl feed settings contain duplicate entries.");
        }
        Revision = parsedRevision;
    }

    private void Save()
    {
        string directory = Path.GetDirectoryName(_path)!;
        Directory.CreateDirectory(directory);
        string temporary = Path.Combine(directory, $"feeds-{Guid.NewGuid():N}.tmp");
        try
        {
            using MemoryStream buffer = new();
            using (Utf8JsonWriter writer = new(buffer, new JsonWriterOptions
            {
                Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping
            }))
            {
                writer.WriteStartObject();
                writer.WriteNumber("schemaVersion", 1);
                writer.WriteNumber("revision", Revision);
                writer.WritePropertyName("feeds");
                writer.WriteStartArray();
                foreach (DependencyControlConfiguredFeed feed in List())
                {
                    writer.WriteStartObject();
                    writer.WriteString("id", feed.Id);
                    writer.WriteString("label", feed.Label);
                    writer.WriteString("url", feed.Url);
                    writer.WriteBoolean("enabled", feed.Enabled);
                    writer.WriteEndObject();
                }
                writer.WriteEndArray();
                writer.WriteEndObject();
            }
            if (buffer.Length > MaximumFileBytes)
                throw new InvalidOperationException(
                    "DependencyControl feed settings exceed the size limit.");
            using (FileStream stream = new(
                temporary,
                FileMode.CreateNew,
                FileAccess.Write,
                FileShare.None,
                16 * 1024,
                FileOptions.WriteThrough))
            {
                buffer.Position = 0;
                buffer.CopyTo(stream);
                stream.Flush(flushToDisk: true);
            }
            File.Move(temporary, _path, overwrite: true);
        }
        finally
        {
            File.Delete(temporary);
        }
    }

    private void Restore(
        Dictionary<string, DependencyControlConfiguredFeed> previous,
        long revision)
    {
        _feeds.Clear();
        foreach ((string key, DependencyControlConfiguredFeed value) in previous)
            _feeds.Add(key, value);
        Revision = revision;
    }

    private void Quarantine()
    {
        string quarantine = $"{_path}.corrupted-{Guid.NewGuid():N}";
        File.Move(_path, quarantine);
    }

    private static string ReadString(JsonElement root, string property)
    {
        if (!root.TryGetProperty(property, out JsonElement value) ||
            value.ValueKind != JsonValueKind.String)
            throw new InvalidOperationException(
                $"DependencyControl configured feed requires string '{property}'.");
        return value.GetString()!;
    }
}
