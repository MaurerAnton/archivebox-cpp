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
    int64_t enqueue(const std::string& url);
    std::optional<Snapshot> next_queued();
    std::optional<Snapshot> get(int64_t id);
    bool mark_running(int64_t id);
    void finish(int64_t id, const CaptureUpdate& update);
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
