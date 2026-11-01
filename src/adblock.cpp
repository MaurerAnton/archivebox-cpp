#include "arcbox/adblock.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>

#include "arcbox/util.hpp"

namespace arcbox {

bool parse_block_rule(const std::string& raw, BlockRule& out) {
    std::string line = trim(raw);
    if (line.empty() || line[0] == '#' || line[0] == '!' || line[0] == '[') {
        return false;
    }

    bool exception = false;
    if (starts_with(line, "@@")) {
        exception = true;
        line = line.substr(2);
    }

    if (starts_with(line, "||")) {
        line = line.substr(2);
    }

    std::size_t options = line.find('$');
    if (options != std::string::npos) {
        line.erase(options);
    }
    std::size_t caret = line.find('^');
    if (caret != std::string::npos) {
        line.erase(caret);
    }
    std::size_t slash = line.find('/');
    if (slash != std::string::npos) {
        line.erase(slash);
    }

    std::istringstream fields_stream(line);
    std::vector<std::string> fields;
    std::string field;
    while (fields_stream >> field) {
        fields.push_back(field);
    }
    if (fields.empty()) {
        return false;
    }
    if (fields.size() >= 2 &&
        (fields[0] == "0.0.0.0" || fields[0] == "127.0.0.1")) {
        line = fields[1];
    } else {
        line = fields[0];
    }

    line = lower(trim(line));
    if (line.empty() || line.find('*') != std::string::npos ||
        line.find('?') != std::string::npos) {
        return false;
    }
    out.host = line;
    out.exception = exception;
    return true;
}

Adblock::Adblock(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        return;
    }

    std::string line;
    while (std::getline(in, line)) {
        BlockRule rule;
        if (!parse_block_rule(line, rule)) {
            continue;
        }
        if (rule.exception) {
            exceptions_.insert(rule.host);
        } else {
            domains_.insert(rule.host);
        }
    }
}

bool Adblock::matches(const std::unordered_set<std::string>& set,
                      const std::string& host) {
    std::string candidate = host;
    while (true) {
        if (set.count(candidate)) {
            return true;
        }
        std::size_t dot = candidate.find('.');
        if (dot == std::string::npos) {
            return false;
        }
        candidate = candidate.substr(dot + 1);
    }
}

std::string Adblock::matched_rule(const std::string& url) const {
    std::string candidate = url_host(url);
    if (candidate.empty() || matches(exceptions_, candidate)) {
        return "";
    }
    while (true) {
        if (domains_.count(candidate)) {
            return candidate;
        }
        const std::size_t dot = candidate.find('.');
        if (dot == std::string::npos) {
            return "";
        }
        candidate = candidate.substr(dot + 1);
    }
}

bool Adblock::blocked(const std::string& url) const {
    const std::string host = url_host(url);
    if (host.empty()) {
        return false;
    }
    if (matches(exceptions_, host)) {
        return false;
    }
    return matches(domains_, host);
}

std::size_t Adblock::rules() const {
    return domains_.size() + exceptions_.size();
}

std::vector<std::string> Adblock::resolver_rules(const std::string& page_url,
                                                 std::size_t limit) const {
    const std::string page_host = url_host(page_url);
    std::vector<std::string> rules;
    rules.reserve(domains_.size());
    for (const std::string& domain : domains_) {
        if (matches(exceptions_, domain)) {
            continue;
        }
        // Never shadow the document itself: if the page lives on the blocked
        // domain (capture with --no-block), its own host and parents stay
        // resolvable.
        if (!page_host.empty() &&
            (page_host == domain || ends_with(page_host, "." + domain))) {
            continue;
        }
        rules.push_back(domain);
    }
    std::sort(rules.begin(), rules.end());
    if (rules.size() > limit) {
        rules.resize(limit);
    }
    return rules;
}

} // namespace arcbox
