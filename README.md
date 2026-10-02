# SpaceLedger

A Windows storage-history prototype for answering **which files and folders changed their storage use between two scans**.

Version 0.3 adds a WinUI 3 desktop window over the existing C++/SQLite scanner. You choose a folder, take scans manually, and compare them visually. It measures metadata, never reads file contents, has no file-cleanup commands, and does not start monitoring when opened.

## Open the Windows app

In PowerShell 7, build the scanner and the window together:

```powershell
.\tools\build-ui.ps1
```

Then open `ui\SpaceLedger.App\bin\x64\Release\net10.0-windows10.0.26100.0\win-x64\SpaceLedger.App.exe`. Choose a **local NTFS** folder and click **Scan now**. The first scan establishes a baseline; after a later scan, the window shows the largest folder and file changes, along with any coverage warnings. **Cancel scan** stops collection before saving a snapshot. Nothing scans automatically.

The UI uses `%LOCALAPPDATA%\SpaceLedger\history.db` by default, separate from the CLI's `.spaceledger\history.db`. Expand **History file (advanced)** to open another SpaceLedger database; use **Load history** after changing it. To inspect GUI history from the CLI, pass `--database` with the UI database path. The UI build currently requires the .NET 10 SDK, Visual Studio C++ tools, and access to Microsoft's NuGet packages on first restore. The output is a development build, not an installer. Its Windows App SDK components are copied beside the app; a target PC also needs the .NET 10 desktop runtime.

## Build

CLI requirements: Windows 10/11, Visual Studio 2022 or 2026 with **Desktop development with C++**, a Windows 10/11 SDK, and the Visual Studio CMake tools component. The C++ build uses the Windows-provided `winsqlite3` library; it downloads no dependencies.

From PowerShell 7 in this directory:

```powershell
.\tools\build.ps1
```

The script finds the installed C++ toolchain, builds Release, and runs CTest. The executable is `build\Release\spaceledger.exe`. Use `-Fresh` to regenerate CMake's cache after changing the compiler, or `-SkipTests` to build only. The script normalizes duplicate environment variable names for child processes because MSBuild rejects hosts that supply both `Path` and `PATH`.

Alternatively, from a developer shell with CMake on PATH and Visual Studio 2026:

```powershell
cmake --preset windows
cmake --build --preset windows
ctest --preset windows
```

For Visual Studio 2022, use `cmake -S . -B build -G "Visual Studio 17 2022" -A x64`, followed by `cmake --build build --config Release` and `ctest --test-dir build -C Release --output-on-failure`. Use a separate build directory when switching generators.

## Try it

Start with a small local directory. These commands assume the project is your working directory:

```powershell
.\build\Release\spaceledger.exe scan 'C:\path\to\your\folder'

# Later, after the folder has changed:
.\build\Release\spaceledger.exe scan 'C:\path\to\your\folder'
.\build\Release\spaceledger.exe scans
.\build\Release\spaceledger.exe diff --from 1 --to 2
```

Replace the example path and use the snapshot IDs printed by `scan` or `scans`. A drive root is `C:\`, not `C:`; the latter is drive-relative and is rejected.

By default, snapshots go into `.spaceledger\history.db` in the current directory. To use another file, append `--database 'C:\path\history.db'` to **each** command. Its parent directory is created when saving a scan. The database and its journal/WAL sidecars are excluded by path if they lie inside the scanned tree.

```powershell
.\build\Release\spaceledger.exe issues --scan 2
.\build\Release\spaceledger.exe diff --from 1 --to 2 --limit 100
.\build\Release\spaceledger.exe diff --from 1 --to 2 --json
.\build\Release\spaceledger.exe latest --path 'C:\path\to\your\folder'
.\build\Release\spaceledger.exe diff --since 24h --path 'C:\path\to\your\folder'
.\build\Release\spaceledger.exe --help
```

`--since` accepts hours, days or weeks (for example `24h`, `7d`, `2w`). It compares the latest scan with the most recent scan completed before the requested cutoff; it reports an error until such a baseline exists. Use `--path` when history contains multiple roots. `scan` shows progress in an interactive console; `--quiet` suppresses it. Ctrl+C cancels before saving and returns 130. Other exit codes are 0 on success, 2 when a scan is saved with coverage issues, and 1 on failure. `issues` explains skipped entries and Windows access errors. Read commands do not create a missing database.

## Daily history and retention

Preview a daily task for a specific local directory, then register it only after checking the displayed root, database, time and command:

```powershell
.\tools\schedule.ps1 -Mode Plan -Root 'C:\path\to\your\folder' -At 03:00 -Keep 30
.\tools\schedule.ps1 -Mode Add  -Root 'C:\path\to\your\folder' -At 03:00 -Keep 30
```

The task runs each day while you are signed in. `-RunWhenLoggedOff` requests Windows' S4U logon instead; access to some encrypted or network resources may differ. Use `-Mode Show` to inspect it and `-Mode Remove` to unregister it, passing the same `-Root` and, if specified originally, `-Database`. Removing a task leaves the history intact. The task uses this project's built executable and scripts, so keep this directory in place. No daily task is registered by building SpaceLedger.

Each scheduled run scans the selected root, then prunes its history if the scan was saved (including scans with reported coverage issues). It retains the newest 30 by default, plus the latest baseline without recorded coverage issues. It does not affect other roots in the database. To do this manually, preview first:

```powershell
.\build\Release\spaceledger.exe retention --path 'C:\path\to\your\folder' --keep 30
.\build\Release\spaceledger.exe retention --path 'C:\path\to\your\folder' --keep 30 --apply
.\build\Release\spaceledger.exe compact
```

`--apply` permanently deletes the listed snapshots; `compact` is a separate, explicit operation to reclaim unused database pages and may require temporary disk space. Retention never deletes files in the monitored directory.

## Reading the comparison

All sizes are exact **bytes**, all timestamps are **UTC**, and positive file/folder deltas mean growth. A positive *free-space* change means more space is available.

- **Logical size** is the file's reported data length. **Allocated size** is the reported storage for its unnamed data stream, accounting for ordinary NTFS compression and sparse allocation.
- Files are matched by volume serial, 128-bit file ID, and creation time. Moves and renames retain identity; a replacement at the same path can appear as a removal and addition.
- **Added/removed** means entered/left the observed tree. It does not prove that a file was created/deleted on the volume, or that its allocation was newly consumed/freed.
- Hard-linked data is counted once within the selected root. Folder attribution uses the first sorted observed path. Adding/removing a hard link can move attributed bytes between folders without changing the total. Links outside the scanned root are not inventoried.
- Folder rows include descendants, so nested rows overlap. Do not add all displayed folder rows together.
- If a matching path/subtree is unavailable in the other snapshot, its comparison is marked **unavailable**, not treated as growth or deletion. The comparable delta excludes these files. New or moved files hidden in unavailable regions cannot be reconstructed.
- Inconsistent sizes observed through multiple hard links are also flagged as uncertain. Every scan spans a time interval, not one atomic instant.
- Whole-volume free-space movement is shown as **context only**. It covers other folders, filesystem metadata, shadow copies, history-database writes and background activity. It is not presented as a reconciled explanation of the selected directory's delta.

## Current boundaries

This remains a snapshot prototype. The GUI is a first working window, not yet a full interactive timeline chart. It does not implement continuous USN journal monitoring, process attribution, application classification, or cleanup guidance. Optional scheduling uses Windows Task Scheduler rather than a permanently running service; no task is registered by the GUI.

It supports local NTFS drive-letter paths. It skips reparse points, junctions and entries marked as cloud/offline/recall files, reporting those coverage gaps. It requests metadata access only and never reads file contents. Cloud-provider behavior has not yet been validated on a live OneDrive account. Do not use it as a security boundary against concurrent malicious path substitutions.

Alternate data streams, directory/MFT overhead, VSS storage, deduplication and other special storage arrangements are not included in file totals. Tiny resident files can report zero separate data allocation. Empty-directory-only changes are stored but are not shown as file changes. Creation-time changes and file-ID reuse can affect identity matching.

The scanner keeps a snapshot in memory and opens files individually. It is intended for selected directories first; whole-drive performance has not been benchmarked. Every scan stores a full snapshot. Automatic pruning happens only for a task explicitly registered with the scheduling helper. Paths and names in the database are sensitive metadata; store it in a private local directory. The database inherits its directory's Windows permissions and is not encrypted.

## Verification

CTest runs synthetic accounting cases and real NTFS fixtures under `build\spaceledger-test-*`. Tests cover growth, shrinkage, additions/removals, moves, hard links, sparse and compressed files, Unicode and long paths, content locks, denied directory listing, exclusions, persistence, read-only database access, transaction rollback and cancellation. A symbolic-link loop is tested where the OS permits creating it; otherwise that case explicitly reports `SKIP`. CLI workflows check the commands and invalid inputs, including time selection, JSON reports, retention and named-event cancellation. All five automated targets pass on this development PC. The GUI builds and opens; its scan/compare workflow has not yet been manually exercised end to end. Fixtures are retained for inspection. All fixture content and ACL changes are confined to newly created test directories.

Source layout:

```text
include/spaceledger/   Shared data model and interfaces
src/windows.cpp       Windows handles, Unicode and path helpers
src/scanner.cpp       NTFS metadata collection
src/database.cpp      SQLite schema and transactions
src/diff.cpp          Identity matching and folder accounting
src/history.cpp       Time selection and retention policy
src/main.cpp          CLI, human-readable and JSON reports
ui/SpaceLedger.App/  WinUI 3 desktop window and CLI bridge
tests/tests.cpp       Accounting and filesystem integration tests
tools/build.ps1       Build and test entry point
tools/build-ui.ps1    Build CLI and desktop window
tools/schedule.ps1    Task Scheduler preview and management
tools/run-scheduled.ps1  Single scheduled scan and retention
docs/DEVELOPMENT.md    Decisions, tested status and next milestones
```
