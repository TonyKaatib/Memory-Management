#include "spaceledger/model.hpp"
#include "spaceledger/windows.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace spaceledger {
namespace {
struct File {
    std::set<std::string> paths;
    Bytes logical = 0;
    Bytes allocated = 0;
    bool consistent = true;
};
using Files = std::map<std::string, File>;

Files files(const Snapshot& snapshot) {
    Files result;
    for (const auto& entry : snapshot.entries) {
        if (entry.directory || entry.status != "observed") continue;
        auto [it, inserted] = result.try_emplace(entry.identity);
        auto& file = it->second;
        if (!inserted && (file.logical != entry.logical || file.allocated != entry.allocated)) file.consistent = false;
        file.paths.insert(entry.path);
        file.logical = entry.logical;
        file.allocated = entry.allocated;
        if (entry.identity.empty() || entry.logical < 0 || entry.allocated < 0) file.consistent = false;
    }
    return result;
}

struct OrdinalLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
    }
};

struct Coverage {
    std::set<std::wstring, OrdinalLess> paths;
    std::set<std::wstring, OrdinalLess> directories;
    explicit Coverage(const Snapshot& snapshot) {
        for (const auto& e : snapshot.entries) {
            if (e.status == "observed") continue;
            paths.insert(wide(e.path));
            if (e.directory) directories.insert(wide(e.path));
        }
    }
};

bool obscured(const File& file, const Coverage& other) {
    if (other.paths.empty()) return false;
    for (const auto& path : file.paths) {
        auto current = wide(path);
        if (other.paths.contains(current)) return true;
        for (;;) {
            const auto slash = current.rfind(L'/');
            current = slash == std::wstring::npos ? L"" : current.substr(0, slash);
            if (other.directories.contains(current)) return true;
            if (current.empty()) break;
        }
    }
    return false;
}

void attribute(std::map<std::string, FolderDelta>& folders, const File& file, int direction) {
    // One deterministic owner path avoids charging shared hard-linked data twice.
    auto path = *file.paths.begin();
    for (;;) {
        const auto slash = path.rfind('/');
        path = slash == std::string::npos ? "" : path.substr(0, slash);
        auto& folder = folders[path];
        folder.path = path;
        folder.logical += direction * file.logical;
        folder.allocated += direction * file.allocated;
        if (path.empty()) break;
    }
}
}

bool path_within(const std::string& path, const std::string& parent) {
    if (parent.empty()) return true;
    const auto p = wide(path);
    const auto base = wide(parent);
    if (p.size() < base.size()) return false;
    if (CompareStringOrdinal(p.data(), static_cast<int>(base.size()), base.data(), static_cast<int>(base.size()), TRUE) != CSTR_EQUAL)
        return false;
    return p.size() == base.size() || p[base.size()] == L'/';
}

Totals totals(const Snapshot& snapshot) {
    Totals result;
    for (const auto& e : snapshot.entries) {
        if (e.status == "excluded") ++result.excluded;
        else if (e.status != "observed") ++result.issues;
        else if (!e.directory) ++result.observed_paths;
    }
    for (const auto& [identity, file] : files(snapshot)) {
        (void)identity;
        if (!file.consistent) { ++result.issues; continue; }
        ++result.unique_files;
        result.logical += file.logical;
        result.allocated += file.allocated;
    }
    return result;
}

Diff compare(const Snapshot& before, const Snapshot& after) {
    if (before.volume != after.volume || !same_path(fs::path(wide(before.root)), fs::path(wide(after.root))))
        throw std::runtime_error("Snapshots must have the same root and volume");
    if (before.started > after.started) throw std::runtime_error("The first snapshot must precede the second");
    const auto old_files = files(before);
    const auto new_files = files(after);
    const Coverage old_coverage(before), new_coverage(after);
    std::set<std::string> identities;
    for (const auto& [id, file] : old_files) { (void)file; identities.insert(id); }
    for (const auto& [id, file] : new_files) { (void)file; identities.insert(id); }
    Diff result;
    std::map<std::string, FolderDelta> folders;
    for (const auto& id : identities) {
        const auto old_it = old_files.find(id), new_it = new_files.find(id);
        const File* old_file = old_it == old_files.end() ? nullptr : &old_it->second;
        const File* new_file = new_it == new_files.end() ? nullptr : &new_it->second;
        Change change;
        if (old_file) change.old_path = *old_file->paths.begin();
        if (new_file) change.new_path = *new_file->paths.begin();
        if ((old_file && (!old_file->consistent || obscured(*old_file, new_coverage))) ||
            (new_file && (!new_file->consistent || obscured(*new_file, old_coverage)))) {
            change.kind = "unavailable";
            change.known = false;
            ++result.uncertain;
            result.changes.push_back(std::move(change));
            continue;
        }
        if (old_file) {
            change.logical_delta -= old_file->logical;
            change.allocated_delta -= old_file->allocated;
            attribute(folders, *old_file, -1);
        }
        if (new_file) {
            change.logical_delta += new_file->logical;
            change.allocated_delta += new_file->allocated;
            attribute(folders, *new_file, 1);
        }
        result.logical_delta += change.logical_delta;
        result.allocated_delta += change.allocated_delta;
        if (!old_file) change.kind = "added";
        else if (!new_file) change.kind = "removed";
        else {
            if (old_file->paths != new_file->paths) {
                change.kind = old_file->paths.size() == 1 && new_file->paths.size() == 1 ? "moved/renamed" : "links changed";
            }
            if (change.logical_delta || change.allocated_delta) {
                if (!change.kind.empty()) change.kind += "+";
                const auto delta = change.allocated_delta ? change.allocated_delta : change.logical_delta;
                change.kind += delta > 0 ? "grew" : "shrank";
            }
            if (change.kind.empty()) continue;
        }
        result.changes.push_back(std::move(change));
    }
    for (auto& [path, folder] : folders) {
        (void)path;
        if (folder.logical || folder.allocated) result.folders.push_back(std::move(folder));
    }
    const auto magnitude = [](Bytes value) { return value < 0 ? -value : value; };
    std::sort(result.changes.begin(), result.changes.end(), [&](const auto& a, const auto& b) {
        if (magnitude(a.allocated_delta) != magnitude(b.allocated_delta)) return magnitude(a.allocated_delta) > magnitude(b.allocated_delta);
        if (magnitude(a.logical_delta) != magnitude(b.logical_delta)) return magnitude(a.logical_delta) > magnitude(b.logical_delta);
        return std::tie(a.old_path, a.new_path) < std::tie(b.old_path, b.new_path);
    });
    std::sort(result.folders.begin(), result.folders.end(), [&](const auto& a, const auto& b) {
        if (magnitude(a.allocated) != magnitude(b.allocated)) return magnitude(a.allocated) > magnitude(b.allocated);
        if (magnitude(a.logical) != magnitude(b.logical)) return magnitude(a.logical) > magnitude(b.logical);
        return a.path < b.path;
    });
    return result;
}
}
