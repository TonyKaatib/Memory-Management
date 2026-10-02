#include "spaceledger/model.hpp"
#include "spaceledger/windows.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace spaceledger {
namespace {
struct FindHandle {
    HANDLE value;
    ~FindHandle() { if (value != INVALID_HANDLE_VALUE) FindClose(value); }
};

void failed(Entry& entry, DWORD error) {
    entry.status = "error";
    entry.error = error;
    entry.logical = -1;
    entry.allocated = -1;
}

Entry inspect(const fs::path& path, const std::string& relative, DWORD attributes) {
    Entry entry;
    entry.path = relative;
    entry.attributes = attributes;
    entry.directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    // Do not open cloud/reparse entries at all, including for content access.
    constexpr DWORD skip = FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE |
                           FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS;
    if (attributes & skip) {
        entry.status = "skipped_reparse_or_cloud";
        return entry;
    }
    Handle handle(CreateFileW(extended(path).c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!handle) { failed(entry, GetLastError()); return entry; }

    FILE_ATTRIBUTE_TAG_INFO tag{};
    FILE_ID_INFO id{};
    FILE_BASIC_INFO basic{};
    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &tag, sizeof(tag))) {
        failed(entry, GetLastError()); return entry;
    }
    entry.attributes = tag.FileAttributes;
    entry.directory = (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (tag.FileAttributes & skip) { entry.status = "skipped_reparse_or_cloud"; return entry; }
    if (!GetFileInformationByHandleEx(handle.get(), FileIdInfo, &id, sizeof(id)) ||
        !GetFileInformationByHandleEx(handle.get(), FileBasicInfo, &basic, sizeof(basic)) ||
        !GetFileInformationByHandleEx(handle.get(), FileStandardInfo, &standard, sizeof(standard))) {
        failed(entry, GetLastError()); return entry;
    }
    std::ostringstream identity;
    identity << std::hex << id.VolumeSerialNumber << ':';
    for (const auto byte : id.FileId.Identifier) identity << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    identity << ':' << basic.CreationTime.QuadPart;
    entry.identity = identity.str();
    entry.links = standard.NumberOfLinks;
    entry.last_write = basic.LastWriteTime.QuadPart;
    entry.logical = entry.directory ? 0 : standard.EndOfFile.QuadPart;
    entry.allocated = entry.directory ? 0 : standard.AllocationSize.QuadPart;
    if (!entry.directory && (tag.FileAttributes & (FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE))) {
        FILE_COMPRESSION_INFO compression{};
        if (!GetFileInformationByHandleEx(handle.get(), FileCompressionInfo, &compression, sizeof(compression))) {
            failed(entry, GetLastError()); return entry;
        }
        entry.allocated = compression.CompressedFileSize.QuadPart;
    }
    return entry;
}

Bytes free_space(const fs::path& root, Bytes& total) {
    ULARGE_INTEGER total_bytes{}, free_bytes{};
    if (!GetDiskFreeSpaceExW(extended(root).c_str(), nullptr, &total_bytes, &free_bytes))
        throw std::runtime_error("Cannot measure volume space: " + windows_error(GetLastError()));
    total = static_cast<Bytes>(total_bytes.QuadPart);
    return static_cast<Bytes>(free_bytes.QuadPart);
}
}

Snapshot scan(const fs::path& input, const std::vector<fs::path>& exclusions) {
    auto root = absolute_path(input);
    if (root.native().starts_with(L"\\\\")) throw std::runtime_error("This prototype supports local drive paths only");
    const auto attributes = GetFileAttributesW(extended(root).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) throw std::runtime_error(windows_error(GetLastError()));
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) throw std::runtime_error("Scan root must be a directory");
    auto first = inspect(root, "", attributes);
    if (first.status != "observed") throw std::runtime_error("Scan root is inaccessible or a reparse/cloud entry");

    // Resolve ancestor aliases so comparisons consistently refer to the same tree.
    Handle root_handle(CreateFileW(extended(root).c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!root_handle) throw std::runtime_error(windows_error(GetLastError()));
    std::wstring final_path(32768, L'\0');
    const auto length = GetFinalPathNameByHandleW(root_handle.get(), final_path.data(), static_cast<DWORD>(final_path.size()), FILE_NAME_NORMALIZED);
    if (!length || length >= final_path.size()) throw std::runtime_error("Cannot resolve scan root");
    final_path.resize(length);
    if (final_path.starts_with(L"\\\\?\\")) final_path.erase(0, 4);
    root = fs::path(final_path).lexically_normal();

    wchar_t mount[32768]{}, volume_name[64]{}, filesystem[32]{};
    if (!GetVolumePathNameW(root.c_str(), mount, static_cast<DWORD>(std::size(mount))) ||
        !GetVolumeNameForVolumeMountPointW(mount, volume_name, static_cast<DWORD>(std::size(volume_name))) ||
        !GetVolumeInformationW(mount, nullptr, 0, nullptr, nullptr, nullptr, filesystem, static_cast<DWORD>(std::size(filesystem))))
        throw std::runtime_error("Cannot identify volume: " + windows_error(GetLastError()));
    if (std::wstring(filesystem) != L"NTFS") throw std::runtime_error("This prototype supports NTFS volumes only");

    Snapshot result;
    result.started = utc_now();
    result.root = utf8(root.native());
    result.volume = utf8(volume_name);
    result.free_before = free_space(root, result.volume_total);
    result.entries.push_back(std::move(first));
    std::vector<std::pair<fs::path, std::size_t>> pending{{root, 0}};
    std::unordered_set<std::string> visited{result.entries.front().identity};

    while (!pending.empty()) {
        auto [directory, index] = std::move(pending.back());
        pending.pop_back();
        const auto relative = result.entries[index].path;
        WIN32_FIND_DATAW data{};
        FindHandle find{FindFirstFileExW((extended(directory) + L"\\*").c_str(), FindExInfoBasic, &data,
                                       FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH)};
        if (find.value == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND) failed(result.entries[index], error);
            continue;
        }
        do {
            const std::wstring name(data.cFileName);
            if (name == L"." || name == L"..") continue;
            const auto path = directory / name;
            const auto rel = relative.empty() ? utf8(name) : relative + '/' + utf8(name);
            Entry entry;
            if (std::any_of(exclusions.begin(), exclusions.end(), [&](const auto& excluded) { return same_path(path, excluded); })) {
                entry.path = rel;
                entry.directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                entry.status = "excluded";
            } else {
                entry = inspect(path, rel, data.dwFileAttributes);
            }
            if (entry.directory && entry.status == "observed") {
                if (visited.insert(entry.identity).second) pending.emplace_back(path, result.entries.size());
                else entry.status = "skipped_duplicate_directory";
            }
            result.entries.push_back(std::move(entry));
        } while (FindNextFileW(find.value, &data));
        const auto error = GetLastError();
        if (error != ERROR_NO_MORE_FILES) failed(result.entries[index], error);
    }
    result.free_after = free_space(root, result.volume_total);
    result.finished = utc_now();
    std::sort(result.entries.begin(), result.entries.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
    return result;
}
}
