using System.Globalization;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.Windows.Storage.Pickers;

namespace SpaceLedger_App;

public sealed partial class MainPage : Page
{
    private SpaceLedgerClient _client = new();
    private CancellationTokenSource? _scanCancellation;
    private DiffReport? _currentReport;
    private bool _busy;

    public MainPage()
    {
        InitializeComponent();
        DatabasePathBox.Text = _client.DatabasePath;
        DatabaseLocation.Text = $"History database: {_client.DatabasePath}. No background monitoring is enabled.";
    }

    private async void Browse_Click(object sender, RoutedEventArgs e)
    {
        if (App.MainWindow is null) return;
        try
        {
            var picker = new FolderPicker(App.MainWindow.AppWindow.Id)
            {
                CommitButtonText = "Use this folder",
                SuggestedStartLocation = PickerLocationId.DocumentsLibrary,
            };
            var result = await picker.PickSingleFolderAsync();
            if (result is null) return;
            FolderPathBox.Text = result.Path;
            await LoadHistoryAsync();
        }
        catch (Exception error) { ShowStatus(error.Message, InfoBarSeverity.Error); }
    }

    private async void LoadHistory_Click(object sender, RoutedEventArgs e)
    {
        try { await LoadHistoryAsync(); }
        catch (Exception error) { ShowStatus(error.Message, InfoBarSeverity.Error); }
    }

    private async Task LoadHistoryAsync()
    {
        UseDatabasePath();
        var root = SelectedRoot();
        var scans = (await _client.GetScansAsync())
            .Where(s => SamePath(s.Root, root))
            .OrderBy(s => s.Id)
            .ToList();
        HistoryList.ItemsSource = scans.AsEnumerable().Reverse()
            .Select(s => new HistoryRow($"Scan {s.Id} · {LocalTime(s.Finished)}",
                s.CoverageIssues ? "Coverage gaps recorded" : "No recorded coverage gaps"))
            .ToList();
        HistoryHint.Text = scans.Count switch
        {
            0 => "No scans yet. Scan now to establish a baseline.",
            1 => "One scan saved. Scan again later to see what changed.",
            _ => $"{scans.Count} scans saved for this folder."
        };
        var options = scans.Select(s => new SnapshotOption(s.Id, $"{LocalTime(s.Finished)}  ·  #{s.Id}")).ToList();
        BeforePicker.ItemsSource = options;
        AfterPicker.ItemsSource = options;
        BeforePicker.SelectedIndex = scans.Count >= 2 ? scans.Count - 2 : -1;
        AfterPicker.SelectedIndex = scans.Count >= 2 ? scans.Count - 1 : -1;
        CompareButton.IsEnabled = scans.Count >= 2 && !_busy;
        if (scans.Count >= 2) await CompareSelectedAsync();
        else ClearComparison();
    }

    private async void Scan_Click(object sender, RoutedEventArgs e)
    {
        string root;
        try { UseDatabasePath(); root = SelectedRoot(); }
        catch (Exception error) { ShowStatus(error.Message, InfoBarSeverity.Error); return; }

        _scanCancellation = new CancellationTokenSource();
        SetBusy(true);
        ProgressText.Text = "Starting scan…";
        ShowStatus("Scanning file metadata. No file contents are read.", InfoBarSeverity.Informational);
        try
        {
            var result = await _client.ScanAsync(root, progress =>
                DispatcherQueue.TryEnqueue(() => ProgressText.Text =
                    $"{progress.Entries:N0} entries · {progress.Directories:N0} folders · {progress.Issues:N0} gaps"),
                _scanCancellation.Token);
            await LoadHistoryAsync();
            var comparisonShown = _currentReport is not null;
            ShowStatus(result.Issues == 0
                ? comparisonShown ? $"Scan {result.Id} saved. Changes since the previous scan are shown below."
                    : $"Scan {result.Id} saved as a baseline. Scan again later to see changes."
                : $"Scan {result.Id} saved with {result.Issues} coverage gaps. Treat comparisons cautiously.",
                result.Issues == 0 ? InfoBarSeverity.Success : InfoBarSeverity.Warning);
        }
        catch (OperationCanceledException) { ShowStatus("Scan cancelled; no snapshot saved.", InfoBarSeverity.Informational); }
        catch (Exception error) { ShowStatus(error.Message, InfoBarSeverity.Error); }
        finally
        {
            _scanCancellation.Dispose();
            _scanCancellation = null;
            SetBusy(false);
            ProgressText.Text = "No scan in progress";
        }
    }

    private void Cancel_Click(object sender, RoutedEventArgs e)
    {
        CancelButton.IsEnabled = false;
        ProgressText.Text = "Stopping scan…";
        _scanCancellation?.Cancel();
    }

    private async void Compare_Click(object sender, RoutedEventArgs e)
    {
        try { await CompareSelectedAsync(); }
        catch (Exception error) { ShowStatus(error.Message, InfoBarSeverity.Error); }
    }

    private async Task CompareSelectedAsync()
    {
        if (!string.Equals(_client.DatabasePath, DatabasePathBox.Text.Trim(), StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("Load history after changing the history file.");
        if (BeforePicker.SelectedItem is not SnapshotOption before || AfterPicker.SelectedItem is not SnapshotOption after)
            return;
        if (before.Id >= after.Id) throw new InvalidOperationException("Choose an earlier scan on the left and a later one on the right.");
        var report = await _client.CompareAsync(before.Id, after.Id);
        _currentReport = report;
        ResultHeadline.Text = report.AllocatedDelta switch
        {
            > 0 => $"Allocated space grew by {FormatBytes(report.AllocatedDelta, false)}",
            < 0 => $"Allocated space fell by {FormatBytes(-report.AllocatedDelta, false)}",
            _ => "No net change in allocated space"
        };
        ResultDetail.Text = $"Observed folder: {report.Root}\nFile length (logical): {FormatBytes(report.LogicalDelta)}\n" +
                            $"Whole-drive free space: {FormatBytes(report.FreeSpaceChange)} (context only)";
        SizeExplanation.Visibility = Visibility.Visible;
        CoverageBar.IsOpen = report.CoverageIssuesBefore > 0 || report.CoverageIssuesAfter > 0 || report.Uncertain > 0;
        CoverageBar.Message = $"Coverage gaps before/after: {report.CoverageIssuesBefore}/{report.CoverageIssuesAfter}; " +
                              $"unavailable file comparisons: {report.Uncertain}.";
        var folders = report.Folders
            .Where(f => !string.IsNullOrEmpty(f.Path))
            .OrderByDescending(f => Math.Abs((decimal)f.AllocatedDelta))
            .ToList();
        FolderChangesList.ItemsSource = folders.Take(12)
            .Select(f => new ChangeRow(f.Path, FormatBytes(f.AllocatedDelta)))
            .ToList();
        FolderChangeSummary.Text = folders.Count == 0 ? "No observed folder changes."
            : $"Showing {Math.Min(folders.Count, 12):N0} of {folders.Count:N0} folder changes, largest first.";
        RefreshFileChanges();
    }

    private void FileFilter_TextChanged(object sender, TextChangedEventArgs e) => RefreshFileChanges();

    private void FileSort_SelectionChanged(object sender, SelectionChangedEventArgs e) => RefreshFileChanges();

    private void RefreshFileChanges()
    {
        if (_currentReport is null) return;
        var all = _currentReport.Files;
        var filter = FileFilterBox.Text.Trim();
        var matching = all.Where(f => filter.Length == 0 ||
            FilePath(f).Contains(filter, StringComparison.OrdinalIgnoreCase) ||
            f.Kind.Contains(filter, StringComparison.OrdinalIgnoreCase)).ToList();
        IOrderedEnumerable<FileChange> ordered = FileSortBox.SelectedIndex switch
        {
            1 => matching.OrderByDescending(f => f.Known).ThenByDescending(f => f.AllocatedDelta),
            2 => matching.OrderByDescending(f => f.Known).ThenBy(f => f.AllocatedDelta),
            3 => matching.OrderBy(f => FilePath(f), StringComparer.OrdinalIgnoreCase),
            _ => matching.OrderByDescending(f => f.Known)
                .ThenByDescending(f => Math.Abs((decimal)f.AllocatedDelta))
        };
        var rows = ordered.Take(100).Select(f =>
            new ChangeRow($"{FilePath(f)} · {f.Kind}", f.Known ? FormatBytes(f.AllocatedDelta) : "unavailable"))
            .ToList();
        FileChangesList.ItemsSource = rows;
        FileChangeSummary.Text = all.Count == 0 ? "No observed file changes."
            : matching.Count == 0 ? $"No matches among {all.Count:N0} file changes."
            : $"Showing {rows.Count:N0} of {matching.Count:N0} matching file changes" +
              (filter.Length == 0 ? "." : $" ({all.Count:N0} total).");
    }

    private void ClearComparison()
    {
        _currentReport = null;
        ResultHeadline.Text = "Two scans are needed to show changes.";
        ResultDetail.Text = "No comparison loaded.";
        SizeExplanation.Visibility = Visibility.Collapsed;
        FolderChangeSummary.Text = "";
        FileChangeSummary.Text = "";
        FolderChangesList.ItemsSource = null;
        FileChangesList.ItemsSource = null;
        CoverageBar.IsOpen = false;
    }

    private void SetBusy(bool value)
    {
        _busy = value;
        FolderPathBox.IsEnabled = !value;
        DatabasePathBox.IsEnabled = !value;
        BrowseButton.IsEnabled = !value;
        LoadButton.IsEnabled = !value;
        ScanButton.IsEnabled = !value;
        CompareButton.IsEnabled = !value && BeforePicker.SelectedItem is SnapshotOption;
        BeforePicker.IsEnabled = !value;
        AfterPicker.IsEnabled = !value;
        CancelButton.Visibility = value ? Visibility.Visible : Visibility.Collapsed;
        CancelButton.IsEnabled = value;
        ScanSpinner.Visibility = value ? Visibility.Visible : Visibility.Collapsed;
        ScanSpinner.IsActive = value;
    }

    private string SelectedRoot()
    {
        if (string.IsNullOrWhiteSpace(FolderPathBox.Text)) throw new InvalidOperationException("Choose a folder first.");
        var path = Path.GetFullPath(FolderPathBox.Text.Trim());
        if (!Directory.Exists(path)) throw new DirectoryNotFoundException("That folder is not accessible.");
        return path;
    }

    private void UseDatabasePath()
    {
        var value = DatabasePathBox.Text.Trim();
        if (value.Length == 0) value = SpaceLedgerClient.DefaultDatabasePath;
        if (!Path.IsPathFullyQualified(value)) throw new InvalidOperationException("Use a full path for the history file.");
        _client = new SpaceLedgerClient(Path.GetFullPath(value));
        DatabasePathBox.Text = _client.DatabasePath;
        DatabaseLocation.Text = $"History database: {_client.DatabasePath}. No background monitoring is enabled.";
    }

    private static bool SamePath(string first, string second) =>
        string.Equals(Path.TrimEndingDirectorySeparator(Path.GetFullPath(first)),
            Path.TrimEndingDirectorySeparator(Path.GetFullPath(second)), StringComparison.OrdinalIgnoreCase);

    private static string LocalTime(string utc) => DateTimeOffset.TryParse(utc, CultureInfo.InvariantCulture,
        DateTimeStyles.AssumeUniversal, out var time) ? time.ToLocalTime().ToString("g") : utc;

    private static string FilePath(FileChange file) =>
        !string.IsNullOrEmpty(file.OldPath) && !string.IsNullOrEmpty(file.NewPath) && file.OldPath != file.NewPath
            ? $"{file.OldPath} → {file.NewPath}" :
            string.IsNullOrEmpty(file.NewPath) ? file.OldPath : file.NewPath;

    private static string FormatBytes(long bytes, bool signed = true)
    {
        var magnitude = Math.Abs((decimal)bytes);
        var units = new[] { "B", "KB", "MB", "GB", "TB", "PB" };
        var index = 0;
        while (magnitude >= 1024 && index < units.Length - 1) { magnitude /= 1024; index++; }
        var prefix = signed && bytes > 0 ? "+" : bytes < 0 ? "−" : "";
        return $"{prefix}{magnitude:0.#} {units[index]}";
    }

    private void ShowStatus(string message, InfoBarSeverity severity)
    {
        StatusBar.Severity = severity;
        StatusBar.Message = message;
        StatusBar.IsOpen = true;
    }
}

public sealed record SnapshotOption(long Id, string Label);
public sealed record HistoryRow(string Title, string Subtitle);
public sealed record ChangeRow(string Description, string SizeText);
