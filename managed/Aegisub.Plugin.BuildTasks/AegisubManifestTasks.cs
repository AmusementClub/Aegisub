using System.Text.Json;
using Microsoft.Build.Framework;
using Microsoft.Build.Utilities;
using BuildTask = Microsoft.Build.Utilities.Task;

namespace Aegisub.Plugin.BuildTasks;

public sealed class GenerateAegisubPluginManifest : BuildTask
{
    [Required]
    public string ManifestPath { get; set; } = "";

    [Required]
    public string PluginId { get; set; } = "";

    [Required]
    public string PluginName { get; set; } = "";

    public string Description { get; set; } = "";
    public string Author { get; set; } = "";

    [Required]
    public string PluginVersion { get; set; } = "";

    [Required]
    public string ContractsVersion { get; set; } = "";

    [Required]
    public string RuntimeKind { get; set; } = "";

    public string EntryAssembly { get; set; } = "";
    public string EntryType { get; set; } = "";
    public string EntryPoint { get; set; } = "aegisub_plugin_init_v1";
    public ITaskItem[] RuntimeLibraries { get; set; } = [];
    public string ContributionsFile { get; set; } = "";

    [Output]
    public string GeneratedManifestPath { get; private set; } = "";

    public override bool Execute()
    {
        try
        {
            ValidateCommonMetadata();
            string fullPath = Path.GetFullPath(ManifestPath);
            Directory.CreateDirectory(Path.GetDirectoryName(fullPath)!);
            using FileStream stream = new(
                fullPath,
                FileMode.Create,
                FileAccess.Write,
                FileShare.None);
            using Utf8JsonWriter writer = new(stream, new JsonWriterOptions
            {
                Indented = true
            });
            writer.WriteStartObject();
            writer.WriteNumber("manifestVersion", 2);
            writer.WriteString("id", PluginId);
            writer.WriteString("name", PluginName);
            writer.WriteString("description", Description);
            writer.WriteString("author", Author);
            writer.WriteString("version", PluginVersion);
            writer.WriteString("contractsVersion", ContractsVersion);
            WriteRuntime(writer);
            writer.WritePropertyName("contributions");
            WriteContributions(writer);
            writer.WriteEndObject();
            writer.Flush();
            GeneratedManifestPath = fullPath;
            Log.LogMessage(
                MessageImportance.Low,
                "Generated Aegisub plugin manifest '{0}'.",
                ManifestPath);
            return true;
        }
        catch (Exception error)
        {
            Log.LogErrorFromException(error, showStackTrace: false);
            return false;
        }
    }

    private void ValidateCommonMetadata()
    {
        RequireId(PluginId, "plugin ID");
        RequireBounded(PluginName, "plugin name", 256, allowEmpty: false);
        RequireBounded(Description, "plugin description", 4096, allowEmpty: true);
        RequireBounded(Author, "plugin author", 256, allowEmpty: true);
        RequireVersion(PluginVersion, "plugin version");
        RequireVersion(ContractsVersion, "Contracts version");
        if (RuntimeKind is not "coreclr" and not "native")
            throw new InvalidOperationException(
                "Aegisub runtime kind must be 'coreclr' or 'native'.");
    }

    private void WriteRuntime(Utf8JsonWriter writer)
    {
        writer.WritePropertyName("runtime");
        writer.WriteStartObject();
        writer.WriteString("kind", RuntimeKind);
        if (RuntimeKind == "coreclr")
        {
            RequireRelativePath(EntryAssembly, "CoreCLR entry assembly");
            RequireBounded(EntryType, "CoreCLR entry type", 1024, allowEmpty: false);
            writer.WriteString("entryAssembly", EntryAssembly);
            writer.WriteString("entryType", EntryType);
        }
        else
        {
            if (EntryPoint != "aegisub_plugin_init_v1")
                throw new InvalidOperationException(
                    "Native Aegisub plugins must use entry point 'aegisub_plugin_init_v1'.");
            if (RuntimeLibraries.Length == 0)
                throw new InvalidOperationException(
                    "Native Aegisub plugins require at least one RID library.");
            writer.WriteString("entryPoint", EntryPoint);
            writer.WritePropertyName("libraries");
            writer.WriteStartObject();
            HashSet<string> rids = new(StringComparer.Ordinal);
            foreach (ITaskItem library in RuntimeLibraries)
            {
                string rid = library.GetMetadata("Rid");
                if (!IsValidRid(rid) || !rids.Add(rid))
                    throw new InvalidOperationException(
                        $"Native Aegisub runtime RID '{rid}' is invalid or duplicated.");
                string path = library.ItemSpec.Replace('\\', '/');
                RequireRelativePath(path, $"native library for RID '{rid}'");
                string expectedPrefix = $"runtimes/{rid}/native/";
                if (!path.StartsWith(expectedPrefix, StringComparison.Ordinal))
                    throw new InvalidOperationException(
                        $"Native library for RID '{rid}' must be under '{expectedPrefix}'.");
                writer.WriteString(rid, path);
            }
            writer.WriteEndObject();
        }
        writer.WriteEndObject();
    }

    private void WriteContributions(Utf8JsonWriter writer)
    {
        if (string.IsNullOrEmpty(ContributionsFile))
        {
            writer.WriteStartArray();
            writer.WriteEndArray();
            return;
        }
        using JsonDocument document = JsonDocument.Parse(
            File.ReadAllBytes(ContributionsFile),
            new JsonDocumentOptions
            {
                AllowTrailingCommas = false,
                CommentHandling = JsonCommentHandling.Disallow,
                MaxDepth = 32
            });
        if (document.RootElement.ValueKind != JsonValueKind.Array)
            throw new InvalidOperationException(
                "Aegisub contributions file must contain one JSON array.");
        ValidateContributionIds(document.RootElement);
        document.RootElement.WriteTo(writer);
    }

    private static void ValidateContributionIds(JsonElement contributions)
    {
        HashSet<string> ids = new(StringComparer.Ordinal);
        foreach (JsonElement contribution in contributions.EnumerateArray())
        {
            if (contribution.ValueKind != JsonValueKind.Object ||
                !contribution.TryGetProperty("id", out JsonElement idElement) ||
                idElement.ValueKind != JsonValueKind.String)
                throw new InvalidOperationException(
                    "Each Aegisub contribution requires a string ID.");
            string id = idElement.GetString()!;
            RequireId(id, "contribution ID");
            if (!ids.Add(id))
                throw new InvalidOperationException(
                    $"Aegisub contribution ID '{id}' is duplicated.");
        }
    }

    internal static void RequireRelativePath(string value, string description)
    {
        if (string.IsNullOrEmpty(value) || Path.IsPathRooted(value) ||
            value.Contains('\\') || value.Split('/').Any(
                static segment => segment is "" or "." or ".."))
            throw new InvalidOperationException(
                $"Aegisub {description} must be a normalized relative path.");
    }

    internal static void RequireId(string value, string description)
    {
        if (value.Length is < 1 or > 128 ||
            value[0] is not (>= 'a' and <= 'z') and not (>= '0' and <= '9') ||
            value[^1] is '.' or '-' ||
            value.Any(static character =>
                character is not (>= 'a' and <= 'z') and
                not (>= '0' and <= '9') and not '.' and not '-') ||
            value.Contains("..", StringComparison.Ordinal))
            throw new InvalidOperationException(
                $"Aegisub {description} '{value}' is not a stable lowercase ID.");
    }

    private static bool IsValidRid(string value) =>
        value.Length is >= 3 and <= 64 &&
        value[0] is >= 'a' and <= 'z' &&
        value[^1] is >= '0' and <= 'z' &&
        value.All(static character =>
            character is >= 'a' and <= 'z' or >= '0' and <= '9' or '-');

    private static void RequireVersion(string value, string description)
    {
        if (!Version.TryParse(value, out _))
            throw new InvalidOperationException(
                $"Aegisub {description} '{value}' is not a numeric version.");
    }

    private static void RequireBounded(
        string value,
        string description,
        int maximumLength,
        bool allowEmpty)
    {
        if ((!allowEmpty && string.IsNullOrWhiteSpace(value)) ||
            value.Length > maximumLength || value.Any(char.IsControl))
            throw new InvalidOperationException(
                $"Aegisub {description} is invalid or exceeds its length limit.");
    }
}

public sealed class ValidateAegisubPluginPackage : BuildTask
{
    [Required]
    public string ManifestPath { get; set; } = "";

    [Required]
    public string PackageRoot { get; set; } = "";

    public override bool Execute()
    {
        try
        {
            string packageRoot = Path.GetFullPath(PackageRoot);
            using JsonDocument manifest = JsonDocument.Parse(
                File.ReadAllBytes(ManifestPath));
            JsonElement runtime = manifest.RootElement.GetProperty("runtime");
            string kind = runtime.GetProperty("kind").GetString() ?? "";
            if (kind == "coreclr")
            {
                RequirePackageFile(
                    packageRoot,
                    runtime.GetProperty("entryAssembly").GetString() ?? "",
                    "CoreCLR entry assembly");
            }
            else if (kind == "native")
            {
                if (runtime.GetProperty("entryPoint").GetString() !=
                    "aegisub_plugin_init_v1")
                    throw new InvalidOperationException(
                        "Native Aegisub manifest has an unexpected entry point.");
                foreach (JsonProperty library in runtime.GetProperty("libraries")
                    .EnumerateObject())
                    RequirePackageFile(
                        packageRoot,
                        library.Value.GetString() ?? "",
                        $"native library for RID '{library.Name}'");
            }
            else
            {
                throw new InvalidOperationException(
                    "Aegisub package manifest has an unsupported runtime kind.");
            }
            Log.LogMessage(
                MessageImportance.Low,
                "Validated Aegisub plugin package root '{0}'.",
                PackageRoot);
            return true;
        }
        catch (Exception error)
        {
            Log.LogErrorFromException(error, showStackTrace: false);
            return false;
        }
    }

    private static void RequirePackageFile(
        string packageRoot,
        string relativePath,
        string description)
    {
        GenerateAegisubPluginManifest.RequireRelativePath(relativePath, description);
        string fullPath = Path.GetFullPath(Path.Combine(
            packageRoot,
            relativePath.Replace('/', Path.DirectorySeparatorChar)));
        string prefix = packageRoot.TrimEnd(
            Path.DirectorySeparatorChar,
            Path.AltDirectorySeparatorChar) + Path.DirectorySeparatorChar;
        if (!fullPath.StartsWith(
                prefix,
                OperatingSystem.IsWindows()
                    ? StringComparison.OrdinalIgnoreCase
                    : StringComparison.Ordinal) ||
            !File.Exists(fullPath))
            throw new InvalidOperationException(
                $"Aegisub {description} is missing from the package root.");
    }
}
