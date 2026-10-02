#pragma once

#include "model.hpp"
#include <optional>

namespace spaceledger {
struct HistoryPair {
    std::int64_t from = 0;
    std::int64_t to = 0;
};

std::int64_t parse_duration_hours(const std::wstring& text);
std::string utc_hours_ago(std::int64_t hours);
Snapshot latest_snapshot(const std::vector<Snapshot>& headers, const std::optional<fs::path>& root);
HistoryPair since_pair(const std::vector<Snapshot>& headers, const std::optional<fs::path>& root,
                       const std::string& cutoff_utc);
std::vector<std::int64_t> retention_candidates(const std::vector<Snapshot>& headers,
    const std::optional<fs::path>& root, std::size_t keep,
    const std::function<bool(std::int64_t)>& has_coverage_issues);
}
