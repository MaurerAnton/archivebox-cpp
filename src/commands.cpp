#include "arcbox/commands.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "arcbox/adblock.hpp"
#include "arcbox/browser.hpp"
#include "arcbox/capture_http.hpp"
#include "arcbox/cgroup.hpp"
#include "arcbox/db.hpp"
#include "arcbox/memory.hpp"
#include "arcbox/metadata.hpp"
#include "arcbox/util.hpp"
#include "arcbox/wacz.hpp"
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

// A refusal is easy to lose: afterwards the archive just looks like it never
// saw the content. Keeping the decision beside the artifacts says which stage
// said no (the HTTP fetch, or the browser's own request) and which list entry
// decided it.
static void note_block_decision(const Config& cfg, int64_t id,
                                const std::string& stage,
                                const std::string& url,
                                const Adblock& adblock) {
    const std::string relative =
        "snapshots/" + std::to_string(id) + "/blocked.txt";
    const std::string line =
        stage + "\t" + url + "\t" + adblock.matched_rule(url) + "\n";
    const std::filesystem::path path = cfg.resolve(relative);
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::app | std::ios::binary);
    if (!out) {
        std::cerr << "arcbox: cannot append to the block decision log: "
                  << path.string() << "\n";
        return;
    }
    out.write(line.data(), static_cast<std::streamsize>(line.size()));
}

// Bodies go into a content-addressed store: the bytes are written once under
// objects/ and each snapshot keeps its own snapshots/<id>/body.bin as another
// link to the very same inode. Nothing that reads an archive has to change, a
// second capture of identical content costs one directory entry, and deleting
// a snapshot can never pull the bytes out from under a sibling that still uses
// them.
static bool store_body_cas(const Config& cfg, const std::string& relative_body,
                           const std::string& body, const std::string& sha256,
                           std::string& error) {
    if (sha256.size() < 4) {
        error = "cannot address a body without its digest";
        return false;
    }
    const std::filesystem::path object =
        cfg.objects_dir() / sha256.substr(0, 2) / sha256;
    const std::filesystem::path link = cfg.resolve(relative_body);
    std::error_code ec;
    std::filesystem::create_directories(link.parent_path(), ec);
    std::filesystem::remove(link, ec);

    if (!std::filesystem::exists(object)) {
        if (!write_text_file(object, body, error)) {
            return false;
        }
    }
    ec.clear();
    std::filesystem::create_hard_link(object, link, ec);
    if (!ec) {
        return true;
    }
    // A filesystem without hard links (or a store on another device) must not
    // lose the capture: fall back to its own copy.
    return write_text_file(link, body, error);
}

// Once no snapshot links an object any more, the bytes are dead weight. The
// link count is the truth here: an object referenced anywhere still has at
// least two names.
static int64_t sweep_objects(const Config& cfg) {
    const std::filesystem::path objects = cfg.objects_dir();
    std::error_code ec;
    if (!std::filesystem::exists(objects, ec)) {
        return 0;
    }
    int64_t reclaimed = 0;
    std::error_code walk_error;
    for (std::filesystem::recursive_directory_iterator it(objects, walk_error), end;
         it != end; it.increment(walk_error)) {
        if (walk_error) {
            break;
        }
        if (!it->is_regular_file()) {
            continue;
        }
        std::error_code count_error;
        if (it->hard_link_count(count_error) > 1 && !count_error) {
            continue;
        }
        const uintmax_t size = it->file_size(ec);
        std::filesystem::remove(it->path(), ec);
        if (!ec) {
            reclaimed += static_cast<int64_t>(size);
        }
    }
    for (std::filesystem::recursive_directory_iterator it(objects, walk_error), end;
         it != end; it.increment(walk_error)) {
        if (walk_error) {
            break;
        }
        if (it->is_directory() && std::filesystem::is_empty(it->path(), ec)) {
            std::filesystem::remove(it->path(), ec);
        }
    }
    return reclaimed;
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

static void note_issue(CaptureUpdate& update, const std::string& message) {
    if (update.status != "failed" && update.status != "blocked") {
        update.status = "partial";
    }
    if (!update.error.empty()) {
        update.error += "; ";
    }
    update.error += message;
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
        note_block_decision(cfg, id, "http", snapshot->url, adblock);
        db.finish(id, blocked);
        std::cout << "#" << id << " blocked " << snapshot->url << "\n";
        return 4;
    }

    HttpCapture capture = fetch_http(snapshot->url, cfg);

    CaptureUpdate update;
    update.status = capture.ok() ? "ok" : "failed";
    update.http_status = capture.status_code;
    update.content_type = capture.content_type;
    update.canonical_url = capture.effective_url;
    update.size_bytes = static_cast<int64_t>(capture.body.size());
    update.error = capture.error;

    if (capture.ok()) {
        std::filesystem::create_directories(cfg.snapshots_dir() /
                                            std::to_string(id));
        const std::string body_relative =
            "snapshots/" + std::to_string(id) + "/body.bin";
        const std::string text_relative =
            "snapshots/" + std::to_string(id) + "/text.txt";
        const std::string headers_relative =
            "snapshots/" + std::to_string(id) + "/headers.txt";
        const std::string metadata_relative =
            "snapshots/" + std::to_string(id) + "/metadata.json";

        update.sha256 = sha256_hex(capture.body);
        std::string write_error;
        if (!store_body_cas(cfg, body_relative, capture.body, update.sha256,
                            write_error)) {
            update.status = "failed";
            update.error = write_error;
        } else {
            update.body_path = body_relative;
        }

        std::string headers_text = capture.status_line;
        if (!capture.headers.empty()) {
            headers_text += "\n" + capture.headers;
        }
        if (!write_text_file(cfg.resolve(headers_relative), headers_text,
                             write_error)) {
            note_issue(update, "headers: " + write_error);
        } else {
            update.headers_path = headers_relative;
        }

        const std::string warc_relative =
            "warc/" + std::to_string(id) + "-" + timestamp_slug() + ".warc.gz";
        std::string warc_error;
        if (!write_warc(cfg.resolve(warc_relative), capture, cfg, warc_error)) {
            note_issue(update, "WARC: " + warc_error);
        } else {
            update.warc_path = warc_relative;
        }

        if (looks_like_html(capture.content_type, capture.body)) {
            std::string base_url = capture.effective_url.empty()
                                       ? snapshot->url
                                       : capture.effective_url;
            PageMetadata metadata = extract_metadata(capture.body, base_url);
            std::string extracted_json = metadata_json(metadata);
            update.title = std::move(metadata.title);
            update.description = std::move(metadata.description);
            update.site_name = std::move(metadata.site_name);
            update.author = std::move(metadata.author);
            update.published_at = std::move(metadata.published_at);
            update.language = std::move(metadata.language);
            update.favicon_url = std::move(metadata.favicon_url);
            if (!metadata.canonical_url.empty()) {
                update.canonical_url = std::move(metadata.canonical_url);
            }
            update.links = std::move(metadata.links);

            std::ostringstream metadata_text;
            metadata_text << "{\"url\":\"" << json_escape(snapshot->url)
                          << "\",\"effective_url\":\""
                          << json_escape(capture.effective_url)
                          << "\",\"http_status\":" << capture.status_code
                          << ",\"content_type\":\""
                          << json_escape(capture.content_type)
                          << "\",\"extracted\":" << extracted_json << "}";
            if (!write_text_file(cfg.resolve(metadata_relative),
                                 metadata_text.str(), write_error)) {
                note_issue(update, "metadata: " + write_error);
            } else {
                update.metadata_path = metadata_relative;
            }

            std::string text = html_to_text(capture.body);
            if (!text.empty()) {
                if (write_text_file(cfg.resolve(text_relative), text,
                                    write_error)) {
                    update.text_path = text_relative;
                } else {
                    note_issue(update, "text: " + write_error);
                }
                if (text.size() > 500000) {
                    text.resize(500000);
                }
                update.body_text = std::move(text);
            }
        }
    }

    // Rendering is the one capture step that can cost hundreds of megabytes,
    // so it stays opt-in, HTML-only, and re-reads host memory after the fetch:
    // a page that was cheap to download can still be expensive to render. The
    // ladder then picks the cheapest artifact set the host can afford.
    if (cfg.render_enabled && update.status == "ok" &&
        starts_with(update.content_type, "text/html")) {
        HostMemory mem;
        read_host_memory(mem);
        const RenderPlan plan = choose_render_plan(cfg, mem, update.size_bytes);
        if (!plan.dom()) {
            // Not an error: the page itself is archived, and the reason names
            // the resource that ran out first (memory, size, or the guard).
            std::cerr << "arcbox: #" << id << ": render skipped: " << plan.reason
                      << "\n";
        } else {
            RenderResult rendered = render_page(cfg, snapshot->url, id, plan);
            if (rendered.attempted) {
                update.rendered_html_path = rendered.rendered_html_path;
                update.screenshot_path = rendered.screenshot_path;
                update.pdf_path = rendered.pdf_path;
                if (!rendered.ok && !rendered.error.empty()) {
                    note_issue(update, "render: " + rendered.error);
                }
                for (const std::string& url : rendered.blocked_urls) {
                    note_block_decision(cfg, id, "render", url, adblock);
                }
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

int cmd_blocklist_check(Config& cfg, const std::string& url) {
    Adblock adblock(cfg.blocklist_path());
    const bool is_blocked = adblock.blocked(url);
    std::cout << (is_blocked ? "blocked: " : "allowed: ") << url << "\n"
              << "blocklist: " << cfg.blocklist_path().string() << " ("
              << adblock.rules() << " rules)\n";
    return 0;
}

int cmd_blocklist_import(Config& cfg, const std::string& path) {
    cfg.ensure_dirs();

    // Dedupe against what is already installed, so importing the same list
    // twice stays idempotent.
    std::unordered_set<std::string> known;
    {
        std::ifstream existing(cfg.blocklist_path());
        std::string line;
        while (std::getline(existing, line)) {
            BlockRule rule;
            if (parse_block_rule(line, rule)) {
                known.insert(rule.host);
            }
        }
    }

    std::ifstream in(path);
    if (!in) {
        return fail("cannot read blocklist source: " + path);
    }

    std::ofstream out(cfg.blocklist_path(), std::ios::app);
    if (!out) {
        return fail("cannot append to blocklist: " +
                    cfg.blocklist_path().string());
    }

    std::size_t added = 0;
    std::size_t duplicates = 0;
    std::string line;
    while (std::getline(in, line)) {
        BlockRule rule;
        if (!parse_block_rule(line, rule)) {
            continue;
        }
        if (!known.insert(rule.host).second) {
            ++duplicates;
            continue;
        }
        out << (rule.exception ? "@@" : "") << rule.host << "\n";
        ++added;
    }
    if (!out) {
        return fail("failed while writing " + cfg.blocklist_path().string());
    }

    std::cout << "imported " << added << " new rules (" << duplicates
              << " duplicates skipped) into " << cfg.blocklist_path().string()
              << "\n";
    return 0;
}

// Removes every artifact of a snapshot from disk and returns how many bytes
// that freed. With dry_run nothing is touched, but the size still measured, so
// `prune --dry-run` reports what it would do.
int64_t remove_snapshot_files(const Config& cfg,
                              const Snapshot& snapshot,
                              bool dry_run) {
    const std::vector<std::string> paths = {
        snapshot.warc_path,        snapshot.body_path,
        snapshot.text_path,        snapshot.headers_path,
        snapshot.metadata_path,    snapshot.rendered_html_path,
        snapshot.screenshot_path,  snapshot.pdf_path};

    int64_t freed = 0;
    // Files counted here are counted again by the directory walk below, so
    // they are remembered and skipped there.
    std::vector<std::filesystem::path> counted;
    for (const std::string& relative : paths) {
        if (relative.empty()) {
            continue;
        }
        std::error_code ec;
        const std::filesystem::path target = cfg.resolve(relative);
        if (!std::filesystem::is_regular_file(target, ec) || ec) {
            continue;
        }
        const uintmax_t size = std::filesystem::file_size(target, ec);
        if (ec) {
            continue;
        }
        if (!dry_run) {
            std::filesystem::remove(target, ec);
            if (ec) {
                continue;
            }
        }
        freed += static_cast<int64_t>(size);
        counted.push_back(target);
    }

    // The per-snapshot directory itself, which also covers artifacts the
    // schema does not know about yet.
    std::error_code ec;
    const std::filesystem::path dir =
        cfg.resolve("snapshots/" + std::to_string(snapshot.id));
    if (std::filesystem::is_directory(dir, ec) && !ec) {
        for (std::filesystem::recursive_directory_iterator it(dir, ec), end;
             it != end; it.increment(ec)) {
            if (ec) {
                break;
            }
            if (!it->is_regular_file()) {
                continue;
            }
            if (std::find(counted.begin(), counted.end(), it->path()) !=
                counted.end()) {
                continue;
            }
            freed += static_cast<int64_t>(it->file_size(ec));
        }
        if (!dry_run) {
            std::filesystem::remove_all(dir, ec);
        }
    }
    return freed;
}

int cmd_recrawl(Config& cfg, int64_t older_than_seconds, int limit) {
    cfg.ensure_dirs();
    Db db(cfg.db_path());
    db.migrate();

    const std::vector<int64_t> ids =
        db.enqueue_recrawl(older_than_seconds, limit);
    for (int64_t id : ids) {
        const std::optional<Snapshot> snapshot = db.get(id);
        if (snapshot) {
            std::cout << "queued #" << id << " " << snapshot->url << "\n";
        }
    }
    if (ids.empty()) {
        std::cout << "nothing to recrawl\n";
    } else {
        std::cout << "recrawled " << ids.size() << " url(s)\n";
    }
    return 0;
}

int cmd_prune(Config& cfg,
              int keep,
              int64_t older_than_seconds,
              bool dry_run) {
    if (keep <= 0 && older_than_seconds <= 0) {
        return fail("prune needs --keep N or --older-than DURATION");
    }

    cfg.ensure_dirs();
    Db db(cfg.db_path());
    db.migrate();

    const std::vector<int64_t> ids =
        db.prune_candidates(keep, older_than_seconds, 0);
    if (ids.empty()) {
        std::cout << "nothing to prune\n";
        return 0;
    }

    int64_t freed = 0;
    std::size_t removed = 0;
    for (int64_t id : ids) {
        const std::optional<Snapshot> snapshot = db.get(id);
        if (!snapshot) {
            continue;
        }
        const int64_t bytes = remove_snapshot_files(cfg, *snapshot, dry_run);
        freed += bytes;
        if (dry_run) {
            ++removed;
        } else if (db.delete_snapshot(id)) {
            ++removed;
        }
        std::cout << (dry_run ? "would prune #" : "pruned #") << id << " "
                  << snapshot->url << " (" << format_size(bytes) << ")\n";
    }

    std::cout << (dry_run ? "would prune " : "pruned ") << removed
              << " snapshot(s), freed " << format_size(freed) << "\n";
    if (!dry_run && removed > 0) {
        const int64_t objects = sweep_objects(cfg);
        if (objects > 0) {
            std::cout << "reclaimed " << format_size(objects)
                      << " of bodies no snapshot links any more\n";
        }
    }
    return 0;
}

// WACZ wants an ISO timestamp per page; a queued snapshot has no finish time
// yet, so the creation time stands in for it.
std::string snapshot_timestamp(const Snapshot& snapshot) {
    return snapshot.finished_at.empty() ? snapshot.created_at
                                        : snapshot.finished_at;
}

std::string wacz_datapackage(const std::string& resources) {
    std::ostringstream out;
    out << "{\"profile\":\"data-package\",\"title\":\"arcbox export\","
        << "\"software\":\"arcbox\",\"wacz_version\":\"1.1.1\","
        << "\"created\":\"" << now_iso8601() << "\","
        << "\"resources\":[" << resources << "]}";
    return out.str();
}

int cmd_wacz(Config& cfg,
             const std::string& output,
             int limit,
             const std::string& tag) {
    cfg.ensure_dirs();
    Db db(cfg.db_path());
    db.migrate();

    const std::vector<Snapshot> snapshots = db.list(limit, tag);
    if (snapshots.empty()) {
        std::cout << "nothing to export\n";
        return 0;
    }

    std::ostringstream pages;
    std::ostringstream resources;
    resources << "{\"name\":\"pages\",\"path\":\"pages/pages.jsonl\","
                 "\"format\":\"jsonl\",\"mediatype\":\"application/jsonl\"}";

    ZipWriter zip(output);
    if (!zip.ok()) {
        return fail(zip.error());
    }

    std::size_t pages_written = 0;
    std::size_t archives_written = 0;
    int64_t archived_bytes = 0;
    for (const Snapshot& snapshot : snapshots) {
        const std::string captured = snapshot_timestamp(snapshot);
        pages << "{\"id\":\"" << snapshot.id << "\",\"url\":\""
              << json_escape(snapshot.url) << "\",\"title\":\""
              << json_escape(snapshot.title) << "\",\"ts\":\""
              << json_escape(captured) << "\"}\n";
        ++pages_written;

        // Already gzipped WARCs go in untouched: recompressing them costs CPU
        // and memory for nothing.
        if (snapshot.warc_path.empty()) {
            continue;
        }
        std::error_code ec;
        const std::filesystem::path source = cfg.resolve(snapshot.warc_path);
        if (!std::filesystem::is_regular_file(source, ec) || ec) {
            continue;
        }
        const uintmax_t size = std::filesystem::file_size(source, ec);
        if (ec) {
            continue;
        }
        const std::string name =
            "archive/" + source.filename().string();
        if (!zip.add_file(name, source, true)) {
            return fail(zip.error());
        }
        ++archives_written;
        archived_bytes += static_cast<int64_t>(size);
        resources << ",{\"name\":\"" << json_escape(source.stem().string())
                  << "\",\"path\":\"" << json_escape(name)
                  << "\",\"format\":\"warc\",\"mediatype\":\"application/warc\""
                  << ",\"bytes\":" << size << "}";
    }

    if (!zip.add_bytes("pages/pages.jsonl", pages.str()) ||
        !zip.add_bytes("datapackage.json", wacz_datapackage(resources.str()))) {
        return fail(zip.error());
    }
    if (!zip.close()) {
        std::error_code ec;
        std::filesystem::remove(output, ec);
        return fail(zip.error());
    }

    std::cout << "wrote " << output << ": " << pages_written << " page(s), "
              << archives_written << " archive(s), "
              << format_size(archived_bytes) << "\n";
    return 0;
}

int cmd_add(Config& cfg,
            const std::string& url,
            bool capture_now,
            bool use_blocklist,
            int priority) {
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

    int64_t id = db.enqueue(canonical, priority);
    std::cout << "queued #" << id << " " << canonical;
    if (priority != 0) {
        std::cout << " (priority " << priority << ")";
    }
    std::cout << "\n";
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

int cmd_list(Config& cfg, int limit, const std::string& tag) {
    cfg.ensure_dirs();
    Db db(cfg.db_path());
    db.migrate();

    std::vector<Snapshot> snapshots = db.list(limit, tag);
    if (snapshots.empty()) {
        std::cout << "no snapshots\n";
        return 0;
    }
    std::cout << "ID\tSTATUS\tHTTP\tSIZE\tCAPTURED\tTAGS\tURL\n";
    for (const auto& snapshot : snapshots) {
        std::string tags;
        for (const std::string& item : db.tags(snapshot.id)) {
            if (!tags.empty()) {
                tags.push_back(',');
            }
            tags += item;
        }
        std::cout << snapshot.id << "\t" << snapshot.status << "\t"
                  << (snapshot.http_status > 0
                          ? std::to_string(snapshot.http_status)
                          : std::string("-"))
                  << "\t" << format_size(snapshot.size_bytes) << "\t"
                  << snapshot.finished_at << "\t" << tags << "\t"
                  << snapshot.url << "\n";
    }
    return 0;
}

int cmd_search(Config& cfg,
               const std::string& query,
               int limit,
               const std::string& tag) {
    cfg.ensure_dirs();
    Db db(cfg.db_path());
    db.migrate();

    std::vector<Snapshot> snapshots = db.search(query, limit, tag);
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
              << "canonical:   " << snapshot->canonical_url << "\n"
              << "status:      " << snapshot->status << "\n"
              << "http_status: " << snapshot->http_status << "\n";
    if (snapshot->priority != 0) {
        std::cout << "priority:    " << snapshot->priority << "\n";
    }
    if (snapshot->attempts > 0) {
        std::cout << "attempts:    " << snapshot->attempts << "\n";
    }
    if (!snapshot->screenshot_path.empty()) {
        std::cout << "screenshot:  " << snapshot->screenshot_path << "\n";
    }
    if (!snapshot->rendered_html_path.empty()) {
        std::cout << "rendered:    " << snapshot->rendered_html_path << "\n";
    }
    if (!snapshot->pdf_path.empty()) {
        std::cout << "pdf:         " << snapshot->pdf_path << "\n";
    }
    const std::string decision_relative =
        "snapshots/" + std::to_string(snapshot->id) + "/blocked.txt";
    std::ifstream decisions(cfg.resolve(decision_relative));
    if (decisions) {
        std::size_t refusals = 0;
        std::string line;
        while (std::getline(decisions, line)) {
            if (!line.empty()) {
                ++refusals;
            }
        }
        std::cout << "blocked:     " << refusals << " request(s), "
                  << decision_relative << "\n";
    }
    std::cout << "content:     " << snapshot->content_type << "\n"
              << "title:       " << snapshot->title << "\n";
    if (!snapshot->description.empty()) {
        std::cout << "description: " << snapshot->description << "\n";
    }
    if (!snapshot->site_name.empty()) {
        std::cout << "site:        " << snapshot->site_name << "\n";
    }
    if (!snapshot->author.empty()) {
        std::cout << "author:      " << snapshot->author << "\n";
    }
    if (!snapshot->published_at.empty()) {
        std::cout << "published:   " << snapshot->published_at << "\n";
    }
    if (!snapshot->language.empty()) {
        std::cout << "language:    " << snapshot->language << "\n";
    }
    if (!snapshot->favicon_url.empty()) {
        std::cout << "favicon:     " << snapshot->favicon_url << "\n";
    }
    std::cout << "size:        " << format_size(snapshot->size_bytes) << "\n"
              << "sha256:      " << snapshot->sha256 << "\n"
              << "created:     " << snapshot->created_at << "\n"
              << "finished:    " << snapshot->finished_at << "\n";
    std::vector<std::string> tags = db.tags(id);
    if (!tags.empty()) {
        std::cout << "tags:        ";
        for (std::size_t i = 0; i < tags.size(); ++i) {
            if (i > 0) {
                std::cout << ", ";
            }
            std::cout << tags[i];
        }
        std::cout << "\n";
    }
    std::vector<ExtractedLink> links = db.links(id, 100000);
    std::cout << "links:       " << links.size() << "\n";
    if (!snapshot->warc_path.empty()) {
        std::cout << "warc:        " << cfg.resolve(snapshot->warc_path).string()
                  << "\n";
    }
    if (!snapshot->body_path.empty()) {
        std::cout << "body:        " << cfg.resolve(snapshot->body_path).string()
                  << "\n";
    }
    if (!snapshot->text_path.empty()) {
        std::cout << "text:        " << cfg.resolve(snapshot->text_path).string()
                  << "\n";
    }
    if (!snapshot->headers_path.empty()) {
        std::cout << "headers:     "
                  << cfg.resolve(snapshot->headers_path).string() << "\n";
    }
    if (!snapshot->metadata_path.empty()) {
        std::cout << "metadata:    "
                  << cfg.resolve(snapshot->metadata_path).string() << "\n";
    }
    if (!snapshot->error.empty()) {
        std::cout << "error:       " << snapshot->error << "\n";
    }
    return 0;
}

int cmd_tag(Config& cfg,
            int64_t id,
            const std::vector<std::string>& tags,
            bool remove) {
    cfg.ensure_dirs();
    Db db(cfg.db_path());
    db.migrate();

    auto snapshot = db.get(id);
    if (!snapshot) {
        return fail("snapshot not found: " + std::to_string(id));
    }

    std::vector<std::string> clean;
    for (const std::string& raw : tags) {
        std::string tag = trim(raw);
        if (tag.empty()) {
            return fail("tag must not be empty");
        }
        for (char c : tag) {
            if (c < 0x20 || c == ',') {
                return fail("tag contains an unsupported character: " + raw);
            }
        }
        clean.push_back(tag);
    }
    if (clean.empty()) {
        return fail("tag requires at least one tag");
    }

    db.add_tags(id, clean, remove);
    std::vector<std::string> current = db.tags(id);
    std::cout << "#" << id << " tags:";
    for (const std::string& tag : current) {
        std::cout << " " << tag;
    }
    if (current.empty()) {
        std::cout << " (none)";
    }
    std::cout << "\n";
    return 0;
}

int cmd_tags(Config& cfg) {
    cfg.ensure_dirs();
    Db db(cfg.db_path());
    db.migrate();

    std::vector<std::pair<std::string, int64_t>> tags = db.all_tags();
    if (tags.empty()) {
        std::cout << "no tags\n";
        return 0;
    }
    std::cout << "TAG\tSNAPSHOTS\n";
    for (const auto& tag : tags) {
        std::cout << tag.first << "\t" << tag.second << "\n";
    }
    return 0;
}

int cmd_import(Config& cfg,
               const std::string& path,
               bool capture_now,
               bool use_cgroup,
               bool use_blocklist) {
    cfg.ensure_dirs();
    Db db(cfg.db_path());
    db.migrate();

    std::ifstream file;
    std::istream* input = &std::cin;
    if (path != "-") {
        file.open(path);
        if (!file) {
            return fail("cannot open import file: " + path);
        }
        input = &file;
    }

    int imported = 0;
    int invalid = 0;
    int failures = 0;
    std::string line;
    while (std::getline(*input, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        std::string canonical = url_canonicalize(line);
        if (!starts_with(canonical, "http://") &&
            !starts_with(canonical, "https://")) {
            std::cerr << "arcbox: skipped non-HTTP URL: " << line << "\n";
            ++invalid;
            continue;
        }
        if (url_host(canonical).empty()) {
            std::cerr << "arcbox: skipped URL without host: " << line << "\n";
            ++invalid;
            continue;
        }

        try {
            int64_t id = db.enqueue(canonical);
            ++imported;
            std::cout << "queued #" << id << " " << canonical << "\n";
            if (capture_now) {
                if (do_capture(cfg, db, id, use_cgroup, use_blocklist) != 0) {
                    ++failures;
                }
            }
        } catch (const std::exception& error) {
            std::cerr << "arcbox: failed to queue " << canonical << ": "
                      << error.what() << "\n";
            ++invalid;
        }
    }

    std::cout << "imported " << imported << ", skipped " << invalid
              << ", capture failures " << failures << "\n";
    if (imported == 0) {
        return failures == 0 ? 1 : 2;
    }
    return failures == 0 ? 0 : 1;
}

int cmd_export(Config& cfg,
               int limit,
               const std::string& tag,
               bool urls_only) {
    cfg.ensure_dirs();
    Db db(cfg.db_path());
    db.migrate();

    std::vector<Snapshot> snapshots = db.list(limit, tag);
    for (const auto& snapshot : snapshots) {
        if (urls_only) {
            std::cout << snapshot.url << "\n";
            continue;
        }
        std::vector<std::string> tags = db.tags(snapshot.id);
        std::ostringstream out;
        out << "{\"id\":" << snapshot.id
            << ",\"url\":\"" << json_escape(snapshot.url) << "\""
            << ",\"canonical_url\":\""
            << json_escape(snapshot.canonical_url) << "\""
            << ",\"status\":\"" << json_escape(snapshot.status) << "\""
            << ",\"http_status\":" << snapshot.http_status
            << ",\"content_type\":\""
            << json_escape(snapshot.content_type) << "\""
            << ",\"title\":\"" << json_escape(snapshot.title) << "\""
            << ",\"description\":\"" << json_escape(snapshot.description)
            << "\""
            << ",\"site_name\":\"" << json_escape(snapshot.site_name) << "\""
            << ",\"author\":\"" << json_escape(snapshot.author) << "\""
            << ",\"published_at\":\"" << json_escape(snapshot.published_at)
            << "\""
            << ",\"language\":\"" << json_escape(snapshot.language) << "\""
            << ",\"favicon_url\":\"" << json_escape(snapshot.favicon_url)
            << "\""
            << ",\"sha256\":\"" << json_escape(snapshot.sha256) << "\""
            << ",\"size_bytes\":" << snapshot.size_bytes
            << ",\"created_at\":\"" << json_escape(snapshot.created_at)
            << "\""
            << ",\"finished_at\":\"" << json_escape(snapshot.finished_at)
            << "\""
            << ",\"warc_path\":\"" << json_escape(snapshot.warc_path) << "\""
            << ",\"body_path\":\"" << json_escape(snapshot.body_path) << "\""
            << ",\"text_path\":\"" << json_escape(snapshot.text_path) << "\""
            << ",\"headers_path\":\"" << json_escape(snapshot.headers_path)
            << "\""
            << ",\"metadata_path\":\""
            << json_escape(snapshot.metadata_path) << "\""
            << ",\"error\":\"" << json_escape(snapshot.error) << "\""
            << ",\"tags\":[";
        for (std::size_t i = 0; i < tags.size(); ++i) {
            if (i > 0) {
                out << ",";
            }
            out << "\"" << json_escape(tags[i]) << "\"";
        }
        out << "]}";
        std::cout << out.str() << "\n";
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

        HostMemory mem;
        read_host_memory(mem);
        AdmissionDecision decision = admit_capture(cfg, mem);
        if (decision.action != Admission::Proceed) {
            std::cout << "arcbox: admission " << admission_name(decision.action)
                      << ": " << decision.reason;
            if (once) {
                std::cout << "; --once, leaving queued\n";
                std::cout.flush();
                break;
            }
            std::cout << "; retrying in " << decision.sleep_seconds << "s\n";
            std::cout.flush();
            std::this_thread::sleep_for(
                std::chrono::seconds(decision.sleep_seconds));
            continue;
        }

        do_capture(cfg, db, snapshot->id, use_cgroup, use_blocklist);

        // A transient failure is retried with exponential backoff; a snapshot
        // that exhausted its attempts is left failed for `arcbox retry`.
        // "blocked" is a policy decision, not a failure, so it never retries.
        auto finished = db.get(snapshot->id);
        if (finished && finished->status == "failed") {
            bool requeued = db.schedule_retry(
                finished->id, finished->error, cfg.max_attempts,
                cfg.retry_backoff_seconds, cfg.retry_backoff_max_seconds);
            int attempt = finished->attempts + 1;
            if (requeued) {
                std::cout << "arcbox: #" << finished->id << " attempt "
                          << attempt << "/" << cfg.max_attempts
                          << " failed (" << finished->error
                          << "); retrying later\n";
            } else {
                std::cout << "arcbox: #" << finished->id
                          << " gave up after " << attempt << " attempts ("
                          << finished->error << ")\n";
            }
            std::cout.flush();
        }
    }
    return 0;
}

int cmd_retry(Config& cfg, const std::string& target) {
    cfg.ensure_dirs();
    Db db(cfg.db_path());
    db.migrate();

    bool all = target == "all" || target == "--all";
    int64_t id = 0;
    if (!all) {
        if (!is_numeric(target)) {
            return fail("retry expects a snapshot ID or 'all'");
        }
        id = std::stoll(target);
        if (!db.get(id)) {
            return fail("snapshot not found: " + target);
        }
    }

    int changed = db.reset_retry(all, id);
    std::cout << "requeued " << changed << " snapshot(s)\n";
    return 0;
}

int cmd_worker(Config& cfg, int64_t id, bool use_blocklist) {
    cfg.ensure_dirs();
    Db db(cfg.db_path());
    db.migrate();
    return worker_capture(cfg, db, id, use_blocklist);
}

} // namespace arcbox
