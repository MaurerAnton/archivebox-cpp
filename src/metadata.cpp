#include "arcbox/metadata.hpp"

#include <cctype>
#include <map>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "arcbox/util.hpp"

namespace arcbox {

namespace {

bool match_ci(const std::string& haystack,
              std::size_t position,
              const char* needle,
              std::size_t needle_size) {
    if (position + needle_size > haystack.size()) {
        return false;
    }
    for (std::size_t i = 0; i < needle_size; ++i) {
        unsigned char left =
            static_cast<unsigned char>(haystack[position + i]);
        unsigned char right = static_cast<unsigned char>(needle[i]);
        if (std::tolower(left) != std::tolower(right)) {
            return false;
        }
    }
    return true;
}

std::size_t find_ci(const std::string& haystack,
                    const char* needle,
                    std::size_t start = 0) {
    std::size_t needle_size = std::string(needle).size();
    if (needle_size == 0 || start >= haystack.size()) {
        return std::string::npos;
    }
    for (std::size_t i = start; i + needle_size <= haystack.size(); ++i) {
        if (match_ci(haystack, i, needle, needle_size)) {
            return i;
        }
    }
    return std::string::npos;
}

std::size_t tag_end(const std::string& html, std::size_t start) {
    char quote = '\0';
    for (std::size_t i = start; i < html.size(); ++i) {
        char c = html[i];
        if (quote != '\0') {
            if (c == quote) {
                quote = '\0';
            }
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '>') {
            return i;
        }
    }
    return std::string::npos;
}

std::map<std::string, std::string> parse_attributes(const std::string& tag) {
    std::map<std::string, std::string> attributes;
    std::size_t i = 1;
    while (i < tag.size() &&
           !std::isspace(static_cast<unsigned char>(tag[i])) &&
           tag[i] != '>' && tag[i] != '/') {
        ++i;
    }

    while (i < tag.size()) {
        while (i < tag.size() &&
               std::isspace(static_cast<unsigned char>(tag[i]))) {
            ++i;
        }
        if (i >= tag.size() || tag[i] == '>' || tag[i] == '/') {
            break;
        }

        std::string name;
        while (i < tag.size() &&
               !std::isspace(static_cast<unsigned char>(tag[i])) &&
               tag[i] != '=' && tag[i] != '>' && tag[i] != '/') {
            name.push_back(tag[i]);
            ++i;
        }
        if (name.empty()) {
            ++i;
            continue;
        }

        while (i < tag.size() &&
               std::isspace(static_cast<unsigned char>(tag[i]))) {
            ++i;
        }

        std::string value;
        if (i < tag.size() && tag[i] == '=') {
            ++i;
            while (i < tag.size() &&
                   std::isspace(static_cast<unsigned char>(tag[i]))) {
                ++i;
            }
            if (i < tag.size() && (tag[i] == '"' || tag[i] == '\'')) {
                char quote = tag[i++];
                while (i < tag.size() && tag[i] != quote) {
                    value.push_back(tag[i]);
                    ++i;
                }
                if (i < tag.size()) {
                    ++i;
                }
            } else {
                while (i < tag.size() &&
                       !std::isspace(static_cast<unsigned char>(tag[i])) &&
                       tag[i] != '>') {
                    value.push_back(tag[i]);
                    ++i;
                }
            }
            value = html_unescape(value);
        }
        attributes[lower(name)] = value;
    }
    return attributes;
}

std::string attribute(const std::map<std::string, std::string>& attributes,
                      const std::string& name) {
    auto it = attributes.find(lower(name));
    return it == attributes.end() ? std::string() : it->second;
}

std::string truncate(std::string value, std::size_t max_bytes) {
    value = trim(std::move(value));
    if (value.size() > max_bytes) {
        value.resize(max_bytes);
    }
    return value;
}

bool has_http_scheme(const std::string& url) {
    return starts_with(lower(url), "http://") ||
           starts_with(lower(url), "https://");
}

std::string strip_fragment(const std::string& url) {
    std::size_t fragment = url.find('#');
    if (fragment == std::string::npos) {
        return url;
    }
    return url.substr(0, fragment);
}

std::string resolve_http(const std::string& base,
                         const std::string& href) {
    std::string resolved = url_resolve(base, href);
    if (resolved.empty() || !has_http_scheme(resolved)) {
        return {};
    }
    return strip_fragment(resolved);
}

bool rel_contains(const std::string& rel, const std::string& wanted) {
    std::string normalized = lower(rel);
    std::string token;
    for (std::size_t i = 0; i <= normalized.size(); ++i) {
        if (i == normalized.size() ||
            std::isspace(static_cast<unsigned char>(normalized[i]))) {
            if (!token.empty() && token == lower(wanted)) {
                return true;
            }
            token.clear();
        } else {
            token.push_back(normalized[i]);
        }
    }
    return false;
}

std::string icon_from_rel(const std::string& rel) {
    std::string normalized = lower(rel);
    if (normalized.find("mask-icon") != std::string::npos) {
        return {};
    }
    if (normalized.find("apple-touch-icon") != std::string::npos ||
        rel_contains(normalized, "icon")) {
        return normalized;
    }
    return {};
}

} // namespace

PageMetadata extract_metadata(const std::string& html,
                              const std::string& base_url) {
    PageMetadata metadata;
    metadata.title = truncate(html_title(html), 2000);
    std::string document_base = base_url;

    std::size_t html_tag = find_ci(html, "<html");
    if (html_tag != std::string::npos) {
        std::size_t end = tag_end(html, html_tag);
        if (end != std::string::npos) {
            auto attributes =
                parse_attributes(html.substr(html_tag, end - html_tag + 1));
            metadata.language = truncate(attribute(attributes, "lang"), 64);
        }
    }

    std::size_t base_tag = find_ci(html, "<base");
    if (base_tag != std::string::npos) {
        std::size_t end = tag_end(html, base_tag);
        if (end != std::string::npos) {
            auto attributes =
                parse_attributes(html.substr(base_tag, end - base_tag + 1));
            std::string href = attribute(attributes, "href");
            std::string resolved = resolve_http(base_url, href);
            if (!resolved.empty()) {
                document_base = resolved;
            }
        }
    }

    std::size_t position = 0;
    bool canonical_from_meta = false;
    while ((position = find_ci(html, "<meta", position)) !=
           std::string::npos) {
        std::size_t after_name = position + 5;
        if (after_name < html.size() &&
            !std::isspace(static_cast<unsigned char>(html[after_name])) &&
            html[after_name] != '>' && html[after_name] != '/') {
            position = after_name;
            continue;
        }
        std::size_t end = tag_end(html, position);
        if (end == std::string::npos) {
            break;
        }
        auto attributes =
            parse_attributes(html.substr(position, end - position + 1));
        std::string key = lower(attribute(attributes, "name"));
        if (key.empty()) {
            key = lower(attribute(attributes, "property"));
        }
        if (key.empty()) {
            key = lower(attribute(attributes, "itemprop"));
        }
        if (key.empty()) {
            key = lower(attribute(attributes, "http-equiv"));
        }
        std::string content = truncate(attribute(attributes, "content"), 4000);

        if (key == "description" && metadata.description.empty()) {
            metadata.description = content;
        } else if (key == "og:description" && metadata.description.empty()) {
            metadata.description = content;
        } else if (key == "twitter:description" &&
                   metadata.description.empty()) {
            metadata.description = content;
        } else if (key == "og:site_name" && metadata.site_name.empty()) {
            metadata.site_name = truncate(content, 512);
        } else if (key == "author" && metadata.author.empty()) {
            metadata.author = truncate(content, 512);
        } else if (key == "article:author" && metadata.author.empty()) {
            metadata.author = truncate(content, 512);
        } else if (key == "twitter:creator" && metadata.author.empty()) {
            metadata.author = truncate(content, 512);
        } else if (key == "article:published_time" &&
                   metadata.published_at.empty()) {
            metadata.published_at = truncate(content, 256);
        } else if (key == "og:published_time" &&
                   metadata.published_at.empty()) {
            metadata.published_at = truncate(content, 256);
        } else if ((key == "date" || key == "pubdate" ||
                    key == "dc.date" || key == "dc.date.issued" ||
                    key == "dcterms.created") &&
                   metadata.published_at.empty()) {
            metadata.published_at = truncate(content, 256);
        } else if (key == "og:title" && metadata.title.empty()) {
            metadata.title = truncate(content, 2000);
        } else if (key == "og:url" && metadata.canonical_url.empty()) {
            metadata.canonical_url = resolve_http(document_base, content);
            if (metadata.canonical_url.size() > 4096) {
                metadata.canonical_url.clear();
            } else if (!metadata.canonical_url.empty()) {
                canonical_from_meta = true;
            }
        } else if ((key == "content-language" || key == "language") &&
                   metadata.language.empty()) {
            metadata.language = truncate(content, 64);
        } else if (key == "og:locale" && metadata.language.empty()) {
            metadata.language = truncate(content, 64);
        }
        position = end + 1;
    }

    position = 0;
    while ((position = find_ci(html, "<link", position)) !=
           std::string::npos) {
        std::size_t after_name = position + 5;
        if (after_name < html.size() &&
            !std::isspace(static_cast<unsigned char>(html[after_name])) &&
            html[after_name] != '>' && html[after_name] != '/') {
            position = after_name;
            continue;
        }
        std::size_t end = tag_end(html, position);
        if (end == std::string::npos) {
            break;
        }
        auto attributes =
            parse_attributes(html.substr(position, end - position + 1));
        std::string rel = attribute(attributes, "rel");
        std::string href = attribute(attributes, "href");
        std::string resolved = resolve_http(document_base, href);
        if (rel_contains(rel, "canonical") &&
            !resolved.empty() &&
            (metadata.canonical_url.empty() || canonical_from_meta)) {
            metadata.canonical_url = resolved;
            canonical_from_meta = false;
        }
        if (!icon_from_rel(rel).empty() && metadata.favicon_url.empty() &&
            !resolved.empty()) {
            metadata.favicon_url = resolved;
        }
        position = end + 1;
    }

    std::unordered_set<std::string> seen;
    position = 0;
    while ((position = find_ci(html, "<a", position)) !=
           std::string::npos) {
        std::size_t after_name = position + 2;
        if (after_name >= html.size() ||
            (!std::isspace(static_cast<unsigned char>(html[after_name])) &&
             html[after_name] != '>' && html[after_name] != '/')) {
            position = after_name;
            continue;
        }
        std::size_t end = tag_end(html, position);
        if (end == std::string::npos) {
            break;
        }
        auto attributes =
            parse_attributes(html.substr(position, end - position + 1));
        std::string href = attribute(attributes, "href");
        std::string resolved = resolve_http(document_base, href);
        if (!resolved.empty() && resolved.size() <= 4096 &&
            seen.insert(resolved).second) {
            std::size_t close = find_ci(html, "</a", end + 1);
            std::string text;
            if (close != std::string::npos && close > end + 1) {
                std::size_t limit = 4096;
                std::size_t length = close - end - 1;
                if (length > limit) {
                    length = limit;
                }
                text = truncate(html_to_text(html.substr(end + 1, length)),
                                512);
            }
            metadata.links.push_back({std::move(resolved), std::move(text)});
            if (metadata.links.size() >= 2000) {
                break;
            }
        }
        position = end + 1;
    }

    if (metadata.canonical_url.size() > 4096) {
        metadata.canonical_url.clear();
    }
    if (metadata.favicon_url.size() > 4096) {
        metadata.favicon_url.clear();
    }
    return metadata;
}

std::string metadata_json(const PageMetadata& metadata) {
    std::ostringstream out;
    out << "{\"title\":\"" << json_escape(metadata.title) << "\""
        << ",\"description\":\"" << json_escape(metadata.description) << "\""
        << ",\"canonical_url\":\"" << json_escape(metadata.canonical_url)
        << "\""
        << ",\"site_name\":\"" << json_escape(metadata.site_name) << "\""
        << ",\"author\":\"" << json_escape(metadata.author) << "\""
        << ",\"published_at\":\"" << json_escape(metadata.published_at)
        << "\""
        << ",\"language\":\"" << json_escape(metadata.language) << "\""
        << ",\"favicon_url\":\"" << json_escape(metadata.favicon_url) << "\""
        << ",\"links_count\":" << metadata.links.size() << "}";
    return out.str();
}

} // namespace arcbox
