#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "arcbox/commands.hpp"
#include "arcbox/db.hpp"
#include "arcbox/util.hpp"

namespace arcbox {

namespace {

volatile sig_atomic_t g_stop = 0;

void stop_handler(int) {
    g_stop = 1;
}

struct Request {
    std::string method;
    std::string target;
    std::string path;
    std::string query;
};

std::string reason_phrase(int status) {
    switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    default: return "OK";
    }
}

bool send_all(int fd, const char* data, std::size_t size) {
    while (size > 0) {
        ssize_t written = send(fd, data, size, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (written == 0) {
            return false;
        }
        data += written;
        size -= static_cast<std::size_t>(written);
    }
    return true;
}

bool send_all(int fd, const std::string& data) {
    return send_all(fd, data.data(), data.size());
}

std::string response_head(
    int status,
    const std::string& content_type,
    std::size_t content_length,
    const std::vector<std::string>& extra_headers) {
    std::ostringstream out;
    out << "HTTP/1.1 " << status << " " << reason_phrase(status) << "\r\n"
        << "Content-Type: " << content_type << "\r\n"
        << "Content-Length: " << content_length << "\r\n"
        << "Connection: close\r\n"
        << "X-Content-Type-Options: nosniff\r\n"
        << "Cache-Control: no-store\r\n";
    for (const std::string& header : extra_headers) {
        out << header << "\r\n";
    }
    out << "\r\n";
    return out.str();
}

void respond(int fd,
             int status,
             const std::string& content_type,
             const std::string& body,
             const std::vector<std::string>& extra_headers = {},
             bool head_only = false) {
    std::string head = response_head(status, content_type, body.size(),
                                     extra_headers);
    if (!send_all(fd, head)) {
        return;
    }
    if (!head_only && !body.empty()) {
        send_all(fd, body);
    }
}

void respond_text(int fd,
                  int status,
                  const std::string& text,
                  bool head_only = false) {
    respond(fd, status, "text/plain; charset=utf-8", text, {}, head_only);
}

std::string html_headers() {
    return "default-src 'none'; style-src 'unsafe-inline'; "
           "img-src data:; form-action 'self'; base-uri 'none'; "
           "frame-ancestors 'none'";
}

void respond_html(int fd,
                  int status,
                  const std::string& html,
                  bool head_only = false) {
    respond(fd, status, "text/html; charset=utf-8", html,
            {"Content-Security-Policy: " + html_headers()}, head_only);
}

std::string url_decode(const std::string& value, bool plus_as_space) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            int hi = 0;
            int lo = 0;
            auto hex = [](char c, int& target) {
                if (c >= '0' && c <= '9') {
                    target = c - '0';
                } else if (c >= 'a' && c <= 'f') {
                    target = c - 'a' + 10;
                } else if (c >= 'A' && c <= 'F') {
                    target = c - 'A' + 10;
                } else {
                    return false;
                }
                return true;
            };
            if (hex(value[i + 1], hi) && hex(value[i + 2], lo)) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        if (value[i] == '+' && plus_as_space) {
            out.push_back(' ');
        } else {
            out.push_back(value[i]);
        }
    }
    return out;
}

std::string url_encode(const std::string& value) {
    std::string out;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            char buffer[4];
            std::snprintf(buffer, sizeof(buffer), "%%%02X", c);
            out.append(buffer, 3);
        }
    }
    return out;
}

std::map<std::string, std::string> parse_query(const std::string& query) {
    std::map<std::string, std::string> out;
    std::size_t start = 0;
    while (start <= query.size()) {
        std::size_t ampersand = query.find('&', start);
        std::string item = query.substr(
            start, ampersand == std::string::npos
                       ? std::string::npos
                       : ampersand - start);
        std::size_t equals = item.find('=');
        std::string key = url_decode(item.substr(0, equals), true);
        std::string value = equals == std::string::npos
                                ? std::string()
                                : url_decode(item.substr(equals + 1), true);
        if (!key.empty()) {
            out[key] = value;
        }
        if (ampersand == std::string::npos) {
            break;
        }
        start = ampersand + 1;
    }
    return out;
}

bool read_request(int fd, Request& request) {
    std::string raw;
    char buffer[4096];
    while (raw.size() < 16384) {
        ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (count == 0) {
            return false;
        }
        raw.append(buffer, static_cast<std::size_t>(count));
        if (raw.find("\r\n\r\n") != std::string::npos ||
            raw.find("\n\n") != std::string::npos) {
            break;
        }
    }

    std::size_t line_end = raw.find("\r\n");
    if (line_end == std::string::npos) {
        line_end = raw.find('\n');
    }
    if (line_end == std::string::npos || line_end == 0) {
        return false;
    }
    std::string first_line = raw.substr(0, line_end);
    std::size_t first_space = first_line.find(' ');
    if (first_space == std::string::npos) {
        return false;
    }
    std::size_t second_space = first_line.find(' ', first_space + 1);
    request.method = first_line.substr(0, first_space);
    request.target = first_line.substr(
        first_space + 1, second_space == std::string::npos
                             ? std::string::npos
                             : second_space - first_space - 1);
    if (request.target.size() > 4096) {
        return false;
    }

    std::size_t scheme = request.target.find("://");
    if (scheme != std::string::npos) {
        std::size_t slash = request.target.find('/', scheme + 3);
        request.target = slash == std::string::npos
                             ? "/"
                             : request.target.substr(slash);
    }
    if (request.target.empty() || request.target[0] != '/') {
        return false;
    }

    std::size_t question = request.target.find('?');
    if (question == std::string::npos) {
        request.path = url_decode(request.target, false);
    } else {
        request.path = url_decode(request.target.substr(0, question), false);
        request.query = request.target.substr(question + 1);
    }
    return true;
}

std::string css() {
    return R"css(
:root {
  --bg: #f5f2ec;
  --panel: #fffdf9;
  --text: #25221e;
  --muted: #746f66;
  --line: #ddd6c9;
  --accent: #a94728;
  --accent-soft: #f4e2d9;
  --ok: #2f6b4f;
  --warn: #8b6325;
  --bad: #9d3328;
}
@media (prefers-color-scheme: dark) {
  :root {
    --bg: #171613;
    --panel: #211f1b;
    --text: #ebe6dc;
    --muted: #a29b8e;
    --line: #3a352e;
    --accent: #e08a66;
    --accent-soft: #3a271f;
    --ok: #7cc09d;
    --warn: #d5ad68;
    --bad: #ef8c80;
  }
}
* { box-sizing: border-box; }
body {
  margin: 0;
  background: var(--bg);
  color: var(--text);
  font: 15px/1.5 ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont,
        "Segoe UI", sans-serif;
}
a { color: var(--accent); text-decoration: none; }
a:hover { text-decoration: underline; }
header {
  border-bottom: 1px solid var(--line);
  background: var(--panel);
}
.wrap { max-width: 1100px; margin: 0 auto; padding: 0 22px; }
.topbar {
  display: flex; align-items: center; gap: 20px; flex-wrap: wrap;
  padding: 18px 0;
}
.brand {
  font-weight: 700; font-size: 20px; letter-spacing: -0.02em;
  color: var(--text);
}
.brand span { color: var(--accent); }
.search { display: flex; gap: 8px; flex: 1; min-width: 240px; }
.search input {
  width: 100%; border: 1px solid var(--line); background: var(--bg);
  color: var(--text); border-radius: 7px; padding: 9px 11px;
  font: inherit;
}
.search button, .button {
  border: 0; border-radius: 7px; background: var(--accent); color: #fff;
  padding: 9px 14px; font: inherit; font-weight: 600; cursor: pointer;
  white-space: nowrap;
}
main { padding: 30px 0 60px; }
h1 { font-size: 28px; letter-spacing: -0.025em; margin: 0 0 8px; }
h2 { font-size: 19px; margin: 28px 0 10px; }
.sub { color: var(--muted); margin: 0 0 24px; }
.tags { display: flex; gap: 7px; flex-wrap: wrap; margin: 12px 0 22px; }
.tag {
  border: 1px solid var(--line); background: var(--panel);
  border-radius: 999px; padding: 4px 10px; font-size: 13px;
}
.tag.active { background: var(--accent-soft); border-color: var(--accent); }
.card {
  border: 1px solid var(--line); background: var(--panel);
  border-radius: 10px; overflow: hidden;
}
table { width: 100%; border-collapse: collapse; }
th, td { text-align: left; padding: 12px 14px; border-bottom: 1px solid var(--line); }
th { color: var(--muted); font-size: 12px; text-transform: uppercase; letter-spacing: .08em; }
tr:last-child td { border-bottom: 0; }
.title-cell { min-width: 260px; }
.small { color: var(--muted); font-size: 13px; overflow-wrap: anywhere; }
.status { font-weight: 650; font-size: 13px; }
.st-ok { color: var(--ok); }
.st-partial { color: var(--warn); }
.st-failed, .st-blocked { color: var(--bad); }
.st-queued, .st-running { color: var(--muted); }
.chips { display: flex; gap: 5px; flex-wrap: wrap; }
.chip { background: var(--accent-soft); border-radius: 999px; padding: 1px 7px; font-size: 12px; }
.dl { display: grid; grid-template-columns: 145px 1fr; gap: 8px 18px; margin: 0; }
.dl dt { color: var(--muted); }
.dl dd { margin: 0; overflow-wrap: anywhere; }
.actions { display: flex; gap: 10px; flex-wrap: wrap; margin: 20px 0 0; }
.actions a {
  border: 1px solid var(--line); border-radius: 7px; padding: 7px 11px;
  background: var(--panel); font-size: 14px;
}
.error {
  border: 1px solid var(--bad); background: var(--accent-soft);
  border-radius: 8px; padding: 10px 13px; margin-bottom: 18px;
  overflow-wrap: anywhere;
}
pre {
  margin: 0; padding: 16px; border: 1px solid var(--line);
  border-radius: 8px; background: var(--panel);
  white-space: pre-wrap; overflow-wrap: anywhere; font-size: 13px;
  max-height: 520px; overflow: auto;
}
ul.links { margin: 0; padding: 0; list-style: none; }
ul.links li { padding: 8px 0; border-bottom: 1px solid var(--line); }
ul.links li:last-child { border-bottom: 0; }
.empty { padding: 34px 18px; text-align: center; color: var(--muted); }
footer { color: var(--muted); font-size: 13px; padding: 22px 0 36px; }
)css";
}

std::string page_open(const std::string& title,
                      const std::string& heading,
                      const std::string& search_query = {},
                      const std::string& search_tag = {}) {
    std::ostringstream out;
    out << "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        << "<meta name=\"viewport\" content=\"width=device-width, "
           "initial-scale=1\">"
        << "<title>" << html_escape(title) << "</title><style>" << css()
        << "</style></head><body><header><div class=\"wrap topbar\">"
        << "<a class=\"brand\" href=\"/\">ArchiveBox<span>.cpp</span></a>"
        << "<form class=\"search\" method=\"get\" action=\"/\">"
        << "<input name=\"q\" placeholder=\"Search the archive\" "
           "aria-label=\"Search\" value=\""
        << html_escape(search_query) << "\">";
    if (!search_tag.empty()) {
        out << "<input type=\"hidden\" name=\"tag\" value=\""
            << html_escape(search_tag) << "\">";
    }
    out << "<button type=\"submit\">Search</button></form></div></header>"
        << "<main class=\"wrap\"><h1>" << html_escape(heading) << "</h1>";
    return out.str();
}

std::string page_close() {
    return "</main><footer><div class=\"wrap\">ArchiveBox.cpp local "
           "archive browser</div></footer></body></html>";
}

std::string status_slug(const std::string& status) {
    if (status == "ok") return "ok";
    if (status == "partial") return "partial";
    if (status == "failed") return "failed";
    if (status == "blocked") return "blocked";
    if (status == "running") return "running";
    return "queued";
}

std::string tag_chips(const std::vector<std::string>& tags) {
    if (tags.empty()) {
        return "<span class=\"small\">no tags</span>";
    }
    std::ostringstream out;
    out << "<div class=\"chips\">";
    for (const std::string& tag : tags) {
        out << "<a class=\"chip\" href=\"/?tag=" << url_encode(tag) << "\">"
            << html_escape(tag) << "</a>";
    }
    out << "</div>";
    return out.str();
}

std::string build_index(Db& db,
                        const std::string& raw_query,
                        const std::string& raw_tag,
                        std::string& error) {
    std::string query = trim(raw_query);
    std::string tag = lower(trim(raw_tag));
    if (query.size() > 256) {
        query.resize(256);
    }
    if (tag.size() > 128) {
        tag.resize(128);
    }

    std::vector<Snapshot> snapshots;
    std::vector<std::pair<std::string, int64_t>> tags;
    int64_t queued = 0;
    try {
        tags = db.all_tags();
        queued = db.queued_count();
        if (!query.empty()) {
            snapshots = db.search(query, 100, tag);
        } else {
            snapshots = db.list(100, tag);
        }
    } catch (const std::exception& exception) {
        error = exception.what();
    }

    std::ostringstream out;
    std::string heading = query.empty() ? "Local archive" : "Search";
    out << page_open("ArchiveBox.cpp", heading, query, tag)
        << "<p class=\"sub\">A quiet, local-first record of the web. "
        << queued << " queued snapshot" << (queued == 1 ? "" : "s")
        << " waiting for the daemon.</p>";
    if (!error.empty()) {
        out << "<div class=\"error\">" << html_escape(error) << "</div>";
    }
    if (query.empty() && tag.empty()) {
        out << "<div class=\"tags\"><a class=\"tag active\" href=\"/\">all</a>";
    } else {
        out << "<div class=\"tags\"><a class=\"tag\" href=\"/\">all</a>";
    }
    for (const auto& item : tags) {
        bool active = lower(trim(tag)) == item.first;
        out << "<a class=\"tag" << (active ? " active" : "") << "\" href=\"/?tag="
            << url_encode(item.first) << "\">" << html_escape(item.first)
            << " <span class=\"small\">" << item.second << "</span></a>";
    }
    out << "</div>";

    if (snapshots.empty()) {
        out << "<div class=\"card empty\">";
        if (!query.empty()) {
            out << "No archived pages match that search.";
        } else if (!tag.empty()) {
            out << "No snapshots carry that tag.";
        } else {
            out << "The archive is empty. Add a URL with "
                   "<code>arcbox add URL --capture</code>.";
        }
        out << "</div>" << page_close();
        return out.str();
    }

    out << "<div class=\"card\"><table><thead><tr>"
           "<th>ID</th><th>Status</th><th>Page</th><th>Size</th>"
           "<th>Tags</th></tr></thead><tbody>";
    for (const auto& snapshot : snapshots) {
        std::vector<std::string> snapshot_tags;
        try {
            snapshot_tags = db.tags(snapshot.id);
        } catch (...) {
        }
        std::string title =
            snapshot.title.empty() ? snapshot.url : snapshot.title;
        out << "<tr><td><a href=\"/snapshot/" << snapshot.id << "\">#"
            << snapshot.id << "</a></td>"
            << "<td><span class=\"status st-" << status_slug(snapshot.status)
            << "\">" << html_escape(snapshot.status) << "</span></td>"
            << "<td class=\"title-cell\"><a href=\"/snapshot/" << snapshot.id
            << "\">" << html_escape(title) << "</a><div class=\"small\">"
            << html_escape(snapshot.url) << "</div></td>"
            << "<td class=\"small\">" << format_size(snapshot.size_bytes)
            << "</td><td>" << tag_chips(snapshot_tags) << "</td></tr>";
    }
    out << "</tbody></table></div>"
        << "<p class=\"small\">Showing " << snapshots.size()
        << " most recent snapshot" << (snapshots.size() == 1 ? "" : "s")
        << ".</p>" << page_close();
    return out.str();
}

std::string read_prefix(const std::filesystem::path& path,
                        std::size_t max_bytes) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    std::string data;
    data.resize(max_bytes);
    input.read(data.data(), static_cast<std::streamsize>(max_bytes));
    data.resize(static_cast<std::size_t>(input.gcount()));
    return data;
}

std::string not_found_page(const std::string& message) {
    std::ostringstream out;
    out << page_open("Not found", "Not found")
        << "<div class=\"card empty\">" << html_escape(message)
        << "</div><p><a href=\"/\">Return to the archive</a></p>"
        << page_close();
    return out.str();
}

std::string build_detail(Db& db,
                         Config& cfg,
                         int64_t id,
                         std::string& missing) {
    auto snapshot = db.get(id);
    if (!snapshot) {
        missing = "Snapshot #" + std::to_string(id) + " does not exist.";
        return {};
    }

    std::vector<std::string> tags;
    std::vector<ExtractedLink> links;
    try {
        tags = db.tags(id);
        links = db.links(id, 5000);
    } catch (...) {
    }

    std::ostringstream out;
    std::string heading = snapshot->title.empty() ? snapshot->url
                                                   : snapshot->title;
    out << page_open("ArchiveBox.cpp", heading)
        << "<p class=\"sub\"><a href=\"" << html_escape(snapshot->url)
        << "\" rel=\"noreferrer noopener\">" << html_escape(snapshot->url)
        << "</a></p>";
    if (!snapshot->description.empty()) {
        out << "<p>" << html_escape(snapshot->description) << "</p>";
    }
    if (!snapshot->error.empty()) {
        out << "<div class=\"error\">" << html_escape(snapshot->error)
            << "</div>";
    }

    out << "<div class=\"tags\">" << tag_chips(tags) << "</div>"
        << "<div class=\"card\" style=\"padding:18px\"><dl class=\"dl\">"
        << "<dt>Status</dt><dd><span class=\"status st-"
        << status_slug(snapshot->status) << "\">"
        << html_escape(snapshot->status) << "</span></dd>"
        << "<dt>HTTP status</dt><dd>" << snapshot->http_status << "</dd>"
        << "<dt>Content type</dt><dd>" << html_escape(snapshot->content_type)
        << "</dd><dt>Captured</dt><dd>" << html_escape(snapshot->finished_at)
        << "</dd><dt>Size</dt><dd>" << format_size(snapshot->size_bytes)
        << "</dd>";
    if (!snapshot->site_name.empty()) {
        out << "<dt>Site</dt><dd>" << html_escape(snapshot->site_name)
            << "</dd>";
    }
    if (!snapshot->author.empty()) {
        out << "<dt>Author</dt><dd>" << html_escape(snapshot->author)
            << "</dd>";
    }
    if (!snapshot->published_at.empty()) {
        out << "<dt>Published</dt><dd>" << html_escape(snapshot->published_at)
            << "</dd>";
    }
    if (!snapshot->language.empty()) {
        out << "<dt>Language</dt><dd>" << html_escape(snapshot->language)
            << "</dd>";
    }
    if (!snapshot->favicon_url.empty()) {
        out << "<dt>Favicon</dt><dd><a href=\""
            << html_escape(snapshot->favicon_url)
            << "\" rel=\"noreferrer noopener\">"
            << html_escape(snapshot->favicon_url) << "</a></dd>";
    }
    if (!snapshot->canonical_url.empty()) {
        out << "<dt>Canonical URL</dt><dd>"
            << html_escape(snapshot->canonical_url) << "</dd>";
    }
    if (!snapshot->sha256.empty()) {
        out << "<dt>SHA-256</dt><dd><code>" << html_escape(snapshot->sha256)
            << "</code></dd>";
    }
    out << "</dl><div class=\"actions\">";
    if (!snapshot->text_path.empty()) {
        out << "<a href=\"/snapshot/" << id << "/text\">extracted text</a>";
    }
    if (!snapshot->body_path.empty()) {
        out << "<a href=\"/snapshot/" << id << "/body\">raw body</a>";
    }
    if (!snapshot->warc_path.empty()) {
        out << "<a href=\"/snapshot/" << id << "/warc\">WARC</a>";
    }
    if (!snapshot->headers_path.empty()) {
        out << "<a href=\"/snapshot/" << id << "/headers\">headers</a>";
    }
    if (!snapshot->metadata_path.empty()) {
        out << "<a href=\"/snapshot/" << id << "/metadata\">metadata JSON</a>";
    }
    out << "</div></div>";

    if (!snapshot->text_path.empty()) {
        std::string preview;
        try {
            preview = read_prefix(cfg.resolve(snapshot->text_path), 6000);
        } catch (...) {
        }
        out << "<h2>Text preview</h2>";
        if (preview.empty()) {
            out << "<div class=\"card empty\">No text was extracted.</div>";
        } else {
            out << "<pre>" << html_escape(preview);
            if (preview.size() >= 6000) {
                out << "\n\n...";
            }
            out << "</pre>";
            out << "<p class=\"small\"><a href=\"/snapshot/" << id
                << "/text\">Open the full extracted text</a></p>";
        }
    }

    if (!links.empty()) {
        out << "<h2>Outgoing links <span class=\"small\">(" << links.size()
            << ")</span></h2><div class=\"card\" style=\"padding:0 18px\">"
               "<ul class=\"links\">";
        std::size_t shown = 0;
        for (const auto& link : links) {
            if (shown++ >= 300) {
                break;
            }
            out << "<li><a href=\"" << html_escape(link.url)
                << "\" rel=\"noreferrer noopener\">"
                << html_escape(link.text.empty() ? link.url : link.text)
                << "</a><div class=\"small\">" << html_escape(link.url)
                << "</div></li>";
        }
        out << "</ul></div>";
        if (links.size() > 300) {
            out << "<p class=\"small\">Showing the first 300 links.</p>";
        }
    }
    out << page_close();
    return out.str();
}

bool regular_file(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

std::string safe_header_value(const std::string& value) {
    std::string out = value;
    for (char& c : out) {
        unsigned char byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7f) {
            c = ' ';
        }
    }
    return out;
}

void serve_file(int fd,
                const std::filesystem::path& path,
                const std::string& content_type,
                const std::string& disposition,
                bool head_only) {
    std::error_code error;
    std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error) {
        respond_text(fd, 500, "Cannot read archive file.\n", head_only);
        return;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        respond_text(fd, 500, "Cannot read archive file.\n", head_only);
        return;
    }
    std::vector<std::string> headers;
    if (!disposition.empty()) {
        headers.push_back("Content-Disposition: " +
                          safe_header_value(disposition));
    }
    std::string head = response_head(200, safe_header_value(content_type),
                                     static_cast<std::size_t>(size), headers);
    if (!send_all(fd, head) || head_only) {
        return;
    }
    char buffer[65536];
    while (input) {
        input.read(buffer, sizeof(buffer));
        std::size_t count = static_cast<std::size_t>(input.gcount());
        if (count == 0) {
            break;
        }
        if (!send_all(fd, buffer, count)) {
            return;
        }
    }
}

bool parse_id(const std::string& value, int64_t& id) {
    if (value.empty() || value.size() > 18) {
        return false;
    }
    for (char c : value) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    try {
        id = std::stoll(value);
    } catch (...) {
        return false;
    }
    return id > 0;
}

std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> parts;
    std::string part;
    for (std::size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (!part.empty()) {
                parts.push_back(part);
                part.clear();
            }
        } else {
            part.push_back(path[i]);
        }
    }
    return parts;
}

void serve_snapshot_file(int fd,
                         Db& db,
                         Config& cfg,
                         int64_t id,
                         const std::string& kind,
                         bool head_only) {
    auto snapshot = db.get(id);
    if (!snapshot) {
        respond_text(fd, 404, "Snapshot not found\n", head_only);
        return;
    }

    std::string relative;
    std::string content_type;
    std::string disposition;
    if (kind == "text") {
        relative = snapshot->text_path;
        content_type = "text/plain; charset=utf-8";
    } else if (kind == "headers") {
        relative = snapshot->headers_path;
        content_type = "text/plain; charset=utf-8";
    } else if (kind == "metadata") {
        relative = snapshot->metadata_path;
        content_type = "application/json; charset=utf-8";
    } else if (kind == "body") {
        relative = snapshot->body_path;
        content_type = snapshot->content_type.empty()
                           ? "application/octet-stream"
                           : snapshot->content_type;
        disposition = "attachment; filename=\"snapshot-" +
                      std::to_string(id) + ".bin\"";
    } else if (kind == "warc") {
        relative = snapshot->warc_path;
        content_type = "application/gzip";
        disposition = "attachment; filename=\"snapshot-" +
                      std::to_string(id) + ".warc.gz\"";
    } else {
        respond_text(fd, 404, "Unknown snapshot file\n", head_only);
        return;
    }

    if (relative.empty()) {
        respond_text(fd, 404, "This snapshot has no such file.\n", head_only);
        return;
    }
    std::filesystem::path path = cfg.resolve(relative);
    if (!regular_file(path)) {
        respond_text(fd, 404, "Archive file is missing.\n", head_only);
        return;
    }
    serve_file(fd, path, content_type, disposition, head_only);
}

void handle_client(int fd, Db& db, Config& cfg) {
    Request request;
    if (!read_request(fd, request)) {
        respond_text(fd, 400, "Bad request\n");
        return;
    }
    if (request.method != "GET" && request.method != "HEAD") {
        respond(fd, 405, "text/plain; charset=utf-8", "Method not allowed\n",
                {"Allow: GET, HEAD"});
        return;
    }
    bool head = request.method == "HEAD";

    if (request.path == "/healthz") {
        respond_text(fd, 200, "ok\n", head);
        return;
    }
    if (request.path == "/favicon.ico") {
        respond(fd, 204, "image/x-icon", "", {}, head);
        return;
    }
    if (request.path == "/") {
        std::map<std::string, std::string> query = parse_query(request.query);
        std::string tag = query.count("tag") ? trim(query["tag"]) : std::string();
        if (tag.size() > 128) {
            tag.resize(128);
        }
        std::string error;
        std::string page = build_index(db, query["q"], tag, error);
        respond_html(fd, error.empty() ? 200 : 500, page, head);
        return;
    }

    std::vector<std::string> parts = split_path(request.path);
    if (!parts.empty() && parts[0] == "snapshot" && parts.size() >= 2) {
        int64_t id = 0;
        if (!parse_id(parts[1], id)) {
            respond_html(fd, 404, not_found_page("Invalid snapshot id."), head);
            return;
        }
        if (parts.size() == 2) {
            std::string missing;
            std::string page = build_detail(db, cfg, id, missing);
            if (!missing.empty()) {
                respond_html(fd, 404, not_found_page(missing), head);
            } else {
                respond_html(fd, 200, page, head);
            }
            return;
        }
        if (parts.size() == 3) {
            serve_snapshot_file(fd, db, cfg, id, parts[2], head);
            return;
        }
        respond_text(fd, 404, "Not found\n", head);
        return;
    }
    if (!parts.empty() && parts[0] == "search") {
        std::map<std::string, std::string> query = parse_query(request.query);
        std::string error;
        std::string page = build_index(db, query["q"], query["tag"], error);
        respond_html(fd, error.empty() ? 200 : 500, page, head);
        return;
    }

    respond_html(fd, 404, not_found_page("No route for that path."), head);
}

int create_listener(const std::string& host, int port, std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* addresses = nullptr;
    std::string port_text = std::to_string(port);
    int rc = getaddrinfo(host.empty() ? nullptr : host.c_str(), port_text.c_str(),
                         &hints, &addresses);
    if (rc != 0) {
        error = std::string("cannot resolve address: ") + gai_strerror(rc);
        return -1;
    }

    int listener = -1;
    for (addrinfo* address = addresses; address != nullptr;
         address = address->ai_next) {
        int fd = socket(address->ai_family, address->ai_socktype,
                        address->ai_protocol);
        if (fd < 0) {
            continue;
        }
        int reuse = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        if (bind(fd, address->ai_addr, address->ai_addrlen) == 0 &&
            listen(fd, 64) == 0) {
            listener = fd;
            break;
        }
        close(fd);
    }
    freeaddrinfo(addresses);
    if (listener < 0) {
        error = "cannot bind to " + host + ":" + port_text;
    }
    return listener;
}

} // namespace

int cmd_serve(Config& cfg, const std::string& host, int port) {
    if (port <= 0 || port > 65535) {
        std::cerr << "arcbox: invalid port: " << port << "\n";
        return 1;
    }
    cfg.ensure_dirs();
    Db db(cfg.db_path());
    db.migrate();

    if (host != "127.0.0.1" && host != "localhost" && host != "::1") {
        std::cerr << "arcbox: warning: the web UI has no authentication; "
                     "bind it to localhost unless you put it behind a "
                     "trusted reverse proxy\n";
    }

    std::string error;
    int listener = create_listener(host, port, error);
    if (listener < 0) {
        std::cerr << "arcbox: " << error << "\n";
        return 1;
    }

    struct sigaction action {};
    action.sa_handler = stop_handler;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
    signal(SIGPIPE, SIG_IGN);

    std::cout << "arcbox web UI listening on http://" << host << ":" << port
              << "/\n";
    std::cout.flush();

    while (!g_stop) {
        pollfd descriptor{};
        descriptor.fd = listener;
        descriptor.events = POLLIN;
        int ready = poll(&descriptor, 1, 500);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (ready == 0) {
            continue;
        }
        int client = accept(listener, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            continue;
        }
        timeval timeout{};
        timeout.tv_sec = 10;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout));
        try {
            handle_client(client, db, cfg);
        } catch (const std::exception& exception) {
            try {
                respond_text(client, 500, "Internal error\n");
            } catch (...) {
            }
            std::cerr << "arcbox: web request failed: " << exception.what()
                      << "\n";
        }
        close(client);
    }

    close(listener);
    return 0;
}

} // namespace arcbox
