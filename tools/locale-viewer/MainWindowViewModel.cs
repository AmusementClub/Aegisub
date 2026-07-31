using System.Collections.ObjectModel;
using System.Diagnostics;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Karambolo.PO;

namespace Aegisub.LocaleViewer;

public sealed partial class MainWindowViewModel : ObservableObject
{
    private readonly LocalizationStatusService service;
    private readonly TimeSpan entryFilterDelay;
    private readonly SynchronizationContext? synchronizationContext;
    private CancellationTokenSource? refreshCancellation;
    private CancellationTokenSource? entryLoadCancellation;
    private CancellationTokenSource? comparisonLoadCancellation;
    private CancellationTokenSource? filterCancellation;
    private IReadOnlyList<TranslationEntryViewModel> allTranslationEntries = [];

    public Task EntriesLoaded { get; private set; } = Task.CompletedTask;
    public Task ComparisonLoaded { get; private set; } = Task.CompletedTask;
    public Task FilterApplied { get; private set; } = Task.CompletedTask;

    [ObservableProperty]
    private bool isLoading;

    [ObservableProperty]
    private string repositoryRoot;

    [RelayCommand]
    private async Task BrowseRepositoryAsync()
    {
        var dialog = new Avalonia.Platform.Storage.FolderPickerOpenOptions
        {
            Title = "Select Aegisub Repository Root",
            AllowMultiple = false
        };

        var window = Avalonia.Application.Current?.ApplicationLifetime is Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime desktop
            ? desktop.MainWindow
            : null;

        if (window is null)
            return;

        var result = await window.StorageProvider.OpenFolderPickerAsync(dialog);
        if (result.Count > 0)
        {
            var newRoot = result[0].Path.LocalPath;
            if (Directory.Exists(Path.Combine(newRoot, "po")))
            {
                RepositoryRoot = newRoot;
                service.UpdateRepositoryRoot(newRoot);
                await RefreshCommand.ExecuteAsync(null);
            }
            else
            {
                StatusText = "Selected directory does not contain a 'po' folder.";
            }
        }
    }

    [ObservableProperty]
    private string statusText = "Select a repository to begin.";

    [ObservableProperty]
    private int languageCount;

    [ObservableProperty]
    private string averageCoverageText = "0.0%";

    [ObservableProperty]
    private int totalFuzzy;

    [ObservableProperty]
    private int totalMissing;

    [ObservableProperty]
    private IReadOnlyList<LanguageStatusViewModel> languages = [];

    [ObservableProperty]
    private LanguageStatusViewModel? selectedLanguage;

    [ObservableProperty]
    private string selectedLanguageText = "No language selected";

    [ObservableProperty]
    private string entryFilter = "";

    [ObservableProperty]
    private TranslationState filterState = TranslationState.All;

    [ObservableProperty]
    private IReadOnlyList<TranslationEntryViewModel> translationEntries = [];

    [ObservableProperty]
    private bool isEntriesEmpty;

    [ObservableProperty]
    private TranslationEntryViewModel selectedEntry = TranslationEntryViewModel.Empty;

    [ObservableProperty]
    private bool isEntryEditorEnabled;

    [ObservableProperty]
    private bool hasUnsavedChanges;

    [ObservableProperty]
    private string editStatusText = "Select a language to inspect translations.";

    [ObservableProperty]
    private IReadOnlyList<TranslationComparisonViewModel> comparisonEntries = [];

    [ObservableProperty]
    private HashSet<string> comparisonLanguageFilter = [];

    [RelayCommand]
    private async Task FilterComparisonLanguagesAsync()
    {
        if (Languages.Count == 0)
            return;

        var window = Avalonia.Application.Current?.ApplicationLifetime is Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime desktop
            ? desktop.MainWindow
            : null;

        if (window is null)
            return;

        var options = Languages
            .Where(lang => !string.Equals(lang.Language, SelectedLanguage?.Language, StringComparison.OrdinalIgnoreCase))
            .Select(lang => new Avalonia.Controls.CheckBox
            {
                Content = $"{lang.Language} ({lang.CoverageText})",
                IsChecked = ComparisonLanguageFilter.Count == 0 || ComparisonLanguageFilter.Contains(lang.Language),
                Tag = lang.Language
            })
            .ToList();

        if (options.Count == 0)
            return;

        var selectAllButton = new Avalonia.Controls.Button { Content = "Select All", MinWidth = 85, Margin = new Avalonia.Thickness(0, 0, 4, 0) };
        var selectNoneButton = new Avalonia.Controls.Button { Content = "Select None", MinWidth = 85 };
        selectAllButton.Click += (_, _) => { foreach (var cb in options) cb.IsChecked = true; };
        selectNoneButton.Click += (_, _) => { foreach (var cb in options) cb.IsChecked = false; };

        var topButtonPanel = new Avalonia.Controls.StackPanel
        {
            Orientation = Avalonia.Layout.Orientation.Horizontal,
            Spacing = 4,
            Margin = new Avalonia.Thickness(10, 10, 10, 6)
        };
        topButtonPanel.Children.Add(selectAllButton);
        topButtonPanel.Children.Add(selectNoneButton);

        var panel = new Avalonia.Controls.StackPanel { Spacing = 4, Margin = new Avalonia.Thickness(10, 0, 10, 10) };
        foreach (var cb in options)
            panel.Children.Add(cb);

        var scrollViewer = new Avalonia.Controls.ScrollViewer
        {
            Content = panel,
            MaxHeight = 350
        };

        var contentPanel = new Avalonia.Controls.StackPanel();
        contentPanel.Children.Add(topButtonPanel);
        contentPanel.Children.Add(scrollViewer);

        var okButton = new Avalonia.Controls.Button { Content = "OK", Width = 80 };
        var cancelButton = new Avalonia.Controls.Button { Content = "Cancel", Width = 80 };

        var buttonPanel = new Avalonia.Controls.StackPanel
        {
            Orientation = Avalonia.Layout.Orientation.Horizontal,
            HorizontalAlignment = Avalonia.Layout.HorizontalAlignment.Right,
            Spacing = 8,
            Margin = new Avalonia.Thickness(10)
        };
        buttonPanel.Children.Add(cancelButton);
        buttonPanel.Children.Add(okButton);

        var mainPanel = new Avalonia.Controls.DockPanel();
        Avalonia.Controls.DockPanel.SetDock(buttonPanel, Avalonia.Controls.Dock.Bottom);
        mainPanel.Children.Add(buttonPanel);
        mainPanel.Children.Add(contentPanel);

        var dialog = new Avalonia.Controls.Window
        {
            Title = "Filter Comparison Languages",
            Width = 280,
            Height = 480,
            Content = mainPanel,
            WindowStartupLocation = Avalonia.Controls.WindowStartupLocation.CenterOwner,
            CanResize = false
        };

        var dialogResult = false;
        okButton.Click += (_, _) => { dialogResult = true; dialog.Close(); };
        cancelButton.Click += (_, _) => dialog.Close();

        await dialog.ShowDialog(window);

        if (dialogResult)
        {
            var selected = options.Where(cb => cb.IsChecked == true).Select(cb => (string)cb.Tag!).ToHashSet(StringComparer.OrdinalIgnoreCase);
            ComparisonLanguageFilter = selected.Count == options.Count ? [] : selected;

            if (!ReferenceEquals(SelectedEntry, TranslationEntryViewModel.Empty))
                ComparisonLoaded = LoadComparisonAsync(SelectedEntry);
        }
    }

    [ObservableProperty]
    private string comparisonStatusText = "Select an entry to compare languages.";

    public MainWindowViewModel(LocalizationStatusService service, bool autoRefresh = true, TimeSpan? entryFilterDelay = null)
    {
        this.service = service;
        this.entryFilterDelay = entryFilterDelay ?? TimeSpan.FromMilliseconds(200);
        synchronizationContext = SynchronizationContext.Current;
        repositoryRoot = service.RepositoryRoot;
        if (autoRefresh)
            _ = RefreshCommand.ExecuteAsync(null);
    }

    [RelayCommand(AllowConcurrentExecutions = true)]
    private async Task RefreshAsync()
    {
        var previousCancellation = refreshCancellation;
        previousCancellation?.Cancel();

        var currentCancellation = new CancellationTokenSource();
        refreshCancellation = currentCancellation;
        previousCancellation?.Dispose();

        var cancellationToken = currentCancellation.Token;
        var stopwatch = Stopwatch.StartNew();
        var selectedLanguageId = SelectedLanguage?.Language;

        try
        {
            IsLoading = true;
            StatusText = "Loading localization status...";
            var snapshot = await service.LoadAsync(cancellationToken);
            var rows = snapshot.Languages
                .Select(language => new LanguageStatusViewModel(language))
                .ToArray();

            cancellationToken.ThrowIfCancellationRequested();

            Languages = rows;
            LanguageCount = rows.Length;
            TotalFuzzy = snapshot.TotalFuzzy;
            TotalMissing = snapshot.TotalMissing;
            AverageCoverageText = rows.Length == 0
                ? "n/a"
                : $"{snapshot.AverageCoverage:0.0}%";

            SelectedLanguage = rows.FirstOrDefault(language => language.Language == selectedLanguageId)
                ?? rows.FirstOrDefault(language => language.Language == "zh_CN")
                ?? rows.FirstOrDefault();

            StatusText = $"Loaded {LanguageCount} languages from po/LINGUAS in {stopwatch.ElapsedMilliseconds} ms";
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            if (ReferenceEquals(refreshCancellation, currentCancellation))
                StatusText = "Refresh cancelled";
        }
        catch (Exception ex)
        {
            StatusText = ex.GetBaseException().Message;
        }
        finally
        {
            IsLoading = false;
            if (ReferenceEquals(refreshCancellation, currentCancellation))
                refreshCancellation = null;

            currentCancellation.Dispose();
        }
    }

    [RelayCommand(CanExecute = nameof(CanSave))]
    private async Task SaveAsync()
    {
        if (SelectedLanguage is null)
            return;

        var language = SelectedLanguage.Language;
        var updates = allTranslationEntries
            .Select(entry => entry.ToUpdate())
            .ToArray();

        try
        {
            EditStatusText = $"Saving {language}.po...";
            await service.SaveTranslationFileAsync(language, updates);

            foreach (var entry in allTranslationEntries)
                entry.AcceptChanges();

            HasUnsavedChanges = false;
            EditStatusText = $"Saved {language}.po";

            await RefreshCommand.ExecuteAsync(null);
        }
        catch (Exception ex)
        {
            EditStatusText = ex.GetBaseException().Message;
        }
    }

    private bool CanSave() =>
        SelectedLanguage is not null && HasUnsavedChanges && allTranslationEntries.Count > 0;

    partial void OnSelectedLanguageChanged(LanguageStatusViewModel? value)
    {
        SelectedLanguageText = value is null ? "No language selected" : $"{value.Language} entries";
        SaveCommand.NotifyCanExecuteChanged();
        EntriesLoaded = LoadSelectedLanguageAsync(value);
    }

    partial void OnEntryFilterChanged(string value) =>
        FilterApplied = ApplyEntryFilterDebouncedAsync(value);

    partial void OnFilterStateChanged(TranslationState value) =>
        ApplyEntryFilter(EntryFilter);

    partial void OnHasUnsavedChangesChanged(bool value) => SaveCommand.NotifyCanExecuteChanged();

    partial void OnSelectedEntryChanged(TranslationEntryViewModel value) =>
        ComparisonLoaded = LoadComparisonAsync(value);

    private async Task LoadSelectedLanguageAsync(LanguageStatusViewModel? language)
    {
        var previousCancellation = entryLoadCancellation;
        previousCancellation?.Cancel();

        var currentCancellation = new CancellationTokenSource();
        entryLoadCancellation = currentCancellation;
        previousCancellation?.Dispose();

        var cancellationToken = currentCancellation.Token;

        if (language is null)
        {
            ClearEntries();
            return;
        }

        try
        {
            EditStatusText = $"Loading {language.Language}.po...";
            var snapshot = await service.LoadTranslationFileAsync(language.Language, cancellationToken);
            var entries = snapshot.Entries
                .Select(unit => new TranslationEntryViewModel(unit, MarkTranslationsDirty))
                .ToArray();

            cancellationToken.ThrowIfCancellationRequested();

            allTranslationEntries = entries;
            HasUnsavedChanges = false;
            ApplyEntryFilter(EntryFilter);
            EditStatusText = $"Loaded {entries.Length} source strings from {language.Language}.po";
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        catch (Exception ex)
        {
            ClearEntries();
            EditStatusText = ex.GetBaseException().Message;
        }
        finally
        {
            if (ReferenceEquals(entryLoadCancellation, currentCancellation))
                entryLoadCancellation = null;

            currentCancellation.Dispose();
        }
    }

    private async Task ApplyEntryFilterDebouncedAsync(string filterText)
    {
        var previousCancellation = filterCancellation;
        previousCancellation?.Cancel();

        var currentCancellation = new CancellationTokenSource();
        filterCancellation = currentCancellation;
        previousCancellation?.Dispose();

        var cancellationToken = currentCancellation.Token;

        try
        {
            if (entryFilterDelay > TimeSpan.Zero)
                await Task.Delay(entryFilterDelay, cancellationToken);

            cancellationToken.ThrowIfCancellationRequested();
            await RunOnCapturedContextAsync(() => ApplyEntryFilter(filterText));
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        finally
        {
            if (ReferenceEquals(filterCancellation, currentCancellation))
                filterCancellation = null;

            currentCancellation.Dispose();
        }
    }

    private void ApplyEntryFilter(string filterText)
    {
        var previousKey = SelectedEntry.Key;
        var filter = TranslationEntryViewModel.NormalizeFilter(filterText);

        var entries = allTranslationEntries.AsEnumerable();

        if (!string.IsNullOrEmpty(filter))
            entries = entries.Where(entry => entry.Matches(filter));

        entries = FilterState switch
        {
            TranslationState.Translated => entries.Where(entry => entry.HasTranslation && !entry.IsFuzzy),
            TranslationState.Fuzzy => entries.Where(entry => entry.IsFuzzy),
            TranslationState.Missing => entries.Where(entry => !entry.HasTranslation && !entry.IsFuzzy),
            _ => entries
        };

        var filteredEntries = entries.ToArray();
        TranslationEntries = filteredEntries;
        IsEntriesEmpty = filteredEntries.Length == 0 && SelectedLanguage is not null;
        SelectedEntry = filteredEntries.FirstOrDefault(entry => entry.Key == previousKey)
            ?? filteredEntries.FirstOrDefault()
            ?? TranslationEntryViewModel.Empty;
        IsEntryEditorEnabled = !ReferenceEquals(SelectedEntry, TranslationEntryViewModel.Empty);
        SelectedLanguageText = SelectedLanguage is null
            ? "No language selected"
            : $"{SelectedLanguage.Language} entries ({filteredEntries.Length}/{allTranslationEntries.Count})";
    }

    private Task RunOnCapturedContextAsync(Action action)
    {
        if (synchronizationContext is null || ReferenceEquals(SynchronizationContext.Current, synchronizationContext))
        {
            action();
            return Task.CompletedTask;
        }

        var completion = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        synchronizationContext.Post(_ =>
        {
            try
            {
                action();
                completion.SetResult();
            }
            catch (Exception ex)
            {
                completion.SetException(ex);
            }
        }, null);

        return completion.Task;
    }

    private void ClearEntries()
    {
        allTranslationEntries = [];
        TranslationEntries = [];
        SelectedEntry = TranslationEntryViewModel.Empty;
        IsEntryEditorEnabled = false;
        HasUnsavedChanges = false;
        EditStatusText = "Select a language to inspect translations.";
        SelectedLanguageText = "No language selected";
        ComparisonEntries = [];
        ComparisonStatusText = "Select an entry to compare languages.";
    }

    private void MarkTranslationsDirty()
    {
        HasUnsavedChanges = true;
        if (SelectedLanguage is not null)
            EditStatusText = $"Unsaved changes in {SelectedLanguage.Language}.po";
    }

    private async Task LoadComparisonAsync(TranslationEntryViewModel entry)
    {
        var previousCancellation = comparisonLoadCancellation;
        previousCancellation?.Cancel();

        var currentCancellation = new CancellationTokenSource();
        comparisonLoadCancellation = currentCancellation;
        previousCancellation?.Dispose();

        var cancellationToken = currentCancellation.Token;

        if (SelectedLanguage is null || entry is null || ReferenceEquals(entry, TranslationEntryViewModel.Empty))
        {
            ComparisonEntries = [];
            ComparisonStatusText = "Select an entry to compare languages.";
            return;
        }

        var selectedLanguage = SelectedLanguage.Language;
        var key = entry.Key;

        try
        {
            ComparisonStatusText = "Loading language comparison...";
            var comparisons = await service.LoadComparisonAsync(key, selectedLanguage, cancellationToken);

            if (comparisons is null)
            {
                ComparisonEntries = [];
                ComparisonStatusText = "No comparison data available.";
                return;
            }

            var rows = comparisons
                .Where(comparison => ComparisonLanguageFilter.Count == 0 || ComparisonLanguageFilter.Contains(comparison.Language))
                .Select(comparison => new TranslationComparisonViewModel(comparison))
                .ToArray();

            cancellationToken.ThrowIfCancellationRequested();

            ComparisonEntries = rows;
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        catch (Exception ex)
        {
            ComparisonEntries = [];
            ComparisonStatusText = ex.GetBaseException().Message;
        }
        finally
        {
            if (ReferenceEquals(comparisonLoadCancellation, currentCancellation))
                comparisonLoadCancellation = null;

            currentCancellation.Dispose();
        }
    }
}

public sealed class LanguageStatusViewModel(LanguageStatus status)
{
    public string Language { get; } = status.Language;
    public int Translated { get; } = status.Translated;
    public int Fuzzy { get; } = status.Fuzzy;
    public int Missing { get; } = status.Missing;
    public double Coverage { get; } = status.Coverage;
    public string CoverageText { get; } = $"{status.Coverage:0.0}%";
    public string WarningText { get; } = status.Warnings.Count == 0 ? "" : string.Join("; ", status.Warnings);
}

public sealed partial class TranslationEntryViewModel : ObservableObject
{
    private readonly Action markDirty;
    private string searchIndex = "";

    public static TranslationEntryViewModel Empty { get; } = new(
        new TranslationUnit(0, new POKey("", "", ""), "", null, null, [], false, "", ""),
        () => { });

    public TranslationEntryViewModel(TranslationUnit unit, Action markDirty)
    {
        this.markDirty = markDirty;

        Number = unit.Number;
        Key = unit.Key;
        SourceText = unit.SourceText;
        PluralSourceText = unit.PluralSourceText ?? "";
        ContextText = unit.ContextText ?? "";
        ReferenceText = unit.ReferenceText;
        CommentText = unit.CommentText;
        isFuzzy = unit.IsFuzzy;
        Forms = new ObservableCollection<TranslationFormViewModel>(
            unit.Translations.Select((translation, index) => new TranslationFormViewModel(index, translation, OnFormChanged)));
        searchIndex = BuildSearchIndex();
    }

    public int Number { get; }
    public POKey Key { get; }
    public string SourceText { get; }
    public string PluralSourceText { get; }
    public string ContextText { get; }
    public string ReferenceText { get; }
    public string CommentText { get; }
    public ObservableCollection<TranslationFormViewModel> Forms { get; }
    public string SourcePreview => Shorten(SourceText, 80);
    public string TranslationPreview => Shorten(string.Join(" / ", Forms.Select(form => form.Translation)), 80);
    public bool HasPlural => !string.IsNullOrEmpty(PluralSourceText);
    public bool HasContext => !string.IsNullOrEmpty(ContextText);
    public bool HasReference => !string.IsNullOrEmpty(ReferenceText);
    public bool HasComment => !string.IsNullOrEmpty(CommentText);
    public bool HasTranslation => Forms.Count > 0 && Forms.All(form => !string.IsNullOrEmpty(form.Translation));
    public string StateText => IsFuzzy ? "Fuzzy" : HasTranslation ? "Translated" : "Missing";

    [ObservableProperty]
    private bool isFuzzy;

    public static string NormalizeFilter(string value) => value.Trim().ToUpperInvariant();

    public bool Matches(string normalizedFilter) => searchIndex.Contains(normalizedFilter, StringComparison.Ordinal);

    public TranslationUpdate ToUpdate() =>
        new(Key, Forms.Select(form => form.Translation).ToArray(), IsFuzzy);

    public void AcceptChanges()
    {
        foreach (var form in Forms)
            form.AcceptChanges();
    }

    partial void OnIsFuzzyChanged(bool value)
    {
        OnPropertyChanged(nameof(StateText));
        markDirty();
    }

    private void OnFormChanged()
    {
        searchIndex = BuildSearchIndex();
        OnPropertyChanged(nameof(TranslationPreview));
        OnPropertyChanged(nameof(HasTranslation));
        OnPropertyChanged(nameof(StateText));
        markDirty();
    }

    private string BuildSearchIndex()
    {
        return string.Join('\u001f',
            SourceText,
            PluralSourceText,
            ContextText,
            ReferenceText,
            string.Join('\u001f', Forms.Select(form => form.Translation)))
            .ToUpperInvariant();
    }

    private static string Shorten(string value, int maxLength)
    {
        var singleLine = value.Replace("\r\n", "\\n", StringComparison.Ordinal)
            .Replace("\n", "\\n", StringComparison.Ordinal)
            .Replace("\r", "\\n", StringComparison.Ordinal);

        return singleLine.Length <= maxLength ? singleLine : singleLine[..(maxLength - 1)] + "...";
    }
}

public sealed partial class TranslationFormViewModel : ObservableObject
{
    private readonly Action changed;
    private string savedTranslation;

    public TranslationFormViewModel(int index, string translation, Action changed)
    {
        this.changed = changed;
        Index = index;
        this.translation = translation;
        savedTranslation = translation;
    }

    public int Index { get; }
    public string Label => Index == 0 ? "Translation" : $"Plural {Index}";

    [ObservableProperty]
    private string translation;

    public void AcceptChanges() => savedTranslation = Translation;

    partial void OnTranslationChanged(string value)
    {
        if (!string.Equals(value, savedTranslation, StringComparison.Ordinal))
            changed();
    }
}

public sealed class TranslationComparisonViewModel(TranslationComparison comparison)
{
    public string Language { get; } = comparison.Language;
    public bool IsFuzzy { get; } = comparison.IsFuzzy;
    public bool IsMissing { get; } = comparison.IsMissing;
    public string StateText => IsMissing ? "Missing" : IsFuzzy ? "Fuzzy" : "Translated";
    public string TranslationText { get; } = comparison.IsMissing
        ? ""
        : string.Join(Environment.NewLine + Environment.NewLine, comparison.Translations.Select((text, index) =>
            comparison.Translations.Count == 1 ? text : $"[{index}] {text}"));
}

public enum TranslationState
{
    All,
    Translated,
    Fuzzy,
    Missing
}
