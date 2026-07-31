using Aegisub.LocaleViewer;
using Avalonia.Controls;
using Avalonia.Headless.XUnit;
using Avalonia.Threading;
using Avalonia.VisualTree;
using Xunit;

namespace Aegisub.LocaleViewer.Tests;

public sealed class LocaleViewerTests
{
    [Fact]
    public void ServiceLoadsLanguagesFromLinguas()
    {
        var snapshot = CreateService().Load();

        var linguas = File.ReadAllText(Path.Combine(RepositoryRoot, "po", "LINGUAS"))
            .Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries)
            .Where(language => !language.StartsWith('#'))
            .ToArray();

        Assert.Equal(linguas.Length, snapshot.Languages.Count);
        Assert.Contains(snapshot.Languages, language => language.Language == "zh_CN" && language.Translated > 0);
        Assert.True(snapshot.TotalMissing >= 0);
    }

    [Fact]
    public async Task ViewModelRefreshCommandPopulatesSummary()
    {
        var viewModel = new MainWindowViewModel(CreateService(), autoRefresh: false);

        await viewModel.RefreshCommand.ExecuteAsync(null);

        Assert.NotEmpty(viewModel.Languages);
        Assert.NotNull(viewModel.SelectedLanguage);
        Assert.True(viewModel.LanguageCount > 0);
        Assert.Contains("po/LINGUAS", viewModel.StatusText);
        Assert.NotEqual("0.0%", viewModel.AverageCoverageText);
    }

    [Fact]
    public async Task ServiceLoadsAndSavesTranslationText()
    {
        using var tempRepository = await TempLocaleRepository.CreateAsync(TestContext.Current.CancellationToken);

        var service = new LocalizationStatusService(tempRepository.Root);
        var snapshot = await service.LoadTranslationFileAsync("aa", TestContext.Current.CancellationToken);
        var entry = snapshot.Entries.Single(entry => entry.SourceText == "Hello");

        await service.SaveTranslationFileAsync(
            "aa",
            [new TranslationUpdate(entry.Key, ["Edited"], IsFuzzy: false)],
            TestContext.Current.CancellationToken);

        var saved = await service.LoadTranslationFileAsync("aa", TestContext.Current.CancellationToken);
        var savedEntry = saved.Entries.Single(entry => entry.SourceText == "Hello");

        Assert.Equal("Edited", Assert.Single(savedEntry.Translations));
        Assert.False(savedEntry.IsFuzzy);
    }

    [Fact]
    public async Task ViewModelPreviewsEditsSavesAndComparesLanguages()
    {
        using var tempRepository = await TempLocaleRepository.CreateAsync(TestContext.Current.CancellationToken);
        var viewModel = new MainWindowViewModel(
            new LocalizationStatusService(tempRepository.Root),
            autoRefresh: false,
            entryFilterDelay: TimeSpan.Zero);

        await viewModel.RefreshCommand.ExecuteAsync(null);
        await viewModel.EntriesLoaded;
        await viewModel.ComparisonLoaded;

        Assert.Equal("aa", viewModel.SelectedLanguage?.Language);
        Assert.NotEmpty(viewModel.TranslationEntries);
        Assert.Contains(viewModel.TranslationEntries, entry => entry.SourceText == "Hello" && entry.TranslationPreview == "Old");

        viewModel.EntryFilter = "Second";
        await viewModel.FilterApplied;
        Assert.Single(viewModel.TranslationEntries);
        Assert.Equal("Second", viewModel.SelectedEntry.SourceText);

        viewModel.EntryFilter = "";
        await viewModel.FilterApplied;
        var editable = viewModel.TranslationEntries.Single(entry => entry.SourceText == "Hello");
        viewModel.SelectedEntry = editable;
        await viewModel.ComparisonLoaded;

        var form = Assert.Single(editable.Forms);
        form.Translation = "Edited in UI";

        Assert.True(viewModel.HasUnsavedChanges);
        Assert.Equal("Edited in UI", editable.TranslationPreview);
        Assert.Contains(viewModel.ComparisonEntries, comparison =>
            comparison.Language == "bb" && comparison.TranslationText == "Bonjour");

        Assert.True(viewModel.SaveCommand.CanExecute(null));
        await viewModel.SaveCommand.ExecuteAsync(null);
        await viewModel.EntriesLoaded;

        var service = new LocalizationStatusService(tempRepository.Root);
        var saved = await service.LoadTranslationFileAsync("aa", TestContext.Current.CancellationToken);

        Assert.Equal("Edited in UI", Assert.Single(saved.Entries.Single(entry => entry.SourceText == "Hello").Translations));
        Assert.False(viewModel.HasUnsavedChanges);
    }

    [Fact]
    public async Task ServiceComparesSelectedEntryAcrossLanguages()
    {
        using var tempRepository = await TempLocaleRepository.CreateAsync(TestContext.Current.CancellationToken);
        var service = new LocalizationStatusService(tempRepository.Root);
        var snapshot = await service.LoadTranslationFileAsync("aa", TestContext.Current.CancellationToken);
        var entry = snapshot.Entries.Single(entry => entry.SourceText == "Hello");

        var comparisons = await service.LoadComparisonAsync(entry.Key, "aa", TestContext.Current.CancellationToken);
        var comparison = Assert.Single(comparisons);

        Assert.Equal("bb", comparison.Language);
        Assert.Equal("Bonjour", Assert.Single(comparison.Translations));
        Assert.False(comparison.IsMissing);
    }

    [Fact]
    public async Task ServiceReportsMissingComparisonLanguageEntry()
    {
        using var tempRepository = await TempLocaleRepository.CreateAsync(TestContext.Current.CancellationToken);
        var service = new LocalizationStatusService(tempRepository.Root);
        var snapshot = await service.LoadTranslationFileAsync("aa", TestContext.Current.CancellationToken);
        var entry = snapshot.Entries.Single(entry => entry.SourceText == "Only aa");

        var comparisons = await service.LoadComparisonAsync(entry.Key, "aa", TestContext.Current.CancellationToken);
        var comparison = Assert.Single(comparisons);

        Assert.Equal("bb", comparison.Language);
        Assert.True(comparison.IsMissing);
        Assert.Empty(comparison.Translations);
    }

    [AvaloniaFact]
    public async Task HeadlessPreviewEditAndComparisonControlsStayBound()
    {
        using var tempRepository = await TempLocaleRepository.CreateAsync(TestContext.Current.CancellationToken);
        var viewModel = new MainWindowViewModel(
            new LocalizationStatusService(tempRepository.Root),
            autoRefresh: false,
            entryFilterDelay: TimeSpan.Zero);

        await viewModel.RefreshCommand.ExecuteAsync(null);
        await viewModel.EntriesLoaded;
        await viewModel.ComparisonLoaded;

        var window = new MainWindow
        {
            DataContext = viewModel
        };

        window.Show();
        Dispatcher.UIThread.RunJobs();

        var languageRows = window.FindControl<ListBox>("LanguageRows");
        var filterBox = window.FindControl<TextBox>("EntryFilterBox");
        var sourceText = window.FindControl<TextBlock>("SourceText");
        var comparisonRows = window.FindControl<ItemsControl>("ComparisonRows");
        var entryList = window.FindControl<ListBox>("EntryList");
        var saveButton = window.FindControl<Button>("SaveButton");

        Assert.NotNull(languageRows);
        Assert.NotNull(filterBox);
        Assert.NotNull(sourceText);
        Assert.NotNull(comparisonRows);
        Assert.NotNull(entryList);
        Assert.NotNull(saveButton);

        languageRows!.SelectedItem = viewModel.Languages.Single(language => language.Language == "aa");
        await viewModel.EntriesLoaded;
        Dispatcher.UIThread.RunJobs();

        filterBox!.Text = "Hello";
        await viewModel.FilterApplied;
        Dispatcher.UIThread.RunJobs();

        Assert.Single(viewModel.TranslationEntries);
        entryList!.SelectedItem = viewModel.TranslationEntries[0];
        await viewModel.ComparisonLoaded;
        Dispatcher.UIThread.RunJobs();

        var translationText = window.GetVisualDescendants().OfType<TextBox>().FirstOrDefault(textBox => textBox.Name == "TranslationText");
        Assert.NotNull(translationText);

        translationText!.Text = "Headless edit";
        Dispatcher.UIThread.RunJobs();

        Assert.Equal("Hello", sourceText!.Text);
        Assert.Same(viewModel.ComparisonEntries, comparisonRows!.ItemsSource);
        Assert.True(viewModel.HasUnsavedChanges);
        Assert.True(saveButton!.Command!.CanExecute(null));
        Assert.Contains(viewModel.ComparisonEntries, comparison => comparison.Language == "bb");
        Assert.Equal("Headless edit", viewModel.SelectedEntry.TranslationPreview);

        await viewModel.SaveCommand.ExecuteAsync(null);
        await viewModel.EntriesLoaded;

        var service = new LocalizationStatusService(tempRepository.Root);
        var saved = await service.LoadTranslationFileAsync("aa", TestContext.Current.CancellationToken);
        Assert.Equal("Headless edit", Assert.Single(saved.Entries.Single(entry => entry.SourceText == "Hello").Translations));

        window.Close();
    }

    [AvaloniaFact]
    public async Task MainWindowBindsHeadlessControls()
    {
        var viewModel = new MainWindowViewModel(CreateService(), autoRefresh: false);
        await viewModel.RefreshCommand.ExecuteAsync(null);

        var window = new MainWindow
        {
            DataContext = viewModel
        };

        window.Show();
        Dispatcher.UIThread.RunJobs();

        var refreshButton = window.FindControl<Button>("RefreshButton");
        var saveButton = window.FindControl<Button>("SaveButton");
        var statusText = window.FindControl<TextBlock>("StatusText");
        var languageRows = window.FindControl<ListBox>("LanguageRows");
        var entryList = window.FindControl<ListBox>("EntryList");
        var translationEditor = window.FindControl<Border>("TranslationEditor");

        Assert.NotNull(refreshButton);
        Assert.NotNull(saveButton);
        Assert.NotNull(statusText);
        Assert.NotNull(languageRows);
        Assert.NotNull(entryList);
        Assert.NotNull(translationEditor);
        Assert.Same(viewModel.Languages, languageRows!.ItemsSource);
        Assert.Same(viewModel.TranslationEntries, entryList!.ItemsSource);
        Assert.Equal(viewModel.StatusText, statusText!.Text);

        window.Close();
    }

    private static LocalizationStatusService CreateService() => new(RepositoryRoot);

    private static string RepositoryRoot { get; } = FindRepositoryRoot();

    private static string FindRepositoryRoot()
    {
        var current = new DirectoryInfo(AppContext.BaseDirectory);
        while (current is not null)
        {
            if (File.Exists(Path.Combine(current.FullName, "CMakeLists.txt"))
                && Directory.Exists(Path.Combine(current.FullName, "po")))
                return current.FullName;

            current = current.Parent;
        }

        throw new InvalidOperationException("Could not locate repository root for locale-viewer tests.");
    }

    private sealed class TempLocaleRepository : IDisposable
    {
        private TempLocaleRepository(string root) => Root = root;

        public string Root { get; }

        public static async Task<TempLocaleRepository> CreateAsync(CancellationToken cancellationToken)
        {
            var root = Path.Combine(Path.GetTempPath(), $"locale-viewer-{Guid.NewGuid():N}");
            var poDir = Path.Combine(root, "po");
            Directory.CreateDirectory(poDir);
            await File.WriteAllTextAsync(Path.Combine(root, "CMakeLists.txt"), "# temp\n", cancellationToken);
            await File.WriteAllTextAsync(Path.Combine(poDir, "LINGUAS"), "aa bb\n", cancellationToken);
            await File.WriteAllTextAsync(
                Path.Combine(poDir, "aa.po"),
                """
                msgid ""
                msgstr ""
                "Language: aa\n"
                "Content-Type: text/plain; charset=UTF-8\n"

                #: src/example.cpp:10
                #, fuzzy
                msgid "Hello"
                msgstr "Old"

                #: src/example.cpp:11
                msgid "Second"
                msgstr "Second aa"

                #: src/example.cpp:12
                msgid "Only aa"
                msgstr "Only aa translation"

                """,
                cancellationToken);
            await File.WriteAllTextAsync(
                Path.Combine(poDir, "bb.po"),
                """
                msgid ""
                msgstr ""
                "Language: bb\n"
                "Content-Type: text/plain; charset=UTF-8\n"

                #: src/example.cpp:10
                msgid "Hello"
                msgstr "Bonjour"

                #: src/example.cpp:11
                msgid "Second"
                msgstr "Second bb"

                """,
                cancellationToken);

            return new TempLocaleRepository(root);
        }

        public void Dispose()
        {
            if (Directory.Exists(Root))
                Directory.Delete(Root, recursive: true);
        }
    }
}
