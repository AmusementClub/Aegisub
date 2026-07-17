using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;

var options = Options.Parse(args);
var repoRoot = Tooling.FindRepoRoot(Directory.GetCurrentDirectory());
var headerPath = options.HeaderPath ?? Tooling.FindDefaultHeader(Directory.GetCurrentDirectory(), repoRoot);
if (headerPath is null)
    throw new InvalidOperationException("Could not locate ass/ass.h. Pass --header explicitly.");

var outputPath = options.OutputPath ?? Path.Combine(repoRoot, "vendor", "libass", "libass_functions.inc");
var headerVersion = Generator.GetHeaderVersion(headerPath);
var headerSha256 = Generator.GetHeaderSha256(headerPath);
var exportedFunctions = Generator.GetExportedFunctions(headerPath);
var generatedFunctions = options.Mode == GenerationMode.All
    ? exportedFunctions
    : Generator.GetUsedFunctions(exportedFunctions, Tooling.GetDefaultSources(repoRoot));

Directory.CreateDirectory(Path.GetDirectoryName(outputPath)!);
File.WriteAllText(
    outputPath,
    Generator.Render(options.Mode, headerVersion, headerSha256, generatedFunctions),
    new UTF8Encoding(false));
Console.WriteLine($"Wrote {generatedFunctions.Count} libass functions to {outputPath}");

enum GenerationMode {
    All,
    Used
}

sealed class Options {
    public GenerationMode Mode { get; private set; } = GenerationMode.Used;
    public string? HeaderPath { get; private set; }
    public string? OutputPath { get; private set; }

    public static Options Parse(string[] args) {
        var options = new Options();
        for (var index = 0; index < args.Length; index++) {
            var arg = args[index];
            switch (arg) {
                case "--mode":
                    options.Mode = ParseMode(RequireValue(args, ref index, arg));
                    break;
                case "--header":
                    options.HeaderPath = RequireValue(args, ref index, arg);
                    break;
                case "--output":
                    options.OutputPath = RequireValue(args, ref index, arg);
                    break;
                case "--help":
                case "-h":
                    PrintUsage();
                    Environment.Exit(0);
                    break;
                default:
                    throw new ArgumentException($"Unknown argument: {arg}");
            }
        }

        return options;
    }

    private static string RequireValue(string[] args, ref int index, string option) {
        if (index + 1 >= args.Length)
            throw new ArgumentException($"Missing value for {option}");
        return args[++index];
    }

    private static GenerationMode ParseMode(string value) => value.ToLowerInvariant() switch {
        "all" => GenerationMode.All,
        "used" => GenerationMode.Used,
        _ => throw new ArgumentException($"Unsupported mode: {value}")
    };

    private static void PrintUsage() {
        Console.WriteLine("Usage:");
        Console.WriteLine("  dotnet vendor/libass/gen_libass_functions.cs -- [--mode all|used] [--header path] [--output path]");
        Console.WriteLine();
        Console.WriteLine("Examples:");
        Console.WriteLine("  dotnet vendor/libass/gen_libass_functions.cs -- --mode used");
        Console.WriteLine("  dotnet vendor/libass/gen_libass_functions.cs -- --mode all --header include/ass/ass.h");
    }
}

static class Tooling {
    private static readonly Regex PresetIncludePattern = new(
        "\\\"(?:ass_INCLUDE_DIR|ass_INCLUDE_DIRS|LIBASS_INCLUDE_DIR)\\\"\\s*:\\s*\\\"([^\\\"]+)\\\"",
        RegexOptions.Compiled);

    public static string FindRepoRoot(string startDirectory) {
        var current = new DirectoryInfo(startDirectory);
        while (current is not null) {
            if (File.Exists(Path.Combine(current.FullName, "CMakeLists.txt")) && Directory.Exists(Path.Combine(current.FullName, "src")))
                return current.FullName;
            current = current.Parent;
        }

        throw new InvalidOperationException("Could not locate repository root. Run the tool from inside the repository.");
    }

    public static string? FindDefaultHeader(string currentDirectory, string repoRoot) {
        foreach (var root in new[] { currentDirectory, repoRoot }.Distinct(StringComparer.OrdinalIgnoreCase)) {
            var header = Path.Combine(root, "include", "ass", "ass.h");
            if (File.Exists(header))
                return header;
        }

        var presetHeader = TryReadHeaderFromCMakePresets(repoRoot);
        if (presetHeader is not null)
            return presetHeader;

        return TryReadHeaderFromCaches(repoRoot);
    }

    private static string? TryReadHeaderFromCMakePresets(string repoRoot) {
        var presetsPath = Path.Combine(repoRoot, "CMakePresets.json");
        if (!File.Exists(presetsPath))
            return null;

        var content = File.ReadAllText(presetsPath, Encoding.UTF8);
        var match = PresetIncludePattern.Match(content);
        if (!match.Success)
            return null;

        var includeDir = match.Groups[1].Value.Replace("${sourceDir}", repoRoot.Replace('\\', '/'));
        return FindHeaderUnderIncludeRoot(includeDir);
    }

    private static string? TryReadHeaderFromCaches(string repoRoot) {
        var cachePaths = Directory
            .EnumerateDirectories(repoRoot, "build*", SearchOption.TopDirectoryOnly)
            .Select(directory => Path.Combine(directory, "CMakeCache.txt"))
            .Where(File.Exists);

        foreach (var cachePath in cachePaths) {
            foreach (var line in File.ReadLines(cachePath, Encoding.UTF8)) {
                if (!line.StartsWith("ass_INCLUDE_DIR:", StringComparison.Ordinal)
                    && !line.StartsWith("ass_INCLUDE_DIRS:", StringComparison.Ordinal))
                    continue;

                var parts = line.Split('=', 2);
                if (parts.Length != 2)
                    continue;

                var header = FindHeaderUnderIncludeRoot(parts[1].Trim());
                if (header is not null)
                    return header;
            }
        }

        return null;
    }

    private static string? FindHeaderUnderIncludeRoot(string includeDir) {
        includeDir = includeDir.Replace('/', Path.DirectorySeparatorChar);
        var nested = Path.Combine(includeDir, "ass", "ass.h");
        if (File.Exists(nested))
            return nested;

        var direct = Path.Combine(includeDir, "ass.h");
        return File.Exists(direct) ? direct : null;
    }

    public static IReadOnlyList<string> GetDefaultSources(string repoRoot) {
        return [
            Path.Combine(repoRoot, "src", "libass_runtime.cpp"),
            Path.Combine(repoRoot, "src", "subtitles_provider_libass.cpp")
        ];
    }
}

static class Generator {
    private static readonly Regex HeaderApiPattern = new(
        "\\b(ass_[A-Za-z0-9_]+)\\s*\\(",
        RegexOptions.Compiled);
    private static readonly Regex LibassVersionPattern = new(
        "^\\s*#\\s*define\\s+LIBASS_VERSION\\s+(0[xX][0-9A-Fa-f]+|[0-9]+)\\s*$",
        RegexOptions.Compiled | RegexOptions.Multiline);
    private static readonly Regex UsedApiPattern = new(
        "\\b(?:api|loaded)\\s*\\.\\s*(ass_[A-Za-z0-9_]+)\\b",
        RegexOptions.Compiled);

    public static List<string> GetExportedFunctions(string headerPath) {
        var content = StripComments(File.ReadAllText(headerPath, Encoding.UTF8));
        var ordered = new List<string>();
        var seen = new HashSet<string>(StringComparer.Ordinal);

        foreach (Match match in HeaderApiPattern.Matches(content)) {
            var name = match.Groups[1].Value;
            if (seen.Add(name))
                ordered.Add(name);
        }

        return ordered;
    }

    public static string GetHeaderVersion(string headerPath) {
        var content = File.ReadAllText(headerPath, Encoding.UTF8);
        var match = LibassVersionPattern.Match(content);
        if (!match.Success)
            throw new InvalidOperationException("Could not find LIBASS_VERSION in ass.h");

        var version = match.Groups[1].Value.StartsWith("0x", StringComparison.OrdinalIgnoreCase)
            ? Convert.ToInt64(match.Groups[1].Value[2..], 16)
            : long.Parse(match.Groups[1].Value, CultureInfo.InvariantCulture);
        return FormattableString.Invariant(
            $"{(version >> 28) & 0xF}.{((version >> 24) & 0xF) * 10 + ((version >> 20) & 0xF)}.{((version >> 16) & 0xF) * 10 + ((version >> 12) & 0xF)}");
    }

    public static string GetHeaderSha256(string headerPath) {
        var hash = SHA256.HashData(File.ReadAllBytes(headerPath));
        return Convert.ToHexString(hash);
    }

    public static List<string> GetUsedFunctions(IReadOnlyCollection<string> exportedFunctions, IReadOnlyList<string> sourcePaths) {
        var exported = new HashSet<string>(exportedFunctions, StringComparer.Ordinal);
        var ordered = new List<string>();
        var seen = new HashSet<string>(StringComparer.Ordinal);

        foreach (var sourcePath in sourcePaths) {
            var content = StripComments(File.ReadAllText(sourcePath, Encoding.UTF8));
            foreach (Match match in UsedApiPattern.Matches(content)) {
                var function = match.Groups[1].Value;
                if (exported.Contains(function) && seen.Add(function))
                    ordered.Add(function);
            }
        }

        return ordered;
    }

    public static string Render(GenerationMode mode, string headerVersion, string headerSha256, IReadOnlyList<string> functions) {
        var builder = new StringBuilder();
        builder.AppendLine($"/* Auto-generated by vendor/libass/gen_libass_functions.cs. Mode: {mode.ToString().ToLowerInvariant()}. LIBASS_VERSION: {headerVersion}. ass.h SHA256: {headerSha256}. */");
        foreach (var function in functions)
            builder.AppendLine($"AGI_LIBASS_FN({function})");
        return builder.ToString();
    }

    private static string StripComments(string content) {
        var builder = new StringBuilder(content.Length);
        var inBlockComment = false;
        var inLineComment = false;
        var inString = false;
        var quote = '\0';

        for (var index = 0; index < content.Length; index++) {
            var current = content[index];
            var next = index + 1 < content.Length ? content[index + 1] : '\0';

            if (inLineComment) {
                if (current == '\n') {
                    inLineComment = false;
                    builder.Append(current);
                }
                continue;
            }

            if (inBlockComment) {
                if (current == '*' && next == '/') {
                    inBlockComment = false;
                    index++;
                }
                else if (current == '\n') {
                    builder.Append(current);
                }
                continue;
            }

            if (inString) {
                builder.Append(current);
                if (current == '\\' && next != '\0') {
                    builder.Append(next);
                    index++;
                }
                else if (current == quote) {
                    inString = false;
                }
                continue;
            }

            if (current == '/' && next == '/') {
                inLineComment = true;
                index++;
                continue;
            }

            if (current == '/' && next == '*') {
                inBlockComment = true;
                index++;
                continue;
            }

            if (current is '\'' or '"') {
                inString = true;
                quote = current;
            }
            builder.Append(current);
        }

        return builder.ToString();
    }
}
