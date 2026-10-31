#include "arcbox/commands.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "arcbox/adblock.hpp"
#include "arcbox/capture_http.hpp"
#include "arcbox/cgroup.hpp"
#include "arcbox/db.hpp"
#include "arcbox/util.hpp"
#include "arcbox/warc.hpp"

namespace arcbox {

static int fail(const std::string& message) {
    std::cerr << "arcbox: " << message << "\n";
    return 1;
}

static bool is_numeric(const std::string& value) {
    if (value.empty()) {
        return false;
    }
    for (char c : value) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

static bool write_text_file(const std::filesystem::path& path,
                            const std::string& data,
                            std::string& error) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error = "cannot open for writing: " + path.string();
        return false;
    }
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!out) {
        error = "write failed: " + path.string();
        return false;
    }
    return true;
}

static bool looks_like_html(const std::string& content_type,
                            const std::string& body) {
    std::string type = lower(content_type);
    if (type.find("text/html") != std::string::npos ||
        type.find("application/xhtml") != std::string::npos) {
        return true;
    }
    std::string prefix = lower(body.substr(0, 256));
    return prefix.find("<html") != std::string::npos ||
           prefix.find("<!doctype html") != std::string::npos;
}

static int worker_capture(Config& cfg,
                          Db& db,
                          int64_t id,
                          bool use_blocklist) {
    auto snapshot = db.get(id);
    if (!snapshot) {
        return fail("snapshot not found: " + std::to_string(id));
    }
    if (!db.mark_running(id)) {
        return fail("snapshot #" + std::to_string(id) +
                    " is not queued (status=" + snapshot->status + ")");
    }

    Adblock adblock(cfg.blocklist_path());
    if (use_blocklist && adblock.blocked(snapshot->url)) {
        CaptureUpdate blocked;
        blocked.status = "blocked";
        blocked.error = "blocked by host blocklist";
        db.finish(id, blocked);
        std::cout << "#" << id << " blocked " << snapshot->url << "\n";
        return 4;
    }

    HttpCapture capture = fetch_http(snapshot->url, cfg);

    CaptureUpdate update;
    update.status = capture.ok() ? "ok" : "failed";
    update.http_status = capture.status_code;
    update.content_type = capture.content_type;
    update.size_bytes = static_cast<int64_t>(capture.body.size());
    update.error = capture.error;

    if (capture.ok()) {
        std::filesystem::create_directories(cfg.snapshots_dir() /
                                            std::to_string(id));
        const std::string body_relative =
            "snapshots/" + std::to_string(id) + "/body.bin";
        const std::string text_relative =
            "snapshots/" + std::to_string(id) + "/text.txt";

        std::string write_error;
        if (!write_text_file(cfg.resolve(body_relative), capture.body,
                             write_error)) {
            update.status = "failed";
            update.error = write_error;
        } else {
            update.body_path = body_relative;
            update.sha256 = sha256_hex(capture.body);
        }

        const std::string warc_relative =
            "warc/" + std::to_string(id) + "-" + timestamp_slug() + ".warc.gz";
        std::string warc_error;
        if (!write_warc(cfg.resolve(warc_relative), capture, cfg, warc_error)) {
            update.status = "partial";
            update.error = "WARC: " + warc_error;
        } else {
            update.warc_path = warc_relative;
        }

        if (looks_like_html(capture.content_type, capture.body)) {
            update.title = html_title(capture.body);
            std::string text = html_to_text(capture.body);
            if (!text.empty()) {
                if (write_text_file(cfg.resolve(text_relative), text,
                                    write_error)) {
                    update.text_path = text_relative;
                }
                if (text.size() > 500000) {
                    text.resize(500000);
                }
                update.body_text = std::move(text);
            }
        }
    }

    db.finish(id, update);

    std::cout << "#" << id << " " << update.status << " "
              << (update.http_status > 0 ? std::to_string(update.http_status)
                                         : std::string("-"))
              << " " << format_size(update.size_bytes) << " "
              << snapshot->url << "\n";
    if (!update.error.empty()) {
        std::cerr << "arcbox: #" << id << ": " << update.error << "\n";
    }
    return capture.ok() ? 0 : 1;
}

static int do_capture(Config& cfg,
                      Db& db,
                      int64_t id,
                      bool use_cgroup,
                      bool use_blocklist) {
    if (use_cgroup && cfg.use_cgroup && cgroup_available()) {
        std::vector<std::string> argv = {
            read_self_exe(), "--data", cfg.data_dir.string(), "__worker",
            "--id", std::to_string(id)};
        if (!use_blocklist) {
            argv.push_back("--no-block");
        }

        std::string error;
        int rc = run_in_cgroup(cfg, argv, error);
        if (rc != 0) {
            auto snapshot = db.get(id);
            if (snapshot && (snapshot->status == "queued" ||
                             snapshot->status == "running")) {
                CaptureUpdate failed;
                failed.status = "failed";
                failed.error = "cgroup runner: " + error;
                db.finish(id, failed);
                std::cerr << "arcbox: cgroup capture failed: " << error
                          << "\n";
            }
            return rc;
        }
        return 0;
    }
    return worker_capture(cfg, db, id, use_blocklist);
}

int cmd_init(Config& cfg) {
    cfg.ensure_dirs();
    Db db(cfg.db_path());
    db.migrate();
    Adblock adblock(cfg.blocklist_path());
    std::cout << "initialized " << cfg.data_dir.string() << "\n"
              << "database: " << cfg.db_path().string() << "\n"
              << "blocklist: " << cfg.blocklist_path().string() << " ("
              << adblock.rules() << " rules)\n";
    return 0;
}

int cmd_add(Config& cfg,
            const std::string& url,
            bool capture_now,
            bool use_blocklist) {
    cfg.ensure_dirs();
    Db db(cfg.db_path());
    db.migrate();

    std::string canonical = url_canonicalize(url);
    if (!starts_with(canonical, "http://") &&
        !starts_with(canonical, "https://")) {
        return fail("only http:// and https:// URLs are supported");
    }
    if (url_host(canonical).empty()) {
        return fail("URL has no host: " + canonical);
    }

    int64_t id = db.enqueue(canonical);
    std::cout << "queued #" << id << " " << canonical << "\n";
    if (capture_now) {
        return do_capture(cfg, db, id, true, use_blocklist);
    }
    return 0;
}

int cmd_capture(Config& cfg,
                const std::string& target,
                bool use_cgroup,
                bool use_blocklist) {
    cfg.ensure_dirs();
    Db db(cfg.db_path());
    db.migrate();

    int64_t id = 0;
    if (is_numeric(target)) {
        id = std::stoll(target);
        if (!db.get(id)) {
            return fail("snapshot not found: " + target);
        }
    } else {
        std::string canonical = url_canonicalize(target);
        if (!starts_with(canonical, "http://") &&
            !starts_with(canonical, "https://")) {
            return fail("only http:// and https:// URLs are supported");
        }
        if (url_host(canonical).empty()) {
            return fail("URL has no host: " + canonical);
        }
        id = db.enqueue(canonical);
        std::cout << "queued #" << id << " " << canonical << "\n";
    }
    return do_capture(cfg, db, id, use_cgroup, use_blocklist);
}

int cmd_list(Config& cfg, int limit) {
    cfg.ensure_dirs();
    Db db(cfg.db_path());
    db.migrate();

    std::vector<Snapshot> snapshots = db.list(limit);
    if (snapshots.empty()) {
        std::cout << "no snapshots\n";
        return 0;
    }
    std::cout << "ID\tSTATUS\tHTTP\tSIZE\tCAPTURED\tURL\n";
    for (const auto& snapshot : snapshots) {
        std::cout << snapshot.id << "\t" << snapshot.status << "\t"
                  << (snapshot.http_status > 0
                          ? std::to_string(snapshot.http_status)
                          : std::string("-"))
                  << "\t" << format_size(snapshot.size_bytes) << "\t"
                  << snapshot.finished_at << "\t" << snapshot.url << "\n";
    }
    return 0;
}

int cmd_search(Config& cfg, const std::string& query, int limit) {
    cfg.ensure_dirs();
    Db db(cfg.db_path());
    db.migrate();

    std::vector<Snapshot> snapshots = db.search(query, limit);
    if (snapshots.empty()) {
        std::cout << "no matches\n";
        return 0;
    }
    std::cout << "ID\tSTATUS\tHTTP\tSIZE\tURL\tTITLE\n";
    for (const auto& snapshot : snapshots) {
        std::cout << snapshot.id << "\t" << snapshot.status << "\t"
                  << (snapshot.http_status > 0
                          ? std::to_string(snapshot.http_status)
                          : std::string("-"))
                  << "\t" << format_size(snapshot.size_bytes) << "\t"
                  << snapshot.url << "\t" << snapshot.title << "\n";
    }
    return 0;
}

int cmd_show(Config& cfg, int64_t id) {
    cfg.ensure_dirs();
    Db db(cfg.db_path());
    db.migrate();

    auto snapshot = db.get(id);
    if (!snapshot) {
        return fail("snapshot not found: " + std::to_string(id));
    }
    std::cout << "id:          " << snapshot->id << "\n"
              << "url:         " << snapshot->url << "\n"
              << "status:      " << snapshot->status << "\n"
              << "http_status: " << snapshot->http_status << "\n"
              << "content:     " << snapshot->content_type << "\n"
              << "title:       " << snapshot->title << "\n"
              << "size:        " << format_size(snapshot->size_bytes) << "\n"
              << "sha256:      " << snapshot->sha256 << "\n"
              << "created:     " << snapshot->created_at << "\n"
              << "finished:    " << snapshot->finished_at << "\n";
    if (!snapshot->warc_path.empty()) {
        std::cout << "warc:        " << cfg.resolve(snapshot->warc_path).string() << "\n";
    }
    if (!snapshot->body_path.empty()) {
        std::cout << "body:        " << cfg.resolve(snapshot->body_path).string() << "\n";
    }
    if (!snapshot->text_path.empty()) {
        std::cout << "text:        " << cfg.resolve(snapshot->text_path).string() << "\n";
    }
    if (!snapshot->error.empty()) {
        std::cout << "error:       " << snapshot->error << "\n";
    }
    return 0;
}

int cmd_daemon(Config& cfg,
               bool once,
               bool use_cgroup,
               bool use_blocklist) {
    cfg.ensure_dirs();
    Db db(cfg.db_path());
    db.migrate();
    db.recover_stale();
    std::cout << "arcbox daemon started; data=" << cfg.data_dir.string()
              << " queued=" << db.queued_count() << "\n";

    for (;;) {
        auto snapshot = db.next_queued();
        if (!snapshot) {
            if (once) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        do_capture(cfg, db, snapshot->id, use_cgroup, use_blocklist);
    }
    return 0;
}

int cmd_worker(Config& cfg, int64_t id, bool use_blocklist) {
    cfg.ensure_dirs();
    Db db(cfg.db_path());
    db.migrate();
    return worker_capture(cfg, db, id, use_blocklist);
}

} // namespace arcbox
