#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace spaceledger {
namespace fs = std::filesystem;
using Bytes = std::int64_t;

struct Entry {
    std::string path; // UTF-8, relative to the root, '/' separators; root is "".
    std::string identity; // Volume serial + 128-bit file ID + creation time.
    bool directory = false;
    Bytes logical = -1;
    Bytes allocated = -1;
    std::uint32_t attributes = 0;
    std::uint32_t links = 0;
    std::int64_t last_write = 0;
    std::string status = "observed";
    std::uint32_t error = 0;
};

struct Snapshot {
    std::int64_t id = 0;
    std::string started;
    std::string finished;
    std::string root;
    std::string volume;
    Bytes volume_total = 0;
    Bytes free_before = 0;
    Bytes free_after = 0;
    std::vector<Entry> entries;
};

struct Totals {
    Bytes logical = 0;
    Bytes allocated = 0;
    std::size_t unique_files = 0;
    std::size_t observed_paths = 0;
    std::size_t issues = 0;
    std::size_t excluded = 0;
};

struct Change {
    std::string kind;
    std::string old_path;
    std::string new_path;
    Bytes logical_delta = 0;
    Bytes allocated_delta = 0;
    bool known = true;
};

struct FolderDelta {
    std::string path;
    Bytes logical = 0;
    Bytes allocated = 0;
};

struct Diff {
    std::vector<Change> changes;
    std::vector<FolderDelta> folders;
    Bytes logical_delta = 0;
    Bytes allocated_delta = 0;
    std::size_t uncertain = 0;
};

struct ScanProgress {
    std::size_t entries = 0;
    std::size_t directories = 0;
    std::size_t issues = 0;
    std::string current_path;
};

class ScanCancelled : public std::runtime_error {
public:
    ScanCancelled() : std::runtime_error("Scan cancelled; no snapshot saved") {}
};

Snapshot scan(const fs::path& root, const std::vector<fs::path>& exclusions = {},
              const std::function<void(const ScanProgress&)>& on_progress = {},
              const std::function<bool()>& cancelled = {});
Totals totals(const Snapshot& snapshot);
Diff compare(const Snapshot& before, const Snapshot& after);
bool path_within(const std::string& path, const std::string& parent);
}
