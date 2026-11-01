#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace arcbox {

std::string trim(std::string s);
std::string lower(std::string s);
bool starts_with(const std::string& s, const std::string& prefix);
bool ends_with(const std::string& s, const std::string& suffix);
std::vector<std::string> split(const std::string& s, char delimiter);

std::string now_iso8601();
std::string timestamp_slug();
std::string uuid_v4();

std::string sha256_hex(const std::string& data);
std::string sha256_hex(const std::vector<unsigned char>& data);

std::string url_host(const std::string& url);
std::string url_authority(const std::string& url);
std::string url_path(const std::string& url);
std::string url_origin(const std::string& url);
std::string url_canonicalize(const std::string& url);
std::string url_resolve(const std::string& base, const std::string& relative);

std::string html_title(const std::string& html);
std::string html_to_text(const std::string& html);
std::string html_unescape(const std::string& text);
std::string html_escape(const std::string& text);

std::string json_escape(const std::string& s);
std::string format_size(int64_t bytes);
// Parses "30d", "12h", "90m", "45s" and bare numbers (days). Returns false for
// anything else, leaving `out` untouched.
bool parse_duration_seconds(const std::string& text, int64_t& out);
std::string getenv_or(const char* name, const std::string& fallback);
std::string read_self_exe();

} // namespace arcbox
