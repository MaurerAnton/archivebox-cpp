#include "arcbox/adblock.hpp"

#include <fstream>
#include <sstream>
#include <string>

#include "arcbox/util.hpp"

namespace arcbox {

Adblock::Adblock(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        return;
    }

    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == '!') {
            continue;
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
            continue;
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
            continue;
        }
        if (exception) {
            exceptions_.insert(line);
        } else {
            domains_.insert(line);
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

} // namespace arcbox
