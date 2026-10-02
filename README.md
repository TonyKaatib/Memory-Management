# SpaceLedger

A Windows command-line prototype for answering **which files and folders changed their storage use between two scans**.

Version 0.1 implements the first working slice: scan a selected NTFS directory, save a snapshot in SQLite, scan again, and compare file and folder sizes. It measures metadata and writes its own history database. It has no cleanup commands.

## Build

Requirements: Windows 10/11, Visual Studio 2022 or 2026 with **Desktop development with C++**, a Windows 10/11 SDK, and the Visual Studio CMake tools component. The build uses C++20 and the Windows-provided `winsqlite3` library; it downloads no dependencies.

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
.\build\Release\spaceledger.exe --help
```

`scan` exits with 0 on success, 2 when it saves a snapshot containing coverage issues, and 1 on failure. `issues` explains skipped entries and Windows access errors. `scans`, `diff`, and `issues` open the database read-only and do not create a missing database.

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

This is a manual snapshot prototype. It does not yet implement a GUI, scheduling, retention, USN journal monitoring, process attribution, application classification, or cleanup guidance.

It supports local NTFS drive-letter paths. It skips reparse points, junctions and entries marked as cloud/offline/recall files, reporting those coverage gaps. It requests metadata access only and never reads file contents. Cloud-provider behavior has not yet been validated on a live OneDrive account. Do not use it as a security boundary against concurrent malicious path substitutions.

Alternate data streams, directory/MFT overhead, VSS storage, deduplication and other special storage arrangements are not included in file totals. Tiny resident files can report zero separate data allocation. Empty-directory-only changes are stored but are not shown as file changes. Creation-time changes and file-ID reuse can affect identity matching.

The scanner keeps a snapshot in memory and opens files individually. It is intended for selected directories first; whole-drive performance has not been benchmarked. Every scan stores a full snapshot, with no automatic pruning. Paths and names in the database are sensitive metadata; store it in a private local directory. The database inherits its directory's Windows permissions and is not encrypted.

## Verification

CTest runs synthetic accounting cases and real NTFS fixtures under `build\spaceledger-test-*`. Tests cover growth, shrinkage, additions/removals, moves, hard links, sparse and compressed files, Unicode, content locks, denied directory listing, exclusions, persistence, read-only database access and transaction rollback. A symbolic-link loop is tested where the OS permits creating it; otherwise that case explicitly reports `SKIP`. A separate CLI workflow checks the scan/list/diff/issues commands, invalid inputs, incompatible roots, missing databases and preservation of an unrelated file passed as a database. Fixtures are retained for inspection. All fixture content and ACL changes are confined to newly created test directories.

Source layout:

```text
include/spaceledger/   Shared data model and interfaces
src/windows.cpp       Windows handles, Unicode and path helpers
src/scanner.cpp       NTFS metadata collection
src/database.cpp      SQLite schema and transactions
src/diff.cpp          Identity matching and folder accounting
src/main.cpp          CLI and reports
tests/tests.cpp       Accounting and filesystem integration tests
tools/build.ps1       Build and test entry point
docs/DEVELOPMENT.md    Decisions, tested status and next milestones
```
