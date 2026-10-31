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
std::string url_canonicalize(const std::string& url);

std::string html_title(const std::string& html);
std::string html_to_text(const std::string& html);

std::string json_escape(const std::string& s);
std::string format_size(int64_t bytes);
std::string getenv_or(const char* name, const std::string& fallback);
std::string read_self_exe();

} // namespace arcbox
