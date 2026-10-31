#include "arcbox/db.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "arcbox/util.hpp"

namespace arcbox {

static const std::vector<std::string> kSnapshotColumnNames = {
    "id",
    "url",
    "canonical_url",
    "status",
    "created_at",
    "started_at",
    "finished_at",
    "http_status",
    "content_type",
    "title",
    "description",
    "site_name",
    "author",
    "published_at",
    "language",
    "favicon_url",
    "sha256",
    "size_bytes",
    "warc_path",
    "body_path",
    "text_path",
    "headers_path",
    "metadata_path",
    "error",
    "priority",
    "attempts",
    "next_attempt_at",
};

static std::string snapshot_columns(const std::string& prefix = {}) {
    std::string out;
    for (std::size_t i = 0; i < kSnapshotColumnNames.size(); ++i) {
        if (i > 0) {
            out.push_back(',');
        }
        out += prefix + kSnapshotColumnNames[i];
    }
    return out;
}

static std::string column_text(sqlite3_stmt* stmt, int index) {
    const unsigned char* value = sqlite3_column_text(stmt, index);
    return value ? std::string(reinterpret_cast<const char*>(value))
                 : std::string();
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
    s.description = column_text(stmt, 10);
    s.site_name = column_text(stmt, 11);
    s.author = column_text(stmt, 12);
    s.published_at = column_text(stmt, 13);
    s.language = column_text(stmt, 14);
    s.favicon_url = column_text(stmt, 15);
    s.sha256 = column_text(stmt, 16);
    s.size_bytes = sqlite3_column_int64(stmt, 17);
    s.warc_path = column_text(stmt, 18);
    s.body_path = column_text(stmt, 19);
    s.text_path = column_text(stmt, 20);
    s.headers_path = column_text(stmt, 21);
    s.metadata_path = column_text(stmt, 22);
    s.error = column_text(stmt, 23);
    s.priority = sqlite3_column_int(stmt, 24);
    s.attempts = sqlite3_column_int(stmt, 25);
    s.next_attempt_at = sqlite3_column_int64(stmt, 26);
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

static std::string normalize_tag(const std::string& raw) {
    std::string tag = lower(trim(raw));
    while (!tag.empty() && tag.front() == '#') {
        tag.erase(tag.begin());
    }
    if (tag.size() > 128) {
        tag.resize(128);
    }
    return tag;
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

bool Db::has_column(const std::string& table, const std::string& column) {
    sqlite3_stmt* stmt = prepare(db_, "PRAGMA table_info(" + table + ")");
    bool found = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (lower(column_text(stmt, 1)) == lower(column)) {
            found = true;
            break;
        }
    }
    sqlite3_finalize(stmt);
    return found;
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
            description TEXT,
            site_name TEXT,
            author TEXT,
            published_at TEXT,
            language TEXT,
            favicon_url TEXT,
            sha256 TEXT,
            size_bytes INTEGER,
            warc_path TEXT,
            body_path TEXT,
            text_path TEXT,
            headers_path TEXT,
            metadata_path TEXT,
            error TEXT
        );
        CREATE INDEX IF NOT EXISTS idx_snapshots_status
            ON snapshots(status, id);
        CREATE VIRTUAL TABLE IF NOT EXISTS search
            USING fts5(url, title, body);
        CREATE TABLE IF NOT EXISTS tags (
            id INTEGER PRIMARY KEY,
            name TEXT NOT NULL UNIQUE,
            created_at TEXT NOT NULL
        );
        CREATE TABLE IF NOT EXISTS snapshot_tags (
            snapshot_id INTEGER NOT NULL REFERENCES snapshots(id) ON DELETE CASCADE,
            tag_id INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,
            PRIMARY KEY(snapshot_id, tag_id)
        );
        CREATE INDEX IF NOT EXISTS idx_snapshot_tags_tag
            ON snapshot_tags(tag_id, snapshot_id);
        CREATE TABLE IF NOT EXISTS links (
            id INTEGER PRIMARY KEY,
            snapshot_id INTEGER NOT NULL REFERENCES snapshots(id) ON DELETE CASCADE,
            url TEXT NOT NULL,
            text TEXT,
            position INTEGER NOT NULL,
            UNIQUE(snapshot_id, url)
        );
        CREATE INDEX IF NOT EXISTS idx_links_snapshot
            ON links(snapshot_id, position);
    )");

    // Older v0.1 databases predate the extractor and replay metadata columns.
    struct Column {
        const char* name;
        const char* declaration;
    };
    static const Column columns[] = {
        {"description", "TEXT"},
        {"site_name", "TEXT"},
        {"author", "TEXT"},
        {"published_at", "TEXT"},
        {"language", "TEXT"},
        {"favicon_url", "TEXT"},
        {"headers_path", "TEXT"},
        {"metadata_path", "TEXT"},
        // Queue scheduling columns, added for v0.4 retry support.
        {"priority", "INTEGER NOT NULL DEFAULT 0"},
        {"attempts", "INTEGER NOT NULL DEFAULT 0"},
        {"next_attempt_at", "INTEGER"},
    };
    for (const auto& column : columns) {
        if (!has_column("snapshots", column.name)) {
            exec("ALTER TABLE snapshots ADD COLUMN " + std::string(column.name) +
                 " " + column.declaration);
        }
    }
}

void Db::recover_stale() {
    exec(R"(
        UPDATE snapshots
        SET status='queued', error='recovered after restart'
        WHERE status='running';
    )");
}

int64_t Db::enqueue(const std::string& url, int priority) {
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
            "INSERT INTO snapshots(url_id,url,canonical_url,status,created_at,"
            "priority) VALUES(?,?,?,'queued',?,?)");
        sqlite3_bind_int64(stmt, 1, url_id);
        sqlite3_bind_text(stmt, 2, url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, now.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 5, priority);
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
    // Higher priority first, then oldest first. Snapshots with a backoff delay
    // stay invisible until their retry time passes.
    sqlite3_stmt* stmt = prepare(
        db_, "SELECT " + snapshot_columns() +
                 " FROM snapshots WHERE status='queued'"
                 " AND (next_attempt_at IS NULL"
                 "      OR next_attempt_at <= strftime('%s','now'))"
                 " ORDER BY priority DESC, id ASC LIMIT 1");
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
        db_, "SELECT " + snapshot_columns() + " FROM snapshots WHERE id=?");
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
            "content_type=?, title=?, description=?, site_name=?, author=?, "
            "published_at=?, language=?, favicon_url=?, sha256=?, "
            "size_bytes=?, warc_path=?, body_path=?, text_path=?, "
            "headers_path=?, metadata_path=?, "
            "canonical_url=COALESCE(NULLIF(?, ''), canonical_url), "
            "error=? WHERE id=?");
        sqlite3_bind_text(stmt, 1, update.status.c_str(), -1, SQLITE_TRANSIENT);
        std::string now = now_iso8601();
        sqlite3_bind_text(stmt, 2, now.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, update.http_status);
        sqlite3_bind_text(stmt, 4, update.content_type.c_str(), -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, update.title.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, update.description.c_str(), -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, update.site_name.c_str(), -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 8, update.author.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 9, update.published_at.c_str(), -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 10, update.language.c_str(), -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 11, update.favicon_url.c_str(), -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 12, update.sha256.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 13, update.size_bytes);
        sqlite3_bind_text(stmt, 14, update.warc_path.c_str(), -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 15, update.body_path.c_str(), -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 16, update.text_path.c_str(), -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 17, update.headers_path.c_str(), -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 18, update.metadata_path.c_str(), -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 19, update.canonical_url.c_str(), -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 20, update.error.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 21, id);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            throw std::runtime_error(std::string("update snapshot failed: ") +
                                     sqlite3_errmsg(db_));
        }

        stmt = prepare(db_, "DELETE FROM links WHERE snapshot_id=?");
        sqlite3_bind_int64(stmt, 1, id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        for (std::size_t i = 0; i < update.links.size(); ++i) {
            const ExtractedLink& link = update.links[i];
            if (link.url.empty()) {
                continue;
            }
            stmt = prepare(
                db_,
                "INSERT OR IGNORE INTO links(snapshot_id,url,text,position) "
                "VALUES(?,?,?,?)");
            sqlite3_bind_int64(stmt, 1, id);
            sqlite3_bind_text(stmt, 2, link.url.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, link.text.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 4, static_cast<int64_t>(i));
            rc = sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            if (rc != SQLITE_DONE) {
                throw std::runtime_error(std::string("link insert failed: ") +
                                         sqlite3_errmsg(db_));
            }
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
            sqlite3_bind_text(stmt, 3, update.title.c_str(), -1,
                              SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, update.body_text.c_str(), -1,
                              SQLITE_TRANSIENT);
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

bool Db::schedule_retry(int64_t id,
                        const std::string& error,
                        int max_attempts,
                        long backoff_seconds,
                        long backoff_max_seconds) {
    sqlite3_stmt* stmt =
        prepare(db_, "SELECT attempts FROM snapshots WHERE id=?");
    sqlite3_bind_int64(stmt, 1, id);
    int attempts = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        attempts = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    attempts += 1;

    const bool requeue = attempts < max_attempts;
    long delay = 0;
    if (requeue) {
        delay = backoff_seconds;
        for (int i = 1; i < attempts; ++i) {
            delay *= 2;
            if (delay >= backoff_max_seconds) {
                delay = backoff_max_seconds;
                break;
            }
        }
    }

    if (requeue) {
        stmt = prepare(db_,
                       "UPDATE snapshots SET status='queued', attempts=?,"
                       " next_attempt_at=strftime('%s','now') + ?, error=?"
                       " WHERE id=?");
        sqlite3_bind_int(stmt, 1, attempts);
        sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(delay));
        sqlite3_bind_text(stmt, 3, error.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 4, id);
    } else {
        stmt = prepare(db_,
                       "UPDATE snapshots SET status='failed', attempts=?,"
                       " next_attempt_at=NULL, error=? WHERE id=?");
        sqlite3_bind_int(stmt, 1, attempts);
        sqlite3_bind_text(stmt, 2, error.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, id);
    }
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        throw std::runtime_error(std::string("schedule retry failed: ") +
                                 sqlite3_errmsg(db_));
    }
    return requeue;
}

int Db::reset_retry(bool all, int64_t id) {
    sqlite3_stmt* stmt;
    if (all) {
        stmt = prepare(db_,
                       "UPDATE snapshots SET status='queued', attempts=0,"
                       " next_attempt_at=NULL, error='' WHERE status='failed'");
    } else {
        stmt = prepare(db_,
                       "UPDATE snapshots SET status='queued', attempts=0,"
                       " next_attempt_at=NULL, error='' WHERE id=?");
        sqlite3_bind_int64(stmt, 1, id);
    }
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        throw std::runtime_error(std::string("reset retry failed: ") +
                                 sqlite3_errmsg(db_));
    }
    return sqlite3_changes(db_);
}

std::vector<Snapshot> Db::list(int limit, const std::string& tag) {
    std::vector<Snapshot> out;
    sqlite3_stmt* stmt = nullptr;
    std::string normalized = normalize_tag(tag);
    if (normalized.empty()) {
        stmt = prepare(db_, "SELECT " + snapshot_columns() +
                               " FROM snapshots ORDER BY id DESC LIMIT ?");
        sqlite3_bind_int(stmt, 1, limit);
    } else {
        stmt = prepare(
            db_, "SELECT " + snapshot_columns("s.") +
                     " FROM snapshots s "
                     "JOIN snapshot_tags st ON st.snapshot_id=s.id "
                     "JOIN tags t ON t.id=st.tag_id "
                     "WHERE t.name=? ORDER BY s.id DESC LIMIT ?");
        sqlite3_bind_text(stmt, 1, normalized.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, limit);
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.push_back(read_snapshot(stmt));
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<Snapshot> Db::search(const std::string& query,
                                 int limit,
                                 const std::string& tag) {
    std::string normalized = normalize_tag(tag);
    sqlite3_stmt* stmt = nullptr;
    if (normalized.empty()) {
        stmt = prepare(
            db_, "SELECT " + snapshot_columns("s.") +
                     " FROM search JOIN snapshots s ON s.id=search.rowid "
                     "WHERE search MATCH ? ORDER BY rank LIMIT ?");
        sqlite3_bind_text(stmt, 1, query.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, limit);
    } else {
        stmt = prepare(
            db_, "SELECT " + snapshot_columns("s.") +
                     " FROM search JOIN snapshots s ON s.id=search.rowid "
                     "JOIN snapshot_tags st ON st.snapshot_id=s.id "
                     "JOIN tags t ON t.id=st.tag_id "
                     "WHERE search MATCH ? AND t.name=? "
                     "ORDER BY rank LIMIT ?");
        sqlite3_bind_text(stmt, 1, query.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, normalized.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, limit);
    }
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

std::vector<std::string> Db::tags(int64_t id) {
    sqlite3_stmt* stmt = prepare(
        db_,
        "SELECT t.name FROM tags t "
        "JOIN snapshot_tags st ON st.tag_id=t.id "
        "WHERE st.snapshot_id=? ORDER BY t.name");
    sqlite3_bind_int64(stmt, 1, id);
    std::vector<std::string> out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.push_back(column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<std::pair<std::string, int64_t>> Db::all_tags() {
    sqlite3_stmt* stmt = prepare(
        db_,
        "SELECT t.name, COUNT(*) FROM tags t "
        "JOIN snapshot_tags st ON st.tag_id=t.id "
        "GROUP BY t.id ORDER BY t.name");
    std::vector<std::pair<std::string, int64_t>> out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.emplace_back(column_text(stmt, 0),
                         sqlite3_column_int64(stmt, 1));
    }
    sqlite3_finalize(stmt);
    return out;
}

void Db::add_tags(int64_t id,
                  const std::vector<std::string>& tags_to_change,
                  bool remove) {
    exec("BEGIN IMMEDIATE");
    try {
        sqlite3_stmt* check = prepare(db_, "SELECT 1 FROM snapshots WHERE id=?");
        sqlite3_bind_int64(check, 1, id);
        int rc = sqlite3_step(check);
        sqlite3_finalize(check);
        if (rc != SQLITE_ROW) {
            throw std::runtime_error("snapshot not found: " +
                                     std::to_string(id));
        }

        for (const std::string& raw : tags_to_change) {
            std::string tag = normalize_tag(raw);
            if (tag.empty()) {
                continue;
            }

            sqlite3_stmt* stmt =
                prepare(db_, "SELECT id FROM tags WHERE name=?");
            sqlite3_bind_text(stmt, 1, tag.c_str(), -1, SQLITE_TRANSIENT);
            int tag_rc = sqlite3_step(stmt);
            int64_t tag_id = 0;
            if (tag_rc == SQLITE_ROW) {
                tag_id = sqlite3_column_int64(stmt, 0);
            }
            sqlite3_finalize(stmt);

            if (remove) {
                if (tag_id != 0) {
                    stmt = prepare(
                        db_,
                        "DELETE FROM snapshot_tags "
                        "WHERE snapshot_id=? AND tag_id=?");
                    sqlite3_bind_int64(stmt, 1, id);
                    sqlite3_bind_int64(stmt, 2, tag_id);
                    sqlite3_step(stmt);
                    sqlite3_finalize(stmt);
                }
                continue;
            }

            if (tag_id == 0) {
                stmt = prepare(
                    db_, "INSERT INTO tags(name, created_at) VALUES(?,?)");
                sqlite3_bind_text(stmt, 1, tag.c_str(), -1, SQLITE_TRANSIENT);
                std::string now = now_iso8601();
                sqlite3_bind_text(stmt, 2, now.c_str(), -1, SQLITE_TRANSIENT);
                int insert_rc = sqlite3_step(stmt);
                sqlite3_finalize(stmt);
                if (insert_rc != SQLITE_DONE) {
                    throw std::runtime_error(
                        std::string("tag insert failed: ") +
                        sqlite3_errmsg(db_));
                }
                tag_id = sqlite3_last_insert_rowid(db_);
            }

            stmt = prepare(
                db_,
                "INSERT OR IGNORE INTO snapshot_tags(snapshot_id, tag_id) "
                "VALUES(?,?)");
            sqlite3_bind_int64(stmt, 1, id);
            sqlite3_bind_int64(stmt, 2, tag_id);
            int link_rc = sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            if (link_rc != SQLITE_DONE) {
                throw std::runtime_error(std::string("tag link failed: ") +
                                         sqlite3_errmsg(db_));
            }
        }

        if (remove) {
            exec("DELETE FROM tags WHERE id NOT IN "
                 "(SELECT tag_id FROM snapshot_tags)");
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

std::vector<ExtractedLink> Db::links(int64_t id, int limit) {
    sqlite3_stmt* stmt = prepare(
        db_,
        "SELECT url, text FROM links WHERE snapshot_id=? "
        "ORDER BY position LIMIT ?");
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_bind_int(stmt, 2, limit);
    std::vector<ExtractedLink> out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ExtractedLink link;
        link.url = column_text(stmt, 0);
        link.text = column_text(stmt, 1);
        out.push_back(std::move(link));
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
