#pragma once

#include "model.hpp"

struct sqlite3;

namespace spaceledger {
class Database {
public:
    explicit Database(const fs::path& path, bool writable = false);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    std::int64_t save(const Snapshot& snapshot);
    Snapshot load(std::int64_t id) const;
    std::vector<Snapshot> list() const;
private:
    sqlite3* db_ = nullptr;
    bool writable_ = false;
};
}
