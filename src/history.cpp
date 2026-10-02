#include "spaceledger/history.hpp"
#include "spaceledger/windows.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <set>
#include <stdexcept>

namespace spaceledger {
namespace {
bool same_root(const Snapshot& a, const Snapshot& b) {
    return a.volume == b.volume && same_path(fs::path(wide(a.root)), fs::path(wide(b.root)));
}
}

std::int64_t parse_duration_hours(const std::wstring& value) {
    if (value.size() < 2) throw std::runtime_error("Duration must look like 24h, 7d or 2w");
    std::int64_t multiplier = 0;
    switch (value.back()) {
    case L'h': multiplier = 1; break;
    case L'd': multiplier = 24; break;
    case L'w': multiplier = 24 * 7; break;
    default: throw std::runtime_error("Duration must end in h, d or w");
    }
    std::int64_t number = 0;
    for (std::size_t i = 0; i + 1 < value.size(); ++i) {
        if (value[i] < L'0' || value[i] > L'9') throw std::runtime_error("Duration must be positive");
        const auto digit = static_cast<std::int64_t>(value[i] - L'0');
        if (number > (std::numeric_limits<std::int64_t>::max() / multiplier - digit) / 10)
            throw std::runtime_error("Duration is too large");
        number = number * 10 + digit;
    }
    if (number == 0) throw std::runtime_error("Duration must be positive");
    return number * multiplier;
}

std::string utc_hours_ago(std::int64_t hours) {
    if (hours <= 0 || hours > 100 * 365 * 24) throw std::runtime_error("Duration must be within 100 years");
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER ticks{};
    ticks.LowPart = now.dwLowDateTime;
    ticks.HighPart = now.dwHighDateTime;
    const auto elapsed = static_cast<ULONGLONG>(hours) * 3600ULL * 10000000ULL;
    if (ticks.QuadPart <= elapsed) throw std::runtime_error("Duration exceeds system clock");
    ticks.QuadPart -= elapsed;
    FILETIME past{};
    past.dwLowDateTime = ticks.LowPart;
    past.dwHighDateTime = ticks.HighPart;
    SYSTEMTIME time{};
    if (!FileTimeToSystemTime(&past, &time)) throw std::runtime_error("Cannot compute UTC cutoff");
    char buffer[40]{};
    std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                  time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    return buffer;
}

Snapshot latest_snapshot(const std::vector<Snapshot>& headers, const std::optional<fs::path>& root) {
    const Snapshot* latest = nullptr;
    for (const auto& s : headers) {
        if (root && !same_path(fs::path(wide(s.root)), *root)) continue;
        if (!latest || s.id > latest->id) latest = &s;
    }
    if (!latest) throw std::runtime_error("No snapshots for the selected root");
    if (!root) {
        for (const auto& s : headers) {
            if (!same_root(s, *latest)) throw std::runtime_error("History contains multiple roots or volumes; specify --path");
        }
    }
    return *latest;
}

HistoryPair since_pair(const std::vector<Snapshot>& headers, const std::optional<fs::path>& root,
                       const std::string& cutoff_utc) {
    const auto newest = latest_snapshot(headers, root);
    if (newest.finished < cutoff_utc) throw std::runtime_error("No snapshot in the requested interval");
    const Snapshot* baseline = nullptr;
    for (const auto& s : headers) {
        if (!same_root(s, newest) || s.id >= newest.id || s.finished > cutoff_utc) continue;
        if (!baseline || s.finished > baseline->finished || (s.finished == baseline->finished && s.id > baseline->id))
            baseline = &s;
    }
    if (!baseline) throw std::runtime_error("No baseline snapshot at or before the requested interval");
    return {baseline->id, newest.id};
}

std::vector<std::int64_t> retention_candidates(const std::vector<Snapshot>& headers,
    const std::optional<fs::path>& root, std::size_t keep,
    const std::function<bool(std::int64_t)>& has_coverage_issues) {
    if (keep < 2) throw std::runtime_error("Retention must keep at least two snapshots");
    const auto newest = latest_snapshot(headers, root);
    std::vector<std::int64_t> ids;
    for (const auto& s : headers) if (same_root(s, newest)) ids.push_back(s.id);
    std::sort(ids.begin(), ids.end());
    if (ids.size() <= keep) return {};
    // Keep a prior scan without recorded coverage gaps when the newest N have them.
    std::int64_t last_complete = 0;
    for (auto it = ids.rbegin(); it != ids.rend(); ++it) {
        if (!has_coverage_issues(*it)) { last_complete = *it; break; }
    }
    std::vector<std::int64_t> candidates;
    for (std::size_t i = 0; i < ids.size() - keep; ++i)
        if (ids[i] != last_complete) candidates.push_back(ids[i]);
    return candidates;
}
}
