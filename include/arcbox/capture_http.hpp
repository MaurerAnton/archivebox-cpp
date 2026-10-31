#pragma once

#include <string>

#include "arcbox/config.hpp"

namespace arcbox {

struct HttpCapture {
    std::string requested_url;
    std::string effective_url;
    int status_code = 0;
    std::string status_line;
    std::string headers;
    std::string body;
    std::string content_type;
    std::string error;

    bool ok() const { return error.empty() && status_code > 0; }
};

HttpCapture fetch_http(const std::string& url, const Config& cfg);

} // namespace arcbox
