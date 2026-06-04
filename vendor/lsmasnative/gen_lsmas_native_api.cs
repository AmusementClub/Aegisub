using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;

var options = Options.Parse(args);
var repoRoot = Tooling.FindRepoRoot(Directory.GetCurrentDirectory());
var headerPath = options.HeaderPath ?? Tooling.FindDefaultHeader(Directory.GetCurrentDirectory(), repoRoot);
if (headerPath is null)
    throw new InvalidOperationException("Could not locate lsmas_native.h. Pass --header explicitly.");

var abiOutputPath = options.AbiOutputPath ?? Path.Combine(repoRoot, "src", "lsmas_native_api.generated.h");
var functionsOutputPath = options.FunctionsOutputPath ?? Path.Combine(repoRoot, "vendor", "lsmasnative", "lsmas_native_api.functions.inc");
var sourcePaths = Tooling.GetDefaultSources(repoRoot);

var header = HeaderModel.Load(headerPath);
var requiredMembers = options.Mode == GenerationMode.AllRequired
    ? header.Functions.Select(function => function.MemberName).ToHashSet(StringComparer.Ordinal)
    : Generator.GetUsedApiMembers(sourcePaths, header.Functions);

Directory.CreateDirectory(Path.GetDirectoryName(abiOutputPath)!);
Directory.CreateDirectory(Path.GetDirectoryName(functionsOutputPath)!);

File.WriteAllText(
    abiOutputPath,
    Generator.RenderAbiHeader(repoRoot, headerPath, header),
    new UTF8Encoding(false));
var functionList = Generator.RenderFunctionList(repoRoot, headerPath, header, requiredMembers, options.Mode);
File.WriteAllText(
    functionsOutputPath,
    functionList,
    new UTF8Encoding(false));

Console.WriteLine($"Wrote LsmasNative ABI declarations to {abiOutputPath}");
Console.WriteLine($"Wrote {header.Functions.Count} LsmasNative functions to {functionsOutputPath}");
Console.WriteLine($"Required symbols: {Generator.CountRequiredFunctionListEntries(functionList)}; optional symbols: {Generator.CountOptionalFunctionListEntries(functionList)}");

enum GenerationMode {
    UsedRequired,
    AllRequired
}

sealed class Options {
    public GenerationMode Mode { get; private set; } = GenerationMode.UsedRequired;
    public string? HeaderPath { get; private set; }
    public string? AbiOutputPath { get; private set; }
    public string? FunctionsOutputPath { get; private set; }

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
                case "--abi-output":
                    options.AbiOutputPath = RequireValue(args, ref index, arg);
                    break;
                case "--functions-output":
                    options.FunctionsOutputPath = RequireValue(args, ref index, arg);
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
        "used" => GenerationMode.UsedRequired,
        "all" => GenerationMode.AllRequired,
        _ => throw new ArgumentException($"Unsupported mode: {value}")
    };

    private static void PrintUsage() {
        Console.WriteLine("Usage:");
        Console.WriteLine("  dotnet vendor/lsmasnative/gen_lsmas_native_api.cs -- [--mode used|all] [--header path] [--abi-output path] [--functions-output path]");
        Console.WriteLine();
        Console.WriteLine("Examples:");
        Console.WriteLine("  dotnet vendor/lsmasnative/gen_lsmas_native_api.cs -- --mode used");
        Console.WriteLine("  dotnet vendor/lsmasnative/gen_lsmas_native_api.cs -- --mode used --header path\\to\\lsmas_native.h");
    }
}

static class Tooling {
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
        var candidates = new List<string>();

        var environmentHeader = Environment.GetEnvironmentVariable("LSMASNATIVE_HEADER");
        if (!string.IsNullOrWhiteSpace(environmentHeader))
            candidates.Add(environmentHeader);

        foreach (var root in new[] { currentDirectory, repoRoot }.Distinct(StringComparer.OrdinalIgnoreCase)) {
            candidates.Add(Path.Combine(root, "vendor", "lsmasnative", "lsmas_native.h"));
        }

        return candidates
            .Select(Path.GetFullPath)
            .FirstOrDefault(File.Exists);
    }

    public static IReadOnlyList<string> GetDefaultSources(string repoRoot) {
        return [
            Path.Combine(repoRoot, "src", "lsmas_native_runtime.cpp"),
            Path.Combine(repoRoot, "src", "lsmas_provider_common.cpp"),
            Path.Combine(repoRoot, "src", "audio_provider_lsmasnative.cpp"),
            Path.Combine(repoRoot, "src", "video_provider_lsmasnative.cpp")
        ];
    }
}

sealed record HeaderFunction(string SymbolName) {
    public string MemberName { get; } = SymbolName.StartsWith("lsmas_", StringComparison.Ordinal)
        ? SymbolName["lsmas_".Length..]
        : SymbolName;
}

sealed record HeaderModel(string Body, string VersionDefines, string Sha256, IReadOnlyList<HeaderFunction> Functions) {
    private static readonly Regex ExportPattern = new(
        @"LSMAS_NATIVE_API\s+[^;]*?\b(lsmas_[A-Za-z0-9_]+)\s*\(",
        RegexOptions.Compiled | RegexOptions.Singleline);

    public static HeaderModel Load(string headerPath) {
        var content = File.ReadAllText(headerPath, Encoding.UTF8).Replace("\r\n", "\n");
        var body = ExtractExternCBody(content);
        var functions = ExtractFunctions(body);
        var sanitizedBody = SanitizeBody(body);
        var versionDefines = ExtractVersionDefines(content);
        return new HeaderModel(sanitizedBody, versionDefines, ComputeSha256(content), functions);
    }

    private static string ExtractExternCBody(string content) {
        var externMatch = Regex.Match(content, "extern\\s+\"C\"\\s*\\{");
        if (!externMatch.Success)
            throw new InvalidOperationException("Could not find extern \"C\" block in lsmas_native.h");

        var start = externMatch.Index + externMatch.Length;
        var end = content.LastIndexOf("#ifdef __cplusplus", StringComparison.Ordinal);
        if (end <= start)
            throw new InvalidOperationException("Could not find end of extern \"C\" block in lsmas_native.h");

        return content[start..end].Trim();
    }

    private static IReadOnlyList<HeaderFunction> ExtractFunctions(string body) {
        var ordered = new List<HeaderFunction>();
        var seen = new HashSet<string>(StringComparer.Ordinal);

        foreach (Match match in ExportPattern.Matches(body)) {
            var symbol = match.Groups[1].Value;
            if (seen.Add(symbol))
                ordered.Add(new HeaderFunction(symbol));
        }

        return ordered;
    }

    private static string SanitizeBody(string body) {
        var sanitized = body.Replace("LSMAS_NATIVE_API ", string.Empty);
        sanitized = Regex.Replace(sanitized, @"^\s*#endif\s*", string.Empty);
        return Regex.Replace(sanitized, @"[ \t]+$", string.Empty, RegexOptions.Multiline).Trim();
    }

    private static string ExtractVersionDefines(string content) {
        var externMatch = Regex.Match(content, "extern\\s+\"C\"\\s*\\{");
        var prelude = externMatch.Success ? content[..externMatch.Index] : content;
        var builder = new StringBuilder();
        var keepContinuation = false;

        foreach (var line in prelude.Split('\n')) {
            var includeLine = keepContinuation
                || Regex.IsMatch(line, @"^\s*#define\s+LSMAS_NATIVE_(?:API_VERSION|MAKE_API_VERSION|STRINGIFY)");
            if (!includeLine)
                continue;

            builder.AppendLine(Regex.Replace(line, @"[ \t]+$", string.Empty));
            keepContinuation = line.TrimEnd().EndsWith("\\", StringComparison.Ordinal);
        }

        return builder.ToString().Trim();
    }

    private static string ComputeSha256(string value) {
        var bytes = SHA256.HashData(Encoding.UTF8.GetBytes(value));
        return Convert.ToHexString(bytes).ToLowerInvariant();
    }
}

static class Generator {
    private static readonly Regex ApiMemberPattern = new(@"\b(?:api|loaded)\.([A-Za-z_][A-Za-z0-9_]*)\b", RegexOptions.Compiled);
    private static readonly Regex GetApiMemberPattern = new(@"\bGetApi\(\)\.([A-Za-z_][A-Za-z0-9_]*)\b", RegexOptions.Compiled);
    private static readonly HashSet<string> AlwaysOptionalMembers = new(StringComparer.Ordinal) {
        "video_frame_get_side_data",
        "video_frame_get_dovi_metadata"
    };

    public static HashSet<string> GetUsedApiMembers(IReadOnlyList<string> sourcePaths, IReadOnlyList<HeaderFunction> functions) {
        var exportedMembers = functions.Select(function => function.MemberName).ToHashSet(StringComparer.Ordinal);
        var used = new HashSet<string>(StringComparer.Ordinal);

        foreach (var sourcePath in sourcePaths) {
            if (!File.Exists(sourcePath))
                continue;

            var inBlockComment = false;
            foreach (var rawLine in File.ReadLines(sourcePath, Encoding.UTF8)) {
                var line = StripComments(rawLine, ref inBlockComment);
                AddMatches(line, ApiMemberPattern, exportedMembers, used);
                AddMatches(line, GetApiMemberPattern, exportedMembers, used);
            }
        }

        return used;
    }

    public static string RenderAbiHeader(string repoRoot, string headerPath, HeaderModel header) {
        var builder = new StringBuilder();
        builder.AppendLine("/* Auto-generated by vendor/lsmasnative/gen_lsmas_native_api.cs.");
        builder.AppendLine($" * Source: {FormatSourcePath(repoRoot, headerPath)}");
        builder.AppendLine($" * SHA256: {header.Sha256}");
        builder.AppendLine(" */");
        builder.AppendLine("#pragma once");
        builder.AppendLine();
        builder.AppendLine("#include <stddef.h>");
        builder.AppendLine("#include <stdint.h>");
        builder.AppendLine();
        if (!string.IsNullOrWhiteSpace(header.VersionDefines)) {
            builder.AppendLine(header.VersionDefines);
            builder.AppendLine();
        }
        builder.AppendLine("#ifdef __cplusplus");
        builder.AppendLine("extern \"C\" {");
        builder.AppendLine("#endif");
        builder.AppendLine();
        builder.AppendLine(header.Body);
        builder.AppendLine();
        builder.AppendLine("#ifdef __cplusplus");
        builder.AppendLine("}");
        builder.AppendLine("#endif");
        return builder.ToString();
    }

    public static string RenderFunctionList(string repoRoot, string headerPath, HeaderModel header, HashSet<string> requiredMembers, GenerationMode mode) {
        var builder = new StringBuilder();
        builder.AppendLine("/* Auto-generated by vendor/lsmasnative/gen_lsmas_native_api.cs.");
        builder.AppendLine($" * Source: {FormatSourcePath(repoRoot, headerPath)}");
        builder.AppendLine($" * SHA256: {header.Sha256}");
        builder.AppendLine($" * Mode: {(mode == GenerationMode.AllRequired ? "all" : "used")}");
        builder.AppendLine(" */");
        foreach (var function in header.Functions) {
            var forceOptional = mode == GenerationMode.UsedRequired && AlwaysOptionalMembers.Contains(function.MemberName);
            var macro = requiredMembers.Contains(function.MemberName) && !forceOptional ? "AGI_LSMAS_REQUIRED" : "AGI_LSMAS_OPTIONAL";
            builder.AppendLine($"{macro}({function.SymbolName}, {function.MemberName})");
        }

        return builder.ToString();
    }

    public static int CountRequiredFunctionListEntries(string functionList) =>
        Regex.Matches(functionList, @"^AGI_LSMAS_REQUIRED\(", RegexOptions.Multiline).Count;

    public static int CountOptionalFunctionListEntries(string functionList) =>
        Regex.Matches(functionList, @"^AGI_LSMAS_OPTIONAL\(", RegexOptions.Multiline).Count;

    private static string FormatSourcePath(string repoRoot, string headerPath) {
        var fullRepoRoot = Path.GetFullPath(repoRoot);
        var fullHeaderPath = Path.GetFullPath(headerPath);
        if (fullHeaderPath.StartsWith(fullRepoRoot + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            return Path.GetRelativePath(fullRepoRoot, fullHeaderPath).Replace('\\', '/');
        return Path.GetFileName(fullHeaderPath);
    }

    private static void AddMatches(string line, Regex pattern, HashSet<string> exportedMembers, HashSet<string> used) {
        foreach (Match match in pattern.Matches(line)) {
            var member = match.Groups[1].Value;
            if (exportedMembers.Contains(member))
                used.Add(member);
        }
    }

    private static string StripComments(string line, ref bool inBlockComment) {
        var builder = new StringBuilder();
        for (var index = 0; index < line.Length; index++) {
            if (inBlockComment) {
                var end = line.IndexOf("*/", index, StringComparison.Ordinal);
                if (end < 0)
                    return builder.ToString();
                index = end + 1;
                inBlockComment = false;
                continue;
            }

            if (index + 1 < line.Length && line[index] == '/' && line[index + 1] == '*') {
                inBlockComment = true;
                index++;
                continue;
            }

            if (index + 1 < line.Length && line[index] == '/' && line[index + 1] == '/')
                break;

            builder.Append(line[index]);
        }

        return builder.ToString();
    }
}
