using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace Aegisub.DependencyControl;

public static partial class DependencyControlFeedParser
{
    private const int MaximumFeedBytes = 4 * 1024 * 1024;
    private const int MaximumStringLength = 16 * 1024;
    private const int MaximumTemplatePasses = 16;

    public static DependencyControlFeed Parse(string json, string sourceUrl)
    {
        ArgumentNullException.ThrowIfNull(json);
        ArgumentException.ThrowIfNullOrWhiteSpace(sourceUrl);
        byte[] utf8 = Encoding.UTF8.GetBytes(json);
        if (utf8.Length > MaximumFeedBytes)
            throw new DependencyControlFeedException(
                "DependencyControl feed exceeds the maximum supported size.");
        RejectDuplicateProperties(utf8);

        JsonDocument document;
        try
        {
            document = JsonDocument.Parse(utf8, new JsonDocumentOptions
            {
                // Several established 0.2 feeds contain legacy trailing commas.
                AllowTrailingCommas = true,
                CommentHandling = JsonCommentHandling.Disallow,
                MaxDepth = 32
            });
        }
        catch (JsonException error)
        {
            throw new DependencyControlFeedException(
                $"DependencyControl feed is not valid JSON: {error.Message}");
        }
        using (document)
        {
            JsonElement root = document.RootElement;
            RequireObject(root, "DependencyControl feed");

            string formatVersion = RequireString(
                root, "dependencyControlFeedFormatVersion", "DependencyControl feed");
            if (formatVersion is not ("0.2.0" or "0.3.0"))
                throw new DependencyControlFeedException(
                    $"Unsupported DependencyControl feed format '{formatVersion}'.");
            string name = RequireString(root, "name", "DependencyControl feed");
            string description = OptionalString(root, "description", "DependencyControl feed");
            string maintainer = OptionalString(root, "maintainer", "DependencyControl feed");
            string baseUrl = OptionalString(root, "baseUrl", "DependencyControl feed");
            string rawFileBaseUrl = OptionalString(root, "fileBaseUrl", "DependencyControl feed");

            Dictionary<string, string> rootVariables = new(StringComparer.Ordinal)
            {
                ["feedName"] = name,
                ["baseUrl"] = baseUrl
            };
            baseUrl = Expand(baseUrl, rootVariables, EmptyFeeds, false);
            rootVariables["baseUrl"] = baseUrl;

            Dictionary<string, string> knownFeeds = ParseKnownFeeds(
                root, rootVariables);
            string rootFileBaseUrl = Expand(
                rawFileBaseUrl, rootVariables, knownFeeds, false);
            Dictionary<string, DependencyControlPackage> macros = ParsePackages(
                root,
                "macros",
                DependencyControlPackageKind.Macro,
                rootVariables,
                knownFeeds,
                rootFileBaseUrl);
            Dictionary<string, DependencyControlPackage> modules = ParsePackages(
                root,
                "modules",
                DependencyControlPackageKind.Module,
                rootVariables,
                knownFeeds,
                rootFileBaseUrl);

            return new(
                formatVersion,
                name,
                description,
                maintainer,
                ValidateHttpUrl(sourceUrl, "DependencyControl feed source"),
                knownFeeds,
                macros,
                modules);
        }
    }

    public static void ValidateNamespace(string value)
    {
        ArgumentNullException.ThrowIfNull(value);
        if (value.Length is 0 or > 256 ||
            value[0] == '.' ||
            value[^1] == '.' ||
            !value.Contains('.', StringComparison.Ordinal) ||
            value.Contains("..", StringComparison.Ordinal) ||
            !value.All(IsNamespaceCharacter))
            throw new DependencyControlFeedException(
                $"DependencyControl namespace '{value}' is invalid.");
    }

    private static Dictionary<string, string> ParseKnownFeeds(
        JsonElement root,
        IReadOnlyDictionary<string, string> variables)
    {
        Dictionary<string, string> result = new(StringComparer.Ordinal);
        if (!root.TryGetProperty("knownFeeds", out JsonElement knownFeeds))
            return result;
        RequireObject(knownFeeds, "DependencyControl knownFeeds");
        foreach (JsonProperty property in knownFeeds.EnumerateObject())
        {
            ValidateIdentifier(property.Name, "known feed ID");
            string rawUrl = RequireString(
                property.Value, $"DependencyControl known feed '{property.Name}'");
            result[property.Name] = rawUrl;
        }
        foreach ((string key, string rawUrl) in result.ToArray())
        {
            string expanded = Expand(rawUrl, variables, result, true);
            result[key] = ValidateHttpUrl(
                expanded, $"DependencyControl known feed '{key}'");
        }
        return result;
    }

    private static Dictionary<string, DependencyControlPackage> ParsePackages(
        JsonElement root,
        string propertyName,
        DependencyControlPackageKind kind,
        IReadOnlyDictionary<string, string> rootVariables,
        IReadOnlyDictionary<string, string> knownFeeds,
        string inheritedFileBaseUrl)
    {
        Dictionary<string, DependencyControlPackage> result = new(StringComparer.Ordinal);
        if (!root.TryGetProperty(propertyName, out JsonElement packages))
            return result;
        RequireObject(packages, $"DependencyControl {propertyName}");
        foreach (JsonProperty property in packages.EnumerateObject())
        {
            string packageNamespace = property.Name;
            ValidateNamespace(packageNamespace);
            JsonElement package = property.Value;
            RequireObject(package, $"DependencyControl package '{packageNamespace}'");
            Dictionary<string, string> packageVariables = CopyVariables(rootVariables);
            packageVariables["namespace"] = packageNamespace;
            packageVariables["namespacePath"] = packageNamespace.Replace('.', '/');

            string packageName = RequireString(
                package, "name", $"DependencyControl package '{packageNamespace}'");
            packageVariables["scriptName"] = packageName;
            string packageFileBaseUrl = ExpandRollingFileBaseUrl(
                package,
                inheritedFileBaseUrl,
                packageVariables,
                knownFeeds,
                false);
            string description = Expand(
                OptionalString(package, "description", "DependencyControl package"),
                packageVariables,
                knownFeeds,
                false);
            string author = Expand(
                OptionalString(package, "author", "DependencyControl package"),
                packageVariables,
                knownFeeds,
                false);
            string url = Expand(
                OptionalString(package, "url", "DependencyControl package"),
                packageVariables,
                knownFeeds,
                false);
            if (url.Length > 0)
                url = ValidateHttpUrl(url, $"DependencyControl package '{packageNamespace}' URL");

            if (!package.TryGetProperty("channels", out JsonElement channelsElement))
                throw new DependencyControlFeedException(
                    $"DependencyControl package '{packageNamespace}' requires channels.");
            RequireObject(channelsElement, "DependencyControl channels");
            Dictionary<string, DependencyControlChannel> channels = new(StringComparer.Ordinal);
            int defaultChannelCount = 0;
            foreach (JsonProperty channelProperty in channelsElement.EnumerateObject())
            {
                string channelName = channelProperty.Name;
                ValidateIdentifier(channelName, "channel name");
                JsonElement channel = channelProperty.Value;
                RequireObject(channel, $"DependencyControl channel '{channelName}'");
                string version = RequireString(
                    channel, "version", $"DependencyControl channel '{channelName}'");
                ValidateBoundedValue(version, "DependencyControl version", 256);
                Dictionary<string, string> channelVariables = CopyVariables(packageVariables);
                channelVariables["channel"] = channelName;
                channelVariables["version"] = version;
                string channelFileBaseUrl = ExpandRollingFileBaseUrl(
                    channel,
                    packageFileBaseUrl,
                    channelVariables,
                    knownFeeds,
                    false);
                bool isDefault = OptionalBool(
                    channel, "default", $"DependencyControl channel '{channelName}'");
                if (isDefault) ++defaultChannelCount;
                string released = OptionalString(
                    channel, "released", $"DependencyControl channel '{channelName}'");
                IReadOnlyList<string> platforms = ParseStringArray(
                    channel, "platforms", $"DependencyControl channel '{channelName}'");
                IReadOnlyList<DependencyControlFile> files = ParseFiles(
                    channel,
                    kind,
                    packageNamespace,
                    channelVariables,
                    knownFeeds,
                    channelFileBaseUrl);
                IReadOnlyList<DependencyControlRequirement> requirements = ParseRequirements(
                    channel, channelVariables, knownFeeds);
                channels.Add(channelName, new(
                    channelName,
                    version,
                    released,
                    isDefault,
                    platforms,
                    files,
                    requirements));
            }
            if (channels.Count == 0)
                throw new DependencyControlFeedException(
                    $"DependencyControl package '{packageNamespace}' has no channels.");
            if (defaultChannelCount > 1)
                throw new DependencyControlFeedException(
                    $"DependencyControl package '{packageNamespace}' has multiple default channels.");
            result.Add(packageNamespace, new(
                kind,
                packageNamespace,
                packageName,
                description,
                author,
                url,
                channels));
        }
        return result;
    }

    private static IReadOnlyList<DependencyControlFile> ParseFiles(
        JsonElement channel,
        DependencyControlPackageKind kind,
        string packageNamespace,
        IReadOnlyDictionary<string, string> channelVariables,
        IReadOnlyDictionary<string, string> knownFeeds,
        string inheritedFileBaseUrl)
    {
        if (!channel.TryGetProperty("files", out JsonElement files))
            return [];
        RequireArray(files, "DependencyControl files");
        List<DependencyControlFile> result = [];
        foreach (JsonElement file in files.EnumerateArray())
        {
            RequireObject(file, "DependencyControl file");
            string rawName = RequireString(file, "name", "DependencyControl file");
            Dictionary<string, string> fileVariables = CopyVariables(channelVariables);
            string platform = OptionalString(file, "platform", "DependencyControl file");
            fileVariables["platform"] = platform;
            string name = Expand(rawName, fileVariables, knownFeeds, true);
            fileVariables["fileName"] = name;
            string fileBaseUrl = ExpandRollingFileBaseUrl(
                file,
                inheritedFileBaseUrl,
                fileVariables,
                knownFeeds,
                true);
            fileVariables["fileBaseUrl"] = fileBaseUrl;
            bool delete = OptionalBool(file, "delete", "DependencyControl file");
            string type = OptionalString(file, "type", "DependencyControl file");
            if (type.Length == 0) type = "script";
            string sha1 = OptionalString(file, "sha1", "DependencyControl file");
            string url = OptionalString(file, "url", "DependencyControl file");
            if (!delete)
            {
                if (!IsSha1(sha1))
                    throw new DependencyControlFeedException(
                        $"DependencyControl file '{name}' requires a 40-digit SHA-1 value.");
                url = Expand(url, fileVariables, knownFeeds, true);
                url = ValidateHttpUrl(url, $"DependencyControl file '{name}' URL");
            }
            else
            {
                sha1 = string.Empty;
                url = string.Empty;
            }
            string target = type is "script" or "test"
                ? DependencyControlTargetPath.Build(kind, packageNamespace, name, type)
                : string.Empty;
            result.Add(new(name, url, sha1, type, platform, delete, target));
        }
        return result;
    }

    private static IReadOnlyList<DependencyControlRequirement> ParseRequirements(
        JsonElement channel,
        IReadOnlyDictionary<string, string> variables,
        IReadOnlyDictionary<string, string> knownFeeds)
    {
        if (!channel.TryGetProperty("requiredModules", out JsonElement requirements))
            return [];
        RequireArray(requirements, "DependencyControl requiredModules");
        List<DependencyControlRequirement> result = [];
        foreach (JsonElement requirement in requirements.EnumerateArray())
        {
            string moduleName;
            string version = string.Empty;
            string feed = string.Empty;
            string channelName = string.Empty;
            bool optional = false;
            if (requirement.ValueKind == JsonValueKind.String)
            {
                moduleName = requirement.GetString() ?? string.Empty;
            }
            else
            {
                RequireObject(requirement, "DependencyControl module requirement");
                moduleName = RequireString(
                    requirement, "moduleName", "DependencyControl module requirement");
                version = OptionalString(
                    requirement, "version", "DependencyControl module requirement");
                feed = OptionalString(
                    requirement, "feed", "DependencyControl module requirement");
                channelName = OptionalString(
                    requirement, "channel", "DependencyControl module requirement");
                optional = OptionalBool(
                    requirement, "optional", "DependencyControl module requirement");
            }
            ValidateModuleName(moduleName);
            if (feed.Length > 0)
            {
                feed = Expand(feed, variables, knownFeeds, true);
                feed = ValidateHttpUrl(feed, $"DependencyControl requirement '{moduleName}' feed");
            }
            result.Add(new(moduleName, version, feed, channelName, optional));
        }
        return result;
    }

    private static string ExpandRollingFileBaseUrl(
        JsonElement element,
        string inheritedValue,
        IReadOnlyDictionary<string, string> variables,
        IReadOnlyDictionary<string, string> knownFeeds,
        bool requireComplete)
    {
        Dictionary<string, string> scopedVariables = CopyVariables(variables);
        scopedVariables["fileBaseUrl"] = Expand(
            inheritedValue, scopedVariables, knownFeeds, false);
        string raw = OptionalString(element, "fileBaseUrl", "DependencyControl record");
        string value = raw.Length == 0 ? scopedVariables["fileBaseUrl"] : raw;
        return Expand(value, scopedVariables, knownFeeds, requireComplete);
    }

    private static string Expand(
        string value,
        IReadOnlyDictionary<string, string> variables,
        IReadOnlyDictionary<string, string> knownFeeds,
        bool requireComplete)
    {
        if (value.Length == 0) return value;
        string current = value;
        for (int pass = 0; pass < MaximumTemplatePasses; ++pass)
        {
            string expanded = TemplateExpression().Replace(current, match =>
            {
                string name = match.Groups["name"].Value;
                string key = match.Groups["key"].Value;
                if (string.Equals(name, "feed", StringComparison.Ordinal) &&
                    key.Length > 0 && knownFeeds.TryGetValue(key, out string? feed))
                    return feed;
                if (key.Length == 0 && variables.TryGetValue(name, out string? variable))
                    return variable;
                return match.Value;
            });
            if (string.Equals(expanded, current, StringComparison.Ordinal)) break;
            current = expanded;
            if (current.Length > MaximumStringLength)
                throw new DependencyControlFeedException(
                    "Expanded DependencyControl template is too long.");
        }
        if (requireComplete && TemplateExpression().IsMatch(current))
            throw new DependencyControlFeedException(
                $"DependencyControl template '{current}' contains an unresolved variable.");
        return current;
    }

    private static Dictionary<string, string> CopyVariables(
        IReadOnlyDictionary<string, string> source)
    {
        Dictionary<string, string> result = new(StringComparer.Ordinal);
        foreach ((string key, string value) in source) result.Add(key, value);
        return result;
    }

    private static IReadOnlyList<string> ParseStringArray(
        JsonElement element,
        string propertyName,
        string source)
    {
        if (!element.TryGetProperty(propertyName, out JsonElement array)) return [];
        RequireArray(array, $"{source} field '{propertyName}'");
        List<string> result = [];
        foreach (JsonElement item in array.EnumerateArray())
        {
            string value = RequireString(item, $"{source} field '{propertyName}' item");
            ValidateBoundedValue(value, $"{source} field '{propertyName}'", 128);
            result.Add(value);
        }
        return result;
    }

    private static string RequireString(JsonElement element, string propertyName, string source)
    {
        if (!element.TryGetProperty(propertyName, out JsonElement value))
            throw new DependencyControlFeedException(
                $"{source} requires '{propertyName}'.");
        return RequireString(value, $"{source} field '{propertyName}'");
    }

    private static string RequireString(JsonElement value, string source)
    {
        if (value.ValueKind != JsonValueKind.String)
            throw new DependencyControlFeedException($"{source} must be a string.");
        string result = value.GetString() ?? string.Empty;
        if (result.Length == 0)
            throw new DependencyControlFeedException($"{source} cannot be empty.");
        ValidateBoundedValue(result, source, MaximumStringLength);
        return result;
    }

    private static string OptionalString(JsonElement element, string propertyName, string source)
    {
        if (!element.TryGetProperty(propertyName, out JsonElement value)) return string.Empty;
        if (value.ValueKind != JsonValueKind.String)
            throw new DependencyControlFeedException(
                $"{source} field '{propertyName}' must be a string.");
        string result = value.GetString() ?? string.Empty;
        ValidateBoundedValue(result, $"{source} field '{propertyName}'", MaximumStringLength);
        return result;
    }

    private static bool OptionalBool(JsonElement element, string propertyName, string source)
    {
        if (!element.TryGetProperty(propertyName, out JsonElement value)) return false;
        if (value.ValueKind is not (JsonValueKind.True or JsonValueKind.False))
            throw new DependencyControlFeedException(
                $"{source} field '{propertyName}' must be boolean.");
        return value.GetBoolean();
    }

    private static void RequireObject(JsonElement value, string source)
    {
        if (value.ValueKind != JsonValueKind.Object)
            throw new DependencyControlFeedException($"{source} must be an object.");
    }

    private static void RequireArray(JsonElement value, string source)
    {
        if (value.ValueKind != JsonValueKind.Array)
            throw new DependencyControlFeedException($"{source} must be an array.");
    }

    private static void ValidateIdentifier(string value, string source)
    {
        if (value.Length is 0 or > 128 ||
            !value.All(character => IsAsciiAlphaNumeric(character) ||
                character is '.' or '_' or '-'))
            throw new DependencyControlFeedException(
                $"DependencyControl {source} '{value}' is invalid.");
    }

    private static void ValidateModuleName(string value)
    {
        if (value.Length is 0 or > 256 ||
            value.Any(char.IsControl) ||
            value.Contains('/') ||
            value.Contains('\\'))
            throw new DependencyControlFeedException(
                $"DependencyControl module name '{value}' is invalid.");
    }

    private static void ValidateBoundedValue(string value, string source, int maximumLength)
    {
        if (value.Length > maximumLength || value.Any(char.IsControl))
            throw new DependencyControlFeedException($"{source} is invalid.");
    }

    private static string ValidateHttpUrl(string value, string source)
    {
        if (TemplateExpression().IsMatch(value) ||
            !Uri.TryCreate(value, UriKind.Absolute, out Uri? uri) ||
            (uri.Scheme != Uri.UriSchemeHttp && uri.Scheme != Uri.UriSchemeHttps) ||
            string.IsNullOrEmpty(uri.Host))
            throw new DependencyControlFeedException($"{source} is not a valid HTTP URL.");
        return value;
    }

    private static bool IsSha1(string value) =>
        value.Length == 40 && value.All(IsHexDigit);

    private static bool IsNamespaceCharacter(char value) =>
        IsAsciiAlphaNumeric(value) || value is '.' or '_' or '-';

    private static bool IsAsciiAlphaNumeric(char value) =>
        (value >= 'a' && value <= 'z') ||
        (value >= 'A' && value <= 'Z') ||
        (value >= '0' && value <= '9');

    private static bool IsHexDigit(char value) =>
        (value >= '0' && value <= '9') ||
        (value >= 'a' && value <= 'f') ||
        (value >= 'A' && value <= 'F');

    private static void RejectDuplicateProperties(ReadOnlySpan<byte> utf8)
    {
        Utf8JsonReader reader = new(utf8, new JsonReaderOptions
        {
            AllowTrailingCommas = true,
            CommentHandling = JsonCommentHandling.Disallow,
            MaxDepth = 32
        });
        Stack<HashSet<string>> objects = new();
        try
        {
            while (reader.Read())
            {
                if (reader.TokenType == JsonTokenType.StartObject)
                    objects.Push(new(StringComparer.Ordinal));
                else if (reader.TokenType == JsonTokenType.EndObject)
                    objects.Pop();
                else if (reader.TokenType == JsonTokenType.PropertyName)
                {
                    string propertyName = reader.GetString() ?? string.Empty;
                    if (objects.Count == 0 || !objects.Peek().Add(propertyName))
                        throw new DependencyControlFeedException(
                            $"DependencyControl JSON contains duplicate property '{propertyName}'.");
                }
            }
        }
        catch (JsonException error)
        {
            throw new DependencyControlFeedException(
                $"DependencyControl feed is not valid JSON: {error.Message}");
        }
    }

    private static readonly IReadOnlyDictionary<string, string> EmptyFeeds =
        new Dictionary<string, string>();

    [GeneratedRegex("@\\{(?<name>[A-Za-z0-9_.-]+)(?::(?<key>[A-Za-z0-9_.-]+))?\\}",
        RegexOptions.CultureInvariant)]
    private static partial Regex TemplateExpression();
}
