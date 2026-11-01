#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace arcbox {

struct BlockRule {
    std::string host;
    bool exception = false;
};

// Parses one blocklist line. Accepts bare domains, hosts-file lines
// ("0.0.0.0 host") and ABP host rules ("||host^", "@@host"). Returns false for
// comments, list headers and patterns that would need a full ABP engine.
bool parse_block_rule(const std::string& line, BlockRule& out);

class Adblock {
public:
    explicit Adblock(const std::filesystem::path& path);

    bool blocked(const std::string& url) const;
    std::size_t rules() const;

    // Domains to point at ~NOTFOUND inside a rendering browser, so ads cannot
    // be fetched by Chrome itself. Mirrors blocked() while skipping exceptions
    // and the page's own domain (and its parents).
    std::vector<std::string> resolver_rules(const std::string& page_url,
                                            std::size_t limit) const;

private:
    std::unordered_set<std::string> domains_;
    std::unordered_set<std::string> exceptions_;

    static bool matches(const std::unordered_set<std::string>& set,
                        const std::string& host);
};

} // namespace arcbox
