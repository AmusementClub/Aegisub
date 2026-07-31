#:package System.CommandLine@2.0.5
#:package Spectre.Console@0.54.0
#:package Karambolo.PO@1.13.0
#:property PublishAot=false

using System.CommandLine;
using System.Text;
using System.Text.RegularExpressions;
using Karambolo.PO;
using Spectre.Console;

var repoRoot = Tooling.FindRepoRoot(Directory.GetCurrentDirectory());

var poDirOption = new Option<string>("--po-dir")
{
    Description = "Directory containing LINGUAS and .po files.",
    DefaultValueFactory = _ => Path.Combine(repoRoot, "po")
};

var linguasOption = new Option<string?>("--linguas")
{
    Description = "Path to a LINGUAS file. Defaults to <po-dir>/LINGUAS."
};

var languageOption = new Option<string?>("--language")
{
    Description = "Compile or inspect one language instead of all languages from LINGUAS."
};

var outputOption = new Option<string?>("--output")
{
    Description = "Output .mo/.gmo path. Only valid with --language."
};

var outputDirOption = new Option<string>("--output-dir")
{
    Description = "Directory for compiled .gmo files.",
    DefaultValueFactory = _ => Directory.GetCurrentDirectory()
};

var strictPlaceholdersOption = new Option<bool>("--strict-placeholders")
{
    Description = "Fail when printf-style placeholders do not match translated strings."
};

var compileCommand = new Command("compile", "Compile gettext .po files to .mo/.gmo files.");
compileCommand.Options.Add(poDirOption);
compileCommand.Options.Add(linguasOption);
compileCommand.Options.Add(languageOption);
compileCommand.Options.Add(outputOption);
compileCommand.Options.Add(outputDirOption);
compileCommand.Options.Add(strictPlaceholdersOption);
compileCommand.SetAction(parseResult =>
{
    var options = ToolOptions.From(parseResult, poDirOption, linguasOption, languageOption, outputOption, outputDirOption, strictPlaceholdersOption);
    var languages = options.GetLanguages();

    if (options.OutputPath is not null && languages.Count != 1)
        throw new InvalidOperationException("--output can only be used when exactly one --language is supplied.");

    foreach (var language in languages)
    {
        var poPath = options.GetPoPath(language);
        var outputPath = options.OutputPath ?? Path.Combine(options.OutputDir, $"{language}.gmo");
        var catalog = PoCatalogFile.Load(poPath);
        var problems = CatalogChecks.Check(language, catalog, options.StrictPlaceholders);
        if (problems.HasErrors)
            throw new InvalidOperationException($"Cannot compile {language}: fix validation errors first.");

        var stats = MoCompiler.Compile(catalog.Catalog, outputPath);
        Log("compile", $"{language} -> {Tooling.DisplayPath(repoRoot, outputPath)} ({stats.CompiledMessages} messages)", "green");
    }
});

var statsCommand = new Command("stats", "Show translation coverage for .po files.");
statsCommand.Options.Add(poDirOption);
statsCommand.Options.Add(linguasOption);
statsCommand.Options.Add(languageOption);
statsCommand.SetAction(parseResult =>
{
    var options = ToolOptions.From(parseResult, poDirOption, linguasOption, languageOption, outputOption, outputDirOption, strictPlaceholdersOption);
    var table = new Table()
        .AddColumn("Language")
        .AddColumn(new TableColumn("Translated").RightAligned())
        .AddColumn(new TableColumn("Fuzzy").RightAligned())
        .AddColumn(new TableColumn("Missing").RightAligned())
        .AddColumn(new TableColumn("Coverage").RightAligned());

    foreach (var language in options.GetLanguages())
    {
        var catalog = PoCatalogFile.Load(options.GetPoPath(language));
        var stats = TranslationStats.For(catalog.Catalog);
        table.AddRow(
            language,
            stats.Translated.ToString(),
            stats.Fuzzy.ToString(),
            stats.Missing.ToString(),
            stats.Total == 0 ? "n/a" : $"{stats.Coverage:0.0}%");
    }

    AnsiConsole.Write(table);
});

var checkCommand = new Command("check", "Parse and validate .po files.");
checkCommand.Options.Add(poDirOption);
checkCommand.Options.Add(linguasOption);
checkCommand.Options.Add(languageOption);
checkCommand.Options.Add(strictPlaceholdersOption);
checkCommand.SetAction(parseResult =>
{
    var options = ToolOptions.From(parseResult, poDirOption, linguasOption, languageOption, outputOption, outputDirOption, strictPlaceholdersOption);
    var failed = false;

    foreach (var language in options.GetLanguages())
    {
        var catalog = PoCatalogFile.Load(options.GetPoPath(language));
        var problems = CatalogChecks.Check(language, catalog, options.StrictPlaceholders);
        if (problems.HasErrors)
            failed = true;

        if (problems.HasMessages)
        {
            foreach (var message in problems.Messages)
                Log(message.Kind, $"{language}: {message.Text}", message.Kind == "error" ? "red" : "yellow");
        }
        else
        {
            Log("check", $"{language}: ok", "green");
        }
    }

    if (failed)
        throw new InvalidOperationException("Localization validation failed.");
});

var rootCommand = new RootCommand("Aegisub localization maintenance tool.");
rootCommand.Subcommands.Add(compileCommand);
rootCommand.Subcommands.Add(statsCommand);
rootCommand.Subcommands.Add(checkCommand);

return await rootCommand.Parse(args).InvokeAsync();

static void Log(string label, string value, string color) =>
    AnsiConsole.MarkupLine($"[{color}]{Markup.Escape(label)}[/] {Markup.Escape(value)}");

sealed record ToolOptions(
    string PoDir,
    string LinguasPath,
    string? Language,
    string? OutputPath,
    string OutputDir,
    bool StrictPlaceholders)
{
    public static ToolOptions From(
        ParseResult parseResult,
        Option<string> poDirOption,
        Option<string?> linguasOption,
        Option<string?> languageOption,
        Option<string?> outputOption,
        Option<string> outputDirOption,
        Option<bool> strictPlaceholdersOption)
    {
        var poDir = Path.GetFullPath(parseResult.GetValue(poDirOption)!);
        var linguas = parseResult.GetValue(linguasOption);
        return new ToolOptions(
            poDir,
            Path.GetFullPath(linguas is null ? Path.Combine(poDir, "LINGUAS") : linguas),
            parseResult.GetValue(languageOption),
            ResolveOptional(parseResult.GetValue(outputOption)),
            Path.GetFullPath(parseResult.GetValue(outputDirOption) ?? Directory.GetCurrentDirectory()),
            parseResult.GetValue(strictPlaceholdersOption));
    }

    public IReadOnlyList<string> GetLanguages()
    {
        if (!string.IsNullOrWhiteSpace(Language))
            return [Language];

        if (!File.Exists(LinguasPath))
            throw new FileNotFoundException($"Missing LINGUAS file: {LinguasPath}");

        var languages = new List<string>();
        foreach (var line in File.ReadLines(LinguasPath, Encoding.UTF8))
        {
            var content = line.Split('#', 2)[0];
            languages.AddRange(content.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries));
        }

        return languages;
    }

    public string GetPoPath(string language) => Path.Combine(PoDir, $"{language}.po");

    private static string? ResolveOptional(string? path) =>
        string.IsNullOrWhiteSpace(path) ? null : Path.GetFullPath(path);
}

sealed record PoCatalogFile(string Path, POCatalog Catalog, IReadOnlyList<Diagnostic> Diagnostics)
{
    public static PoCatalogFile Load(string path)
    {
        if (!File.Exists(path))
            throw new FileNotFoundException($"Missing PO file: {path}");

        using var stream = File.OpenRead(path);
        var encoding = POParser.DetectEncoding(stream) ?? new UTF8Encoding(false);
        stream.Position = 0;

        using var reader = new StreamReader(stream, encoding, detectEncodingFromByteOrderMarks: true);
        var parser = new POParser(new POParserSettings
        {
            StringDecodingOptions = new POStringDecodingOptions
            {
                KeepKeyStringsPlatformIndependent = true,
                KeepTranslationStringsPlatformIndependent = true
            }
        });
        var result = parser.Parse(reader);

        if (!result.Success)
        {
            var diagnostics = string.Join(Environment.NewLine, result.Diagnostics.Select(d => $"{d.Severity}: {d}"));
            throw new InvalidOperationException($"Failed to parse {path}:{Environment.NewLine}{diagnostics}");
        }

        return new PoCatalogFile(path, result.Catalog, result.Diagnostics.ToArray());
    }
}

static class CatalogChecks
{
    public static ProblemSet Check(string language, PoCatalogFile catalogFile, bool strictPlaceholders)
    {
        var problems = new ProblemSet();

        foreach (var diagnostic in catalogFile.Diagnostics.Where(d => d.Severity == DiagnosticSeverity.Warning))
            problems.Add("warn", diagnostic.ToString());

        var headerLanguage = catalogFile.Catalog.Language;
        if (!string.IsNullOrWhiteSpace(headerLanguage) && !LanguageMatchesFile(language, headerLanguage))
            problems.Add("warn", $"header Language is '{headerLanguage}', expected '{language}'");

        if (strictPlaceholders)
            CheckPlaceholders(catalogFile.Catalog, problems);

        return problems;
    }

    private static bool LanguageMatchesFile(string fileLanguage, string headerLanguage) =>
        string.Equals(fileLanguage, headerLanguage, StringComparison.OrdinalIgnoreCase)
        || string.Equals(fileLanguage.Replace('_', '-'), headerLanguage.Replace('_', '-'), StringComparison.OrdinalIgnoreCase);

    private static void CheckPlaceholders(POCatalog catalog, ProblemSet problems)
    {
        foreach (var entry in catalog)
        {
            if (EntryInfo.IsFuzzy(entry))
                continue;

            foreach (var (translation, index) in entry.Select((value, index) => (value, index)))
            {
                if (string.IsNullOrEmpty(translation))
                    continue;

                var expected = PlaceholderSet.ForEntry(entry.Key);
                var actual = PlaceholderSet.FromTranslation(translation);
                if (actual.Except(expected).Any())
                    problems.Add("error", $"placeholder mismatch for '{Shorten(entry.Key.Id)}'");
            }
        }
    }

    private static string Shorten(string value) =>
        value.Length <= 72 ? value : value[..69] + "...";
}

sealed class ProblemSet
{
    private readonly List<ProblemMessage> messages = [];

    public IReadOnlyList<ProblemMessage> Messages => messages;
    public bool HasMessages => messages.Count > 0;
    public bool HasErrors => messages.Any(message => message.Kind == "error");

    public void Add(string kind, string text) => messages.Add(new ProblemMessage(kind, text));
}

sealed record ProblemMessage(string Kind, string Text);

static class TranslationStats
{
    public static TranslationSummary For(POCatalog catalog)
    {
        var total = 0;
        var translated = 0;
        var fuzzy = 0;

        foreach (var entry in catalog)
        {
            total++;
            if (EntryInfo.IsFuzzy(entry))
            {
                fuzzy++;
                continue;
            }

            if (EntryInfo.IsTranslated(entry))
                translated++;
        }

        return new TranslationSummary(total, translated, fuzzy, total - translated - fuzzy);
    }
}

sealed record TranslationSummary(int Total, int Translated, int Fuzzy, int Missing)
{
    public double Coverage => Total == 0 ? 0 : Translated * 100.0 / Total;
}

static class MoCompiler
{
    public static CompileStats Compile(POCatalog catalog, string outputPath)
    {
        var messages = new Dictionary<string, string>(StringComparer.Ordinal)
        {
            [""] = BuildHeader(catalog)
        };

        foreach (var entry in catalog)
        {
            if (!EntryInfo.TryBuildMessage(entry, out var message))
                continue;
            messages[message.Original] = message.Translation;
        }

        var ordered = messages
            .OrderBy(pair => pair.Key, StringComparer.Ordinal)
            .Select(pair => new EncodedMessage(
                Encoding.UTF8.GetBytes(pair.Key),
                Encoding.UTF8.GetBytes(pair.Value)))
            .ToArray();

        Directory.CreateDirectory(Path.GetDirectoryName(outputPath)!);
        using var output = File.Create(outputPath);
        using var writer = new BinaryWriter(output, Encoding.UTF8, leaveOpen: false);

        var count = ordered.Length;
        var originalTableOffset = 7 * sizeof(uint);
        var translationTableOffset = originalTableOffset + count * 2 * sizeof(uint);
        var stringOffset = translationTableOffset + count * 2 * sizeof(uint);
        var originalOffset = stringOffset;
        var translationOffset = originalOffset + ordered.Sum(message => message.Original.Length + 1);

        writer.Write(0x950412deu);
        writer.Write(0u);
        writer.Write((uint)count);
        writer.Write((uint)originalTableOffset);
        writer.Write((uint)translationTableOffset);
        writer.Write(0u);
        writer.Write(0u);

        var nextOffset = originalOffset;
        foreach (var message in ordered)
        {
            writer.Write((uint)message.Original.Length);
            writer.Write((uint)nextOffset);
            nextOffset += message.Original.Length + 1;
        }

        nextOffset = translationOffset;
        foreach (var message in ordered)
        {
            writer.Write((uint)message.Translation.Length);
            writer.Write((uint)nextOffset);
            nextOffset += message.Translation.Length + 1;
        }

        foreach (var message in ordered)
        {
            writer.Write(message.Original);
            writer.Write((byte)0);
        }

        foreach (var message in ordered)
        {
            writer.Write(message.Translation);
            writer.Write((byte)0);
        }

        return new CompileStats(count - 1);
    }

    private static string BuildHeader(POCatalog catalog)
    {
        if (catalog.Headers is null || catalog.Headers.Count == 0)
            return "Content-Type: text/plain; charset=UTF-8\n";

        var builder = new StringBuilder();
        foreach (var (key, value) in catalog.Headers)
            builder.Append(key).Append(": ").Append(value).Append('\n');
        return builder.ToString();
    }
}

sealed record CompileStats(int CompiledMessages);
sealed record EncodedMessage(byte[] Original, byte[] Translation);
sealed record MoMessage(string Original, string Translation);

static class EntryInfo
{
    public static bool IsFuzzy(IPOEntry entry) =>
        entry.Comments?.OfType<POFlagsComment>().Any(comment => comment.Flags?.Contains("fuzzy") == true) == true;

    public static bool IsTranslated(IPOEntry entry) =>
        !IsFuzzy(entry) && entry.All(value => !string.IsNullOrEmpty(value));

    public static bool TryBuildMessage(IPOEntry entry, out MoMessage message)
    {
        message = null!;

        if (!IsTranslated(entry))
            return false;

        var original = entry.Key.ContextId is null
            ? entry.Key.Id
            : entry.Key.ContextId + "\u0004" + entry.Key.Id;

        if (entry.Key.PluralId is not null)
            original += "\0" + entry.Key.PluralId;

        message = new MoMessage(original, string.Join('\0', entry));
        return true;
    }
}

static class PlaceholderSet
{
    private static readonly Regex PrintfPlaceholder = new(
        @"%(?:\d+\$)?[#0\-+']*(?:\*|\d+)?(?:\.(?:\*|\d+))?(?:hh|h|ll|l|j|z|t|L)?[diuoxXfFeEgGaAcCsSpn%]",
        RegexOptions.Compiled);

    public static HashSet<string> ForEntry(POKey key)
    {
        var result = FromTranslation(key.Id);
        if (key.PluralId is not null)
            result.UnionWith(FromTranslation(key.PluralId));
        return result;
    }

    public static HashSet<string> FromTranslation(string value)
    {
        var result = new HashSet<string>(StringComparer.Ordinal);
        foreach (Match match in PrintfPlaceholder.Matches(value))
        {
            if (match.Value == "%%")
                continue;
            result.Add(Normalize(match.Value));
        }
        return result;
    }

    private static string Normalize(string placeholder)
    {
        var specifier = placeholder[^1];
        if (specifier is 'd' or 'i' or 'u')
            specifier = 'd';
        var positional = Regex.Match(placeholder, @"^%(\d+\$)");
        return positional.Success ? "%" + positional.Groups[1].Value + specifier : "%" + specifier;
    }
}

static class Tooling
{
    public static string FindRepoRoot(string startDirectory)
    {
        var current = new DirectoryInfo(startDirectory);
        while (current is not null)
        {
            if (File.Exists(Path.Combine(current.FullName, "CMakeLists.txt")) && Directory.Exists(Path.Combine(current.FullName, "po")))
                return current.FullName;
            current = current.Parent;
        }

        throw new InvalidOperationException("Could not locate repository root. Run the tool from inside the repository.");
    }

    public static string DisplayPath(string repoRoot, string path)
    {
        var fullRepoRoot = Path.GetFullPath(repoRoot);
        var fullPath = Path.GetFullPath(path);
        if (fullPath.StartsWith(fullRepoRoot + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            return Path.GetRelativePath(fullRepoRoot, fullPath).Replace('\\', '/');
        return fullPath;
    }
}
