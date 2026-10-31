#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <unordered_set>

namespace arcbox {

class Adblock {
public:
    explicit Adblock(const std::filesystem::path& path);

    bool blocked(const std::string& url) const;
    std::size_t rules() const;

private:
    std::unordered_set<std::string> domains_;
    std::unordered_set<std::string> exceptions_;

    static bool matches(const std::unordered_set<std::string>& set,
                        const std::string& host);
};

} // namespace arcbox
