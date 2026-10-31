#pragma once

#include <string>
#include <vector>

namespace arcbox {

struct ExtractedLink {
    std::string url;
    std::string text;
};

struct PageMetadata {
    std::string title;
    std::string description;
    std::string canonical_url;
    std::string site_name;
    std::string author;
    std::string published_at;
    std::string language;
    std::string favicon_url;
    std::vector<ExtractedLink> links;
};

PageMetadata extract_metadata(const std::string& html,
                              const std::string& base_url);
std::string metadata_json(const PageMetadata& metadata);

} // namespace arcbox
