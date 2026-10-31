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

std::vector<std::string> base_args(const Config& cfg,
                                   const std::string& browser) {
    return {browser,
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

RenderResult render_page(const Config& cfg,
                         const std::string& url,
                         int64_t id) {
    RenderResult result;
    const std::string browser = find_browser(cfg);
    if (browser.empty()) {
        result.error = "no headless browser found";
        return result;
    }
    result.attempted = true;

    cfg.ensure_dirs();
    const std::filesystem::path directory = cfg.snapshots_dir() / std::to_string(id);
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);

    std::string error;
    const int timeout = static_cast<int>(cfg.render_timeout_seconds);

    // Rendered DOM first: it is the cheapest artifact and the one replay
    // needs when the rest fails.
    const std::filesystem::path html_path = cfg.resolve(
        relative_artifact(id, "rendered.html"));
    std::vector<std::string> args = base_args(cfg, browser);
    args.push_back("--dump-dom");
    args.push_back(url);
    int rc = run_browser_process(args, html_path.string(), timeout, error);
    if (rc == 0 && keep_artifact(html_path, cfg.render_max_bytes)) {
        result.rendered_html_path = relative_artifact(id, "rendered.html");
    } else if (result.error.empty() && rc != 0) {
        result.error = "rendered html: " + error;
    }

    const std::filesystem::path shot_path =
        cfg.resolve(relative_artifact(id, "screenshot.png"));
    args = base_args(cfg, browser);
    args.push_back("--screenshot=" + shot_path.string());
    args.push_back(url);
    error.clear();
    rc = run_browser_process(args, {}, timeout, error);
    if (rc == 0 && keep_artifact(shot_path, cfg.render_max_bytes)) {
        result.screenshot_path = relative_artifact(id, "screenshot.png");
    } else if (result.error.empty() && rc != 0) {
        result.error = "screenshot: " + error;
    }

    if (cfg.render_pdf) {
        const std::filesystem::path pdf_path =
            cfg.resolve(relative_artifact(id, "page.pdf"));
        args = base_args(cfg, browser);
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
