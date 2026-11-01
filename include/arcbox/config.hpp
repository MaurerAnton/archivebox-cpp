#pragma once

#include <filesystem>
#include <string>

namespace arcbox {

struct Config {
    std::filesystem::path data_dir;
    std::string user_agent =
        "ArchiveBox.cpp/0.1 (+https://github.com/MaurerAnton/archivebox-cpp)";
    long max_body_bytes = 100LL * 1024 * 1024;
    long connect_timeout_seconds = 15;
    long timeout_seconds = 60;

    // PSI-aware admission control.
    long min_mem_available_kb = 1500LL * 1024;
    // How much of free *zram* swap counts towards the memory floor. A zram
    // device is compressed RAM: pages sitting in it are ~2-3x smaller than in
    // RAM, so counting all of it would be optimistic and half of it is a safe
    // default. Disk-backed swap is never credited (see effective_available_kb).
    double zram_swap_weight = 0.5;
    double psi_some_avg10_max = 10.0;
    long throttle_sleep_seconds = 15;
    long pause_sleep_seconds = 60;

    // Queue retry policy. A failed capture is retried with exponential
    // backoff instead of being dropped on the first error.
    int max_attempts = 5;
    long retry_backoff_seconds = 30;
    long retry_backoff_max_seconds = 3600;

    bool use_cgroup = true;
    std::string slice;
    std::string memory_max = "1800M";
    std::string memory_high = "1200M";
    std::string memory_swap_max = "512M";
    std::string cpu_quota = "200%";
    int tasks_max = 300;

    // Headless browser rendering (v0.3). Off by default: this is the one
    // capture step that can eat hundreds of megabytes, so it is opt-in and
    // always runs behind the same admission guard as everything else.
    bool render_enabled = false;
    bool render_pdf = false;
    // Answer renderer requests through the DevTools protocol so the blocklist
    // applies to what Chrome fetches itself, not just to arcbox's own copy.
    bool render_intercept = true;
    std::string browser_path;
    long render_timeout_seconds = 60;
    long render_virtual_time_ms = 10000;
    long render_max_bytes = 25LL * 1024 * 1024;
    int render_width = 1280;
    int render_height = 2000;

    // Degradation ladder: rendering is never all-or-nothing. The less headroom
    // the host has, the cheaper the artifact set; below the cheapest tier the
    // page is archived with no browser at all. render_min_mb sits below the
    // default admission floor on purpose: that tier is only reachable when an
    // operator lowers ARCBOX_MIN_MEM_AVAILABLE_KB deliberately.
    bool render_degrade = true;
    long render_full_mb = 2500;
    long render_lite_mb = 1500;
    long render_min_mb = 1000;
    long render_max_source_bytes = 20LL * 1024 * 1024;

    static Config load();
    void ensure_dirs() const;

    std::filesystem::path db_path() const;
    std::filesystem::path warc_dir() const;
    std::filesystem::path snapshots_dir() const;
    std::filesystem::path objects_dir() const;
    std::filesystem::path blocklist_path() const;
    std::filesystem::path resolve(const std::string& relative) const;
};

} // namespace arcbox
