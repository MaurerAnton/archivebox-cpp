#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "arcbox/metadata.hpp"

struct sqlite3;

namespace arcbox {

struct Snapshot {
    int64_t id = 0;
    std::string url;
    std::string canonical_url;
    std::string status;
    std::string created_at;
    std::string started_at;
    std::string finished_at;
    int http_status = 0;
    std::string content_type;
    std::string title;
    std::string description;
    std::string site_name;
    std::string author;
    std::string published_at;
    std::string language;
    std::string favicon_url;
    std::string sha256;
    int64_t size_bytes = 0;
    std::string warc_path;
    std::string body_path;
    std::string text_path;
    std::string headers_path;
    std::string metadata_path;
    std::string error;
    // Headless browser artifacts.
    std::string rendered_html_path;
    std::string screenshot_path;
    std::string pdf_path;
    int priority = 0;
    int attempts = 0;
    // Unix seconds when the snapshot becomes eligible again; 0 means "due now".
    int64_t next_attempt_at = 0;
};

struct CaptureUpdate {
    std::string status;
    int http_status = 0;
    std::string content_type;
    std::string canonical_url;
    std::string title;
    std::string description;
    std::string site_name;
    std::string author;
    std::string published_at;
    std::string language;
    std::string favicon_url;
    std::string sha256;
    int64_t size_bytes = 0;
    std::string warc_path;
    std::string body_path;
    std::string text_path;
    std::string headers_path;
    std::string metadata_path;
    std::string rendered_html_path;
    std::string screenshot_path;
    std::string pdf_path;
    std::string body_text;
    std::string error;
    std::vector<ExtractedLink> links;
};

class Db {
public:
    explicit Db(const std::filesystem::path& path);
    ~Db();

    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    void migrate();
    void recover_stale();
    int64_t enqueue(const std::string& url, int priority = 0);
    std::optional<Snapshot> next_queued();
    std::optional<Snapshot> get(int64_t id);
    bool mark_running(int64_t id);
    void finish(int64_t id, const CaptureUpdate& update);
    // Counts one more attempt: either requeues the snapshot with an
    // exponential delay, or gives up and marks it failed. Returns true when
    // the snapshot was requeued.
    bool schedule_retry(int64_t id,
                        const std::string& error,
                        int max_attempts,
                        long backoff_seconds,
                        long backoff_max_seconds);
    // Clears the attempt counter so failed work can be picked up again.
    int reset_retry(bool all, int64_t id);
    // Queues a fresh capture for every URL whose newest snapshot is older than
    // `older_than_seconds`; 0 drops the age check. URLs that already have
    // queued or running work are skipped. Returns the new snapshot ids.
    std::vector<int64_t> enqueue_recrawl(int64_t older_than_seconds, int limit);
    // Snapshot ids that a retention policy may drop: beyond the newest `keep`
    // per URL, or older than `older_than_seconds`. Either filter is off when
    // its value is 0, and the newest snapshot per URL is never a candidate.
    std::vector<int64_t> prune_candidates(int keep,
                                          int64_t older_than_seconds,
                                          int limit);
    // Removes a snapshot row with its search entry, links and tags. Files on
    // disk are the caller's job, so nothing here can leave an orphan row.
    bool delete_snapshot(int64_t id);
    std::vector<Snapshot> list(int limit, const std::string& tag = {});
    std::vector<Snapshot> search(const std::string& query,
                                 int limit,
                                 const std::string& tag = {});
    std::vector<std::string> tags(int64_t id);
    std::vector<std::pair<std::string, int64_t>> all_tags();
    void add_tags(int64_t id,
                  const std::vector<std::string>& tags,
                  bool remove);
    std::vector<ExtractedLink> links(int64_t id, int limit);
    int64_t queued_count();

private:
    sqlite3* db_ = nullptr;
    void exec(const std::string& sql);
    bool has_column(const std::string& table, const std::string& column);
};

} // namespace arcbox
