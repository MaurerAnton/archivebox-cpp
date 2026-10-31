#pragma once

#include <filesystem>
#include <string>

#include "arcbox/capture_http.hpp"
#include "arcbox/config.hpp"

namespace arcbox {

bool write_warc(const std::filesystem::path& output,
                const HttpCapture& capture,
                const Config& cfg,
                std::string& error);

} // namespace arcbox
