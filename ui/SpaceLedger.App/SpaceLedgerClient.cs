using System.Diagnostics;
using System.Text;
using System.Text.Json;

namespace SpaceLedger_App;

internal sealed class SpaceLedgerClient
{
    private static readonly JsonSerializerOptions JsonOptions = new() { PropertyNameCaseInsensitive = true };
    private readonly string _executable = Path.Combine(AppContext.BaseDirectory, "spaceledger.exe");

    public static string DefaultDatabasePath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "SpaceLedger", "history.db");

    public string DatabasePath { get; }

    public SpaceLedgerClient(string? databasePath = null) => DatabasePath = databasePath ?? DefaultDatabasePath;

    public async Task<IReadOnlyList<ScanSnapshot>> GetScansAsync()
    {
        if (!File.Exists(DatabasePath)) return [];
        var json = await RunJsonAsync("scans", "--json", "--database", DatabasePath);
        return JsonSerializer.Deserialize<ScanList>(json, JsonOptions)?.Snapshots ?? [];
    }

    public async Task<DiffReport> CompareAsync(long from, long to)
    {
        var json = await RunJsonAsync("diff", "--from", from.ToString(), "--to", to.ToString(),
            "--json", "--database", DatabasePath);
        return JsonSerializer.Deserialize<DiffReport>(json, JsonOptions)
            ?? throw new InvalidDataException("SpaceLedger returned an empty comparison.");
    }

    public async Task<ScanResult> ScanAsync(string root, Action<ScanProgress> onProgress, CancellationToken cancellationToken)
    {
        var eventName = $@"Local\SpaceLedger-{Guid.NewGuid():N}";
        using var signal = new EventWaitHandle(false, EventResetMode.ManualReset, eventName);
        using var process = Start("scan", root, "--json-stream", "--cancel-event", eventName,
            "--database", DatabasePath);
        using var registration = cancellationToken.Register(() => signal.Set());
        var errors = process.StandardError.ReadToEndAsync();
        ScanResult? saved = null;
        string? line;
        while ((line = await process.StandardOutput.ReadLineAsync()) is not null)
        {
            using var document = JsonDocument.Parse(line);
            var type = document.RootElement.GetProperty("type").GetString();
            if (type == "progress")
            {
                var progress = JsonSerializer.Deserialize<ScanProgress>(line, JsonOptions);
                if (progress is not null) onProgress(progress);
            }
            else if (type == "saved") saved = JsonSerializer.Deserialize<ScanResult>(line, JsonOptions);
        }
        await process.WaitForExitAsync();
        var errorText = await errors;
        if (process.ExitCode == 130)
            throw new OperationCanceledException("Scan cancelled; no snapshot saved.");
        if (process.ExitCode is not (0 or 2)) throw new InvalidOperationException(ErrorText(errorText, process.ExitCode));
        return saved ?? throw new InvalidDataException("SpaceLedger did not confirm that it saved the scan.");
    }

    private async Task<string> RunJsonAsync(params string[] arguments)
    {
        using var process = Start(arguments);
        var output = process.StandardOutput.ReadToEndAsync();
        var errors = process.StandardError.ReadToEndAsync();
        await process.WaitForExitAsync();
        var errorText = await errors;
        if (process.ExitCode != 0) throw new InvalidOperationException(ErrorText(errorText, process.ExitCode));
        return await output;
    }

    private Process Start(params string[] arguments)
    {
        if (!File.Exists(_executable)) throw new FileNotFoundException("SpaceLedger's scanner is missing. Rebuild the app.", _executable);
        var info = new ProcessStartInfo(_executable)
        {
            CreateNoWindow = true,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
        };
        foreach (var argument in arguments) info.ArgumentList.Add(argument);
        return Process.Start(info) ?? throw new InvalidOperationException("Cannot start SpaceLedger's scanner.");
    }

    private static string ErrorText(string errorText, int code) =>
        string.IsNullOrWhiteSpace(errorText) ? $"SpaceLedger exited with code {code}." : errorText.Trim();
}

internal sealed class ScanList
{
    public List<ScanSnapshot> Snapshots { get; set; } = [];
}

internal sealed class ScanSnapshot
{
    public long Id { get; set; }
    public string Started { get; set; } = "";
    public string Finished { get; set; } = "";
    public string Root { get; set; } = "";
    public string Volume { get; set; } = "";
    public bool CoverageIssues { get; set; }
}

internal sealed class ScanProgress
{
    public long Entries { get; set; }
    public long Directories { get; set; }
    public long Issues { get; set; }
}

internal sealed class ScanResult
{
    public long Id { get; set; }
    public long Issues { get; set; }
}

internal sealed class DiffReport
{
    public long From { get; set; }
    public long To { get; set; }
    public string Root { get; set; } = "";
    public long LogicalDelta { get; set; }
    public long AllocatedDelta { get; set; }
    public long FreeSpaceChange { get; set; }
    public long CoverageIssuesBefore { get; set; }
    public long CoverageIssuesAfter { get; set; }
    public long Uncertain { get; set; }
    public List<FolderChange> Folders { get; set; } = [];
    public List<FileChange> Files { get; set; } = [];
}

internal sealed class FolderChange
{
    public string Path { get; set; } = "";
    public long AllocatedDelta { get; set; }
    public long LogicalDelta { get; set; }
}

internal sealed class FileChange
{
    public string Kind { get; set; } = "";
    public string OldPath { get; set; } = "";
    public string NewPath { get; set; } = "";
    public long AllocatedDelta { get; set; }
    public long LogicalDelta { get; set; }
    public bool Known { get; set; }
}
