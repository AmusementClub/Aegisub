using System.Text;
using System.Text.Json;

namespace Aegisub.DependencyControl.Plugin;

internal sealed record DependencyControlLogEntry(
    DateTimeOffset Timestamp,
    int Level,
    string Source,
    string Message);

internal sealed record DependencyControlLogTrimResult(
    int DeletedFiles,
    long DeletedBytes,
    int TotalFiles,
    long TotalBytes);

internal sealed class DependencyControlLogStore
{
    private const long RotationBytes = 10 * 1024 * 1024;
    private const int MaximumSourceLength = 256;
    private const int MaximumMessageLength = 16 * 1024;
    private const int MaximumRetainedEntries = 2048;
    private readonly object _gate = new();
    private readonly string _root;
    private readonly string _currentPath;

    public DependencyControlLogStore(string stateRoot)
    {
        _root = Path.Combine(stateRoot, "logs");
        _currentPath = Path.Combine(_root, "dependency-control.log");
    }

    public void Append(int level, string source, string message)
    {
        if (level is < 0 or > 5)
            throw new InvalidOperationException(
                "DependencyControl log level must be between 0 and 5.");
        string normalizedSource = ValidateText(
            source, MaximumSourceLength, "source", allowNewlines: false);
        string normalizedMessage = ValidateText(
            message, MaximumMessageLength, "message", allowNewlines: true);
        lock (_gate)
        {
            Directory.CreateDirectory(_root);
            if (File.Exists(_currentPath) && new FileInfo(_currentPath).Length >= RotationBytes)
                RotateCurrent();
            string line = DependencyControlServiceContribution.BuildJson(writer =>
            {
                writer.WriteStartObject();
                writer.WriteString("timestamp", DateTimeOffset.UtcNow);
                writer.WriteNumber("level", level);
                writer.WriteString("source", normalizedSource);
                writer.WriteString("message", normalizedMessage);
                writer.WriteEndObject();
            });
            using FileStream stream = new(
                _currentPath,
                FileMode.Append,
                FileAccess.Write,
                FileShare.Read,
                16 * 1024,
                FileOptions.WriteThrough);
            byte[] payload = Encoding.UTF8.GetBytes(line + "\n");
            stream.Write(payload);
            stream.Flush(flushToDisk: true);
        }
    }

    public IReadOnlyList<DependencyControlLogEntry> List()
    {
        lock (_gate)
        {
            if (!Directory.Exists(_root))
                return [];
            Queue<DependencyControlLogEntry> retained = new(MaximumRetainedEntries);
            foreach (string path in LogFiles().Order(StringComparer.Ordinal))
            {
                foreach (string line in File.ReadLines(path, Encoding.UTF8))
                {
                    if (!TryParse(line, out DependencyControlLogEntry? entry) || entry is null)
                        continue;
                    if (retained.Count == MaximumRetainedEntries)
                        retained.Dequeue();
                    retained.Enqueue(entry);
                }
            }
            return retained.Reverse().ToArray();
        }
    }

    public DependencyControlLogTrimResult Trim(
        bool wipe,
        TimeSpan maximumAge,
        long maximumBytes,
        int maximumFiles)
    {
        if (maximumAge < TimeSpan.Zero || maximumBytes < 0 || maximumFiles < 0)
            throw new InvalidOperationException(
                "DependencyControl log retention values must be non-negative.");
        lock (_gate)
        {
            if (!Directory.Exists(_root))
                return new(0, 0, 0, 0);
            FileInfo[] files = LogFiles()
                .Select(path => new FileInfo(path))
                .OrderByDescending(file => file.LastWriteTimeUtc)
                .ThenBy(file => file.Name, StringComparer.Ordinal)
                .ToArray();
            long totalBytes = files.Sum(file => file.Length);
            long retainedBytes = 0;
            int retainedFiles = 0;
            int deletedFiles = 0;
            long deletedBytes = 0;
            DateTime cutoff = DateTime.UtcNow - maximumAge;
            foreach (FileInfo file in files)
            {
                bool delete = wipe || retainedFiles >= maximumFiles ||
                    retainedBytes + file.Length > maximumBytes ||
                    file.LastWriteTimeUtc < cutoff;
                if (delete)
                {
                    long length = file.Length;
                    file.Delete();
                    ++deletedFiles;
                    deletedBytes += length;
                }
                else
                {
                    ++retainedFiles;
                    retainedBytes += file.Length;
                }
            }
            return new(deletedFiles, deletedBytes, files.Length, totalBytes);
        }
    }

    private IEnumerable<string> LogFiles() => Directory.EnumerateFiles(
        _root, "dependency-control*.log", SearchOption.TopDirectoryOnly);

    private void RotateCurrent()
    {
        string destination = Path.Combine(
            _root,
            $"dependency-control-{DateTime.UtcNow:yyyyMMdd-HHmmss}-{Guid.NewGuid():N}.log");
        File.Move(_currentPath, destination);
    }

    private static bool TryParse(
        string line,
        out DependencyControlLogEntry? entry)
    {
        entry = null;
        try
        {
            using JsonDocument document = JsonDocument.Parse(line);
            JsonElement root = document.RootElement;
            if (!root.TryGetProperty("timestamp", out JsonElement timestamp) ||
                !timestamp.TryGetDateTimeOffset(out DateTimeOffset parsedTimestamp) ||
                !root.TryGetProperty("level", out JsonElement level) ||
                !level.TryGetInt32(out int parsedLevel) || parsedLevel is < 0 or > 5 ||
                !root.TryGetProperty("source", out JsonElement source) ||
                source.ValueKind != JsonValueKind.String ||
                !root.TryGetProperty("message", out JsonElement message) ||
                message.ValueKind != JsonValueKind.String)
                return false;
            entry = new(
                parsedTimestamp,
                parsedLevel,
                source.GetString()!,
                message.GetString()!);
            return true;
        }
        catch (JsonException)
        {
            return false;
        }
    }

    private static string ValidateText(
        string value,
        int maximumLength,
        string description,
        bool allowNewlines)
    {
        if (string.IsNullOrWhiteSpace(value) || value.Length > maximumLength ||
            value.Any(character => char.IsControl(character) &&
                !(allowNewlines && character is '\r' or '\n' or '\t')))
            throw new InvalidOperationException(
                $"DependencyControl log {description} is invalid.");
        return value;
    }
}
