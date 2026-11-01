#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "arcbox/config.hpp"
#include "arcbox/memory.hpp"

namespace arcbox {

// How much of the page to render. The ladder always starts with the DOM: it is
// the cheapest artifact and the one replay needs when the rest fails.
enum class RenderTier {
    Skip,
    Dom,
    Screenshot,
    Full,
};

struct RenderPlan {
    RenderTier tier = RenderTier::Skip;
    std::string reason;

    bool dom() const {
        return tier == RenderTier::Dom || tier == RenderTier::Screenshot ||
               tier == RenderTier::Full;
    }
    bool screenshot() const {
        return tier == RenderTier::Screenshot || tier == RenderTier::Full;
    }
    bool pdf() const { return tier == RenderTier::Full; }
};

struct RenderResult {
    bool attempted = false;
    bool ok = false;
    std::string rendered_html_path;
    std::string screenshot_path;
    std::string pdf_path;
    std::string error;
    // Request URLs the browser wanted and interception refused.
    std::vector<std::string> blocked_urls;
};

// First usable headless browser, or an empty string when none is installed.
std::string find_browser(const Config& cfg);

// Pick the cheapest artifact set the host can currently afford. Takes a fresh
// memory reading and the already fetched source size, because a page that was
// cheap to download can still be expensive to render.
RenderPlan choose_render_plan(const Config& cfg,
                              const HostMemory& mem,
                              int64_t source_bytes);

const char* render_tier_name(RenderTier tier);

// Renders one page into the snapshot directory, honouring the plan. Each
// artifact gets its own short-lived browser process, so a renderer that hangs
// or balloons in memory is disposable instead of taking the daemon with it.
RenderResult render_page(const Config& cfg,
                         const std::string& url,
                         int64_t id,
                         const RenderPlan& plan);

} // namespace arcbox
