#include "arcbox/util.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace arcbox {

std::string trim(std::string s) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::string lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<std::string> split(const std::string& s, char delimiter) {
    std::vector<std::string> out;
    std::string part;
    for (char c : s) {
        if (c == delimiter) {
            out.push_back(part);
            part.clear();
        } else {
            part.push_back(c);
        }
    }
    out.push_back(part);
    return out;
}

std::string now_iso8601() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

std::string timestamp_slug() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tm);
    return std::string(buf);
}

std::string uuid_v4() {
    unsigned char bytes[16];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
        throw std::runtime_error("RAND_bytes failed");
    }
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);

    char buf[40];
    std::snprintf(buf, sizeof(buf),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
                  "%02x%02x%02x%02x%02x%02x",
                  bytes[0], bytes[1], bytes[2], bytes[3], bytes[4],
                  bytes[5], bytes[6], bytes[7], bytes[8], bytes[9],
                  bytes[10], bytes[11], bytes[12], bytes[13], bytes[14],
                  bytes[15]);
    return std::string(buf);
}

static std::string digest_hex(const unsigned char* data, size_t size) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        throw std::runtime_error("EVP_MD_CTX_new failed");
    }
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int md_len = 0;
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx, data, size) != 1 ||
        EVP_DigestFinal_ex(ctx, md, &md_len) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("SHA-256 failed");
    }
    EVP_MD_CTX_free(ctx);

    std::string out;
    out.reserve(md_len * 2);
    for (unsigned int i = 0; i < md_len; ++i) {
        char buf[3];
        std::snprintf(buf, sizeof(buf), "%02x", md[i]);
        out.append(buf);
    }
    return out;
}

std::string sha256_hex(const std::string& data) {
    return digest_hex(reinterpret_cast<const unsigned char*>(data.data()),
                      data.size());
}

std::string sha256_hex(const std::vector<unsigned char>& data) {
    return digest_hex(data.data(), data.size());
}

std::string url_host(const std::string& url) {
    std::string authority = url_authority(url);
    std::size_t colon = authority.find(':');
    if (colon != std::string::npos) {
        authority = authority.substr(0, colon);
    }
    return authority;
}

std::string url_authority(const std::string& url) {
    std::size_t scheme = url.find("://");
    if (scheme == std::string::npos) {
        return {};
    }
    std::size_t begin = scheme + 3;
    std::size_t end = url.find_first_of("/?#", begin);
    std::string authority =
        url.substr(begin, end == std::string::npos ? std::string::npos
                                                   : end - begin);
    std::size_t at = authority.rfind('@');
    if (at != std::string::npos) {
        authority = authority.substr(at + 1);
    }
    return lower(authority);
}

std::string url_path(const std::string& url) {
    std::size_t scheme = url.find("://");
    if (scheme == std::string::npos) {
        return "/";
    }
    std::size_t begin = scheme + 3;
    std::size_t slash = url.find('/', begin);
    std::size_t query = url.find('?', begin);
    std::size_t fragment = url.find('#', begin);

    if (slash == std::string::npos || (query != std::string::npos && query < slash)) {
        std::string out = "/";
        if (query != std::string::npos && query < url.size()) {
            out += url.substr(query,
                              fragment == std::string::npos
                                  ? std::string::npos
                                  : fragment - query);
        }
        return out;
    }

    size_t end = fragment == std::string::npos ? url.size() : fragment;
    return url.substr(slash, end - slash);
}

std::string url_canonicalize(const std::string& input) {
    std::string url = trim(input);
    if (url.empty()) {
        return url;
    }
    std::size_t scheme_pos = url.find("://");
    if (scheme_pos == std::string::npos) {
        url = "http://" + url;
        scheme_pos = 4;
    }

    std::string scheme = lower(url.substr(0, scheme_pos));
    std::string rest = url.substr(scheme_pos + 3);

    std::size_t fragment = rest.find('#');
    if (fragment != std::string::npos) {
        rest.erase(fragment);
    }

    std::size_t authority_end = rest.find_first_of("/?");
    std::string authority = rest.substr(0, authority_end);
    std::string tail = authority_end == std::string::npos
                           ? std::string{}
                           : rest.substr(authority_end);

    authority = lower(authority);
    std::size_t at = authority.rfind('@');
    if (at != std::string::npos) {
        authority = authority.substr(at + 1);
    }
    std::size_t colon = authority.find(':');
    if (colon != std::string::npos) {
        std::string port = authority.substr(colon + 1);
        authority = authority.substr(0, colon);
        if ((scheme == "http" && port == "80") ||
            (scheme == "https" && port == "443")) {
            // Drop the default port.
        } else {
            authority += ":" + port;
        }
    }

    return scheme + "://" + authority + tail;
}

std::string url_origin(const std::string& url) {
    std::size_t scheme = url.find("://");
    if (scheme == std::string::npos) {
        return {};
    }
    std::size_t begin = scheme + 3;
    std::size_t end = url.find_first_of("/?#", begin);
    return url.substr(0, end == std::string::npos ? url.size() : end);
}

static std::string normalize_path(const std::string& url) {
    std::size_t scheme = url.find("://");
    if (scheme == std::string::npos) {
        return url;
    }
    std::size_t authority_end = url.find_first_of("/?#", scheme + 3);
    if (authority_end == std::string::npos ||
        url[authority_end] != '/') {
        return url;
    }

    std::size_t path_end = url.find_first_of("?#", authority_end);
    std::string path = url.substr(
        authority_end, path_end == std::string::npos
                           ? std::string::npos
                           : path_end - authority_end);
    std::string suffix = path_end == std::string::npos
                             ? std::string{}
                             : url.substr(path_end);

    std::vector<std::string> segments;
    std::string segment;
    for (std::size_t i = 1; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (segment.empty()) {
                // Collapse repeated slashes.
            } else if (segment == ".") {
                // Skip.
            } else if (segment == "..") {
                if (!segments.empty()) {
                    segments.pop_back();
                }
            } else {
                segments.push_back(segment);
            }
            segment.clear();
        } else {
            segment.push_back(path[i]);
        }
    }

    std::string out = url.substr(0, authority_end) + "/";
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (i > 0) {
            out.push_back('/');
        }
        out += segments[i];
    }
    if (!path.empty() && path.back() == '/' && !segments.empty()) {
        out.push_back('/');
    }
    return out + suffix;
}

std::string url_resolve(const std::string& base, const std::string& relative) {
    std::string href = trim(relative);
    if (href.empty() || href[0] == '#') {
        return {};
    }

    std::size_t colon = href.find(':');
    std::size_t slash = href.find('/');
    if (colon != std::string::npos &&
        (slash == std::string::npos || colon < slash)) {
        std::string scheme = lower(href.substr(0, colon));
        if (scheme != "http" && scheme != "https") {
            return {};
        }
        return url_canonicalize(href);
    }

    std::string base_clean = base;
    std::size_t fragment = base_clean.find('#');
    if (fragment != std::string::npos) {
        base_clean.erase(fragment);
    }
    std::string origin = url_origin(base_clean);
    if (origin.empty()) {
        return {};
    }

    if (starts_with(href, "//")) {
        std::size_t scheme_end = origin.find("://");
        std::string scheme =
            scheme_end == std::string::npos
                ? "https"
                : lower(origin.substr(0, scheme_end));
        return normalize_path(scheme + ":" + href);
    }
    if (href[0] == '/') {
        return normalize_path(origin + href);
    }
    if (href[0] == '?') {
        std::size_t query = base_clean.find('?');
        if (query != std::string::npos) {
            base_clean.erase(query);
        }
        return normalize_path(base_clean + href);
    }

    std::string base_without_query = base_clean;
    std::size_t query = base_without_query.find('?');
    if (query != std::string::npos) {
        base_without_query.erase(query);
    }

    std::string prefix;
    std::size_t last_slash = base_without_query.rfind('/');
    std::size_t scheme_end = origin.find("://");
    std::size_t authority_begin =
        scheme_end == std::string::npos ? 0 : scheme_end + 3;
    if (last_slash != std::string::npos &&
        last_slash >= authority_begin) {
        prefix = base_without_query.substr(0, last_slash + 1);
    } else {
        prefix = origin + "/";
    }

    return normalize_path(prefix + href);
}

static std::string decode_entity(const std::string& entity) {
    if (entity == "amp") return "&";
    if (entity == "lt") return "<";
    if (entity == "gt") return ">";
    if (entity == "quot") return "\"";
    if (entity == "apos") return "'";
    if (entity == "nbsp") return " ";
    if (entity.size() > 1 && entity[0] == '#') {
        int value = 0;
        try {
            if (entity[1] == 'x' || entity[1] == 'X') {
                value = std::stoi(entity.substr(2), nullptr, 16);
            } else {
                value = std::stoi(entity.substr(1));
            }
        } catch (...) {
            return "&" + entity + ";";
        }
        if (value == 0x09 || value == 0x0A || value == 0x0D) {
            return " ";
        }
        if (value >= 0x20 && value < 0x7F) {
            return std::string(1, static_cast<char>(value));
        }
        if (value >= 0x80 && value <= 0x10FFFF &&
            !(value >= 0xD800 && value <= 0xDFFF)) {
            std::string out;
            if (value < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (value >> 6)));
                out.push_back(static_cast<char>(0x80 | (value & 0x3F)));
            } else if (value < 0x10000) {
                out.push_back(static_cast<char>(0xE0 | (value >> 12)));
                out.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (value & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xF0 | (value >> 18)));
                out.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (value & 0x3F)));
            }
            return out;
        }
    }
    return "&" + entity + ";";
}

std::string html_to_text(const std::string& html) {
    std::string out;
    bool in_tag = false;
    bool skip = false;
    std::string tag;
    bool last_space = true;

    for (std::size_t i = 0; i < html.size(); ++i) {
        char c = html[i];
        if (in_tag) {
            if (c == '>') {
                in_tag = false;
                std::string name = trim(tag);
                bool closing = !name.empty() && name[0] == '/';
                if (closing) {
                    name = trim(name.substr(1));
                }
                std::size_t space = name.find_first_of(" \t\r\n/");
                if (space != std::string::npos) {
                    name = name.substr(0, space);
                }
                name = lower(name);
                if (name == "script" || name == "style") {
                    skip = !closing;
                }
                if (!out.empty() && !last_space) {
                    out.push_back(' ');
                    last_space = true;
                }
                tag.clear();
            } else {
                tag.push_back(c);
            }
            continue;
        }

        if (c == '<') {
            in_tag = true;
            tag.clear();
            continue;
        }
        if (skip) {
            continue;
        }
        if (c == '&') {
            std::size_t semi = html.find(';', i + 1);
            if (semi != std::string::npos && semi - i <= 10) {
                std::string entity = html.substr(i + 1, semi - i - 1);
                out += decode_entity(entity);
                last_space = false;
                i = semi;
                continue;
            }
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!last_space) {
                out.push_back(' ');
                last_space = true;
            }
        } else {
            out.push_back(c);
            last_space = false;
        }
    }
    return trim(out);
}

std::string html_title(const std::string& html) {
    std::string low = lower(html);
    std::size_t begin = low.find("<title");
    if (begin == std::string::npos) {
        return {};
    }
    std::size_t open = low.find('>', begin);
    if (open == std::string::npos) {
        return {};
    }
    std::size_t end = low.find("</title", open + 1);
    if (end == std::string::npos) {
        end = low.find('<', open + 1);
    }
    if (end == std::string::npos) {
        return {};
    }
    return trim(html_to_text(html.substr(open + 1, end - open - 1)));
}

std::string html_unescape(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') {
            out.push_back(text[i]);
            continue;
        }
        std::size_t semi = text.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 12) {
            out.push_back('&');
            continue;
        }
        out += decode_entity(text.substr(i + 1, semi - i - 1));
        i = semi;
    }
    return out;
}

std::string html_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 16);
    for (char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}


std::string base64_encode(const std::vector<unsigned char>& data) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 2 < data.size()) {
        const std::uint32_t block = (static_cast<std::uint32_t>(data[i]) << 16) |
                                    (static_cast<std::uint32_t>(data[i + 1]) << 8) |
                                    static_cast<std::uint32_t>(data[i + 2]);
        out += table[(block >> 18) & 0x3f];
        out += table[(block >> 12) & 0x3f];
        out += table[(block >> 6) & 0x3f];
        out += table[block & 0x3f];
        i += 3;
    }
    const std::size_t left = data.size() - i;
    if (left == 1) {
        const std::uint32_t block = static_cast<std::uint32_t>(data[i]) << 16;
        out += table[(block >> 18) & 0x3f];
        out += table[(block >> 12) & 0x3f];
        out += "==";
    } else if (left == 2) {
        const std::uint32_t block = (static_cast<std::uint32_t>(data[i]) << 16) |
                                    (static_cast<std::uint32_t>(data[i + 1]) << 8);
        out += table[(block >> 18) & 0x3f];
        out += table[(block >> 12) & 0x3f];
        out += table[(block >> 6) & 0x3f];
        out += '=';
    }
    return out;
}

bool base64_decode(const std::string& text, std::vector<unsigned char>& out) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };

    out.clear();
    std::uint32_t block = 0;
    int bits = 0;
    for (const char c : text) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') {
            continue;
        }
        const int digit = value(c);
        if (digit < 0) {
            return false;
        }
        block = (block << 6) | static_cast<std::uint32_t>(digit);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((block >> bits) & 0xff));
        }
    }
    return true;
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    return out;
}

std::string format_size(int64_t bytes) {
    if (bytes < 1024) {
        return std::to_string(bytes) + " B";
    }
    double value = static_cast<double>(bytes);
    const char* units[] = {"KiB", "MiB", "GiB", "TiB"};
    int unit = -1;
    do {
        value /= 1024.0;
        ++unit;
    } while (value >= 1024.0 && unit < 3);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f %s", value, units[unit]);
    return std::string(buf);
}

bool parse_duration_seconds(const std::string& text, int64_t& out) {
    std::string value = lower(trim(text));
    if (value.empty()) {
        return false;
    }

    // A bare number is days: retention thresholds are written that way, and
    // anything finer can use an explicit suffix.
    int64_t multiplier = 86400;
    if (std::isalpha(static_cast<unsigned char>(value.back()))) {
        switch (value.back()) {
            case 's':
                multiplier = 1;
                break;
            case 'm':
                multiplier = 60;
                break;
            case 'h':
                multiplier = 3600;
                break;
            case 'd':
                multiplier = 86400;
                break;
            case 'w':
                multiplier = 604800;
                break;
            default:
                return false;
        }
        value.pop_back();
    }

    try {
        const long long number = std::stoll(value);
        if (number < 0) {
            return false;
        }
        out = static_cast<int64_t>(number) * multiplier;
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

std::string getenv_or(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return value && *value ? std::string(value) : fallback;
}

std::string read_self_exe() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        return std::string(buf);
    }
    return "/proc/self/exe";
}

} // namespace arcbox
