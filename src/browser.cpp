#include "arcbox/browser.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "arcbox/adblock.hpp"
#include "arcbox/memory.hpp"
#include "arcbox/util.hpp"

namespace arcbox {

namespace {

// Runs a process with a hard wall-clock ceiling, optionally capturing stdout.
// Returns the exit status, or 124 when the timeout fired.
int run_browser_process(const std::vector<std::string>& argv,
                        const std::string& stdout_path,
                        int timeout_seconds,
                        std::string& error) {
    if (argv.empty()) {
        error = "empty command";
        return 127;
    }

    pid_t pid = fork();
    if (pid < 0) {
        error = "fork failed";
        return 127;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        if (!stdout_path.empty()) {
            int out = open(stdout_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                           0644);
            if (out >= 0) {
                dup2(out, STDOUT_FILENO);
                close(out);
            }
        } else {
            int discard = open("/dev/null", O_WRONLY);
            if (discard >= 0) {
                dup2(discard, STDOUT_FILENO);
                close(discard);
            }
        }
        int quiet = open("/dev/null", O_WRONLY);
        if (quiet >= 0) {
            dup2(quiet, STDERR_FILENO);
            close(quiet);
        }

        std::vector<char*> raw;
        raw.reserve(argv.size() + 1);
        for (auto& arg : argv) {
            raw.push_back(const_cast<char*>(arg.c_str()));
        }
        raw.push_back(nullptr);
        execvp(raw[0], raw.data());
        _exit(127);
    }

    int status = 0;
    bool exited = false;
    for (int waited = 0; waited < timeout_seconds; ++waited) {
        pid_t rc = waitpid(pid, &status, WNOHANG);
        if (rc == pid) {
            exited = true;
            break;
        }
        if (rc < 0) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            error = "waitpid failed";
            return 127;
        }
        sleep(1);
    }

    if (!exited) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        error = "timed out after " + std::to_string(timeout_seconds) + "s";
        return 124;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    error = "killed by signal " + std::to_string(WTERMSIG(status));
    return 128 + WTERMSIG(status);
}

std::vector<std::string> base_args(
    const Config& cfg,
    const std::string& browser,
    const std::vector<std::string>& resolver_rules) {
    std::vector<std::string> args = {
        browser,
        "--headless",
        "--no-sandbox",
        "--disable-gpu",
        "--disable-dev-shm-usage",
        "--hide-scrollbars",
        "--no-first-run",
        "--no-default-browser-check",
        "--disable-extensions",
        "--disable-background-networking",
        "--user-agent=" + cfg.user_agent,
        "--window-size=" + std::to_string(cfg.render_width) + "," +
            std::to_string(cfg.render_height),
        "--virtual-time-budget=" +
            std::to_string(cfg.render_virtual_time_ms)};

    // Chrome fetches subresources itself, so the HTTP blocklist cannot stop
    // ads a page pulls in. Mapping the same hosts to ~NOTFOUND closes that gap
    // without a proxy, a browser extension, or a persistent process.
    if (!resolver_rules.empty()) {
        std::string value;
        for (std::size_t i = 0; i < resolver_rules.size(); ++i) {
            if (i > 0) {
                value += ", ";
            }
            value += "MAP " + resolver_rules[i] + " ~NOTFOUND";
        }
        args.push_back("--host-resolver-rules=" + value);
    }
    return args;
}

bool keep_artifact(const std::filesystem::path& path, long max_bytes) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0) {
        return false;
    }
    if (size > static_cast<std::uintmax_t>(max_bytes)) {
        std::filesystem::remove(path, ec);
        return false;
    }
    return true;
}

std::string relative_artifact(int64_t id, const std::string& name) {
    return "snapshots/" + std::to_string(id) + "/" + name;
}

} // namespace

std::string find_browser(const Config& cfg) {
    std::vector<std::string> candidates;
    if (!cfg.browser_path.empty()) {
        candidates.push_back(cfg.browser_path);
    }

    // A browser unpacked next to arcbox counts as installed; this is how the
    // Chrome for Testing drop is picked up without touching the system.
    static const char* names[] = {
        "chrome-headless-shell",
        "chromium",
        "chromium-browser",
        "google-chrome-stable",
        "google-chrome",
        "/home/user/.local/share/arcbox-browser/"
        "chrome-headless-shell-linux-arm64/chrome-headless-shell",
    };
    for (const char* name : names) {
        candidates.emplace_back(name);
    }

    std::string error;
    for (const std::string& candidate : candidates) {
        if (candidate.empty()) {
            continue;
        }
        int rc = run_browser_process({candidate, "--version"}, {}, 15, error);
        if (rc == 0) {
            return candidate;
        }
    }
    return std::string();
}

const char* render_tier_name(RenderTier tier) {
    switch (tier) {
        case RenderTier::Skip:
            return "skip";
        case RenderTier::Dom:
            return "dom";
        case RenderTier::Screenshot:
            return "screenshot";
        case RenderTier::Full:
            return "full";
    }
    return "unknown";
}

RenderPlan choose_render_plan(const Config& cfg,
                              const HostMemory& mem,
                              int64_t source_bytes) {
    RenderPlan plan;
    if (!cfg.render_enabled) {
        plan.reason = "rendering disabled (ARCBOX_RENDER=1 enables it)";
        return plan;
    }

    // Size-aware admission: the renderer's footprint grows with the DOM, so a
    // page that is already huge before rendering is not worth the risk. The
    // HTTP copy is archived regardless.
    if (source_bytes > cfg.render_max_source_bytes) {
        plan.reason = "source is " + format_size(source_bytes) +
                      ", above the render ceiling of " +
                      format_size(cfg.render_max_source_bytes);
        return plan;
    }

    // The queue guard stays authoritative: a Pause means the host is short on
    // memory, so no browser starts even for the cheapest artifact.
    const AdmissionDecision admission = admit_capture(cfg, mem);
    if (admission.action == Admission::Pause) {
        plan.reason = "admission pause (" + admission.reason + ")";
        return plan;
    }

    const long available_mb = static_cast<long>(mem.mem_available_kb / 1024);
    if (available_mb >= cfg.render_full_mb) {
        plan.tier = RenderTier::Full;
    } else if (available_mb >= cfg.render_lite_mb) {
        plan.tier = RenderTier::Screenshot;
    } else if (available_mb >= cfg.render_min_mb) {
        plan.tier = RenderTier::Dom;
    }

    // Throttling means PSI is elevated: drop one artifact even when the
    // free-memory counter still looks comfortable.
    if (plan.tier != RenderTier::Skip &&
        admission.action == Admission::Throttle) {
        if (plan.tier == RenderTier::Full) {
            plan.tier = RenderTier::Screenshot;
        } else if (plan.tier == RenderTier::Screenshot) {
            plan.tier = RenderTier::Dom;
        } else {
            plan.tier = RenderTier::Skip;
        }
        plan.reason = "admission throttle (" + admission.reason +
                      "), degraded to " + render_tier_name(plan.tier);
    }

    if (plan.tier == RenderTier::Skip) {
        if (plan.reason.empty()) {
            plan.reason = "MemAvailable " + std::to_string(available_mb) +
                          "M below the cheapest render tier " +
                          std::to_string(cfg.render_min_mb) + "M";
        }
        return plan;
    }

    // With degradation disabled the render is all-or-nothing again: either the
    // full artifact set is affordable or the browser is not started at all.
    if (!cfg.render_degrade && plan.tier != RenderTier::Full) {
        plan.reason = "only " + std::to_string(available_mb) +
                      "M free and degradation is disabled";
        plan.tier = RenderTier::Skip;
        return plan;
    }

    if (plan.reason.empty()) {
        plan.reason = "render tier " + std::string(render_tier_name(plan.tier)) +
                      " at " + std::to_string(available_mb) + "M free";
    }
    return plan;
}

RenderResult render_page(const Config& cfg,
                         const std::string& url,
                         int64_t id,
                         const RenderPlan& plan) {
    RenderResult result;
    if (!plan.dom() && !plan.screenshot() && !plan.pdf()) {
        result.error = "empty render plan";
        return result;
    }

    const std::string browser = find_browser(cfg);
    if (browser.empty()) {
        result.error = "no headless browser found";
        return result;
    }
    result.attempted = true;

    cfg.ensure_dirs();
    const std::filesystem::path directory =
        cfg.snapshots_dir() / std::to_string(id);
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);

    std::string error;
    const int timeout = static_cast<int>(cfg.render_timeout_seconds);

    // One blocklist read per render, not per artifact.
    constexpr std::size_t kMaxResolverRules = 512;
    Adblock adblock(cfg.blocklist_path());
    const std::vector<std::string> resolver_rules =
        adblock.resolver_rules(url, kMaxResolverRules);

    // Rendered DOM first: it is the cheapest artifact and the one replay
    // needs when the rest fails.
    std::vector<std::string> args;
    int rc = 0;
    if (plan.dom()) {
        const std::filesystem::path html_path =
            cfg.resolve(relative_artifact(id, "rendered.html"));
        args = base_args(cfg, browser, resolver_rules);
        args.push_back("--dump-dom");
        args.push_back(url);
        rc = run_browser_process(args, html_path.string(), timeout, error);
        if (rc == 0 && keep_artifact(html_path, cfg.render_max_bytes)) {
            result.rendered_html_path =
                relative_artifact(id, "rendered.html");
        } else if (result.error.empty() && rc != 0) {
            result.error = "rendered html: " + error;
        }
    }

    if (plan.screenshot()) {
        const std::filesystem::path shot_path =
            cfg.resolve(relative_artifact(id, "screenshot.png"));
        args = base_args(cfg, browser, resolver_rules);
        args.push_back("--screenshot=" + shot_path.string());
        args.push_back(url);
        error.clear();
        rc = run_browser_process(args, {}, timeout, error);
        if (rc == 0 && keep_artifact(shot_path, cfg.render_max_bytes)) {
            result.screenshot_path = relative_artifact(id, "screenshot.png");
        } else if (result.error.empty() && rc != 0) {
            result.error = "screenshot: " + error;
        }
    }

    if (plan.pdf() && cfg.render_pdf) {
        const std::filesystem::path pdf_path =
            cfg.resolve(relative_artifact(id, "page.pdf"));
        args = base_args(cfg, browser, resolver_rules);
        args.push_back("--print-to-pdf=" + pdf_path.string());
        args.push_back(url);
        error.clear();
        rc = run_browser_process(args, {}, timeout, error);
        if (rc == 0 && keep_artifact(pdf_path, cfg.render_max_bytes)) {
            result.pdf_path = relative_artifact(id, "page.pdf");
        }
    }

    result.ok = !result.rendered_html_path.empty() ||
                !result.screenshot_path.empty() || !result.pdf_path.empty();
    if (result.ok) {
        result.error.clear();
    }
    return result;
}

} // namespace arcbox
