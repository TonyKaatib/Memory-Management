#include "spaceledger/database.hpp"
#include "spaceledger/windows.hpp"
#include <winsqlite/winsqlite3.h>

#include <limits>
#include <stdexcept>

namespace spaceledger {
namespace {
void execute(sqlite3* db, const char* sql) {
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db));
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Statement() { sqlite3_finalize(stmt_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    void number(int column, std::int64_t value) { check(sqlite3_bind_int64(stmt_, column, value)); }
    void string(int column, const std::string& value) {
        if (value.size() > static_cast<std::size_t>(INT_MAX)) throw std::runtime_error("Database text too large");
        check(sqlite3_bind_text(stmt_, column, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT));
    }
    bool next() {
        const auto result = sqlite3_step(stmt_);
        if (result == SQLITE_ROW) return true;
        if (result != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_));
        return false;
    }
    void reset() { check(sqlite3_reset(stmt_)); check(sqlite3_clear_bindings(stmt_)); }
    std::int64_t number(int column) const { return sqlite3_column_int64(stmt_, column); }
    std::string string(int column) const {
        const auto* value = sqlite3_column_text(stmt_, column);
        return value ? reinterpret_cast<const char*>(value) : "";
    }
private:
    void check(int code) { if (code != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db_)); }
    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;
};

Snapshot read_snapshot(const Statement& row) {
    Snapshot s;
    s.id = row.number(0);
    s.started = row.string(1);
    s.finished = row.string(2);
    s.root = row.string(3);
    s.volume = row.string(4);
    s.volume_total = row.number(5);
    s.free_before = row.number(6);
    s.free_after = row.number(7);
    return s;
}
}

Database::Database(const fs::path& path, bool writable) : writable_(writable) {
    const auto filename = utf8(absolute_path(path).native());
    const auto flags = writable ? SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE : SQLITE_OPEN_READONLY;
    const int code = sqlite3_open_v2(filename.c_str(), &db_, flags, nullptr);
    if (code != SQLITE_OK) {
        const std::string message = db_ ? sqlite3_errmsg(db_) : "Cannot allocate SQLite connection";
        sqlite3_close(db_); db_ = nullptr;
        throw std::runtime_error("Cannot open history database: " + message);
    }
    try {
        sqlite3_busy_timeout(db_, 5000);
        execute(db_, "PRAGMA foreign_keys=ON;");
        int version = 0;
        { Statement query(db_, "PRAGMA user_version"); query.next(); version = static_cast<int>(query.number(0)); }
        if (version == 0 && writable) {
            Statement existing(db_, "SELECT count(*) FROM sqlite_master WHERE name NOT LIKE 'sqlite_%'");
            existing.next();
            if (existing.number(0) != 0) throw std::runtime_error("Refusing to initialize a non-SpaceLedger database");
            execute(db_, R"SQL(
                BEGIN IMMEDIATE;
                CREATE TABLE scans (
                    id INTEGER PRIMARY KEY, started TEXT NOT NULL, finished TEXT NOT NULL,
                    root TEXT NOT NULL, volume TEXT NOT NULL, volume_total INTEGER NOT NULL,
                    free_before INTEGER NOT NULL, free_after INTEGER NOT NULL);
                CREATE TABLE entries (
                    scan_id INTEGER NOT NULL REFERENCES scans(id), path TEXT NOT NULL,
                    identity TEXT NOT NULL, directory INTEGER NOT NULL,
                    logical INTEGER NOT NULL, allocated INTEGER NOT NULL,
                    attributes INTEGER NOT NULL, links INTEGER NOT NULL, last_write INTEGER NOT NULL,
                    status TEXT NOT NULL, error INTEGER NOT NULL,
                    PRIMARY KEY (scan_id, path));
                CREATE INDEX entries_identity ON entries(scan_id, identity);
                PRAGMA application_id=1397507143;
                PRAGMA user_version=1;
                COMMIT;
            )SQL");
            version = 1;
        }
        Statement app(db_, "PRAGMA application_id"); app.next();
        if (version != 1 || app.number(0) != 1397507143)
            throw std::runtime_error("Unsupported database format; expected SpaceLedger schema 1");
    } catch (...) {
        sqlite3_close(db_); db_ = nullptr;
        throw;
    }
}

Database::~Database() { sqlite3_close(db_); }

std::int64_t Database::save(const Snapshot& s) {
    if (!writable_) throw std::runtime_error("Database opened read-only");
    execute(db_, "BEGIN IMMEDIATE");
    try {
        Statement scan_row(db_, "INSERT INTO scans(started,finished,root,volume,volume_total,free_before,free_after) VALUES(?,?,?,?,?,?,?)");
        scan_row.string(1, s.started); scan_row.string(2, s.finished); scan_row.string(3, s.root); scan_row.string(4, s.volume);
        scan_row.number(5, s.volume_total); scan_row.number(6, s.free_before); scan_row.number(7, s.free_after);
        scan_row.next();
        const auto id = sqlite3_last_insert_rowid(db_);
        Statement row(db_, "INSERT INTO entries VALUES(?,?,?,?,?,?,?,?,?,?,?)");
        for (const auto& e : s.entries) {
            row.number(1, id); row.string(2, e.path); row.string(3, e.identity); row.number(4, e.directory);
            row.number(5, e.logical); row.number(6, e.allocated); row.number(7, e.attributes); row.number(8, e.links);
            row.number(9, e.last_write); row.string(10, e.status); row.number(11, e.error);
            row.next(); row.reset();
        }
        execute(db_, "COMMIT");
        return id;
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}

Snapshot Database::load(std::int64_t id) const {
    Statement header(db_, "SELECT id,started,finished,root,volume,volume_total,free_before,free_after FROM scans WHERE id=?");
    header.number(1, id);
    if (!header.next()) throw std::runtime_error("Snapshot " + std::to_string(id) + " does not exist");
    auto snapshot = read_snapshot(header);
    Statement rows(db_, "SELECT path,identity,directory,logical,allocated,attributes,links,last_write,status,error FROM entries WHERE scan_id=? ORDER BY path");
    rows.number(1, id);
    while (rows.next()) {
        Entry e;
        e.path = rows.string(0); e.identity = rows.string(1); e.directory = rows.number(2) != 0;
        e.logical = rows.number(3); e.allocated = rows.number(4); e.attributes = static_cast<std::uint32_t>(rows.number(5));
        e.links = static_cast<std::uint32_t>(rows.number(6)); e.last_write = rows.number(7);
        e.status = rows.string(8); e.error = static_cast<std::uint32_t>(rows.number(9));
        snapshot.entries.push_back(std::move(e));
    }
    return snapshot;
}

std::vector<Snapshot> Database::list() const {
    Statement rows(db_, "SELECT id,started,finished,root,volume,volume_total,free_before,free_after FROM scans ORDER BY id");
    std::vector<Snapshot> result;
    while (rows.next()) result.push_back(read_snapshot(rows));
    return result;
}

bool Database::has_coverage_issues(std::int64_t id) const {
    Statement row(db_, "SELECT EXISTS(SELECT 1 FROM entries WHERE scan_id=? AND status NOT IN ('observed','excluded'))");
    row.number(1, id);
    row.next();
    return row.number(0) != 0;
}

void Database::erase(const std::vector<std::int64_t>& ids) {
    if (!writable_) throw std::runtime_error("Database opened read-only");
    if (ids.empty()) return;
    execute(db_, "BEGIN IMMEDIATE");
    try {
        Statement entries(db_, "DELETE FROM entries WHERE scan_id=?");
        Statement scans(db_, "DELETE FROM scans WHERE id=?");
        for (const auto id : ids) {
            if (id < 1) throw std::runtime_error("Invalid snapshot ID");
            entries.number(1, id); entries.next(); entries.reset();
            scans.number(1, id); scans.next(); scans.reset();
            if (sqlite3_changes(db_) != 1) throw std::runtime_error("Snapshot " + std::to_string(id) + " does not exist");
        }
        execute(db_, "COMMIT");
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}

void Database::compact() {
    if (!writable_) throw std::runtime_error("Database opened read-only");
    execute(db_, "VACUUM");
}
}
