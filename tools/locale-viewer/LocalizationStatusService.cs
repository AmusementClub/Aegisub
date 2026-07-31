using System.Text;
using Karambolo.PO;

namespace Aegisub.LocaleViewer;

public sealed class LocalizationStatusService(string repositoryRoot)
{
    public string RepositoryRoot { get; private set; } = repositoryRoot;

    public void UpdateRepositoryRoot(string newRoot) => RepositoryRoot = newRoot;

    public Task<LocalizationSnapshot> LoadAsync(CancellationToken cancellationToken = default) =>
        Task.Run(() => Load(cancellationToken), cancellationToken);

    public Task<TranslationFileSnapshot> LoadTranslationFileAsync(string language, CancellationToken cancellationToken = default) =>
        Task.Run(() => LoadTranslationFile(language, cancellationToken), cancellationToken);

    public Task<IReadOnlyList<TranslationComparison>> LoadComparisonAsync(
        POKey key,
        string selectedLanguage,
        CancellationToken cancellationToken = default) =>
        Task.Run(() => LoadComparison(key, selectedLanguage, cancellationToken), cancellationToken);

    public Task SaveTranslationFileAsync(string language, IEnumerable<TranslationUpdate> updates, CancellationToken cancellationToken = default) =>
        Task.Run(() => SaveTranslationFile(language, updates, cancellationToken), cancellationToken);

    public LocalizationSnapshot Load() => Load(CancellationToken.None);

    private LocalizationSnapshot Load(CancellationToken cancellationToken)
    {
        var poDir = Path.Combine(RepositoryRoot, "po");
        var linguasPath = Path.Combine(poDir, "LINGUAS");
        if (!File.Exists(linguasPath))
            throw new FileNotFoundException($"Missing LINGUAS file: {linguasPath}");

        cancellationToken.ThrowIfCancellationRequested();
        var languages = ReadLinguas(linguasPath)
            .AsParallel()
            .WithCancellation(cancellationToken)
            .WithDegreeOfParallelism(Math.Max(1, Environment.ProcessorCount))
            .Select(language => LoadLanguage(poDir, language, cancellationToken))
            .OrderBy(language => language.Language, StringComparer.OrdinalIgnoreCase)
            .ToArray();

        return new LocalizationSnapshot(languages);
    }

    private TranslationFileSnapshot LoadTranslationFile(string language, CancellationToken cancellationToken)
    {
        var path = GetPoPath(language);
        if (!File.Exists(path))
            throw new FileNotFoundException($"Missing PO file: {path}");

        cancellationToken.ThrowIfCancellationRequested();
        var catalogFile = PoCatalogFile.Load(path);
        cancellationToken.ThrowIfCancellationRequested();

        var entries = catalogFile.Catalog.Values
            .Where(entry => !string.IsNullOrEmpty(entry.Key.Id))
            .Select((entry, index) => TranslationUnit.From(index + 1, entry))
            .ToArray();

        return new TranslationFileSnapshot(language, path, entries);
    }

    private IReadOnlyList<TranslationComparison> LoadComparison(POKey key, string selectedLanguage, CancellationToken cancellationToken)
    {
        var poDir = Path.Combine(RepositoryRoot, "po");
        var linguasPath = Path.Combine(poDir, "LINGUAS");
        if (!File.Exists(linguasPath))
            throw new FileNotFoundException($"Missing LINGUAS file: {linguasPath}");

        var languages = ReadLinguas(linguasPath);
        var comparisons = languages
            .AsParallel()
            .WithCancellation(cancellationToken)
            .WithDegreeOfParallelism(Math.Max(1, Environment.ProcessorCount))
            .Where(language => !string.Equals(language, selectedLanguage, StringComparison.OrdinalIgnoreCase))
            .Select(language => LoadComparisonLanguage(poDir, language, key, cancellationToken))
            .Where(comparison => comparison is not null)
            .Cast<TranslationComparison>()
            .OrderBy(comparison => comparison.Language, StringComparer.OrdinalIgnoreCase)
            .ToArray();

        return comparisons;
    }

    private void SaveTranslationFile(string language, IEnumerable<TranslationUpdate> updates, CancellationToken cancellationToken)
    {
        var path = GetPoPath(language);
        if (!File.Exists(path))
            throw new FileNotFoundException($"Missing PO file: {path}");

        var updateByKey = updates.ToDictionary(update => update.Key);
        if (updateByKey.Count == 0)
            return;

        cancellationToken.ThrowIfCancellationRequested();
        var catalogFile = PoCatalogFile.Load(path);

        foreach (var entry in catalogFile.Catalog)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (!updateByKey.TryGetValue(entry.Key, out var update))
                continue;

            ApplyTranslations(entry, update.Translations);
            EntryInfo.SetFuzzy(entry, update.IsFuzzy);
        }

        var tempPath = $"{path}.{Guid.NewGuid():N}.tmp";
        try
        {
            using (var output = new FileStream(tempPath, FileMode.CreateNew, FileAccess.Write, FileShare.None, 64 * 1024))
            {
                var generator = new POGenerator(new POGeneratorSettings
                {
                    PreserveHeadersOrder = true
                });
                generator.Generate(output, catalogFile.Catalog, catalogFile.Encoding);
            }

            File.Move(tempPath, path, overwrite: true);
        }
        finally
        {
            if (File.Exists(tempPath))
                File.Delete(tempPath);
        }
    }

    private static IReadOnlyList<string> ReadLinguas(string path)
    {
        var languages = new List<string>();
        foreach (var line in File.ReadLines(path, Encoding.UTF8))
        {
            var content = line.Split('#', 2)[0];
            languages.AddRange(content.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries));
        }

        return languages;
    }

    private static LanguageStatus LoadLanguage(string poDir, string language, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var path = Path.Combine(poDir, $"{language}.po");
        if (!File.Exists(path))
            return new LanguageStatus(language, 0, 0, 0, 0, [$"missing {language}.po"]);

        var catalogFile = PoCatalogFile.Load(path);
        cancellationToken.ThrowIfCancellationRequested();
        var translated = 0;
        var fuzzy = 0;
        var total = 0;

        foreach (var entry in catalogFile.Catalog)
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

        var missing = total - translated - fuzzy;
        var coverage = total == 0 ? 0 : translated * 100.0 / total;
        var warnings = BuildWarnings(language, catalogFile);

        return new LanguageStatus(language, translated, fuzzy, missing, coverage, warnings);
    }

    private static TranslationComparison? LoadComparisonLanguage(
        string poDir,
        string language,
        POKey key,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var path = Path.Combine(poDir, $"{language}.po");
        if (!File.Exists(path))
            return null;

        var catalogFile = PoCatalogFile.Load(path);
        cancellationToken.ThrowIfCancellationRequested();

        return catalogFile.Catalog.TryGetValue(key, out var entry)
            ? TranslationComparison.From(language, entry)
            : new TranslationComparison(language, [], false, true);
    }

    private static IReadOnlyList<string> BuildWarnings(string language, PoCatalogFile catalogFile)
    {
        var warnings = catalogFile.Diagnostics
            .Where(diagnostic => diagnostic.Severity == DiagnosticSeverity.Warning)
            .Select(diagnostic => diagnostic.ToString())
            .ToList();

        var headerLanguage = catalogFile.Catalog.Language;
        if (!string.IsNullOrWhiteSpace(headerLanguage) && !LanguageMatchesFile(language, headerLanguage))
            warnings.Add($"Language: {headerLanguage}");

        return warnings;
    }

    private static bool LanguageMatchesFile(string fileLanguage, string headerLanguage) =>
        string.Equals(fileLanguage, headerLanguage, StringComparison.OrdinalIgnoreCase)
        || string.Equals(fileLanguage.Replace('_', '-'), headerLanguage.Replace('_', '-'), StringComparison.OrdinalIgnoreCase);

    private string GetPoPath(string language) => Path.Combine(RepositoryRoot, "po", $"{language}.po");

    private static void ApplyTranslations(IPOEntry entry, IReadOnlyList<string> translations)
    {
        switch (entry)
        {
            case POSingularEntry singular:
                singular.Translation = translations.Count == 0 ? "" : translations[0];
                break;

            case POPluralEntry plural:
                for (var index = 0; index < translations.Count; index++)
                {
                    if (index < plural.Count)
                        plural[index] = translations[index];
                    else
                        plural.Add(translations[index]);
                }

                break;
        }
    }
}

public sealed record LocalizationSnapshot(IReadOnlyList<LanguageStatus> Languages)
{
    public int TotalFuzzy { get; } = Languages.Sum(language => language.Fuzzy);
    public int TotalMissing { get; } = Languages.Sum(language => language.Missing);
    public double AverageCoverage { get; } = Languages.Count == 0 ? 0 : Languages.Average(language => language.Coverage);
}

public sealed record LanguageStatus(
    string Language,
    int Translated,
    int Fuzzy,
    int Missing,
    double Coverage,
    IReadOnlyList<string> Warnings);

public sealed record TranslationFileSnapshot(
    string Language,
    string Path,
    IReadOnlyList<TranslationUnit> Entries);

public sealed record TranslationUnit(
    int Number,
    POKey Key,
    string SourceText,
    string? PluralSourceText,
    string? ContextText,
    IReadOnlyList<string> Translations,
    bool IsFuzzy,
    string ReferenceText,
    string CommentText)
{
    public bool IsPlural => PluralSourceText is not null;

    public static TranslationUnit From(int number, IPOEntry entry)
    {
        var translations = entry switch
        {
            POSingularEntry singular => [singular.Translation ?? ""],
            POPluralEntry plural => plural.ToArray(),
            _ => []
        };

        var references = entry.Comments?
            .OfType<POReferenceComment>()
            .SelectMany(comment => comment.References ?? [])
            .Select(reference => reference.ToString())
            .ToArray() ?? [];

        var comments = entry.Comments?
            .OfType<POTranslatorComment>()
            .Select(comment => comment.Text)
            .Where(comment => !string.IsNullOrWhiteSpace(comment))
            .ToArray() ?? [];

        return new TranslationUnit(
            number,
            entry.Key,
            entry.Key.Id,
            entry.Key.PluralId,
            entry.Key.ContextId,
            translations,
            EntryInfo.IsFuzzy(entry),
            string.Join("; ", references),
            string.Join(Environment.NewLine, comments));
    }
}

public sealed record TranslationUpdate(
    POKey Key,
    IReadOnlyList<string> Translations,
    bool IsFuzzy);

public sealed record TranslationComparison(
    string Language,
    IReadOnlyList<string> Translations,
    bool IsFuzzy,
    bool IsMissing)
{
    public static TranslationComparison From(string language, IPOEntry entry)
    {
        var translations = entry switch
        {
            POSingularEntry singular => [singular.Translation ?? ""],
            POPluralEntry plural => plural.ToArray(),
            _ => []
        };

        return new TranslationComparison(language, translations, EntryInfo.IsFuzzy(entry), false);
    }
}

internal sealed record PoCatalogFile(POCatalog Catalog, IReadOnlyList<Diagnostic> Diagnostics, Encoding Encoding)
{
    private const int ReadBufferSize = 64 * 1024;

    public static PoCatalogFile Load(string path)
    {
        using var stream = new FileStream(
            path,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read,
            ReadBufferSize,
            FileOptions.SequentialScan);
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
            throw new InvalidOperationException($"Failed to parse {path}");

        return new PoCatalogFile(result.Catalog, result.Diagnostics.ToArray(), encoding);
    }
}

internal static class EntryInfo
{
    public static bool IsFuzzy(IPOEntry entry) =>
        entry.Comments?.OfType<POFlagsComment>().Any(comment => comment.Flags?.Contains("fuzzy") == true) == true;

    public static bool IsTranslated(IPOEntry entry) =>
        !IsFuzzy(entry) && entry.All(value => !string.IsNullOrEmpty(value));

    public static void SetFuzzy(IPOEntry entry, bool isFuzzy)
    {
        entry.Comments ??= [];

        var flagsComment = entry.Comments.OfType<POFlagsComment>().FirstOrDefault();
        if (flagsComment is null)
        {
            if (!isFuzzy)
                return;

            flagsComment = new POFlagsComment
            {
                Flags = new SortedSet<string>(StringComparer.Ordinal)
            };
            entry.Comments.Add(flagsComment);
        }

        flagsComment.Flags ??= new SortedSet<string>(StringComparer.Ordinal);
        if (isFuzzy)
        {
            flagsComment.Flags.Add("fuzzy");
        }
        else
        {
            flagsComment.Flags.Remove("fuzzy");
            if (flagsComment.Flags.Count == 0)
                entry.Comments.Remove(flagsComment);
        }
    }
}
