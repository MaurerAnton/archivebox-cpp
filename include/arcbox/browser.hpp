#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "arcbox/config.hpp"

namespace arcbox {

struct RenderResult {
    bool attempted = false;
    bool ok = false;
    std::string rendered_html_path;
    std::string screenshot_path;
    std::string pdf_path;
    std::string error;
};

// First usable headless browser, or an empty string when none is installed.
std::string find_browser(const Config& cfg);

// Renders one page into the snapshot directory. Each artifact gets its own
// short-lived browser process, so a renderer that hangs or balloons in memory
// is disposable instead of taking the daemon with it.
RenderResult render_page(const Config& cfg,
                         const std::string& url,
                         int64_t id);

} // namespace arcbox
