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

`PRAGMA application_id` identifies SpaceLedger and `user_version` identifies the schema. Nonempty unrelated databases and unsupported schema versions are rejected. Read commands use `SQLITE_OPEN_READONLY`. History retains full snapshots until a later retention feature is designed.

## Next useful work

1. Benchmark selected large directory trees, then reduce memory and metadata-call overhead using measured evidence.
2. Strengthen tests for long paths, junctions, interrupted scans, cloud placeholders and changing directory topology. Test volume-level accounting on a disposable NTFS VHDX.
3. Define stream-level accounting and an explicit reconciliation model for volume metadata and other unexplained allocation.
4. Add scheduled snapshots and a retention policy before continuous monitoring.
5. Add a simple timeline and folder-delta interface over the existing core.
6. Add USN processing, gap detection and reconciliation. Evaluate optional ETW attribution separately.

## API references

- [GetFileInformationByHandleEx](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getfileinformationbyhandleex)
- [FILE_STANDARD_INFO](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_standard_info)
- [FILE_COMPRESSION_INFO](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_compression_info)
- [CreateFileW metadata access and sharing](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew)
- [Reparse points and file operations](https://learn.microsoft.com/en-us/windows/win32/fileio/reparse-points-and-file-operations)
