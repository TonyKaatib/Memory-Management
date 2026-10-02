#include "spaceledger/database.hpp"
#include "spaceledger/windows.hpp"

#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>

using namespace spaceledger;

namespace {
constexpr const char* help = R"HELP(SpaceLedger 0.1 - storage change snapshots for Windows NTFS

  spaceledger scan <directory> [--database <history.db>]
  spaceledger scans [--database <history.db>]
  spaceledger diff --from <id> --to <id> [--limit <rows>] [--database <history.db>]
  spaceledger issues --scan <id> [--database <history.db>]

Default database: .spaceledger/history.db in the current directory.
Use C:\ for a drive root (C: is drive-relative and is rejected).
Sizes are exact bytes; positive deltas mean growth. Times are UTC.
Monitored files are queried for metadata only. Scan writes its history database.
Reparse/cloud entries are skipped and reported. No elevation is required.
Exit codes: 0 success/help, 1 error, 2 scan stored with coverage issues.
)HELP";

std::string size_text(Bytes bytes, bool signed_value = false) {
    std::ostringstream result;
    if (signed_value && bytes > 0) result << '+';
    result << bytes << " B";
    return result.str();
}

std::int64_t positive(const std::wstring& value, const char* name) {
    if (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos)
        throw std::runtime_error(std::string(name) + " must be a positive integer");
    std::size_t used = 0;
    const auto result = std::stoll(value, &used);
    if (used != value.size() || result < 1) throw std::runtime_error(std::string(name) + " must be a positive integer");
    return result;
}

void print_totals(const Snapshot& snapshot) {
    const auto total = totals(snapshot);
    std::cout << "Root: " << snapshot.root << '\n'
              << "Observed files: " << total.unique_files << " unique, " << total.observed_paths << " paths\n"
              << "Logical:   " << size_text(total.logical) << '\n'
              << "Allocated: " << size_text(total.allocated) << '\n'
              << "Coverage issues: " << total.issues << "; own database paths excluded: " << total.excluded << '\n';
}
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    try {
        if (argc < 2 || std::wstring(argv[1]) == L"--help" || std::wstring(argv[1]) == L"help") {
            std::cout << help;
            return 0;
        }
        const std::wstring command = argv[1];
        if (command != L"scan" && command != L"scans" && command != L"diff" && command != L"issues")
            throw std::runtime_error("Unknown command. Use --help.");
        std::map<std::wstring, std::wstring> options;
        fs::path root;
        for (int i = 2; i < argc; ++i) {
            const std::wstring arg = argv[i];
            const bool allowed = arg == L"--database" || (command == L"diff" && (arg == L"--from" || arg == L"--to" || arg == L"--limit")) ||
                                 (command == L"issues" && arg == L"--scan");
            if (allowed) {
                if (i + 1 >= argc || options.contains(arg)) throw std::runtime_error("Missing or repeated option: " + utf8(arg));
                options[arg] = argv[++i];
            } else if (command == L"scan" && root.empty() && !arg.starts_with(L"--")) root = arg;
            else throw std::runtime_error("Unexpected argument: " + utf8(arg));
        }
        const auto required = [&](const wchar_t* key) -> std::wstring {
            if (!options.contains(key)) throw std::runtime_error("Required option: " + utf8(key));
            return options.at(key);
        };
        auto database_path = absolute_path(options.contains(L"--database") ? fs::path(options.at(L"--database")) : fs::path(L".spaceledger/history.db"));
        database_path = fs::weakly_canonical(database_path);

        if (command == L"scan") {
            if (root.empty()) throw std::runtime_error("scan requires a directory");
            // Validate the root before creating database directories.
            root = absolute_path(root);
            const DWORD attr = GetFileAttributesW(extended(root).c_str());
            if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) throw std::runtime_error("Scan root is not an accessible directory");
            const std::vector<fs::path> exclusions{database_path, database_path.native() + L"-journal",
                                                  database_path.native() + L"-wal", database_path.native() + L"-shm"};
            std::cout << "Scanning " << utf8(root.native()) << " ...\n" << std::flush;
            auto snapshot = scan(root, exclusions);
            fs::create_directories(database_path.parent_path());
            Database database(database_path, true);
            snapshot.id = database.save(snapshot);
            std::cout << "Saved snapshot " << snapshot.id << " (" << snapshot.started << " to " << snapshot.finished << ")\n";
            print_totals(snapshot);
            std::cout << "Database: " << utf8(database_path.native()) << '\n';
            if (totals(snapshot).issues) {
                std::cout << "Use issues --scan " << snapshot.id << " with the same database to inspect coverage.\n";
                return 2;
            }
            return 0;
        }

        Database database(database_path);
        if (command == L"scans") {
            const auto snapshots = database.list();
            if (snapshots.empty()) std::cout << "No snapshots stored.\n";
            for (const auto& s : snapshots)
                std::cout << s.id << "  " << s.started << "  " << s.root << '\n';
            return 0;
        }
        if (command == L"issues") {
            const auto snapshot = database.load(positive(required(L"--scan"), "--scan"));
            print_totals(snapshot);
            for (const auto& entry : snapshot.entries) {
                if (entry.status == "observed") continue;
                std::cout << entry.status << "  " << (entry.path.empty() ? "." : entry.path);
                if (entry.error) std::cout << "  " << windows_error(entry.error);
                std::cout << '\n';
            }
            return 0;
        }

        const auto from = positive(required(L"--from"), "--from");
        const auto to = positive(required(L"--to"), "--to");
        if (from >= to) throw std::runtime_error("--from must be an earlier snapshot ID than --to");
        const auto limit = options.contains(L"--limit") ? positive(options.at(L"--limit"), "--limit") : 25;
        const auto before = database.load(from), after = database.load(to);
        const auto diff = compare(before, after);
        std::cout << "Snapshots " << from << " -> " << to << "  " << after.root << '\n'
                  << "Scan intervals: " << before.started << ".." << before.finished << " -> " << after.started << ".." << after.finished << '\n'
                  << "Comparable logical delta:   " << size_text(diff.logical_delta, true) << '\n'
                  << "Comparable allocated delta: " << size_text(diff.allocated_delta, true) << '\n'
                  << "Whole-volume free-space change (context only): " << size_text(after.free_after - before.free_after, true) << '\n'
                  << "Coverage issues before/after: " << totals(before).issues << '/' << totals(after).issues
                  << "; unavailable comparisons: " << diff.uncertain << "\n\n"
                  << "Folder changes (allocated | logical | relative path; nested rows overlap):\n";
        std::int64_t count = 0;
        for (const auto& folder : diff.folders) {
            if (count++ >= limit) break;
            std::cout << size_text(folder.allocated, true) << " | " << size_text(folder.logical, true) << " | " << (folder.path.empty() ? "." : folder.path) << '\n';
        }
        std::cout << "\nFile changes (allocated | logical | kind | path):\n";
        count = 0;
        for (const auto& change : diff.changes) {
            if (count++ >= limit) break;
            std::cout << (change.known ? size_text(change.allocated_delta, true) : "unknown") << " | "
                      << (change.known ? size_text(change.logical_delta, true) : "unknown") << " | " << change.kind << " | ";
            if (!change.old_path.empty() && !change.new_path.empty() && change.old_path != change.new_path)
                std::cout << change.old_path << " -> " << change.new_path;
            else std::cout << (change.new_path.empty() ? change.old_path : change.new_path);
            std::cout << '\n';
        }
        if (diff.changes.empty()) std::cout << "No observed file size or path changes.\n";
        std::cout << "\n" << diff.changes.size() << " file changes; showing up to " << limit << " rows per section.\n"
                  << "Added/removed means entered/left the observed tree, not necessarily created/deleted.\n"
                  << "Hard links are counted once, attributed to their first sorted observed path.\n"
                  << "Live scans are not atomic; coverage and whole-volume totals may differ.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SpaceLedger: " << error.what() << '\n';
        return 1;
    }
}
