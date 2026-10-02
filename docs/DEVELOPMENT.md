# Development notes

## First session — 2026-09-28

Implemented a C++20 CLI with Windows metadata scanning, durable SQLite snapshots, comparison reports and NTFS tests. The collector, database and comparison logic are independent of the console interface.

The initial repository was empty. Visual Studio Community 2026, CMake and Windows SDK 10.0.26100.0 were already installed. This prototype links to the SDK's `winsqlite3.lib` and Windows' SQLite DLL, avoiding a new package manager or downloaded dependency. Deployment currently assumes the Visual C++ runtime installed by Visual Studio; packaging for another PC is future work.

## Verified result

A clean x64 Release build succeeded with MSVC 19.51.36260.0 and optimization enabled. All three CTest targets passed: core/NTFS integration, CLI help and the complete CLI workflow. The symbolic-link loop test ran successfully on this machine; there were no skipped cases in the final run. MSBuild's file tracker needed execution outside the development sandbox.

The real NTFS fixture comparison correctly reported:

```text
grow.bin                         +28672 B
remove.bin                       -16384 B
created.bin                      +12288 B
old/move.bin -> new/renamed.bin        0 B
Net allocated change             +24576 B
```

The same results were retrieved through the CLI from the saved SQLite snapshots. The scanner was exercised on generated test folders, not a full system-drive scan. Live OneDrive behavior, whole-drive performance and a disposable VHDX remain unverified.

## Deliberate scope decisions

- Manual directory snapshots before any persistent service or journal reader.
- Current NTFS allocation, including sparse/compressed files, is measured via handles. `FileStandardInfo` supplies normal allocation; `FileCompressionInfo` supplies sparse/compressed storage.
- Identity includes creation time as an additional guard against recycled IDs, but is not claimed as an eternal file identifier.
- Explicit coverage errors and observation intervals; no claim of an atomic snapshot or complete causal history.
- Shared data counted once in the selected root, with deterministic folder attribution.
- Volume free-space samples are independent contextual measurements; subtree changes are not equated with volume allocation changes.
- No file content access, background startup, cleanup, telemetry, or network requests in the application.
- Snapshot header and entries commit in one SQLite transaction. A failed save rolls back; a scan that fails before saving creates no snapshot. Recorded access gaps remain visible in successfully saved snapshots.

## SQLite schema version 1

`scans` records root, volume GUID, UTC scan interval, total volume size and free space sampled before/after traversal. `entries` records relative paths, identity, directory flag, logical/allocated sizes, attributes, link count, last-write time, observation status and Win32 error code.

`PRAGMA application_id` identifies SpaceLedger and `user_version` identifies the schema. Nonempty unrelated databases and unsupported schema versions are rejected. Read commands use `SQLITE_OPEN_READONLY`. Version 0.2 retains the schema version 1 format and can explicitly prune old full snapshots per selected root.

## Second session — 2026-10-02

Version 0.2 adds bounded console progress reporting, Ctrl+C cancellation before commit, latest/time-window history selection, preview-first retention and a Windows Task Scheduler helper. A scheduled run invokes the built CLI and prunes only after a snapshot has been saved. Task registration remains an explicit user action; no monitored root or daily time has been chosen for this installation. The retention rule protects the newest configured number of snapshots and the latest scan without recorded coverage issues. `compact` reclaims SQLite pages only when explicitly requested.

All four CTest targets passed in Release on this machine. The tests now include cancellation, long paths, time-window selection, retention behavior and command-line workflows. The scheduling helper is previewable without creating a task. The Windows PowerShell scheduled runner is separately exercised by the test suite. A metadata-only scan of the project's build folder observed 242 paths in 83 ms, with four expected skipped symbolic-link loops; this is not a whole-drive benchmark. Whole-drive and live-cloud behavior remain unverified.

## Third session — 2026-10-02

Version 0.3 adds an unpackaged WinUI 3 desktop front end. The existing C++ CLI remains the scanner and comparison engine; its new JSON reports and newline-delimited scan progress are the UI's interface. A named Windows event requests cancellation without killing the process. The window chooses a folder, takes scans only on request, lists prior snapshots, and shows folder/file deltas with coverage warnings. It stores GUI history in the user's local app-data directory and excludes that database during scans. No scheduled task or monitored root was registered.

The first GUI is deliberately read-only with respect to file cleanup and retention. It was launched and visually inspected; opening it did not start a scan. Five CTest targets passed in Release, including JSON output and event-based cancellation. The WinUI Release build completed with zero warnings and errors. No real AppData scan or end-to-end GUI scan was performed. Before larger-directory use, benchmark traversal and database size, then test cancellation and on-disk history under load. Packaging and deployment to another PC remain future work.

## Next useful work

1. Benchmark selected large directory trees, then reduce memory and metadata-call overhead using measured evidence.
2. Test the GUI scan/compare workflow against a disposable local fixture, then improve responsive layout and add a real timeline chart.
3. Correct retention's coverage check for inconsistent hard-link measurements; strengthen tests for junctions, cloud placeholders and changing directory topology.
4. Define stream-level accounting and an explicit reconciliation model for volume metadata and other unexplained allocation. Test on a disposable NTFS VHDX.
5. Add USN processing, gap detection and reconciliation. Evaluate optional ETW attribution separately.

## API references

- [GetFileInformationByHandleEx](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getfileinformationbyhandleex)
- [FILE_STANDARD_INFO](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_standard_info)
- [FILE_COMPRESSION_INFO](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_compression_info)
- [CreateFileW metadata access and sharing](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew)
- [Reparse points and file operations](https://learn.microsoft.com/en-us/windows/win32/fileio/reparse-points-and-file-operations)
