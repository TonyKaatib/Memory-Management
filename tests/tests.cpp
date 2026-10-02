#include "spaceledger/database.hpp"
#include "spaceledger/history.hpp"
#include "spaceledger/windows.hpp"

#include <winioctl.h>
#include <sddl.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace spaceledger;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Entry entry(std::string path, std::string id, Bytes logical, Bytes allocated) {
    Entry e;
    e.path = std::move(path); e.identity = std::move(id); e.logical = logical; e.allocated = allocated; e.links = 1;
    return e;
}

Snapshot sample() {
    Snapshot s;
    s.root = "C:\\fixture"; s.volume = "test-volume";
    s.started = "2026-01-01T00:00:00Z"; s.finished = s.started;
    return s;
}

void diff_tests() {
    auto a = sample(), b = sample();
    a.entries = {entry("old/a", "a", 10, 4096), entry("same/b", "b", 20, 4096), entry("gone", "c", 30, 4096)};
    b.entries = {entry("new/a", "a", 10, 4096), entry("same/b", "b", 5000, 8192), entry("created", "d", 3, 4096)};
    auto d = compare(a, b);
    require(d.changes.size() == 4, "Expected rename, growth, addition and removal");
    require(d.allocated_delta == 4096 && d.logical_delta == 4953, "Wrong net delta");
    require(std::any_of(d.changes.begin(), d.changes.end(), [](const auto& c) { return c.kind == "moved/renamed" && c.allocated_delta == 0; }), "Rename charged as growth");
    require(std::any_of(d.folders.begin(), d.folders.end(), [](const auto& f) { return f.path == "old" && f.allocated == -4096; }), "Move not attributed to old folder");

    b = a;
    b.entries.push_back(entry("second/link", "a", 10, 4096));
    require(totals(b).allocated == 12288 && totals(b).unique_files == 3, "Hard links counted twice");
    d = compare(a, b);
    require(d.allocated_delta == 0 && d.changes.size() == 1 && d.changes[0].kind == "links changed", "Link creation treated as disk growth");

    b = a;
    b.entries.erase(b.entries.begin());
    Entry inaccessible;
    inaccessible.path = "old"; inaccessible.directory = true; inaccessible.status = "error"; inaccessible.error = ERROR_ACCESS_DENIED;
    b.entries.push_back(inaccessible);
    d = compare(a, b);
    require(d.allocated_delta == 0 && d.uncertain == 1 && !d.changes[0].known, "Inaccessible subtree reported as deletion");
    d = compare(b, a);
    require(d.allocated_delta == 0 && d.uncertain == 1, "Recovered coverage reported as growth");
    require(path_within("folder/file", "FOLDER") && !path_within("folder-other/file", "folder"), "Invalid path boundary");

    b = a;
    b.entries[0].identity = "replacement";
    d = compare(a, b);
    require(d.changes.size() == 2 && d.allocated_delta == 0, "Same-path replacement not distinguished");
    b = a;
    b.entries[0].logical = 1; b.entries[0].allocated = 0;
    d = compare(a, b);
    require(d.changes.size() == 1 && d.changes[0].kind == "shrank" && d.allocated_delta == -4096, "Shrink not detected");
    b = a;
    b.entries.push_back(entry("alias", "a", 10000, 12288));
    require(totals(b).issues == 1 && compare(a, b).uncertain == 1, "Inconsistent hard-link observations were trusted");
    b = a; b.volume = "other";
    bool rejected = false;
    try { (void)compare(a, b); } catch (const std::exception&) { rejected = true; }
    require(rejected, "Cross-volume comparison was accepted");
    std::cout << "PASS diff accounting and incomplete coverage\n";
}

void history_tests() {
    require(parse_duration_hours(L"24h") == 24 && parse_duration_hours(L"7d") == 168 &&
            parse_duration_hours(L"2w") == 336, "Duration parsing failed");
    for (const auto& bad : {L"0h", L"-1h", L"2m", L"999999999999999999999w"}) {
        bool rejected = false;
        try { (void)parse_duration_hours(bad); } catch (const std::exception&) { rejected = true; }
        require(rejected, "Invalid duration accepted");
    }
    require(utc_hours_ago(1) < utc_now(), "UTC cutoff calculation failed");
    std::vector<Snapshot> headers;
    for (int i = 1; i <= 5; ++i) {
        auto s = sample();
        s.id = i;
        s.finished = "2026-10-0" + std::to_string(i) + "T12:00:00Z";
        headers.push_back(s);
    }
    require(latest_snapshot(headers, {}).id == 5, "Latest snapshot wrong");
    const auto pair = since_pair(headers, {}, "2026-10-03T00:00:00Z");
    require(pair.from == 2 && pair.to == 5, "Time interval chose wrong baseline");
    auto prune = retention_candidates(headers, {}, 2, [](std::int64_t id) { return id != 1; });
    require(prune == std::vector<std::int64_t>({2, 3}), "Retention did not preserve last complete baseline");
    headers.back().volume = "new-volume";
    bool ambiguous = false;
    try { (void)latest_snapshot(headers, {}); } catch (const std::exception&) { ambiguous = true; }
    require(ambiguous, "Multiple volumes not flagged as ambiguous");
    std::cout << "PASS history selection and retention planning\n";
}

void write_file(const fs::path& path, std::size_t bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    const std::string data(bytes, 'x');
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!file) throw std::runtime_error("Cannot create fixture");
}

class DenyListing {
public:
    explicit DenyListing(const fs::path& path) : path_(path) {
        DWORD needed = 0;
        GetFileSecurityW(path_.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &needed);
        require(needed != 0, "Cannot read fixture security size");
        original_.resize(needed);
        require(GetFileSecurityW(path_.c_str(), DACL_SECURITY_INFORMATION, original_.data(), needed, &needed) != 0, "Cannot read fixture security");
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        require(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(D;;0x1;;;WD)(A;;FA;;;WD)", SDDL_REVISION_1, &descriptor, nullptr) != 0,
                "Cannot build fixture ACL");
        const auto changed = SetFileSecurityW(path_.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor);
        LocalFree(descriptor);
        require(changed != 0, "Cannot apply fixture ACL");
    }
    ~DenyListing() { SetFileSecurityW(path_.c_str(), DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION, original_.data()); }
    DenyListing(const DenyListing&) = delete;
    DenyListing& operator=(const DenyListing&) = delete;
private:
    fs::path path_;
    std::vector<BYTE> original_;
};

const Entry& find_entry(const Snapshot& snapshot, const std::string& path) {
    const auto it = std::find_if(snapshot.entries.begin(), snapshot.entries.end(), [&](const auto& e) { return e.path == path; });
    if (it == snapshot.entries.end()) throw std::runtime_error("Missing fixture entry: " + path);
    return *it;
}

void integration_tests() {
    // Only files underneath this newly-created test directory may be changed.
    const auto base = fs::current_path() / (L"spaceledger-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    require(fs::create_directory(base), "Fixture directory already exists");
    std::cout << "Fixture directory: " << utf8(base.native()) << '\n';
    const auto root = base / L"data";
    fs::create_directories(root / L"old");
    fs::create_directories(root / L"new");
    write_file(root / L"old" / L"move.bin", 8192);
    write_file(root / L"grow.bin", 4096);
    write_file(root / L"remove.bin", 16384);
    write_file(root / L"a shared.bin", 8192);
    require(CreateHardLinkW((root / L"z shared.bin").c_str(), (root / L"a shared.bin").c_str(), nullptr) != 0, "Cannot create NTFS hard link");
    write_file(root / L"ol\u00e1 \u4e16\u754c.txt", 2048);
    fs::path long_directory = root;
    std::wstring long_relative;
    for (int i = 0; i < 4; ++i) {
        const auto part = std::wstring(70, static_cast<wchar_t>(L'a' + i));
        long_directory /= part;
        long_relative += (long_relative.empty() ? L"" : L"/") + part;
        require(CreateDirectoryW(extended(long_directory).c_str(), nullptr) != 0, "Cannot create long-path fixture directory");
    }
    const auto long_file = long_directory / L"deep.bin";
    {
        Handle file(CreateFileW(extended(long_file).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        require(static_cast<bool>(file), "Cannot create long-path fixture file");
        const char byte = 'x';
        DWORD written = 0;
        require(WriteFile(file.get(), &byte, 1, &written, nullptr) != 0 && written == 1, "Cannot write long-path fixture");
    }

    {
        Handle file(CreateFileW((root / L"sparse.bin").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        require(static_cast<bool>(file), "Cannot create sparse fixture");
        DWORD returned = 0;
        require(DeviceIoControl(file.get(), FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr) != 0, "Cannot mark sparse fixture");
        LARGE_INTEGER size{}; size.QuadPart = 64 * 1024 * 1024;
        require(SetFilePointerEx(file.get(), size, nullptr, FILE_BEGIN) && SetEndOfFile(file.get()), "Cannot extend sparse fixture");
    }
    write_file(root / L"compressed.bin", 1024 * 1024);
    {
        Handle file(CreateFileW((root / L"compressed.bin").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        USHORT format = COMPRESSION_FORMAT_DEFAULT;
        DWORD returned = 0;
        require(DeviceIoControl(file.get(), FSCTL_SET_COMPRESSION, &format, sizeof(format), nullptr, 0, &returned, nullptr) != 0, "Cannot compress fixture");
    }
    const auto first = scan(root);
    require(totals(first).issues == 0, "Unexpected scan issues");
    require(totals(first).observed_paths == totals(first).unique_files + 1, "Real hard links not deduplicated");
    const auto& sparse = find_entry(first, "sparse.bin");
    require(sparse.logical == 64 * 1024 * 1024 && sparse.allocated == 0, "Sparse allocation is incorrect");
    const auto& compressed = find_entry(first, "compressed.bin");
    require(compressed.allocated < compressed.logical, "Compressed allocation is incorrect");
    require(find_entry(first, "ol\xc3\xa1 \xe4\xb8\x96\xe7\x95\x8c.txt").logical == 2048, "Unicode filename lost");
    require(find_entry(first, utf8(long_relative + L"/deep.bin")).logical == 1, "Long path lost");

    fs::rename(root / L"old" / L"move.bin", root / L"new" / L"renamed.bin");
    write_file(root / L"grow.bin", 32768);
    fs::remove(root / L"remove.bin");
    write_file(root / L"created.bin", 12288);
    const auto second = scan(root);
    const auto diff = compare(first, second);
    require(diff.changes.size() == 4 && diff.uncertain == 0, "Unexpected real NTFS comparison results");
    require(diff.logical_delta == 24576 && diff.allocated_delta == 24576, "Real byte delta mismatch");
    require(totals(second).allocated - totals(first).allocated == diff.allocated_delta, "File allocation totals do not reconcile");
    ScanProgress final_progress;
    int progress_calls = 0;
    (void)scan(root, {}, [&](const ScanProgress& p) { final_progress = p; ++progress_calls; });
    require(progress_calls >= 2 && final_progress.entries == second.entries.size(), "Scan progress was not completed");
    int cancellation_checks = 0;
    bool cancelled = false;
    try { (void)scan(root, {}, {}, [&] { return ++cancellation_checks >= 4; }); }
    catch (const ScanCancelled&) { cancelled = true; }
    require(cancelled, "Scan cancellation was ignored");

    const auto db_path = base / L"history.db";
    {
        Database db(db_path, true);
        require(db.save(first) == 1 && db.save(second) == 2, "Wrong snapshot IDs");
        require(db.list().size() == 2, "Snapshot list mismatch");
        const auto roundtrip = db.load(1);
        require(roundtrip.entries.size() == first.entries.size(), "Database lost entries");
        require(totals(roundtrip).allocated == totals(first).allocated, "Database lost allocated sizes");
        require(compare(roundtrip, db.load(2)).allocated_delta == 24576, "Stored diff mismatch");
        auto broken = first;
        broken.entries.push_back(first.entries.back());
        bool rejected = false;
        try { db.save(broken); } catch (const std::exception&) { rejected = true; }
        require(rejected && db.list().size() == 2, "Failed snapshot was not rolled back");
    }
    {
        Database db(db_path);
        require(db.list().size() == 2, "Read-only reopen failed");
        bool rejected = false;
        try { db.save(first); } catch (const std::exception&) { rejected = true; }
        require(rejected, "Read-only database accepted a write");
    }
    {
        Database db(db_path, true);
        require(!db.has_coverage_issues(1), "Clean scan marked incomplete");
        auto with_issue = first;
        with_issue.entries.front().status = "error";
        const auto problem_id = db.save(with_issue);
        require(db.has_coverage_issues(problem_id), "Coverage issue not detected");
        db.erase({problem_id});
        require(db.list().size() == 2, "Retention did not remove snapshot and entries");
        bool missing = false;
        try { (void)db.load(problem_id); } catch (const std::exception&) { missing = true; }
        require(missing, "Deleted snapshot still loads");
        db.compact();
    }
    {
        Handle locked(CreateFileW((root / L"grow.bin").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        require(static_cast<bool>(locked), "Cannot lock fixture");
        const auto locked_scan = scan(root);
        require(find_entry(locked_scan, "grow.bin").status == "observed", "Metadata-only scan should handle a content lock");
    }
    {
        DenyListing denied(root / L"new");
        const auto blocked = scan(root);
        require(find_entry(blocked, "new").status == "error", "Directory access denial not recorded");
        require(compare(second, blocked).uncertain == 1 && compare(second, blocked).allocated_delta == 0, "Inaccessible directory reported as freed space");
    }
    write_file(root / L"excluded.db", 4096);
    const auto excluded = scan(root, {root / L"excluded.db"});
    require(find_entry(excluded, "excluded.db").status == "excluded", "Database exclusion failed");

    const auto link = root / L"loop";
    if (CreateSymbolicLinkW(link.c_str(), root.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        const auto linked = scan(root);
        require(find_entry(linked, "loop").status == "skipped_reparse_or_cloud", "Directory link traversed");
        require(linked.entries.size() == excluded.entries.size() + 1, "Link loop expanded");
    } else {
        std::cout << "SKIP symbolic-link fixture: " << windows_error(GetLastError()) << '\n';
    }
    // Keep fixtures under the build directory for inspection; no recursive cleanup.
    std::cout << "PASS real NTFS scan, sparse/compressed files, Unicode, hard links, moves, locks, access denial, SQLite persistence and rollback\n";
}
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    try {
        diff_tests();
        history_tests();
        integration_tests();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
