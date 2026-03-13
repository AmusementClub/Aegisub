using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;

var options = Options.Parse(args);
var repoRoot = Tooling.FindRepoRoot(Directory.GetCurrentDirectory());
var headerPath = options.HeaderPath ?? Tooling.FindDefaultHeader(Directory.GetCurrentDirectory(), repoRoot);
if (headerPath is null)
    throw new InvalidOperationException("Could not locate ffms.h. Pass --header explicitly.");

var outputPath = options.OutputPath ?? Path.Combine(repoRoot, "vendor", "ffms2", "ffms2_functions.inc");
var headerVersion = Generator.GetHeaderVersion(headerPath);
var exportedFunctions = Generator.GetExportedFunctions(headerPath);
var generatedFunctions = options.Mode == GenerationMode.All
    ? exportedFunctions
    : Generator.GetUsedFunctions(headerPath, exportedFunctions, Tooling.GetDefaultSources(repoRoot), Tooling.GetDefaultDefines());

Directory.CreateDirectory(Path.GetDirectoryName(outputPath)!);
File.WriteAllText(outputPath, Generator.Render(options.Mode, headerVersion, generatedFunctions), new UTF8Encoding(false));
Console.WriteLine($"Wrote {generatedFunctions.Count} FFMS2 functions to {outputPath}");

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
        Console.WriteLine("  dotnet vendor/ffms2/gen_ffms2_functions.cs -- [--mode all|used] [--header path] [--output path]");
        Console.WriteLine();
        Console.WriteLine("Examples:");
        Console.WriteLine("  dotnet vendor/ffms2/gen_ffms2_functions.cs -- --mode used");
        Console.WriteLine("  dotnet vendor/ffms2/gen_ffms2_functions.cs -- --mode all");
        Console.WriteLine("  dotnet vendor/ffms2/gen_ffms2_functions.cs -- --mode used --header vendor/ffms2/ffms2-2.23.1-msvc/include/ffms.h --output vendor/ffms2/ffms2_functions.inc");
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
        foreach (var root in new[] { currentDirectory, repoRoot }.Distinct(StringComparer.OrdinalIgnoreCase)) {
            var explicitHeader = Path.Combine(root, "vendor", "ffms2", "ffms2-2.23.1-msvc", "include", "ffms.h");
            if (File.Exists(explicitHeader))
                return explicitHeader;

            var vendorRoot = Path.Combine(root, "vendor", "ffms2");
            if (!Directory.Exists(vendorRoot))
                continue;

            var path = Directory
                .EnumerateFiles(vendorRoot, "ffms.h", SearchOption.AllDirectories)
                .OrderBy(candidate => candidate, StringComparer.OrdinalIgnoreCase)
                .FirstOrDefault();
            if (path is not null)
                return path;
        }

        var presetHeader = TryReadHeaderFromCMakePresets(repoRoot);
        if (presetHeader is not null)
            return presetHeader;

        var cachedHeader = TryReadHeaderFromCaches(repoRoot);
        if (cachedHeader is not null)
            return cachedHeader;

        return null;
    }

    private static string? TryReadHeaderFromCMakePresets(string repoRoot) {
        var presetsPath = Path.Combine(repoRoot, "CMakePresets.json");
        if (!File.Exists(presetsPath))
            return null;

        var content = File.ReadAllText(presetsPath, Encoding.UTF8);
        var match = Regex.Match(content, @"""FFMS2_INCLUDE_DIRS""\s*:\s*""([^""]+)""");
        if (!match.Success)
            return null;

        var includeDir = match.Groups[1].Value.Replace("${sourceDir}", repoRoot.Replace('\\', '/'));
        var header = Path.Combine(includeDir.Replace('/', Path.DirectorySeparatorChar), "ffms.h");
        return File.Exists(header) ? header : null;
    }

    private static string? TryReadHeaderFromCaches(string repoRoot) {
        IEnumerable<string> cachePaths = Directory
            .EnumerateDirectories(repoRoot, "build*", SearchOption.TopDirectoryOnly)
            .Select(directory => Path.Combine(directory, "CMakeCache.txt"))
            .Where(File.Exists);

        foreach (var cachePath in cachePaths) {

            foreach (var line in File.ReadLines(cachePath, Encoding.UTF8)) {
                if (!line.StartsWith("FFMS2_INCLUDE_DIRS:", StringComparison.Ordinal))
                    continue;

                var parts = line.Split('=', 2);
                if (parts.Length != 2)
                    continue;

                var includeDir = parts[1].Trim().Replace("$pwd", repoRoot.Replace('\\', '/'));
                var header = Path.Combine(includeDir.Replace('/', Path.DirectorySeparatorChar), "ffms.h");
                if (File.Exists(header))
                    return header;
            }
        }

        return null;
    }

    public static IReadOnlyList<string> GetDefaultSources(string repoRoot) {
        return [
            Path.Combine(repoRoot, "src", "ffmpegsource_common.cpp"),
            Path.Combine(repoRoot, "src", "audio_provider_ffmpegsource.cpp"),
            Path.Combine(repoRoot, "src", "video_provider_ffmpegsource.cpp")
        ];
    }

    public static Dictionary<string, long> GetDefaultDefines() {
        return new Dictionary<string, long>(StringComparer.Ordinal) {
            ["WITH_FFMS2"] = 1,
            ["WITH_FFMS2_RUNTIME_LOADING"] = 1,
            ["_WIN32"] = 1
        };
    }
}

static class Generator {
    private static readonly Regex HeaderApiPattern = new("FFMS_(?:DEPRECATED_)?API\\([^)]*\\)\\s+FFMS_([A-Za-z0-9_]+)\\(", RegexOptions.Compiled);
    private static readonly Regex FfmsVersionPattern = new("^\\s*#\\s*define\\s+FFMS_VERSION\\s+(.+?)\\s*$", RegexOptions.Compiled | RegexOptions.Multiline);
    private static readonly Regex TokenPattern = new("\\bffms::([A-Za-z0-9_]+)\\b", RegexOptions.Compiled);

    public static List<string> GetExportedFunctions(string headerPath) {
        var content = File.ReadAllText(headerPath, Encoding.UTF8);
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
        var versionMatch = FfmsVersionPattern.Match(content);
        if (!versionMatch.Success)
            throw new InvalidOperationException("Could not find FFMS_VERSION in ffms.h");

        var value = ValueParser.EvaluateInteger(versionMatch.Groups[1].Value, new Dictionary<string, long>(StringComparer.Ordinal));
        return FormatVersion(value);
    }

    public static List<string> GetUsedFunctions(string headerPath, IReadOnlyCollection<string> exportedFunctions, IReadOnlyList<string> sourcePaths, Dictionary<string, long> defines) {
        var content = File.ReadAllText(headerPath, Encoding.UTF8);
        var versionMatch = FfmsVersionPattern.Match(content);
        if (!versionMatch.Success)
            throw new InvalidOperationException("Could not find FFMS_VERSION in ffms.h");

        defines.TryAdd("FFMS_VERSION", ValueParser.EvaluateInteger(versionMatch.Groups[1].Value, defines));

        var exported = new HashSet<string>(exportedFunctions, StringComparer.Ordinal);
        var ordered = new List<string>();
        var seen = new HashSet<string>(StringComparer.Ordinal);
        foreach (var sourcePath in sourcePaths)
            GatherUsedFunctions(sourcePath, exported, defines, ordered, seen);

        return ordered;
    }

    public static string Render(GenerationMode mode, string headerVersion, IReadOnlyList<string> functions) {
        var builder = new StringBuilder();
        builder.AppendLine($"/* Auto-generated by vendor/ffms2/gen_ffms2_functions.cs. Mode: {mode.ToString().ToLowerInvariant()}. FFMS_VERSION: {headerVersion}. */");
        foreach (var function in functions)
            builder.AppendLine($"AGI_FFMS2_FN({function})");
        return builder.ToString();
    }

    private static string FormatVersion(long version) {
        return FormattableString.Invariant($"{(version >> 24) & 0xFF}.{(version >> 16) & 0xFF}.{(version >> 8) & 0xFF}.{version & 0xFF}");
    }

    private static void GatherUsedFunctions(string sourcePath, HashSet<string> exported, Dictionary<string, long> defines, List<string> ordered, HashSet<string> seen) {
        var stack = new Stack<ConditionalFrame>();
        var inBlockComment = false;

        foreach (var rawLine in File.ReadLines(sourcePath, Encoding.UTF8)) {
            var trimmed = rawLine.TrimStart();
            var active = stack.Count == 0 || stack.Peek().Active;

            if (trimmed.StartsWith("#", StringComparison.Ordinal)) {
                var directive = trimmed[1..].Trim();
                if (directive.StartsWith("if ", StringComparison.Ordinal)) {
                    var parent = active;
                    var value = parent && ValueParser.EvaluateBoolean(directive[3..].Trim(), defines);
                    stack.Push(new ConditionalFrame(parent, value, value));
                    continue;
                }

                if (directive.StartsWith("ifdef ", StringComparison.Ordinal)) {
                    var parent = active;
                    var name = directive[6..].Trim();
                    var value = parent && defines.ContainsKey(name);
                    stack.Push(new ConditionalFrame(parent, value, value));
                    continue;
                }

                if (directive.StartsWith("ifndef ", StringComparison.Ordinal)) {
                    var parent = active;
                    var name = directive[7..].Trim();
                    var value = parent && !defines.ContainsKey(name);
                    stack.Push(new ConditionalFrame(parent, value, value));
                    continue;
                }

                if (directive.StartsWith("elif ", StringComparison.Ordinal)) {
                    var frame = stack.Pop();
                    var branchActive = frame.ParentActive && !frame.Taken && ValueParser.EvaluateBoolean(directive[5..].Trim(), defines);
                    stack.Push(frame with { Active = branchActive, Taken = frame.Taken || branchActive });
                    continue;
                }

                if (directive.Equals("else", StringComparison.Ordinal)) {
                    var frame = stack.Pop();
                    var branchActive = frame.ParentActive && !frame.Taken;
                    stack.Push(frame with { Active = branchActive, Taken = frame.Taken || branchActive });
                    continue;
                }

                if (directive.Equals("endif", StringComparison.Ordinal)) {
                    if (stack.Count == 0)
                        throw new InvalidOperationException($"Unmatched #endif in {sourcePath}");
                    stack.Pop();
                    continue;
                }
            }

            if (!active)
                continue;

            var content = StripComments(rawLine, ref inBlockComment);
            foreach (Match match in TokenPattern.Matches(content)) {
                var function = match.Groups[1].Value;
                if (exported.Contains(function) && seen.Add(function))
                    ordered.Add(function);
            }
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

    private readonly record struct ConditionalFrame(bool ParentActive, bool Active, bool Taken);
}

static class ValueParser {
    private static readonly Regex DefinedCallPattern = new("defined\\s*\\(\\s*([A-Za-z_][A-Za-z0-9_]*)\\s*\\)", RegexOptions.Compiled);
    private static readonly Regex DefinedBarePattern = new("defined\\s+([A-Za-z_][A-Za-z0-9_]*)", RegexOptions.Compiled);

    public static long ParseInteger(string value) {
        if (value.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
            return Convert.ToInt64(value[2..], 16);
        return long.Parse(value, CultureInfo.InvariantCulture);
    }

    public static long EvaluateInteger(string expression, IReadOnlyDictionary<string, long> macros) {
        var prepared = PrepareExpression(expression, macros);
        return new ExpressionParser(prepared, macros).Parse();
    }

    public static bool EvaluateBoolean(string expression, IReadOnlyDictionary<string, long> macros) {
        return EvaluateInteger(expression, macros) != 0;
    }

    private static string PrepareExpression(string expression, IReadOnlyDictionary<string, long> macros) {
        expression = DefinedCallPattern.Replace(expression, match => macros.ContainsKey(match.Groups[1].Value) ? "1" : "0");
        expression = DefinedBarePattern.Replace(expression, match => macros.ContainsKey(match.Groups[1].Value) ? "1" : "0");
        return expression;
    }

    private sealed class ExpressionParser(string expression, IReadOnlyDictionary<string, long> macros) {
        private readonly string expression = expression;
        private readonly IReadOnlyDictionary<string, long> macros = macros;
        private int index;

        public long Parse() {
            var value = ParseLogicalOr();
            SkipWhitespace();
            if (index != expression.Length)
                throw new InvalidOperationException($"Unexpected token in expression: {expression[index..]}");
            return value;
        }

        private long ParseLogicalOr() {
            var value = ParseLogicalAnd();
            while (Consume("||"))
                value = ParseLogicalAnd() != 0 || value != 0 ? 1 : 0;
            return value;
        }

        private long ParseLogicalAnd() {
            var value = ParseBitwiseOr();
            while (Consume("&&"))
                value = ParseBitwiseOr() != 0 && value != 0 ? 1 : 0;
            return value;
        }

        private long ParseBitwiseOr() {
            var value = ParseBitwiseAnd();
            while (Consume("|"))
                value |= ParseBitwiseAnd();
            return value;
        }

        private long ParseBitwiseAnd() {
            var value = ParseEquality();
            while (Consume("&"))
                value &= ParseEquality();
            return value;
        }

        private long ParseEquality() {
            var value = ParseRelational();
            while (true) {
                if (Consume("=="))
                    value = value == ParseRelational() ? 1 : 0;
                else if (Consume("!="))
                    value = value != ParseRelational() ? 1 : 0;
                else
                    return value;
            }
        }

        private long ParseRelational() {
            var value = ParseShift();
            while (true) {
                if (Consume("<="))
                    value = value <= ParseShift() ? 1 : 0;
                else if (Consume(">="))
                    value = value >= ParseShift() ? 1 : 0;
                else if (Consume("<"))
                    value = value < ParseShift() ? 1 : 0;
                else if (Consume(">"))
                    value = value > ParseShift() ? 1 : 0;
                else
                    return value;
            }
        }

        private long ParseShift() {
            var value = ParseAdditive();
            while (true) {
                if (Consume("<<"))
                    value <<= (int)ParseAdditive();
                else if (Consume(">>"))
                    value >>= (int)ParseAdditive();
                else
                    return value;
            }
        }

        private long ParseAdditive() {
            var value = ParseUnary();
            while (true) {
                if (Consume("+"))
                    value += ParseUnary();
                else if (Consume("-"))
                    value -= ParseUnary();
                else
                    return value;
            }
        }

        private long ParseUnary() {
            if (Consume("!"))
                return ParseUnary() == 0 ? 1 : 0;
            if (Consume("~"))
                return ~ParseUnary();
            if (Consume("+"))
                return ParseUnary();
            if (Consume("-"))
                return -ParseUnary();
            return ParsePrimary();
        }

        private long ParsePrimary() {
            SkipWhitespace();
            if (Consume("(")) {
                var value = ParseLogicalOr();
                Expect(")");
                return value;
            }

            if (index >= expression.Length)
                throw new InvalidOperationException("Unexpected end of expression");

            if (char.IsDigit(expression[index]))
                return ParseNumber();
            if (char.IsLetter(expression[index]) || expression[index] == '_')
                return ParseIdentifier();

            throw new InvalidOperationException($"Unexpected token '{expression[index]}' in expression");
        }

        private long ParseNumber() {
            var start = index;
            if (Match("0x") || Match("0X")) {
                index += 2;
                while (index < expression.Length && Uri.IsHexDigit(expression[index]))
                    index++;
            }
            else {
                while (index < expression.Length && char.IsDigit(expression[index]))
                    index++;
            }

            return ParseInteger(expression[start..index]);
        }

        private long ParseIdentifier() {
            var start = index;
            while (index < expression.Length && (char.IsLetterOrDigit(expression[index]) || expression[index] == '_'))
                index++;
            var name = expression[start..index];
            return macros.TryGetValue(name, out var value) ? value : 0;
        }

        private bool Consume(string token) {
            SkipWhitespace();
            if (!Match(token))
                return false;
            index += token.Length;
            return true;
        }

        private bool Match(string token) {
            return index + token.Length <= expression.Length && string.CompareOrdinal(expression, index, token, 0, token.Length) == 0;
        }

        private void Expect(string token) {
            if (!Consume(token))
                throw new InvalidOperationException($"Expected '{token}' in expression");
        }

        private void SkipWhitespace() {
            while (index < expression.Length && char.IsWhiteSpace(expression[index]))
                index++;
        }
    }
}
