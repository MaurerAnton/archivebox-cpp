#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

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
    std::string sha256;
    int64_t size_bytes = 0;
    std::string warc_path;
    std::string body_path;
    std::string text_path;
    std::string error;
};

struct CaptureUpdate {
    std::string status;
    int http_status = 0;
    std::string content_type;
    std::string title;
    std::string sha256;
    int64_t size_bytes = 0;
    std::string warc_path;
    std::string body_path;
    std::string text_path;
    std::string body_text;
    std::string error;
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
    std::vector<Snapshot> list(int limit);
    std::vector<Snapshot> search(const std::string& query, int limit);
    int64_t queued_count();

private:
    sqlite3* db_ = nullptr;
    void exec(const std::string& sql);
};

} // namespace arcbox
