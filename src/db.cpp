#include "arcbox/db.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>

#include <sqlite3.h>

#include "arcbox/util.hpp"

namespace arcbox {

static const char* kSnapshotColumns =
    "id,url,canonical_url,status,created_at,started_at,finished_at,"
    "http_status,content_type,title,sha256,size_bytes,warc_path,body_path,"
    "text_path,error";

static std::string column_text(sqlite3_stmt* stmt, int index) {
    const unsigned char* value = sqlite3_column_text(stmt, index);
    return value ? std::string(reinterpret_cast<const char*>(value)) : std::string();
}

static Snapshot read_snapshot(sqlite3_stmt* stmt) {
    Snapshot s;
    s.id = sqlite3_column_int64(stmt, 0);
    s.url = column_text(stmt, 1);
    s.canonical_url = column_text(stmt, 2);
    s.status = column_text(stmt, 3);
    s.created_at = column_text(stmt, 4);
    s.started_at = column_text(stmt, 5);
    s.finished_at = column_text(stmt, 6);
    s.http_status = sqlite3_column_int(stmt, 7);
    s.content_type = column_text(stmt, 8);
    s.title = column_text(stmt, 9);
    s.sha256 = column_text(stmt, 10);
    s.size_bytes = sqlite3_column_int64(stmt, 11);
    s.warc_path = column_text(stmt, 12);
    s.body_path = column_text(stmt, 13);
    s.text_path = column_text(stmt, 14);
    s.error = column_text(stmt, 15);
    return s;
}

static sqlite3_stmt* prepare(sqlite3* db, const std::string& sql) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("sqlite prepare failed: ") +
                                 sqlite3_errmsg(db));
    }
    return stmt;
}

Db::Db(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        std::string message = db_ ? sqlite3_errmsg(db_) : "cannot open database";
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        throw std::runtime_error(message);
    }
    sqlite3_busy_timeout(db_, 5000);
}

Db::~Db() {
    if (db_) {
        sqlite3_close(db_);
    }
}

void Db::exec(const std::string& sql) {
    char* error = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error);
    if (rc != SQLITE_OK) {
        std::string message = error ? error : sqlite3_errmsg(db_);
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

void Db::migrate() {
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA foreign_keys=ON;");
    exec(R"(
        CREATE TABLE IF NOT EXISTS urls (
            id INTEGER PRIMARY KEY,
            url TEXT NOT NULL UNIQUE,
            created_at TEXT NOT NULL
        );
        CREATE TABLE IF NOT EXISTS snapshots (
            id INTEGER PRIMARY KEY,
            url_id INTEGER NOT NULL REFERENCES urls(id),
            url TEXT NOT NULL,
            canonical_url TEXT NOT NULL,
            status TEXT NOT NULL,
            created_at TEXT NOT NULL,
            started_at TEXT,
            finished_at TEXT,
            http_status INTEGER,
            content_type TEXT,
            title TEXT,
            sha256 TEXT,
            size_bytes INTEGER,
            warc_path TEXT,
            body_path TEXT,
            text_path TEXT,
            error TEXT
        );
        CREATE INDEX IF NOT EXISTS idx_snapshots_status
            ON snapshots(status, id);
        CREATE VIRTUAL TABLE IF NOT EXISTS search
            USING fts5(url, title, body);
    )");
}

void Db::recover_stale() {
    exec(R"(
        UPDATE snapshots
        SET status='queued', error='recovered after restart'
        WHERE status='running';
    )");
}

int64_t Db::enqueue(const std::string& url) {
    exec("BEGIN IMMEDIATE");
    int64_t snapshot_id = 0;
    try {
        sqlite3_stmt* stmt = prepare(
            db_, "INSERT OR IGNORE INTO urls(url, created_at) VALUES(?, ?)");
        sqlite3_bind_text(stmt, 1, url.c_str(), -1, SQLITE_TRANSIENT);
        std::string now = now_iso8601();
        sqlite3_bind_text(stmt, 2, now.c_str(), -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            throw std::runtime_error(std::string("insert url failed: ") +
                                     sqlite3_errmsg(db_));
        }

        stmt = prepare(db_, "SELECT id FROM urls WHERE url=?");
        sqlite3_bind_text(stmt, 1, url.c_str(), -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_ROW) {
            sqlite3_finalize(stmt);
            throw std::runtime_error("url row disappeared");
        }
        int64_t url_id = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);

        stmt = prepare(
            db_,
            "INSERT INTO snapshots(url_id,url,canonical_url,status,created_at) "
            "VALUES(?,?,?,'queued',?)");
        sqlite3_bind_int64(stmt, 1, url_id);
        sqlite3_bind_text(stmt, 2, url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, now.c_str(), -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            throw std::runtime_error(std::string("insert snapshot failed: ") +
                                     sqlite3_errmsg(db_));
        }
        snapshot_id = sqlite3_last_insert_rowid(db_);
        sqlite3_finalize(stmt);
        exec("COMMIT");
    } catch (...) {
        try {
            exec("ROLLBACK");
        } catch (...) {
        }
        throw;
    }
    return snapshot_id;
}

std::optional<Snapshot> Db::next_queued() {
    sqlite3_stmt* stmt = prepare(
        db_, std::string("SELECT ") + kSnapshotColumns +
                 " FROM snapshots WHERE status='queued' ORDER BY id LIMIT 1");
    int rc = sqlite3_step(stmt);
    std::optional<Snapshot> result;
    if (rc == SQLITE_ROW) {
        result = read_snapshot(stmt);
    }
    sqlite3_finalize(stmt);
    return result;
}

std::optional<Snapshot> Db::get(int64_t id) {
    sqlite3_stmt* stmt = prepare(
        db_, std::string("SELECT ") + kSnapshotColumns +
                 " FROM snapshots WHERE id=?");
    sqlite3_bind_int64(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    std::optional<Snapshot> result;
    if (rc == SQLITE_ROW) {
        result = read_snapshot(stmt);
    }
    sqlite3_finalize(stmt);
    return result;
}

bool Db::mark_running(int64_t id) {
    sqlite3_stmt* stmt = prepare(
        db_,
        "UPDATE snapshots SET status='running', started_at=? "
        "WHERE id=? AND status='queued'");
    std::string now = now_iso8601();
    sqlite3_bind_text(stmt, 1, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, id);
    int rc = sqlite3_step(stmt);
    bool changed = rc == SQLITE_DONE && sqlite3_changes(db_) > 0;
    sqlite3_finalize(stmt);
    return changed;
}

void Db::finish(int64_t id, const CaptureUpdate& update) {
    exec("BEGIN IMMEDIATE");
    try {
        sqlite3_stmt* stmt = prepare(
            db_,
            "UPDATE snapshots SET status=?, finished_at=?, http_status=?, "
            "content_type=?, title=?, sha256=?, size_bytes=?, warc_path=?, "
            "body_path=?, text_path=?, error=? WHERE id=?");
        sqlite3_bind_text(stmt, 1, update.status.c_str(), -1, SQLITE_TRANSIENT);
        std::string now = now_iso8601();
        sqlite3_bind_text(stmt, 2, now.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, update.http_status);
        sqlite3_bind_text(stmt, 4, update.content_type.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, update.title.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, update.sha256.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 7, update.size_bytes);
        sqlite3_bind_text(stmt, 8, update.warc_path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 9, update.body_path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 10, update.text_path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 11, update.error.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 12, id);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            throw std::runtime_error(std::string("update snapshot failed: ") +
                                     sqlite3_errmsg(db_));
        }

        stmt = prepare(db_, "DELETE FROM search WHERE rowid=?");
        sqlite3_bind_int64(stmt, 1, id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (!update.body_text.empty()) {
            stmt = prepare(db_, "SELECT url FROM snapshots WHERE id=?");
            sqlite3_bind_int64(stmt, 1, id);
            std::string url;
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                url = column_text(stmt, 0);
            }
            sqlite3_finalize(stmt);

            stmt = prepare(
                db_, "INSERT INTO search(rowid,url,title,body) VALUES(?,?,?,?)");
            sqlite3_bind_int64(stmt, 1, id);
            sqlite3_bind_text(stmt, 2, url.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, update.title.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, update.body_text.c_str(), -1, SQLITE_TRANSIENT);
            rc = sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            if (rc != SQLITE_DONE) {
                throw std::runtime_error(std::string("FTS insert failed: ") +
                                         sqlite3_errmsg(db_));
            }
        }
        exec("COMMIT");
    } catch (...) {
        try {
            exec("ROLLBACK");
        } catch (...) {
        }
        throw;
    }
}

std::vector<Snapshot> Db::list(int limit) {
    sqlite3_stmt* stmt = prepare(
        db_, std::string("SELECT ") + kSnapshotColumns +
                 " FROM snapshots ORDER BY id DESC LIMIT ?");
    sqlite3_bind_int(stmt, 1, limit);
    std::vector<Snapshot> out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.push_back(read_snapshot(stmt));
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<Snapshot> Db::search(const std::string& query, int limit) {
    sqlite3_stmt* stmt = prepare(
        db_, "SELECT s.id,s.url,s.canonical_url,s.status,s.created_at,"
             "s.started_at,s.finished_at,s.http_status,s.content_type,s.title,"
             "s.sha256,s.size_bytes,s.warc_path,s.body_path,s.text_path,s.error "
             "FROM search JOIN snapshots s ON s.id=search.rowid "
             "WHERE search MATCH ? ORDER BY rank LIMIT ?");
    sqlite3_bind_text(stmt, 1, query.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);
    std::vector<Snapshot> out;
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ERROR) {
        std::string message = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        throw std::runtime_error("search failed: " + message);
    }
    while (rc == SQLITE_ROW) {
        out.push_back(read_snapshot(stmt));
        rc = sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    return out;
}

int64_t Db::queued_count() {
    sqlite3_stmt* stmt = prepare(
        db_, "SELECT COUNT(*) FROM snapshots WHERE status='queued'");
    int64_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

} // namespace arcbox
